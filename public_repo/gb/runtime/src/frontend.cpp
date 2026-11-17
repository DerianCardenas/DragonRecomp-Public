#include "gb/frontend.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <vector>

#include "dragon/save_state.h"
#include "gb/cartridge.h"
#include "gb/interpreter.h"

#if defined(DRAGON_HAVE_SDL3)
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

namespace dragon::gb {

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

    const std::string header = "P6\n160 144\n255\n";
    out.write(header.data(), static_cast<std::streamsize>(header.size()));

    std::array<uint8_t, Ppu::kScreenWidth * 3> row_buf{};
    for (uint32_t y = 0; y < Ppu::kScreenHeight; ++y) {
        for (uint32_t x = 0; x < Ppu::kScreenWidth; ++x) {
            const uint32_t rgba = fb[static_cast<size_t>(y) * Ppu::kScreenWidth + x];
            // RGBA format: r in low byte, g in byte 1, b in byte 2
            row_buf[static_cast<size_t>(x) * 3U + 0U] = static_cast<uint8_t>(rgba & 0xFFU);
            row_buf[static_cast<size_t>(x) * 3U + 1U] = static_cast<uint8_t>((rgba >> 8U) & 0xFFU);
            row_buf[static_cast<size_t>(x) * 3U + 2U] = static_cast<uint8_t>((rgba >> 16U) & 0xFFU);
        }
        out.write(reinterpret_cast<const char*>(row_buf.data()),
                  static_cast<std::streamsize>(row_buf.size()));
    }

    return out.good();
}

namespace {

void print_runner_usage(const char* prog) {
    std::printf(
        "usage: %s [options] [rom.gb|rom.gbc]\n"
        "options:\n"
        "  --recomp               Run statically recompiled C99 code\n"
        "  --interp               Run CPU interpreter mode (default)\n"
        "  --rom <path>           Override path to Game Boy ROM file\n"
        "  --save-path <path>     Path to battery-backed SRAM (.sav)\n"
        "  --state-slot <N>       Select initial save state slot (0-9, default: 1)\n"
        "  --load-state <path>    Load save state file on launch\n"
        "  --save-state <path>    Save state file path\n"
        "  --headlessframes <N>   Run N frames without SDL window, then exit\n"
        "  --headless-frames <N>  Alias for --headlessframes <N>\n"
        "  --press-start <frame>  Press Start button at frame <frame> (for 8 frames)\n"
        "  --dump-ppm <file.ppm>  Write final 160x144 framebuffer to a binary PPM file\n"
        "  --scale <S>            Window integer scale factor (default: 4 -> 640x576)\n"
        "  --fullscreen           Launch in fullscreen mode (toggle: F11 / Alt+Enter)\n"
        "  --no-aspect, --stretch Disable 10:9 aspect ratio correction (toggle: F8)\n"
        "  --crt, --scanlines     Enable CRT retro scanlines effect (toggle: F9)\n"
        "  --classic              Use classic pea-green DMG palette\n"
        "  -h, --help             Show this help message\n",
        prog);
}

struct FrameHookContext {
    const RunnerOptions* opts = nullptr;
    uint64_t completed_frames = 0;
    bool should_quit = false;
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
    if (!ctx || !ctx->opts)
        return;

    ctx->completed_frames++;
    const uint64_t frame = ctx->completed_frames;

    uint8_t scripted_buttons = 0U;
    if (ctx->opts->press_start_frame > 0ULL && frame >= ctx->opts->press_start_frame &&
        frame < ctx->opts->press_start_frame + 8ULL) {
        scripted_buttons |= kButtonStart;
    }

    if (ctx->opts->headless) {
        bus->set_joypad(scripted_buttons);
        bus->apu().clear_samples();
        if (ctx->opts->headless_frames > 0ULL && frame >= ctx->opts->headless_frames) {
            ctx->should_quit = true;
            cpu->halted = 1;
        }
        return;
    }

#if defined(DRAGON_HAVE_SDL3)
    bus->sync_apu(cpu->cycles);
    if (ctx->audio_stream) {
        auto samples = bus->apu().samples();
        if (!samples.empty() &&
            SDL_GetAudioStreamQueued(ctx->audio_stream) <
                static_cast<int>(Apu::kSampleRate * 2U * sizeof(int16_t) / 4U)) {
            SDL_PutAudioStreamData(ctx->audio_stream, samples.data(),
                                   static_cast<int>(samples.size() * sizeof(int16_t)));
        }
    }
    bus->apu().clear_samples();

    if (ctx->window && ctx->renderer && ctx->texture) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                ctx->should_quit = true;
                cpu->halted = 1;
                return;
            } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                    ctx->should_quit = true;
                    cpu->halted = 1;
                    return;
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
                                ctx->aspect_ratio ? "ON (10:9)" : "OFF (stretched)");
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
                    ctx->current_slot = static_cast<int>(ev.key.scancode - SDL_SCANCODE_1 + 1);
                    std::printf("[DragonRecomp] Selected state slot %d\n", ctx->current_slot);
                } else if (ev.key.scancode == SDL_SCANCODE_0) {
                    ctx->current_slot = 0;
                    std::printf("[DragonRecomp] Selected state slot %d\n", ctx->current_slot);
                }
            }
        }

        uint8_t current_buttons = scripted_buttons;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (keys) {
            if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_J])
                current_buttons |= kButtonA;
            if (keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_K])
                current_buttons |= kButtonB;
            if (keys[SDL_SCANCODE_BACKSPACE] || keys[SDL_SCANCODE_RSHIFT])
                current_buttons |= kButtonSelect;
            if (keys[SDL_SCANCODE_RETURN])
                current_buttons |= kButtonStart;
            if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D])
                current_buttons |= kButtonRight;
            if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A])
                current_buttons |= kButtonLeft;
            if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W])
                current_buttons |= kButtonUp;
            if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S])
                current_buttons |= kButtonDown;
        }

        bus->set_joypad(current_buttons);

        // Upload framebuffer to texture
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
            constexpr float kTargetAspect = 160.0f / 144.0f;
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

        // Frame timing: ~59.7275 Hz (16,742,706 ns per frame)
        constexpr uint64_t kFrameDurationNs = 16'742'706ULL;
        uint64_t now_ns = SDL_GetTicksNS();
        if (ctx->next_frame_ns == 0ULL) {
            ctx->next_frame_ns = now_ns + kFrameDurationNs;
        } else if (now_ns < ctx->next_frame_ns) {
            SDL_DelayNS(ctx->next_frame_ns - now_ns);
            ctx->next_frame_ns += kFrameDurationNs;
        } else {
            ctx->next_frame_ns = now_ns + kFrameDurationNs;
        }
    }
#endif
}

}  // namespace

bool parse_runner_args(int argc, char** argv, RunnerOptions& opts, int& exit_code) {
    exit_code = 0;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            print_runner_usage(argv[0]);
            return false;
        }
        if (arg == "--recomp") {
            opts.mode = RunMode::Recompiled;
        } else if (arg == "--interp") {
            opts.mode = RunMode::Interpreter;
        } else if (arg == "--rom") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --rom requires a path\n");
                exit_code = 2;
                return false;
            }
            opts.rom_path = argv[++i];
        } else if (arg == "--save-path" || arg == "--save") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a path\n", argv[i]);
                exit_code = 2;
                return false;
            }
            opts.save_path = argv[++i];
        } else if (arg == "--state-slot" || arg == "--slot") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a slot number\n", argv[i]);
                exit_code = 2;
                return false;
            }
            opts.state_slot = std::max(0, std::atoi(argv[++i]));
        } else if (arg == "--load-state") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --load-state requires a path\n");
                exit_code = 2;
                return false;
            }
            opts.load_state_path = argv[++i];
        } else if (arg == "--save-state") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --save-state requires a path\n");
                exit_code = 2;
                return false;
            }
            opts.save_state_path = argv[++i];
        } else if (arg == "--no-aspect" || arg == "--stretch") {
            opts.aspect_ratio = false;
        } else if (arg == "--crt" || arg == "--scanlines") {
            opts.crt_filter = true;
        } else if (arg == "--headlessframes" || arg == "--headless-frames") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a frame count\n", argv[i]);
                exit_code = 2;
                return false;
            }
            opts.headless = true;
            opts.headless_frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--press-start") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --press-start requires a frame number\n");
                exit_code = 2;
                return false;
            }
            opts.press_start_frame = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--dump-ppm") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --dump-ppm requires a file path\n");
                exit_code = 2;
                return false;
            }
            opts.dump_ppm_path = argv[++i];
        } else if (arg == "--scale") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --scale requires an integer\n");
                exit_code = 2;
                return false;
            }
            opts.window_scale = std::atoi(argv[++i]);
            if (opts.window_scale < 1)
                opts.window_scale = 1;
        } else if (arg == "--fullscreen") {
            opts.fullscreen = true;
        } else if (arg == "--classic") {
            opts.classic_palette = true;
        } else if (!arg.empty() && arg[0] != '-') {
            opts.rom_path = argv[i];
        } else {
            std::fprintf(stderr, "warning: unrecognized option %s\n", argv[i]);
        }
    }
    return true;
}

int run_gb_app(const RunnerOptions& opts) {
    if (opts.rom_path.empty()) {
        std::fprintf(stderr, "error: no ROM file specified\n");
        return 1;
    }

    Cartridge cart;
    try {
        cart = load_gb_rom(opts.rom_path, opts.expected_sha1);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error loading ROM '%s': %s\n", opts.rom_path.string().c_str(),
                     e.what());
        return 1;
    }

    Bus bus;
    bus.load_cartridge(cart);
    bus.ppu().set_dmg_palette_classic(opts.classic_palette);

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

    if (!opts.load_state_path.empty()) {
        if (bus.load_state(cpu, opts.load_state_path)) {
            std::printf("[DragonRecomp] Loaded initial state from %s\n",
                        opts.load_state_path.string().c_str());
        } else {
            std::fprintf(stderr, "[DragonRecomp] Failed to load initial state from %s\n",
                         opts.load_state_path.string().c_str());
        }
    }

    FrameHookContext ctx;
    ctx.opts = &opts;
    ctx.current_slot = opts.state_slot;
    ctx.crt_filter = opts.crt_filter;
    ctx.fullscreen = opts.fullscreen;
    ctx.aspect_ratio = opts.aspect_ratio;
    bus.set_frame_callback(on_frame_hook, &ctx);

#if defined(DRAGON_HAVE_SDL3)
    if (!opts.headless) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }

        std::string win_title = opts.window_title;
        if (!cart.title.empty()) {
            win_title += " - " + cart.title;
        }

        int win_w = static_cast<int>(Ppu::kScreenWidth) * opts.window_scale;
        int win_h = static_cast<int>(Ppu::kScreenHeight) * opts.window_scale;
        Uint32 win_flags = SDL_WINDOW_RESIZABLE;
        if (opts.fullscreen)
            win_flags |= SDL_WINDOW_FULLSCREEN;

        ctx.window = SDL_CreateWindow(win_title.c_str(), win_w, win_h, win_flags);
        if (!ctx.window) {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            SDL_Quit();
            return 1;
        }

        ctx.renderer = SDL_CreateRenderer(ctx.window, nullptr);
        if (!ctx.renderer) {
            std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
            SDL_DestroyWindow(ctx.window);
            SDL_Quit();
            return 1;
        }

        ctx.texture =
            SDL_CreateTexture(ctx.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                              Ppu::kScreenWidth, Ppu::kScreenHeight);
        if (!ctx.texture) {
            std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
            SDL_DestroyRenderer(ctx.renderer);
            SDL_DestroyWindow(ctx.window);
            SDL_Quit();
            return 1;
        }
        SDL_SetTextureScaleMode(ctx.texture, SDL_SCALEMODE_NEAREST);

        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_S16;
        spec.channels = 2;
        spec.freq = static_cast<int>(Apu::kSampleRate);
        ctx.audio_stream =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (ctx.audio_stream) {
            SDL_ResumeAudioStreamDevice(ctx.audio_stream);
        } else {
            std::fprintf(stderr, "warning: SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        }
    }
#else
    if (!opts.headless) {
        std::printf("info: compiled without SDL3; running headless.\n");
    }
#endif

    // Main execution loop
    if (opts.mode == RunMode::Recompiled && (opts.dispatch_count > 0U || opts.reset_func)) {
        if (opts.dispatch_count > 0U) {
            bus.set_dispatch_table(opts.dispatch_table, opts.dispatch_count);
        }
        while (!ctx.should_quit) {
            if (cpu.halted) {
                cpu_step(cpu);
            } else {
                RecompFunc fn = bus.lookup_function(cpu.pc, bus.current_rom_bank());
                if (fn) {
                    fn(&cpu);
                } else if (opts.reset_func && cpu.pc == 0x0100U) {
                    opts.reset_func(&cpu);
                } else {
                    gb_runtime_dispatch(&cpu, cpu.pc, cpu.pc);
                }
            }
            if (bus.ppu().consume_frame_complete()) {
                bus.on_frame_complete(&cpu);
            }
        }
    } else {
        while (!ctx.should_quit) {
            cpu_step(cpu);
            if (bus.ppu().consume_frame_complete()) {
                bus.on_frame_complete(&cpu);
            }
        }
    }

    if (!opts.dump_ppm_path.empty()) {
        write_framebuffer_ppm(opts.dump_ppm_path, bus.ppu().framebuffer());
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

#if defined(DRAGON_HAVE_SDL3)
    if (ctx.audio_stream)
        SDL_DestroyAudioStream(ctx.audio_stream);
    if (ctx.texture)
        SDL_DestroyTexture(ctx.texture);
    if (ctx.renderer)
        SDL_DestroyRenderer(ctx.renderer);
    if (ctx.window)
        SDL_DestroyWindow(ctx.window);
    if (!opts.headless)
        SDL_Quit();
#endif

    return 0;
}

}  // namespace dragon::gb
