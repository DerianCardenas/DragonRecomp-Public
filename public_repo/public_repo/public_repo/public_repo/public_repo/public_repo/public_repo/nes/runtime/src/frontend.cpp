#include "nes/frontend.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <vector>

#include "dragon/save_state.h"
#include "nes/cartridge.h"
#include "nes/interpreter.h"

#if defined(DRAGON_HAVE_SDL3)
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

namespace dragon::nes {

FrameStats analyze_framebuffer(
    const std::array<uint32_t, Ppu::kScreenWidth * Ppu::kScreenHeight>& fb) noexcept {
    FrameStats stats;
    stats.backdrop_color = fb[0];

    std::array<uint32_t, 64> seen_colors{};
    size_t seen_count = 0;

    for (uint32_t px : fb) {
        if (px != stats.backdrop_color) {
            ++stats.non_backdrop_pixels;
        }
        bool found = false;
        for (size_t i = 0; i < seen_count; ++i) {
            if (seen_colors[i] == px) {
                found = true;
                break;
            }
        }
        if (!found && seen_count < seen_colors.size()) {
            seen_colors[seen_count++] = px;
        }
    }

    stats.distinct_colors = seen_count;
    return stats;
}

bool write_framebuffer_ppm(const std::filesystem::path& path,
                           const std::array<uint32_t, Ppu::kScreenWidth * Ppu::kScreenHeight>& fb) {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    const std::string header = "P6\n256 240\n255\n";
    out.write(header.data(), static_cast<std::streamsize>(header.size()));

    std::array<uint8_t, Ppu::kScreenWidth * 3> row_buf{};
    for (uint32_t y = 0; y < Ppu::kScreenHeight; ++y) {
        for (uint32_t x = 0; x < Ppu::kScreenWidth; ++x) {
            const uint32_t rgba = fb[static_cast<size_t>(y) * Ppu::kScreenWidth + x];
            row_buf[static_cast<size_t>(x) * 3U + 0U] = static_cast<uint8_t>((rgba >> 24U) & 0xFFU);
            row_buf[static_cast<size_t>(x) * 3U + 1U] = static_cast<uint8_t>((rgba >> 16U) & 0xFFU);
            row_buf[static_cast<size_t>(x) * 3U + 2U] = static_cast<uint8_t>((rgba >> 8U) & 0xFFU);
        }
        out.write(reinterpret_cast<const char*>(row_buf.data()),
                  static_cast<std::streamsize>(row_buf.size()));
    }

    return out.good();
}

namespace {

void print_runner_usage(const char* prog) {
    std::printf(
        "usage: %s [options] [rom.nes]\n"
        "options:\n"
        "  --recomp               Run statically recompiled C99 code (default when linked)\n"
        "  --interp               Run CPU interpreter mode\n"
        "  --verify               Run lock-step verification (recompiled vs interpreter)\n"
        "  --rom <path>           Override path to iNES ROM file\n"
        "  --save-path <path>     Path to battery-backed SRAM (.sav)\n"
        "  --state-slot <N>       Select initial save state slot (0-9, default: 1)\n"
        "  --load-state <path>    Load save state file on launch\n"
        "  --save-state <path>    Save state file path\n"
        "  --headlessframes <N>   Run N frames without SDL window, then exit\n"
        "  --headless-frames <N>  Alias for --headlessframes <N>\n"
        "  --press-start <frame>  Press Start button at frame <frame> (for 8 frames)\n"
        "  --dump-ppm <file.ppm>  Write final 256x240 framebuffer to a binary PPM file\n"
        "  --scale <S>            Window integer scale factor (default: 3)\n"
        "  --fullscreen           Launch in fullscreen mode (toggle: F11 / Alt+Enter)\n"
        "  --no-aspect, --stretch Disable 4:3 aspect ratio correction (toggle: F8)\n"
        "  --crt, --scanlines     Enable CRT retro scanlines effect (toggle: F9)\n"
        "  -h, --help             Show this help message\n",
        prog);
}

struct FrameHookContext {
    const RunnerOptions* opts = nullptr;
    int current_slot = 1;
    bool crt_filter = false;
    bool fullscreen = false;
    bool aspect_ratio = true;
    std::vector<uint32_t> crt_buffer;
#if defined(DRAGON_HAVE_SDL3)
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_AudioStream* audio_stream = nullptr;
    uint64_t next_frame_ns = 0ULL;
#endif
};

void on_frame_hook(CpuState* cpu, Bus* bus, void* user_data) {
    auto* ctx = static_cast<FrameHookContext*>(user_data);
    if (!ctx || !ctx->opts) {
        return;
    }

    const uint64_t frame = bus->completed_frames();
    uint8_t scripted_buttons = 0U;
    if (ctx->opts->press_start_frame > 0ULL && frame >= ctx->opts->press_start_frame &&
        frame < ctx->opts->press_start_frame + 8ULL) {
        scripted_buttons |= kButtonStart;
    }

#if defined(DRAGON_HAVE_SDL3)
    if (!ctx->opts->headless && ctx->renderer && ctx->texture) {
        if (ctx->audio_stream) {
            std::array<int16_t, 2048> pcm_buf{};
            const size_t n = bus->apu().pop_samples(pcm_buf.data(), pcm_buf.size());
            if (n > 0U && SDL_GetAudioStreamQueued(ctx->audio_stream) <
                              static_cast<int>(Apu::kSampleRate * sizeof(int16_t) / 4U)) {
                SDL_PutAudioStreamData(ctx->audio_stream, pcm_buf.data(),
                                       static_cast<int>(n * sizeof(int16_t)));
            }
        } else {
            bus->apu().clear_samples();
        }

        if (ctx->crt_filter) {
            if (ctx->crt_buffer.size() < Ppu::kScreenWidth * Ppu::kScreenHeight) {
                ctx->crt_buffer.resize(Ppu::kScreenWidth * Ppu::kScreenHeight);
            }
            const auto& src = bus->ppu().framebuffer();
            for (size_t y = 0; y < Ppu::kScreenHeight; ++y) {
                const bool scanline = (y % 2 != 0);
                for (size_t x = 0; x < Ppu::kScreenWidth; ++x) {
                    const size_t idx = y * Ppu::kScreenWidth + x;
                    uint32_t px = src[idx];
                    if (scanline) {
                        uint32_t r = ((px >> 24) & 0xFF) * 55 / 100;
                        uint32_t g = ((px >> 16) & 0xFF) * 55 / 100;
                        uint32_t b = ((px >> 8) & 0xFF) * 55 / 100;
                        uint32_t a = px & 0xFF;
                        px = (r << 24) | (g << 16) | (b << 8) | a;
                    }
                    ctx->crt_buffer[idx] = px;
                }
            }
            SDL_UpdateTexture(ctx->texture, nullptr, ctx->crt_buffer.data(),
                              static_cast<int>(Ppu::kScreenWidth * sizeof(uint32_t)));
        } else {
            SDL_UpdateTexture(ctx->texture, nullptr, bus->ppu().framebuffer().data(),
                              static_cast<int>(Ppu::kScreenWidth * sizeof(uint32_t)));
        }

        int out_w = 0, out_h = 0;
        SDL_GetRenderOutputSize(ctx->renderer, &out_w, &out_h);
        SDL_FRect dst_rect{0.0f, 0.0f, static_cast<float>(out_w), static_cast<float>(out_h)};
        if (ctx->aspect_ratio && out_h > 0) {
            constexpr float kTargetAspect = 4.0f / 3.0f;
            const float win_aspect = static_cast<float>(out_w) / static_cast<float>(out_h);
            if (win_aspect > kTargetAspect) {
                dst_rect.w = static_cast<float>(out_h) * kTargetAspect;
                dst_rect.h = static_cast<float>(out_h);
                dst_rect.x = (static_cast<float>(out_w) - dst_rect.w) * 0.5f;
                dst_rect.y = 0.0f;
            } else {
                dst_rect.w = static_cast<float>(out_w);
                dst_rect.h = static_cast<float>(out_w) / kTargetAspect;
                dst_rect.x = 0.0f;
                dst_rect.y = (static_cast<float>(out_h) - dst_rect.h) * 0.5f;
            }
        }

        SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);
        SDL_RenderClear(ctx->renderer);
        SDL_RenderTexture(ctx->renderer, ctx->texture, nullptr, &dst_rect);
        SDL_RenderPresent(ctx->renderer);

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                cpu->halted = 1U;
            } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                    cpu->halted = 1U;
                } else if (ev.key.scancode == SDL_SCANCODE_F5) {
                    const auto target_path =
                        (ctx->opts->save_state_path.empty() ||
                         ctx->current_slot != ctx->opts->state_slot)
                            ? slot_state_path(ctx->opts->rom_path, ctx->current_slot)
                            : ctx->opts->save_state_path;
                    if (bus->save_state(*cpu, target_path)) {
                        std::printf("[DragonRecomp] Saved state to slot %d: %s\n",
                                    ctx->current_slot, target_path.string().c_str());
                    } else {
                        std::fprintf(stderr, "[DragonRecomp] Failed to save state to slot %d: %s\n",
                                     ctx->current_slot, target_path.string().c_str());
                    }
                } else if (ev.key.scancode == SDL_SCANCODE_F7) {
                    const auto target_path =
                        (ctx->opts->load_state_path.empty() ||
                         ctx->current_slot != ctx->opts->state_slot)
                            ? slot_state_path(ctx->opts->rom_path, ctx->current_slot)
                            : ctx->opts->load_state_path;
                    if (bus->load_state(*cpu, target_path)) {
                        std::printf("[DragonRecomp] Loaded state from slot %d: %s\n",
                                    ctx->current_slot, target_path.string().c_str());
                    } else {
                        std::fprintf(stderr,
                                     "[DragonRecomp] Failed to load state from slot %d: %s\n",
                                     ctx->current_slot, target_path.string().c_str());
                    }
                } else if (ev.key.scancode == SDL_SCANCODE_F8) {
                    ctx->aspect_ratio = !ctx->aspect_ratio;
                    std::printf("[DragonRecomp] Aspect ratio correction: %s\n",
                                ctx->aspect_ratio ? "ON (4:3)" : "OFF (stretched)");
                } else if (ev.key.scancode == SDL_SCANCODE_F9) {
                    ctx->crt_filter = !ctx->crt_filter;
                    std::printf("[DragonRecomp] CRT scanlines: %s\n",
                                ctx->crt_filter ? "ON" : "OFF");
                } else if (ev.key.scancode == SDL_SCANCODE_F11 ||
                           (ev.key.scancode == SDL_SCANCODE_RETURN &&
                            (SDL_GetModState() & SDL_KMOD_ALT))) {
                    ctx->fullscreen = !ctx->fullscreen;
                    SDL_SetWindowFullscreen(ctx->window, ctx->fullscreen);
                    std::printf("[DragonRecomp] Fullscreen: %s\n", ctx->fullscreen ? "ON" : "OFF");
                } else if (ev.key.scancode >= SDL_SCANCODE_1 && ev.key.scancode <= SDL_SCANCODE_9) {
                    if (!bus->is_vs_system() ||
                        (ev.key.scancode != SDL_SCANCODE_5 && ev.key.scancode != SDL_SCANCODE_6 &&
                         ev.key.scancode != SDL_SCANCODE_7)) {
                        ctx->current_slot = static_cast<int>(ev.key.scancode - SDL_SCANCODE_1 + 1);
                        std::printf("[DragonRecomp] Selected state slot %d\n", ctx->current_slot);
                    }
                } else if (ev.key.scancode == SDL_SCANCODE_0) {
                    ctx->current_slot = 0;
                    std::printf("[DragonRecomp] Selected state slot %d\n", ctx->current_slot);
                } else if (bus->is_vs_system() && (ev.key.scancode == SDL_SCANCODE_C ||
                                                   ev.key.scancode == SDL_SCANCODE_5 ||
                                                   ev.key.scancode == SDL_SCANCODE_6)) {
                    // Edge-triggered coin insertion: one physical coin per key press.
                    // A Vs. System coin acceptor pulses a single $4016 bit for ~40-70ms
                    // (nesdev.org/wiki/Vs._System); polling the key level would re-pulse
                    // every frame and confuse the game's credit counter.
                    // C / 5 = coin slot 1, 6 = coin slot 2.
                    bus->insert_coin(ev.key.scancode == SDL_SCANCODE_6 ? 1U : 0U);
                } else if (bus->is_vs_system() && ev.key.scancode == SDL_SCANCODE_7) {
                    // Service button credit (key 7): alternate Vs. credit input used by
                    // some games instead of the coin acceptors.
                    bus->insert_service();
                }
            }
        }

        const bool* keys = SDL_GetKeyboardState(nullptr);
        uint8_t buttons = scripted_buttons;
        if (keys) {
            if (keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_J]) {
                buttons |= kButtonA;
            }
            if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_K]) {
                buttons |= kButtonB;
            }
            if (keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_TAB]) {
                buttons |= kButtonSelect;
            }
            if (keys[SDL_SCANCODE_RETURN]) {
                buttons |= kButtonStart;
            }
            if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W]) {
                buttons |= kButtonUp;
            }
            if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S]) {
                buttons |= kButtonDown;
            }
            if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A]) {
                buttons |= kButtonLeft;
            }
            if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) {
                buttons |= kButtonRight;
            }
            // Coin insertion is edge-triggered in the SDL event loop above
            // (keys C / 5 / 6), not polled here, so one press = one coin.
        }
        bus->set_controller(0U, buttons);

        // Auto-insert credit at frame 25 for arcade titles to seamlessly advance past attract mode
        if (bus->is_vs_system() && bus->completed_frames() == 25ULL) {
            bus->insert_coin(0U);
        }

        // NTSC NES frame duration: 89341.5 dots / 5.369318 MHz = ~16,639,267 ns (60.0988 Hz)
        constexpr uint64_t kFrameDurationNs = 16639267ULL;
        const uint64_t now_ns = SDL_GetTicksNS();
        if (ctx->next_frame_ns == 0ULL) {
            ctx->next_frame_ns = now_ns + kFrameDurationNs;
        } else {
            if (now_ns < ctx->next_frame_ns) {
                SDL_DelayNS(ctx->next_frame_ns - now_ns);
            }
            ctx->next_frame_ns += kFrameDurationNs;
            if (now_ns > ctx->next_frame_ns + kFrameDurationNs * 4ULL) {
                ctx->next_frame_ns = now_ns + kFrameDurationNs;
            }
        }
        return;
    }
#endif

    if (ctx->opts->press_start_frame > 0ULL) {
        bus->set_controller(0U, scripted_buttons);
    }
}

int run_verify_mode(const RunnerOptions& opts, const Cartridge& cart) {
    Bus recomp_bus;
    recomp_bus.load_cartridge(cart);
    recomp_bus.set_dispatch_table(opts.dispatch_table, opts.dispatch_count);
    CpuState recomp_cpu{};
    cpu_init(recomp_cpu, &recomp_bus);
    cpu_reset(recomp_cpu);

    Bus interp_bus;
    interp_bus.load_cartridge(cart);
    CpuState interp_cpu{};
    cpu_init(interp_cpu, &interp_bus);
    cpu_reset(interp_cpu);

    const uint64_t total_frames = (opts.headless_frames > 0ULL) ? opts.headless_frames : 180ULL;
    recomp_bus.set_max_frames(total_frames);

    FrameHookContext ctx;
    ctx.opts = &opts;
    recomp_bus.set_frame_callback(&on_frame_hook, &ctx);
    interp_bus.set_frame_callback(&on_frame_hook, &ctx);

    RecompFunc entry_fn = opts.reset_func;
    if (!entry_fn) {
        entry_fn =
            recomp_bus.lookup_function(recomp_cpu.pc, recomp_bus.current_prg_bank(recomp_cpu.pc));
    }
    if (!entry_fn) {
        std::fprintf(stderr, "verify error: no compiled function for RESET vector $%04X\n",
                     recomp_cpu.pc);
        return 1;
    }

    entry_fn(&recomp_cpu);
    cpu_run_frames_interpreter(interp_cpu, total_frames);

    if (recomp_bus.fallback_count() != 0U) {
        std::fprintf(stderr, "verify error: %u interpreter fallbacks occurred\n",
                     recomp_bus.fallback_count());
        return 1;
    }

    for (uint16_t addr = 0U; addr < 0x0800U; ++addr) {
        if (recomp_bus.peek(addr) != interp_bus.peek(addr)) {
            std::fprintf(stderr, "verify error: RAM mismatch at $%04X (recomp=%02X interp=%02X)\n",
                         addr, recomp_bus.peek(addr), interp_bus.peek(addr));
            return 1;
        }
    }

    if (recomp_bus.ppu().framebuffer() != interp_bus.ppu().framebuffer()) {
        std::fprintf(stderr, "verify error: framebuffer mismatch after %llu frames\n",
                     static_cast<unsigned long long>(total_frames));
        return 1;
    }

    if (!opts.dump_ppm_path.empty()) {
        if (!write_framebuffer_ppm(opts.dump_ppm_path, recomp_bus.ppu().framebuffer())) {
            std::fprintf(stderr, "Failed to write PPM to %s\n",
                         opts.dump_ppm_path.string().c_str());
            return 1;
        }
    }

    std::printf("verify ok: %llu frames matched between recompiled and interpreter (0 fallbacks)\n",
                static_cast<unsigned long long>(total_frames));
    return 0;
}

}  // namespace

bool parse_runner_args(int argc, char** argv, RunnerOptions& opts, int& exit_code) {
    exit_code = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_runner_usage(argv[0]);
            return false;
        }
        if (arg == "--recomp") {
            opts.mode = RunMode::Recompiled;
        } else if (arg == "--interp" || arg == "--interpret-only") {
            opts.mode = RunMode::Interpreter;
        } else if (arg == "--verify") {
            opts.mode = RunMode::Verify;
        } else if ((arg == "--headlessframes" || arg == "--headless-frames") && i + 1 < argc) {
            opts.headless = true;
            opts.headless_frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--press-start" && i + 1 < argc) {
            opts.press_start_frame = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--dump-ppm" && i + 1 < argc) {
            opts.dump_ppm_path = argv[++i];
        } else if (arg == "--rom" && i + 1 < argc) {
            opts.rom_path = argv[++i];
        } else if ((arg == "--save-path" || arg == "--save") && i + 1 < argc) {
            opts.save_path = argv[++i];
        } else if ((arg == "--state-slot" || arg == "--slot") && i + 1 < argc) {
            opts.state_slot = std::max(0, std::atoi(argv[++i]));
        } else if (arg == "--load-state" && i + 1 < argc) {
            opts.load_state_path = argv[++i];
        } else if (arg == "--save-state" && i + 1 < argc) {
            opts.save_state_path = argv[++i];
        } else if (arg == "--no-aspect" || arg == "--stretch") {
            opts.aspect_ratio = false;
        } else if (arg == "--scale" && i + 1 < argc) {
            opts.window_scale = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--fullscreen") {
            opts.fullscreen = true;
        } else if (arg == "--crt" || arg == "--scanlines") {
            opts.crt_filter = true;
        } else if (arg == "--palette" && i + 1 < argc) {
            opts.palette = argv[++i];
        } else if (!arg.empty() && arg[0] != '-' && opts.rom_path.empty()) {
            opts.rom_path = arg;
        } else {
            std::fprintf(stderr, "unknown or incomplete option: %s\n", arg.c_str());
            print_runner_usage(argv[0]);
            exit_code = 1;
            return false;
        }
    }
    return true;
}

int run_nes_app(const RunnerOptions& opts) {
    if (opts.rom_path.empty()) {
        std::fprintf(stderr, "error: no ROM path specified\n");
        return 1;
    }

    try {
        const Cartridge cart = load_ines_rom(opts.rom_path, opts.expected_sha1);
        if (opts.mode == RunMode::Verify) {
            return run_verify_mode(opts, cart);
        }

        Bus bus;
        bus.load_cartridge(cart);
        if (!opts.palette.empty()) {
            if (opts.palette == "vs0001" || opts.palette == "rp2c04-0001") {
                bus.set_ppu_palette(PpuPalette::Vs0001);
            } else if (opts.palette == "vs0002" || opts.palette == "rp2c04-0002") {
                bus.set_ppu_palette(PpuPalette::Vs0002);
            } else if (opts.palette == "vs0003" || opts.palette == "rp2c04-0003") {
                bus.set_ppu_palette(PpuPalette::Vs0003);
            } else if (opts.palette == "vs0004" || opts.palette == "rp2c04-0004") {
                bus.set_ppu_palette(PpuPalette::Vs0004);
            } else if (opts.palette == "vs2c03" || opts.palette == "rp2c03") {
                bus.set_ppu_palette(PpuPalette::Vs2C03);
            } else if (opts.palette == "smooth") {
                bus.set_ppu_palette(PpuPalette::SmoothFbx);
            } else if (opts.palette == "2c02" || opts.palette == "default") {
                bus.set_ppu_palette(PpuPalette::Ntsc2C02);
            }
        }
        bus.set_dispatch_table(opts.dispatch_table, opts.dispatch_count);

        std::filesystem::path save_file = opts.save_path;
        if (save_file.empty() && !opts.rom_path.empty()) {
            save_file = opts.rom_path;
            save_file.replace_extension(".sav");
        }
        if (bus.has_battery() && !save_file.empty()) {
            bus.load_battery_ram(save_file);
        }

        CpuState cpu{};
        cpu_init(cpu, &bus);
        cpu_reset(cpu);

        if (!opts.load_state_path.empty()) {
            if (bus.load_state(cpu, opts.load_state_path)) {
                std::printf("[DragonRecomp] Loaded initial state from %s\n",
                            opts.load_state_path.string().c_str());
            } else {
                std::fprintf(stderr, "[DragonRecomp] Failed to load initial state from %s\n",
                             opts.load_state_path.string().c_str());
            }
        }

        if (opts.headless && opts.headless_frames > 0ULL) {
            bus.set_max_frames(opts.headless_frames);
        }

        FrameHookContext ctx;
        ctx.opts = &opts;
        ctx.current_slot = opts.state_slot;
        ctx.crt_filter = opts.crt_filter;
        ctx.fullscreen = opts.fullscreen;
        ctx.aspect_ratio = opts.aspect_ratio;

#if defined(DRAGON_HAVE_SDL3)
        if (!opts.headless) {
            SDL_SetMainReady();
            if (!SDL_Init(SDL_INIT_VIDEO)) {
                std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
                return 1;
            }
            if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
                SDL_AudioSpec audio_spec{};
                audio_spec.format = SDL_AUDIO_S16;
                audio_spec.channels = 1;
                audio_spec.freq = static_cast<int>(Apu::kSampleRate);
                ctx.audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                             &audio_spec, nullptr, nullptr);
                if (ctx.audio_stream) {
                    SDL_ResumeAudioStreamDevice(ctx.audio_stream);
                }
            }
            const int base_h = static_cast<int>(Ppu::kScreenHeight) * opts.window_scale;
            const int win_h = base_h;
            const int win_w = opts.aspect_ratio
                                  ? ((base_h * 4) / 3)
                                  : (static_cast<int>(Ppu::kScreenWidth) * opts.window_scale);
            SDL_WindowFlags win_flags = SDL_WINDOW_RESIZABLE;
            if (opts.fullscreen) {
                win_flags |= SDL_WINDOW_FULLSCREEN;
            }
            if (!SDL_CreateWindowAndRenderer(opts.window_title.c_str(), win_w, win_h, win_flags,
                                             &ctx.window, &ctx.renderer)) {
                std::fprintf(stderr, "SDL_CreateWindowAndRenderer failed: %s\n", SDL_GetError());
                if (ctx.audio_stream) {
                    SDL_DestroyAudioStream(ctx.audio_stream);
                }
                SDL_Quit();
                return 1;
            }
            ctx.texture = SDL_CreateTexture(
                ctx.renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING,
                static_cast<int>(Ppu::kScreenWidth), static_cast<int>(Ppu::kScreenHeight));
            if (!ctx.texture) {
                std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
                if (ctx.audio_stream) {
                    SDL_DestroyAudioStream(ctx.audio_stream);
                }
                SDL_DestroyRenderer(ctx.renderer);
                SDL_DestroyWindow(ctx.window);
                SDL_Quit();
                return 1;
            }
            SDL_SetTextureScaleMode(ctx.texture, SDL_SCALEMODE_NEAREST);
        }
#else
        if (!opts.headless) {
            std::fprintf(stderr, "error: built without SDL3 support; use --headlessframes <N>\n");
            return 1;
        }
#endif

        bus.set_frame_callback(&on_frame_hook, &ctx);

        if (opts.mode == RunMode::Recompiled &&
            (opts.reset_func != nullptr || opts.dispatch_count > 0U)) {
            while (!cpu.halted) {
                RecompFunc fn = bus.lookup_function(cpu.pc, bus.current_prg_bank(cpu.pc));
                if (fn) {
                    fn(&cpu);
                } else {
                    runtime_dispatch(&cpu, cpu.pc, cpu.pc);
                }
            }
        } else {
            while (!cpu.halted) {
                cpu_step_with_ppu(cpu);
            }
        }

#if defined(DRAGON_HAVE_SDL3)
        if (!opts.headless) {
            if (ctx.audio_stream) {
                SDL_DestroyAudioStream(ctx.audio_stream);
            }
            if (ctx.texture) {
                SDL_DestroyTexture(ctx.texture);
            }
            if (ctx.renderer) {
                SDL_DestroyRenderer(ctx.renderer);
            }
            if (ctx.window) {
                SDL_DestroyWindow(ctx.window);
            }
            SDL_Quit();
        }
#endif

        if (!opts.dump_ppm_path.empty()) {
            if (!write_framebuffer_ppm(opts.dump_ppm_path, bus.ppu().framebuffer())) {
                std::fprintf(stderr, "error: failed to write PPM file '%s'\n",
                             opts.dump_ppm_path.string().c_str());
                return 1;
            }
        }

        if (bus.has_battery() && !save_file.empty()) {
            bus.save_battery_ram(save_file);
        }

        if (!opts.save_state_path.empty()) {
            if (bus.save_state(cpu, opts.save_state_path)) {
                std::printf("[DragonRecomp] Saved state to %s\n",
                            opts.save_state_path.string().c_str());
            }
        }

        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "runtime error: %s\n", ex.what());
        return 1;
    }
}

}  // namespace dragon::nes
