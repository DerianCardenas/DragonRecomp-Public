#include "snes/frontend.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "dragon/save_state.h"
#include "snes/cartridge.h"
#include "snes/interpreter.h"

#if defined(DRAGON_HAVE_SDL3)
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

namespace dragon::snes {

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

    const std::string header = "P6\n256 224\n255\n";
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
        "usage: %s [options] [rom.sfc]\n"
        "options:\n"
        "  --recomp               Run statically recompiled C99 code (default when linked)\n"
        "  --interp               Run CPU interpreter mode\n"
        "  --verify               Run lock-step verification\n"
        "  --rom <path>           Override path to SNES ROM file\n"
        "  --save-path <path>     Path to battery-backed SRAM (.sav)\n"
        "  --state-slot <N>       Select initial save state slot (0-9, default: 1)\n"
        "  --load-state <path>    Load save state file on launch\n"
        "  --save-state <path>    Save state file path\n"
        "  --headless             Run without an SDL window\n"
        "  --headless-frames <N>  Run N frames without window, then exit\n"
        "  --headlessframes <N>   Alias for --headless-frames\n"
        "  --press-start <frame>  Press Start button at frame <frame> (for 8 frames)\n"
        "  --dump-ppm <file.ppm>  Write final 256x224 framebuffer to binary PPM\n"
        "  --scale <S>            Window scale factor (default: 3)\n"
        "  --fullscreen           Launch in fullscreen mode (toggle: F11 / Alt+Enter)\n"
        "  --no-aspect, --stretch Disable 4:3 aspect ratio correction (toggle: F8)\n"
        "  --crt, --scanlines     Enable CRT retro scanlines effect (toggle: F9)\n"
        "  -h, --help             Show this help message\n",
        prog);
}

struct FrameHookContext {
    const RunnerOptions* opts = nullptr;
    Bus* bus = nullptr;
    CpuState* cpu = nullptr;
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

void on_frame_hook(void* user_data, uint64_t frame_index) {
    auto* ctx = static_cast<FrameHookContext*>(user_data);
    if (!ctx || !ctx->opts || !ctx->bus || !ctx->cpu) {
        return;
    }

    uint16_t controller_buttons = 0U;
    if (ctx->opts->press_start_frame > 0ULL && frame_index >= ctx->opts->press_start_frame &&
        frame_index < ctx->opts->press_start_frame + 8ULL) {
        controller_buttons |= kButtonStart;
    }

#if defined(DRAGON_HAVE_SDL3)
    if (!ctx->opts->headless) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                ctx->cpu->halted = 1U;
            } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                    ctx->cpu->halted = 1U;
                } else if (ev.key.scancode == SDL_SCANCODE_F5) {
                    const auto target_path =
                        (ctx->opts->save_state_path.empty() ||
                         ctx->current_slot != ctx->opts->state_slot)
                            ? slot_state_path(ctx->opts->rom_path, ctx->current_slot)
                            : ctx->opts->save_state_path;
                    if (ctx->bus->save_state(*ctx->cpu, target_path)) {
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
                    if (ctx->bus->load_state(*ctx->cpu, target_path)) {
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
                    ctx->current_slot = static_cast<int>(ev.key.scancode - SDL_SCANCODE_1 + 1);
                    std::printf("[DragonRecomp] Selected state slot %d\n", ctx->current_slot);
                } else if (ev.key.scancode == SDL_SCANCODE_0) {
                    ctx->current_slot = 0;
                    std::printf("[DragonRecomp] Selected state slot %d\n", ctx->current_slot);
                }
            }
        }

        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (keys) {
            // D-Pad: Arrows or WASD
            if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W])
                controller_buttons |= kButtonUp;
            if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S])
                controller_buttons |= kButtonDown;
            if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A])
                controller_buttons |= kButtonLeft;
            if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D])
                controller_buttons |= kButtonRight;

            // Action buttons
            // kButtonB (Jump / primary action): X (matches NES A Jump), Space, K, C
            if (keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_K] ||
                keys[SDL_SCANCODE_C])
                controller_buttons |= kButtonB;
            // kButtonY (Run / fire / secondary): Z (matches NES B Run), J, U
            if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_J] || keys[SDL_SCANCODE_U])
                controller_buttons |= kButtonY;
            // kButtonA (Alt Jump / spin jump in SMW): C, L
            if (keys[SDL_SCANCODE_C] || keys[SDL_SCANCODE_L])
                controller_buttons |= kButtonA;
            // kButtonX (Alt Run / top action): V, I
            if (keys[SDL_SCANCODE_V] || keys[SDL_SCANCODE_I])
                controller_buttons |= kButtonX;
            if (keys[SDL_SCANCODE_Q])
                controller_buttons |= kButtonL;
            if (keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_R])
                controller_buttons |= kButtonR;
            if (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER])
                controller_buttons |= kButtonStart;
            if (keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_TAB] || keys[SDL_SCANCODE_BACKSPACE])
                controller_buttons |= kButtonSelect;
        }

        if (ctx->renderer && ctx->texture) {
            if (ctx->audio_stream) {
                std::array<int16_t, 1600> pcm_buf{};  // ~800 stereo pairs
                ctx->bus->apu().render_samples(pcm_buf);
                if (SDL_GetAudioStreamQueued(ctx->audio_stream) < 48000 * 4 / 4) {
                    SDL_PutAudioStreamData(ctx->audio_stream, pcm_buf.data(),
                                           static_cast<int>(pcm_buf.size() * sizeof(int16_t)));
                }
            }

            if (ctx->crt_filter) {
                if (ctx->crt_buffer.size() < 256 * 224) {
                    ctx->crt_buffer.resize(256 * 224);
                }
                const auto& src = ctx->bus->ppu().framebuffer();
                for (size_t y = 0; y < 224; ++y) {
                    const bool scanline = (y % 2 != 0);
                    for (size_t x = 0; x < 256; ++x) {
                        const size_t idx = y * 256 + x;
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
                                  static_cast<int>(256U * sizeof(uint32_t)));
            } else {
                SDL_UpdateTexture(ctx->texture, nullptr, ctx->bus->ppu().framebuffer().data(),
                                  static_cast<int>(256U * sizeof(uint32_t)));
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
        }
    }
#endif

    ctx->bus->set_controller_state(controller_buttons, 0U);
}

}  // namespace

bool parse_runner_args(int argc, char** argv, RunnerOptions& opts, int& exit_code) {
    exit_code = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            print_runner_usage(argv[0]);
            return false;
        }
        if (arg == "--recomp") {
            opts.mode = RunMode::Recompiled;
        } else if (arg == "--interp") {
            opts.mode = RunMode::Interpreter;
        } else if (arg == "--verify") {
            opts.mode = RunMode::Verify;
        } else if (arg == "--headless") {
            opts.headless = true;
        } else if (arg == "--headless-frames" || arg == "--headlessframes") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires an argument\n", argv[i]);
                exit_code = 1;
                return false;
            }
            opts.headless_frames = std::stoull(argv[++i]);
            opts.headless = true;
        } else if (arg == "--press-start") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --press-start requires an argument\n");
                exit_code = 1;
                return false;
            }
            opts.press_start_frame = std::stoull(argv[++i]);
        } else if (arg == "--dump-ppm") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --dump-ppm requires an argument\n");
                exit_code = 1;
                return false;
            }
            opts.dump_ppm_path = argv[++i];
        } else if (arg == "--rom") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --rom requires an argument\n");
                exit_code = 1;
                return false;
            }
            opts.rom_path = argv[++i];
        } else if (arg == "--save-path" || arg == "--save") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires an argument\n", argv[i]);
                exit_code = 1;
                return false;
            }
            opts.save_path = argv[++i];
        } else if (arg == "--state-slot" || arg == "--slot") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires an argument\n", argv[i]);
                exit_code = 1;
                return false;
            }
            opts.state_slot = std::max(0, std::stoi(argv[++i]));
        } else if (arg == "--load-state") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --load-state requires an argument\n");
                exit_code = 1;
                return false;
            }
            opts.load_state_path = argv[++i];
        } else if (arg == "--save-state") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --save-state requires an argument\n");
                exit_code = 1;
                return false;
            }
            opts.save_state_path = argv[++i];
        } else if (arg == "--no-aspect" || arg == "--stretch") {
            opts.aspect_ratio = false;
        } else if (arg == "--scale") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --scale requires an argument\n");
                exit_code = 1;
                return false;
            }
            opts.window_scale = std::stoi(argv[++i]);
        } else if (arg == "--fullscreen") {
            opts.fullscreen = true;
        } else if (arg == "--crt" || arg == "--scanlines") {
            opts.crt_filter = true;
        } else if (!arg.starts_with("-")) {
            opts.rom_path = argv[i];
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            exit_code = 1;
            return false;
        }
    }

    if (opts.rom_path.empty()) {
        std::fprintf(stderr, "error: no ROM path specified\n");
        exit_code = 1;
        return false;
    }
    return true;
}

int run_snes_app(const RunnerOptions& opts) {
    if (!std::filesystem::exists(opts.rom_path)) {
        std::fprintf(stderr, "error: ROM '%s' not found\n", opts.rom_path.string().c_str());
        return 77;
    }

    const Cartridge cart = load_snes_rom(opts.rom_path, opts.expected_sha1);
    Bus bus;
    bus.load_cartridge(cart);

    std::filesystem::path save_file = opts.save_path;
    if (save_file.empty() && !opts.rom_path.empty()) {
        save_file = opts.rom_path;
        save_file.replace_extension(".sav");
    }
    if (bus.has_battery() && !save_file.empty()) {
        bus.load_battery_ram(save_file);
    }

    if (opts.mode == RunMode::Recompiled) {
        bus.set_dispatch_table(opts.dispatch_table, opts.dispatch_count);
        bus.set_stack_trick_targets(opts.stack_trick_targets, opts.stack_trick_count);
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

#if defined(DRAGON_HAVE_SDL3)
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_AudioStream* audio_stream = nullptr;

    if (!opts.headless) {
        SDL_SetMainReady();
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
            std::fprintf(stderr, "error: SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }

        const int base_h = 224 * opts.window_scale;
        const int win_h = base_h;
        const int win_w = opts.aspect_ratio ? ((base_h * 4) / 3) : (256 * opts.window_scale);
        SDL_WindowFlags win_flags = SDL_WINDOW_RESIZABLE;
        if (opts.fullscreen) {
            win_flags |= SDL_WINDOW_FULLSCREEN;
        }
        window = SDL_CreateWindow(opts.window_title.c_str(), win_w, win_h, win_flags);
        if (!window) {
            std::fprintf(stderr, "error: SDL_CreateWindow failed: %s\n", SDL_GetError());
            SDL_Quit();
            return 1;
        }

        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer) {
            std::fprintf(stderr, "error: SDL_CreateRenderer failed: %s\n", SDL_GetError());
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING,
                                    256, 224);
        if (!texture) {
            std::fprintf(stderr, "error: SDL_CreateTexture failed: %s\n", SDL_GetError());
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        SDL_AudioSpec spec;
        spec.format = SDL_AUDIO_S16LE;
        spec.channels = 2;
        spec.freq = 48000;
        audio_stream =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audio_stream) {
            SDL_ResumeAudioStreamDevice(audio_stream);
        }
    }
#else
    if (!opts.headless) {
        std::fprintf(stderr, "error: built without SDL3 support; use --headless\n");
        return 1;
    }
#endif

    const uint64_t max_frames = (opts.headless && opts.headless_frames > 0ULL)
                                    ? opts.headless_frames
                                    : 0xFFFFFFFFFFFFFFFFULL;
    bus.set_max_frames(max_frames);

    FrameHookContext ctx;
    ctx.opts = &opts;
    ctx.bus = &bus;
    ctx.cpu = &cpu;
    ctx.current_slot = opts.state_slot;
    ctx.crt_filter = opts.crt_filter;
    ctx.fullscreen = opts.fullscreen;
    ctx.aspect_ratio = opts.aspect_ratio;
#if defined(DRAGON_HAVE_SDL3)
    ctx.window = window;
    ctx.renderer = renderer;
    ctx.texture = texture;
    ctx.audio_stream = audio_stream;
    ctx.next_frame_ns = 0ULL;
#endif

    bus.set_frame_callback(&on_frame_hook, &ctx);
    on_frame_hook(&ctx, 0ULL);

    if (opts.mode == RunMode::Recompiled && opts.dispatch_count > 0U) {
        const uint8_t mflags = cpu_mode_flags(&cpu);
        RecompFunc entry_fn = bus.lookup_function(cpu.pb, cpu.pc, mflags);
        if (!entry_fn) {
            std::fprintf(stderr,
                         "error: no compiled function for RESET vector $%02X:%04X (mode %02X)\n",
                         cpu.pb, cpu.pc, mflags);
            return 1;
        }
        entry_fn(&cpu);
        while (!cpu.halted) {
            runtime_dispatch(&cpu, cpu.pb, cpu.pc, snes_addr24(cpu.pb, cpu.pc));
        }
    } else {
        while (!cpu.halted) {
            if (cpu.waiting) {
                cpu.master_cycles = bus.next_scanline_cycle();
                runtime_poll(&cpu);
                continue;
            }
            cpu_step(cpu);
            runtime_poll(&cpu);
        }
    }

#if defined(DRAGON_HAVE_SDL3)
    if (!opts.headless) {
        if (audio_stream)
            SDL_DestroyAudioStream(audio_stream);
        if (texture)
            SDL_DestroyTexture(texture);
        if (renderer)
            SDL_DestroyRenderer(renderer);
        if (window)
            SDL_DestroyWindow(window);
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
}

}  // namespace dragon::snes
