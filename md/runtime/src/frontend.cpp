#include "md/frontend.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "dragon/save_state.h"
#include "md/interpreter.h"

#if defined(DRAGON_HAVE_SDL3)
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

namespace dragon::md {

FrameStats analyze_framebuffer(const Vdp& vdp) noexcept {
    FrameStats stats{};
    const auto& fb = vdp.framebuffer();
    const uint32_t w = vdp.active_width();
    const uint32_t h = vdp.active_height();
    stats.backdrop_color = fb[0];

    std::array<uint32_t, 128> seen{};
    size_t seen_count = 0;

    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t* row = &fb[y * Vdp::kMaxWidth];
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t px = row[x];
            if (px != stats.backdrop_color) {
                ++stats.non_backdrop_pixels;
            }
            bool found = false;
            for (size_t i = 0; i < seen_count; ++i) {
                if (seen[i] == px) {
                    found = true;
                    break;
                }
            }
            if (!found && seen_count < seen.size()) {
                seen[seen_count++] = px;
            }
        }
    }
    stats.distinct_colors = seen_count;
    return stats;
}

bool write_framebuffer_ppm(const std::filesystem::path& path, const Vdp& vdp) {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;

    const uint32_t w = vdp.active_width();
    const uint32_t h = vdp.active_height();
    const auto& fb = vdp.framebuffer();

    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "P6\n%u %u\n255\n", w, h);
    out.write(hdr, static_cast<std::streamsize>(std::strlen(hdr)));

    std::vector<uint8_t> row_buf(static_cast<size_t>(w) * 3U);
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t* src = &fb[y * Vdp::kMaxWidth];
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t rgba = src[x];
            row_buf[x * 3U + 0U] = static_cast<uint8_t>(rgba & 0xFFU);
            row_buf[x * 3U + 1U] = static_cast<uint8_t>((rgba >> 8U) & 0xFFU);
            row_buf[x * 3U + 2U] = static_cast<uint8_t>((rgba >> 16U) & 0xFFU);
        }
        out.write(reinterpret_cast<const char*>(row_buf.data()),
                  static_cast<std::streamsize>(row_buf.size()));
    }
    return out.good();
}

namespace {

void print_usage(const char* prog) {
    std::printf(
        "usage: %s [options] [rom.md|rom.bin|rom.gen|rom.smd]\n"
        "options:\n"
        "  --recomp               Run statically recompiled C99 code\n"
        "  --interp               Run M68000 interpreter mode (default)\n"
        "  --rom <path>           Override path to Genesis / Mega Drive ROM\n"
        "  --save-path <path>     Path to battery SRAM (.sav)\n"
        "  --state-slot <N>       Select initial save state slot (0-9, default: 1)\n"
        "  --load-state <path>    Load save state on launch\n"
        "  --save-state <path>    Save state file path on clean exit\n"
        "  --headlessframes <N>   Run N frames without SDL window\n"
        "  --press-start <frame>  Press Start at frame <frame>\n"
        "  --dump-ppm <file.ppm>  Write final framebuffer to binary PPM\n"
        "  --scale <S>            Window integer scale factor (default: 3)\n"
        "  --fullscreen           Launch in fullscreen (toggle: F11 / Alt+Enter)\n"
        "  --no-aspect, --stretch Disable 4:3 aspect ratio correction (toggle: F8)\n"
        "  --crt, --scanlines     Enable CRT scanlines (toggle: F9)\n"
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
    std::vector<uint32_t> upload_buf;
#if defined(DRAGON_HAVE_SDL3)
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_AudioStream* audio_stream = nullptr;
    uint32_t tex_w = 0;
    uint32_t tex_h = 0;
    uint64_t next_frame_ns = 0ULL;
#endif
};

void on_frame_hook(MdCpuState* cpu, Bus* bus, void* user_data) {
    auto* ctx = static_cast<FrameHookContext*>(user_data);
    if (!ctx || !ctx->opts)
        return;

    ++ctx->completed_frames;
    const uint64_t frame = ctx->completed_frames;

    uint16_t scripted = 0;
    if (ctx->opts->press_start_frame > 0ULL && frame >= ctx->opts->press_start_frame &&
        frame < ctx->opts->press_start_frame + 10ULL) {
        scripted |= kBtnStart;
    }

    if (ctx->opts->headless) {
        bus->set_controller_state(0, scripted);
        std::array<int16_t, 2048> discard{};
        while (bus->drain_audio_samples(discard.data(), discard.size() / 2U) > 0U) {
        }
        if (ctx->opts->headless_frames > 0ULL && frame >= ctx->opts->headless_frames) {
            ctx->should_quit = true;
            cpu->stopped = 2U;
        }
        return;
    }

#if defined(DRAGON_HAVE_SDL3)
    if (ctx->audio_stream) {
        std::array<int16_t, 4096> audio_buf{};
        const size_t got = bus->drain_audio_samples(audio_buf.data(), audio_buf.size() / 2U);
        if (got > 0U && SDL_GetAudioStreamQueued(ctx->audio_stream) <
                            static_cast<int>(Ym2612::kSampleRate * 2U * sizeof(int16_t) / 4U)) {
            SDL_PutAudioStreamData(ctx->audio_stream, audio_buf.data(),
                                   static_cast<int>(got * 2U * sizeof(int16_t)));
        }
    }

    if (ctx->window && ctx->renderer) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                ctx->should_quit = true;
                cpu->stopped = 2U;
                return;
            } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                    ctx->should_quit = true;
                    cpu->stopped = 2U;
                    return;
                } else if (ev.key.scancode == SDL_SCANCODE_F5) {
                    const auto target =
                        (ctx->opts->save_state_path.empty() ||
                         ctx->current_slot != ctx->opts->state_slot)
                            ? slot_state_path(ctx->opts->rom_path, ctx->current_slot)
                            : ctx->opts->save_state_path;
                    bus->save_state(*cpu, target);
                } else if (ev.key.scancode == SDL_SCANCODE_F7) {
                    const auto target =
                        (ctx->opts->load_state_path.empty() ||
                         ctx->current_slot != ctx->opts->state_slot)
                            ? slot_state_path(ctx->opts->rom_path, ctx->current_slot)
                            : ctx->opts->load_state_path;
                    bus->load_state(*cpu, target);
                } else if (ev.key.scancode == SDL_SCANCODE_F8) {
                    ctx->aspect_ratio = !ctx->aspect_ratio;
                } else if (ev.key.scancode == SDL_SCANCODE_F9) {
                    ctx->crt_filter = !ctx->crt_filter;
                } else if (ev.key.scancode == SDL_SCANCODE_F11 ||
                           (ev.key.scancode == SDL_SCANCODE_RETURN &&
                            (SDL_GetModState() & SDL_KMOD_ALT))) {
                    ctx->fullscreen = !ctx->fullscreen;
                    SDL_SetWindowFullscreen(ctx->window, ctx->fullscreen);
                } else if (ev.key.scancode >= SDL_SCANCODE_1 && ev.key.scancode <= SDL_SCANCODE_9) {
                    ctx->current_slot = static_cast<int>(ev.key.scancode - SDL_SCANCODE_1 + 1);
                } else if (ev.key.scancode == SDL_SCANCODE_0) {
                    ctx->current_slot = 0;
                }
            }
        }

        uint16_t buttons = scripted;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (keys) {
            if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W])
                buttons |= kBtnUp;
            if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S])
                buttons |= kBtnDown;
            if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A])
                buttons |= kBtnLeft;
            if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D])
                buttons |= kBtnRight;
            if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_J])
                buttons |= kBtnA;
            if (keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_K])
                buttons |= kBtnB;
            if (keys[SDL_SCANCODE_C] || keys[SDL_SCANCODE_L])
                buttons |= kBtnC;
            if (keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_U])
                buttons |= kBtnX;
            if (keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_I])
                buttons |= kBtnY;
            if (keys[SDL_SCANCODE_R] || keys[SDL_SCANCODE_O])
                buttons |= kBtnZ;
            if (keys[SDL_SCANCODE_RETURN])
                buttons |= kBtnStart;
            if (keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_BACKSPACE])
                buttons |= kBtnMode;
        }
        bus->set_controller_state(0, buttons);

        const uint32_t w = bus->vdp().active_width();
        const uint32_t h = bus->vdp().active_height();
        if (!ctx->texture || ctx->tex_w != w || ctx->tex_h != h) {
            if (ctx->texture) {
                SDL_DestroyTexture(ctx->texture);
            }
            ctx->texture = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_ABGR8888,
                                             SDL_TEXTUREACCESS_STREAMING, static_cast<int>(w),
                                             static_cast<int>(h));
            if (ctx->texture) {
                SDL_SetTextureScaleMode(ctx->texture, SDL_SCALEMODE_NEAREST);
            }
            ctx->tex_w = w;
            ctx->tex_h = h;
        }

        if (ctx->texture) {
            ctx->upload_buf.resize(static_cast<size_t>(w) * h);
            const auto& fb = bus->vdp().framebuffer();
            for (uint32_t y = 0; y < h; ++y) {
                const uint32_t* src = &fb[y * Vdp::kMaxWidth];
                uint32_t* dst = &ctx->upload_buf[y * w];
                const bool dim = ctx->crt_filter && ((y & 1U) != 0U);
                for (uint32_t x = 0; x < w; ++x) {
                    uint32_t px = src[x];
                    if (dim) {
                        const uint32_t r = ((px & 0xFFU) * 55U) / 100U;
                        const uint32_t g = (((px >> 8U) & 0xFFU) * 55U) / 100U;
                        const uint32_t b = (((px >> 16U) & 0xFFU) * 55U) / 100U;
                        px = 0xFF000000U | (b << 16U) | (g << 8U) | r;
                    }
                    dst[x] = px;
                }
            }
            SDL_UpdateTexture(ctx->texture, nullptr, ctx->upload_buf.data(),
                              static_cast<int>(w * sizeof(uint32_t)));

            int out_w = 0, out_h = 0;
            SDL_GetRenderOutputSize(ctx->renderer, &out_w, &out_h);
            SDL_FRect dst_rect{0.0f, 0.0f, static_cast<float>(out_w), static_cast<float>(out_h)};
            if (ctx->aspect_ratio && out_h > 0) {
                constexpr float kTargetAspect = 4.0f / 3.0f;
                const float win_aspect = static_cast<float>(out_w) / static_cast<float>(out_h);
                if (win_aspect > kTargetAspect) {
                    dst_rect.w = static_cast<float>(out_h) * kTargetAspect;
                    dst_rect.x = (static_cast<float>(out_w) - dst_rect.w) * 0.5f;
                } else {
                    dst_rect.h = static_cast<float>(out_w) / kTargetAspect;
                    dst_rect.y = (static_cast<float>(out_h) - dst_rect.h) * 0.5f;
                }
            }

            SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);
            SDL_RenderClear(ctx->renderer);
            SDL_RenderTexture(ctx->renderer, ctx->texture, nullptr, &dst_rect);
            SDL_RenderPresent(ctx->renderer);
        }

        // Frame pacing: 59.9227 Hz NTSC (16,688,155 ns) / 50.0 Hz PAL (20,000,000 ns)
        const uint64_t frame_ns = bus->vdp().is_pal() ? 20'000'000ULL : 16'688'155ULL;
        const uint64_t now_ns = SDL_GetTicksNS();
        if (ctx->next_frame_ns == 0ULL) {
            ctx->next_frame_ns = now_ns + frame_ns;
        } else if (now_ns < ctx->next_frame_ns) {
            SDL_DelayNS(ctx->next_frame_ns - now_ns);
            ctx->next_frame_ns += frame_ns;
        } else {
            ctx->next_frame_ns = now_ns + frame_ns;
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
            print_usage(argv[0]);
            return false;
        }
        if (arg == "--recomp" || arg == "--recompiled") {
            opts.mode = RunMode::Recompiled;
        } else if (arg == "--interp" || arg == "--interpreter") {
            opts.mode = RunMode::Interpreter;
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
        } else if ((arg == "--headlessframes" || arg == "--headless-frames") && i + 1 < argc) {
            opts.headless = true;
            opts.headless_frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--press-start" && i + 1 < argc) {
            opts.press_start_frame = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--dump-ppm" && i + 1 < argc) {
            opts.dump_ppm_path = argv[++i];
        } else if (arg == "--scale" && i + 1 < argc) {
            opts.window_scale = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--fullscreen") {
            opts.fullscreen = true;
        } else if (arg == "--no-aspect" || arg == "--stretch") {
            opts.aspect_ratio = false;
        } else if (arg == "--crt" || arg == "--scanlines") {
            opts.crt_filter = true;
        } else if (!arg.empty() && arg[0] != '-') {
            opts.rom_path = argv[i];
        }
    }
    return true;
}

int run_md_app(const RunnerOptions& opts) {
    if (opts.rom_path.empty()) {
        std::fprintf(stderr, "error: no Sega Genesis / Mega Drive ROM specified\n");
        return 1;
    }

    Bus bus;
    std::string err;
    if (!bus.load_rom(opts.rom_path, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    std::filesystem::path save_file = opts.save_path;
    if (save_file.empty()) {
        save_file = opts.rom_path;
        save_file.replace_extension(".sav");
    }
    if (bus.cartridge().header().has_sram) {
        bus.load_sram(save_file);
    }

    if (opts.recomp_table && opts.recomp_table_count > 0U) {
        bus.register_recomp_functions(opts.recomp_table, opts.recomp_table_count);
    }

    MdCpuState cpu{};
    bus.reset(cpu);

    if (!opts.load_state_path.empty()) {
        bus.load_state(cpu, opts.load_state_path);
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
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
            std::fprintf(stderr, "error: SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        const std::string title =
            bus.cartridge().header().overseas_title.empty()
                ? "DragonRecomp Genesis"
                : ("DragonRecomp — " + bus.cartridge().header().overseas_title);
        const int win_w = 320 * opts.window_scale;
        const int win_h = 240 * opts.window_scale;
        ctx.window = SDL_CreateWindow(title.c_str(), win_w, win_h, SDL_WINDOW_RESIZABLE);
        if (ctx.window) {
            if (opts.fullscreen) {
                SDL_SetWindowFullscreen(ctx.window, true);
            }
            ctx.renderer = SDL_CreateRenderer(ctx.window, nullptr);
            if (ctx.renderer) {
                SDL_SetRenderVSync(ctx.renderer, 0);
            }
        }
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_S16;
        spec.channels = 2;
        spec.freq = static_cast<int>(Ym2612::kSampleRate);
        ctx.audio_stream =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (ctx.audio_stream) {
            SDL_ResumeAudioStreamDevice(ctx.audio_stream);
        }
    }
#endif

    if (opts.mode == RunMode::Recompiled && opts.recomp_table_count > 0U) {
        md_runtime_dispatch(&cpu, cpu.pc, 0U);
    } else {
        while (!ctx.should_quit && cpu.stopped != 2U) {
            run_frame_interpreter(cpu, bus);
        }
    }

    if (!opts.dump_ppm_path.empty()) {
        write_framebuffer_ppm(opts.dump_ppm_path, bus.vdp());
    }

    if (!opts.save_state_path.empty()) {
        bus.save_state(cpu, opts.save_state_path);
    }
    if (bus.cartridge().header().sram_battery) {
        bus.save_sram(save_file);
    }

#if defined(DRAGON_HAVE_SDL3)
    if (!opts.headless) {
        if (ctx.audio_stream)
            SDL_DestroyAudioStream(ctx.audio_stream);
        if (ctx.texture)
            SDL_DestroyTexture(ctx.texture);
        if (ctx.renderer)
            SDL_DestroyRenderer(ctx.renderer);
        if (ctx.window)
            SDL_DestroyWindow(ctx.window);
        SDL_Quit();
    }
#endif

    return 0;
}

}  // namespace dragon::md
