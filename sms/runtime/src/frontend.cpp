#include "sms/frontend.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

#include "sms/interpreter.h"

#if defined(DRAGON_HAVE_SDL3) && DRAGON_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

namespace dragon::sms {

namespace {

std::filesystem::path default_save_path(const std::filesystem::path& rom_path) {
    auto p = rom_path;
    p.replace_extension(".sav");
    return p;
}

std::filesystem::path slot_state_path(const std::filesystem::path& rom_path, int slot) {
    auto p = rom_path;
    p.replace_extension(".state" + std::to_string(slot));
    return p;
}

struct HeadlessFrameCtx {
    const RunnerOptions* opts = nullptr;
    uint64_t target_frames = 60;
    uint64_t completed_frames = 0;
};

void headless_apply_input(SmsCpuState& cpu, Bus& bus, const RunnerOptions& opts,
                          uint64_t frame_idx) noexcept {
    InputState in{};
    if (opts.press_start_frame > 0U && frame_idx >= opts.press_start_frame &&
        frame_idx < opts.press_start_frame + 10U) {
        in.button1 = true;
        in.pause_or_start = (bus.cartridge().system_type() == SystemType::GameGear);
    }
    bus.set_input(in, cpu);
}

void headless_on_frame(SmsCpuState* cpu, Bus* bus, void* user_data) {
    auto* ctx = static_cast<HeadlessFrameCtx*>(user_data);
    ++ctx->completed_frames;
    if (ctx->completed_frames >= ctx->target_frames) {
        bus->set_frame_stop(true);
        return;
    }
    headless_apply_input(*cpu, *bus, *ctx->opts, ctx->completed_frames);
}

#if defined(DRAGON_HAVE_SDL3) && DRAGON_HAVE_SDL3
struct SdlFrameCtx {
    const RunnerOptions* opts = nullptr;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* tex = nullptr;
    SDL_AudioStream* audio = nullptr;
    bool fullscreen = false;
    bool aspect_ratio = true;
    bool crt_filter = false;
    int slot = 0;
    std::array<int16_t, 2048 * 2> pcm_buf{};
    std::chrono::steady_clock::time_point next_frame{};
};

void sdl_poll_and_present(SmsCpuState* cpu, Bus* bus, void* user_data) {
    auto* ctx = static_cast<SdlFrameCtx*>(user_data);
    constexpr auto kFrameDuration = std::chrono::microseconds(16688);  // ~59.92 Hz NTSC

    // Push audio
    if (ctx->audio) {
        const size_t n = bus->psg().drain_samples(ctx->pcm_buf.data(), 2048U);
        if (n > 0U && SDL_GetAudioStreamQueued(ctx->audio) < 48000 * 4 / 5) {
            SDL_PutAudioStreamData(ctx->audio, ctx->pcm_buf.data(),
                                   static_cast<int>(n * 2U * sizeof(int16_t)));
        }
    }

    // Present video
    if (ctx->tex) {
        SDL_UpdateTexture(ctx->tex, nullptr, bus->vdp().framebuffer().data(),
                          Vdp::kMaxWidth * static_cast<int>(sizeof(uint32_t)));
        SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);
        SDL_RenderClear(ctx->renderer);

        int rw = 0, rh = 0;
        SDL_GetRenderOutputSize(ctx->renderer, &rw, &rh);
        const SDL_FRect src{
            static_cast<float>(bus->vdp().viewport_x()),
            static_cast<float>(bus->vdp().viewport_y()),
            static_cast<float>(bus->vdp().visible_width()),
            static_cast<float>(bus->vdp().visible_height()),
        };
        SDL_FRect dst{0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh)};
        if (ctx->aspect_ratio && rw > 0 && rh > 0) {
            const float target_ar = (bus->cartridge().system_type() == SystemType::GameGear)
                                        ? (10.0f / 9.0f)
                                        : (4.0f / 3.0f);
            const float cur_ar = static_cast<float>(rw) / static_cast<float>(rh);
            if (cur_ar > target_ar) {
                dst.w = static_cast<float>(rh) * target_ar;
                dst.x = (static_cast<float>(rw) - dst.w) * 0.5f;
            } else {
                dst.h = static_cast<float>(rw) / target_ar;
                dst.y = (static_cast<float>(rh) - dst.h) * 0.5f;
            }
        }
        SDL_RenderTexture(ctx->renderer, ctx->tex, &src, &dst);

        if (ctx->crt_filter) {
            SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 80);
            for (float y = dst.y; y < dst.y + dst.h; y += 3.0f) {
                SDL_RenderLine(ctx->renderer, dst.x, y, dst.x + dst.w, y);
            }
        }
        SDL_RenderPresent(ctx->renderer);
    }

    // Frame pacing
    ctx->next_frame += kFrameDuration;
    const auto now = std::chrono::steady_clock::now();
    if (ctx->next_frame > now) {
        std::this_thread::sleep_until(ctx->next_frame);
    } else if (now - ctx->next_frame > std::chrono::milliseconds(100)) {
        ctx->next_frame = now;
    }

    // Events & input for the next frame
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) {
            bus->set_frame_stop(true);
        } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
            const SDL_Keycode k = ev.key.key;
            if (k == SDLK_ESCAPE) {
                bus->set_frame_stop(true);
            } else if (k == SDLK_F11) {
                ctx->fullscreen = !ctx->fullscreen;
                SDL_SetWindowFullscreen(ctx->window, ctx->fullscreen);
            } else if (k == SDLK_F8) {
                ctx->aspect_ratio = !ctx->aspect_ratio;
            } else if (k == SDLK_F9) {
                ctx->crt_filter = !ctx->crt_filter;
            } else if (k == SDLK_F5) {
                bus->save_state(slot_state_path(ctx->opts->rom_path, ctx->slot), *cpu);
            } else if (k == SDLK_F7) {
                bus->load_state(slot_state_path(ctx->opts->rom_path, ctx->slot), *cpu);
            } else if (k >= SDLK_0 && k <= SDLK_9) {
                ctx->slot = static_cast<int>(k - SDLK_0);
            }
        }
    }

    const bool* keys = SDL_GetKeyboardState(nullptr);
    InputState in{};
    in.up = keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W];
    in.down = keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S];
    in.left = keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A];
    in.right = keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D];
    in.button1 = keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_J];
    in.button2 = keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_K];
    in.pause_or_start = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_SPACE];
    bus->set_input(in, *cpu);
}
#endif

}  // namespace

FrameStats analyze_framebuffer(const Vdp& vdp) noexcept {
    FrameStats stats{};
    const auto& fb = vdp.framebuffer();
    const int vx = vdp.viewport_x();
    const int vy = vdp.viewport_y();
    const int vw = vdp.visible_width();
    const int vh = vdp.visible_height();

    stats.backdrop_color = fb[static_cast<size_t>(vy) * Vdp::kMaxWidth + static_cast<size_t>(vx)];
    std::array<uint32_t, 64> seen{};
    size_t seen_count = 0;

    for (int y = 0; y < vh; ++y) {
        for (int x = 0; x < vw; ++x) {
            const uint32_t px =
                fb[static_cast<size_t>(vy + y) * Vdp::kMaxWidth + static_cast<size_t>(vx + x)];
            if (px != stats.backdrop_color) {
                ++stats.non_backdrop_pixels;
            }
            bool found = false;
            for (size_t k = 0; k < seen_count; ++k) {
                if (seen[k] == px) {
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
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    const auto& fb = vdp.framebuffer();
    const int vx = vdp.viewport_x();
    const int vy = vdp.viewport_y();
    const int vw = vdp.visible_width();
    const int vh = vdp.visible_height();

    out << "P6\n" << vw << " " << vh << "\n255\n";
    for (int y = 0; y < vh; ++y) {
        for (int x = 0; x < vw; ++x) {
            const uint32_t px =
                fb[static_cast<size_t>(vy + y) * Vdp::kMaxWidth + static_cast<size_t>(vx + x)];
            const char rgb[3] = {
                static_cast<char>(px & 0xFFU),
                static_cast<char>((px >> 8U) & 0xFFU),
                static_cast<char>((px >> 16U) & 0xFFU),
            };
            out.write(rgb, 3);
        }
    }
    return out.good();
}

bool parse_runner_args(int argc, char** argv, RunnerOptions& opts, int& exit_code) {
    exit_code = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_val = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: missing argument for %s\n", flag);
                exit_code = 2;
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            std::printf(
                "Usage: sms_runner [--rom <file.sms|.gg>] [--headless --frames N] "
                "[--dump-ppm <out.ppm>] [--fullscreen] [--crt] [--save-state <path>] "
                "[--load-state <path>] [--interpreter|--recompiled]\n");
            return false;
        } else if (arg == "--rom") {
            const char* v = need_val("--rom");
            if (!v) return false;
            opts.rom_path = v;
        } else if (arg == "--save") {
            const char* v = need_val("--save");
            if (!v) return false;
            opts.save_path = v;
        } else if (arg == "--save-state") {
            const char* v = need_val("--save-state");
            if (!v) return false;
            opts.save_state_path = v;
        } else if (arg == "--load-state") {
            const char* v = need_val("--load-state");
            if (!v) return false;
            opts.load_state_path = v;
        } else if (arg == "--headless") {
            opts.headless = true;
        } else if (arg == "--frames") {
            const char* v = need_val("--frames");
            if (!v) return false;
            opts.headless_frames = std::strtoull(v, nullptr, 10);
        } else if (arg == "--headlessframes" || arg == "--headless-frames") {
            const char* v = need_val(arg.c_str());
            if (!v) return false;
            opts.headless = true;
            opts.headless_frames = std::strtoull(v, nullptr, 10);
        } else if (arg == "--press-start-frame" || arg == "--press-start") {
            const char* v = need_val(arg.c_str());
            if (!v) return false;
            opts.press_start_frame = std::strtoull(v, nullptr, 10);
        } else if (arg == "--dump-ppm") {
            const char* v = need_val("--dump-ppm");
            if (!v) return false;
            opts.dump_ppm_path = v;
        } else if (arg == "--scale") {
            const char* v = need_val("--scale");
            if (!v) return false;
            opts.window_scale = std::clamp(std::atoi(v), 1, 8);
        } else if (arg == "--fullscreen") {
            opts.fullscreen = true;
        } else if (arg == "--crt" || arg == "--scanlines") {
            opts.crt_filter = true;
        } else if (arg == "--no-aspect" || arg == "--stretch") {
            opts.aspect_ratio = false;
        } else if (arg == "--interpreter" || arg == "--interp") {
            opts.mode = RunMode::Interpreter;
        } else if (arg == "--recompiled" || arg == "--recomp") {
            opts.mode = RunMode::Recompiled;
        } else if (!arg.empty() && arg[0] != '-' && opts.rom_path.empty()) {
            opts.rom_path = arg;
        }
    }
    return true;
}

int run_sms_app(const RunnerOptions& opts) {
    if (opts.rom_path.empty()) {
        std::fprintf(stderr, "error: no ROM specified (pass --rom <file.sms|.gg>)\n");
        return 1;
    }

    Bus bus;
    std::string err;
    if (!bus.cartridge().load_from_file(opts.rom_path, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    const std::filesystem::path save_path =
        opts.save_path.empty() ? default_save_path(opts.rom_path) : opts.save_path;
    bus.cartridge().load_sram(save_path);

    SmsCpuState cpu{};
    bus.reset(cpu);
    bus.set_dispatch_table(opts.dispatch_table, opts.dispatch_count);

    if (!opts.load_state_path.empty() && std::filesystem::exists(opts.load_state_path)) {
        bus.load_state(opts.load_state_path, cpu);
    } else if (!opts.save_state_path.empty() && std::filesystem::exists(opts.save_state_path)) {
        bus.load_state(opts.save_state_path, cpu);
    }

    const bool use_recomp = (opts.mode == RunMode::Recompiled && opts.dispatch_table != nullptr &&
                             opts.dispatch_count > 0U);

    // Headless execution mode (CI & automated ROM verification)
    if (opts.headless) {
        const uint64_t frames = (opts.headless_frames > 0U) ? opts.headless_frames : 60U;
        if (use_recomp) {
            HeadlessFrameCtx hctx{};
            hctx.opts = &opts;
            hctx.target_frames = frames;
            headless_apply_input(cpu, bus, opts, 0U);
            bus.set_frame_callback(headless_on_frame, &hctx);
            while (!bus.stop_requested()) {
                if (sms_runtime_dispatch(&cpu, cpu.pc, cpu.pc) != 0) {
                    break;
                }
            }
            bus.set_frame_callback(nullptr, nullptr);
        } else {
            for (uint64_t f = 0; f < frames; ++f) {
                headless_apply_input(cpu, bus, opts, f);
                run_frame_interpreter(cpu, bus);
            }
        }

        if (!opts.dump_ppm_path.empty()) {
            write_framebuffer_ppm(opts.dump_ppm_path, bus.vdp());
        }
        const FrameStats st = analyze_framebuffer(bus.vdp());
        std::printf("SMS/GG headless done: %llu frames, %zu non-backdrop px, %zu distinct colors, "
                    "%llu fallbacks\n",
                    static_cast<unsigned long long>(frames), st.non_backdrop_pixels,
                    st.distinct_colors, static_cast<unsigned long long>(bus.fallback_count()));

        bus.cartridge().save_sram(save_path);
        if (!opts.save_state_path.empty()) {
            bus.save_state(opts.save_state_path, cpu);
        }
        return 0;
    }

#if !defined(DRAGON_HAVE_SDL3) || !DRAGON_HAVE_SDL3
    std::fprintf(stderr, "error: built without SDL3; run with --headless\n");
    return 1;
#else
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "error: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const int init_w = bus.vdp().visible_width() * opts.window_scale;
    const int init_h = bus.vdp().visible_height() * opts.window_scale;
    const SDL_WindowFlags wflags =
        SDL_WINDOW_RESIZABLE | (opts.fullscreen ? SDL_WINDOW_FULLSCREEN : 0U);
    SDL_Window* window = SDL_CreateWindow(opts.window_title.c_str(), init_w, init_h, wflags);
    if (!window) {
        std::fprintf(stderr, "error: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STREAMING, Vdp::kMaxWidth,
                                         Vdp::kMaxHeight);
    if (tex) {
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = static_cast<int>(Psg::kSampleRate);
    SDL_AudioStream* audio =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (audio) {
        SDL_ResumeAudioStreamDevice(audio);
    }

    SdlFrameCtx sctx{};
    sctx.opts = &opts;
    sctx.window = window;
    sctx.renderer = renderer;
    sctx.tex = tex;
    sctx.audio = audio;
    sctx.fullscreen = opts.fullscreen;
    sctx.aspect_ratio = opts.aspect_ratio;
    sctx.crt_filter = opts.crt_filter;
    sctx.slot = opts.state_slot;
    sctx.next_frame = std::chrono::steady_clock::now();

    if (use_recomp) {
        bus.set_frame_callback(sdl_poll_and_present, &sctx);
        while (!bus.stop_requested()) {
            if (sms_runtime_dispatch(&cpu, cpu.pc, cpu.pc) != 0) {
                break;
            }
        }
        bus.set_frame_callback(nullptr, nullptr);
    } else {
        while (!bus.stop_requested()) {
            run_frame_interpreter(cpu, bus);
            sdl_poll_and_present(&cpu, &bus, &sctx);
        }
    }

    bus.cartridge().save_sram(save_path);
    if (!opts.save_state_path.empty()) {
        bus.save_state(opts.save_state_path, cpu);
    }

    if (audio) SDL_DestroyAudioStream(audio);
    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
#endif
}

}  // namespace dragon::sms
