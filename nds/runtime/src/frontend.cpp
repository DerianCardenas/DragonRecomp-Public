#include "nds/frontend.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "dragon/save_state.h"
#include "nds/runtime.h"

#if defined(DRAGON_HAVE_SDL3)
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

namespace dragon::nds {

FrameStats analyze_framebuffer(const Bus& bus) noexcept {
    FrameStats stats{};
    const auto& fb = bus.framebuffer();
    stats.backdrop_color = fb[0];
    std::array<uint32_t, 128> seen{};
    size_t seen_count = 0;

    for (uint32_t px : fb) {
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
    stats.distinct_colors = seen_count;
    return stats;
}

bool write_framebuffer_ppm(const std::filesystem::path& path, const Bus& bus) {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;

    const auto& fb = bus.framebuffer();
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "P6\n%u %u\n255\n", Bus::kScreenWidth, Bus::kScreenHeight);
    out.write(hdr, static_cast<std::streamsize>(std::strlen(hdr)));

    std::vector<uint8_t> row(Bus::kScreenWidth * 3U);
    for (uint32_t y = 0; y < Bus::kScreenHeight; ++y) {
        for (uint32_t x = 0; x < Bus::kScreenWidth; ++x) {
            const uint32_t rgba = fb[static_cast<size_t>(y) * Bus::kScreenWidth + x];
            row[x * 3U + 0U] = static_cast<uint8_t>(rgba & 0xFFU);
            row[x * 3U + 1U] = static_cast<uint8_t>((rgba >> 8U) & 0xFFU);
            row[x * 3U + 2U] = static_cast<uint8_t>((rgba >> 16U) & 0xFFU);
        }
        out.write(reinterpret_cast<const char*>(row.data()),
                  static_cast<std::streamsize>(row.size()));
    }
    return out.good();
}

namespace {

void print_usage(const char* prog) {
    std::printf(
        "usage: %s [options] [rom.nds]\n"
        "options:\n"
        "  --recomp               Run statically recompiled C99 code\n"
        "  --interp               Run ARM9/ARM7 interpreter mode (default)\n"
        "  --rom <path>           Override path to Nintendo DS ROM\n"
        "  --save-path <path>     Path to SPI Flash/EEPROM save (.sav)\n"
        "  --state-slot <N>       Select initial save state slot (0-9, default: 1)\n"
        "  --load-state <path>    Load save state on launch\n"
        "  --save-state <path>    Save state file path on clean exit\n"
        "  --headlessframes <N>   Run N frames without SDL window\n"
        "  --press-start <frame>  Press Start at frame <frame>\n"
        "  --dump-ppm <file.ppm>  Write final dual-screen framebuffer to binary PPM\n"
        "  --scale <S>            Window integer scale factor (default: 2)\n"
        "  --fullscreen           Launch in fullscreen (toggle: F11 / Alt+Enter)\n"
        "  --no-aspect, --stretch Disable 2:3 aspect ratio correction (toggle: F8)\n"
        "  --crt, --scanlines     Enable CRT scanlines (toggle: F9)\n"
        "  -h, --help             Show this help message\n",
        prog);
}

struct FrameCtx {
    const RunnerOptions* opts = nullptr;
    NdsCpuState* arm7 = nullptr;
    uint64_t frames = 0;
    bool quit = false;
    int slot = 1;
    bool crt = false;
    bool fullscreen = false;
    bool aspect = true;
#if defined(DRAGON_HAVE_SDL3)
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_AudioStream* audio_stream = nullptr;
    SDL_FRect dst_rect{0.0f, 0.0f, 512.0f, 768.0f};
    uint64_t next_frame_ns = 0ULL;
#endif
};

void on_frame(NdsCpuState* arm9, Bus* bus, void* user) {
    auto* ctx = static_cast<FrameCtx*>(user);
    if (!ctx || !ctx->opts)
        return;

    ++ctx->frames;
    uint16_t scripted = 0U;
    if (ctx->opts->press_start_frame > 0ULL && ctx->frames >= ctx->opts->press_start_frame &&
        ctx->frames < ctx->opts->press_start_frame + 10ULL) {
        scripted |= kBtnStart;
    }

    if (ctx->opts->headless) {
        bus->set_buttons(scripted);
        std::array<int16_t, 2048> discard{};
        while (bus->drain_audio_samples(discard.data(), discard.size() / 2U) > 0U) {
        }
        if (ctx->opts->headless_frames > 0ULL && ctx->frames >= ctx->opts->headless_frames) {
            ctx->quit = true;
            arm9->halted = 2U;
        }
        return;
    }

#if defined(DRAGON_HAVE_SDL3)
    if (ctx->audio_stream) {
        std::array<int16_t, 4096> audio_buf{};
        const size_t got = bus->drain_audio_samples(audio_buf.data(), audio_buf.size() / 2U);
        if (got > 0U && SDL_GetAudioStreamQueued(ctx->audio_stream) <
                            static_cast<int>(Spu::kSampleRate * 2U * sizeof(int16_t) / 4U)) {
            SDL_PutAudioStreamData(ctx->audio_stream, audio_buf.data(),
                                   static_cast<int>(got * 2U * sizeof(int16_t)));
        }
    }

    if (ctx->window && ctx->renderer) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                ctx->quit = true;
                arm9->halted = 2U;
                return;
            } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                    ctx->quit = true;
                    arm9->halted = 2U;
                    return;
                } else if (ev.key.scancode == SDL_SCANCODE_F5 && ctx->arm7) {
                    const auto target =
                        (ctx->opts->save_state_path.empty() || ctx->slot != ctx->opts->state_slot)
                            ? slot_state_path(ctx->opts->rom_path, ctx->slot)
                            : ctx->opts->save_state_path;
                    bus->save_state(*arm9, *ctx->arm7, target);
                } else if (ev.key.scancode == SDL_SCANCODE_F7 && ctx->arm7) {
                    const auto target =
                        (ctx->opts->load_state_path.empty() || ctx->slot != ctx->opts->state_slot)
                            ? slot_state_path(ctx->opts->rom_path, ctx->slot)
                            : ctx->opts->load_state_path;
                    bus->load_state(*arm9, *ctx->arm7, target);
                } else if (ev.key.scancode == SDL_SCANCODE_F8) {
                    ctx->aspect = !ctx->aspect;
                } else if (ev.key.scancode == SDL_SCANCODE_F9) {
                    ctx->crt = !ctx->crt;
                } else if (ev.key.scancode == SDL_SCANCODE_F11 ||
                           (ev.key.scancode == SDL_SCANCODE_RETURN &&
                            (SDL_GetModState() & SDL_KMOD_ALT))) {
                    ctx->fullscreen = !ctx->fullscreen;
                    SDL_SetWindowFullscreen(ctx->window, ctx->fullscreen);
                } else if (ev.key.scancode >= SDL_SCANCODE_1 && ev.key.scancode <= SDL_SCANCODE_9) {
                    ctx->slot = static_cast<int>(ev.key.scancode - SDL_SCANCODE_1 + 1);
                } else if (ev.key.scancode == SDL_SCANCODE_0) {
                    ctx->slot = 0;
                }
            }
        }

        uint16_t btns = scripted;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (keys) {
            if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W])
                btns |= kBtnUp;
            if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S])
                btns |= kBtnDown;
            if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A])
                btns |= kBtnLeft;
            if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D])
                btns |= kBtnRight;
            if (keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_J])
                btns |= kBtnA;
            if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_K])
                btns |= kBtnB;
            if (keys[SDL_SCANCODE_V] || keys[SDL_SCANCODE_I])
                btns |= kBtnX;
            if (keys[SDL_SCANCODE_C] || keys[SDL_SCANCODE_L])
                btns |= kBtnY;
            if (keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_U])
                btns |= kBtnL;
            if (keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_O])
                btns |= kBtnR;
            if (keys[SDL_SCANCODE_RETURN])
                btns |= kBtnStart;
            if (keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_BACKSPACE])
                btns |= kBtnSelect;
        }
        bus->set_buttons(btns);

        // Mouse -> Bottom Touchscreen (Y in 192..383 of combined 256x384 framebuffer)
        float mx = 0.0f, my = 0.0f;
        const SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
        if ((mb & SDL_BUTTON_LMASK) && ctx->dst_rect.w > 0.0f && ctx->dst_rect.h > 0.0f) {
            const float nx = (mx - ctx->dst_rect.x) / ctx->dst_rect.w;
            const float ny = (my - ctx->dst_rect.y) / ctx->dst_rect.h;
            if (nx >= 0.0f && nx <= 1.0f && ny >= 0.5f && ny <= 1.0f) {
                const uint8_t tx = static_cast<uint8_t>(std::clamp(nx * 255.0f, 0.0f, 255.0f));
                const uint8_t ty =
                    static_cast<uint8_t>(std::clamp((ny - 0.5f) * 2.0f * 191.0f, 0.0f, 191.0f));
                bus->set_touch(true, tx, ty);
            } else {
                bus->set_touch(false, 0U, 0U);
            }
        } else {
            bus->set_touch(false, 0U, 0U);
        }

        if (ctx->texture) {
            SDL_UpdateTexture(ctx->texture, nullptr, bus->framebuffer().data(),
                              static_cast<int>(Bus::kScreenWidth * sizeof(uint32_t)));
        }

        SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);
        SDL_RenderClear(ctx->renderer);

        int win_w = 0, win_h = 0;
        SDL_GetCurrentRenderOutputSize(ctx->renderer, &win_w, &win_h);
        SDL_FRect dst{0.0f, 0.0f, static_cast<float>(win_w), static_cast<float>(win_h)};
        if (ctx->aspect && win_w > 0 && win_h > 0) {
            const float target = static_cast<float>(Bus::kScreenWidth) / Bus::kScreenHeight;
            const float cur = static_cast<float>(win_w) / static_cast<float>(win_h);
            if (cur > target) {
                dst.w = dst.h * target;
                dst.x = (static_cast<float>(win_w) - dst.w) * 0.5f;
            } else {
                dst.h = dst.w / target;
                dst.y = (static_cast<float>(win_h) - dst.h) * 0.5f;
            }
        }
        ctx->dst_rect = dst;
        if (ctx->texture) {
            SDL_RenderTexture(ctx->renderer, ctx->texture, nullptr, &dst);
        }

        if (ctx->crt) {
            SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 75);
            for (float y = dst.y; y < dst.y + dst.h; y += 3.0f) {
                SDL_RenderLine(ctx->renderer, dst.x, y, dst.x + dst.w, y);
            }
            SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_NONE);
        }
        SDL_RenderPresent(ctx->renderer);

        // ~59.83 Hz pacing
        constexpr uint64_t kFrameNs = 16715000ULL;
        const uint64_t now = SDL_GetTicksNS();
        if (ctx->next_frame_ns == 0ULL) {
            ctx->next_frame_ns = now + kFrameNs;
        } else {
            if (now < ctx->next_frame_ns) {
                SDL_DelayNS(ctx->next_frame_ns - now);
            }
            ctx->next_frame_ns += kFrameNs;
            if (ctx->next_frame_ns + kFrameNs * 4ULL < now) {
                ctx->next_frame_ns = now + kFrameNs;
            }
        }
    }
#endif
}

}  // namespace

bool parse_runner_args(int argc, char** argv, RunnerOptions& opts) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return false;
        } else if (arg == "--recomp") {
            opts.recomp_mode = true;
        } else if (arg == "--interp") {
            opts.recomp_mode = false;
        } else if (arg == "--rom" && i + 1 < argc) {
            opts.rom_path = argv[++i];
        } else if (arg == "--save-path" && i + 1 < argc) {
            opts.save_path = argv[++i];
        } else if (arg == "--state-slot" && i + 1 < argc) {
            opts.state_slot = std::clamp(std::atoi(argv[++i]), 0, 9);
        } else if (arg == "--load-state" && i + 1 < argc) {
            opts.load_state_path = argv[++i];
        } else if (arg == "--save-state" && i + 1 < argc) {
            opts.save_state_path = argv[++i];
        } else if (arg == "--headlessframes" && i + 1 < argc) {
            opts.headless = true;
            opts.headless_frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--press-start" && i + 1 < argc) {
            opts.press_start_frame = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--dump-ppm" && i + 1 < argc) {
            opts.dump_ppm_path = argv[++i];
        } else if (arg == "--scale" && i + 1 < argc) {
            opts.scale = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--fullscreen") {
            opts.fullscreen = true;
        } else if (arg == "--no-aspect" || arg == "--stretch") {
            opts.aspect_ratio = false;
        } else if (arg == "--crt" || arg == "--scanlines") {
            opts.crt_filter = true;
        } else if (!arg.empty() && arg[0] != '-' && opts.rom_path.empty()) {
            opts.rom_path = arg;
        }
    }
    return !opts.rom_path.empty();
}

int run_nds_app(const RunnerOptions& opts, const NdsRecompFuncEntry* recomp_table,
                size_t recomp_count) {
    Bus bus;
    std::string err;
    if (!bus.load_rom(opts.rom_path, &err)) {
        std::fprintf(stderr, "nds_runtime: %s\n", err.c_str());
        return 1;
    }
    if (!opts.save_path.empty()) {
        bus.cartridge().load_save_file(opts.save_path);
    }

    NdsCpuState arm9{};
    NdsCpuState arm7{};
    bus.reset(arm9, arm7);

    if (recomp_table && recomp_count > 0U) {
        bus.register_recomp_functions(recomp_table, recomp_count);
    }
    bus.set_recomp_mode(opts.recomp_mode && recomp_count > 0U);

    if (!opts.load_state_path.empty()) {
        bus.load_state(arm9, arm7, opts.load_state_path);
    }

    FrameCtx ctx{};
    ctx.opts = &opts;
    ctx.arm7 = &arm7;
    ctx.slot = opts.state_slot;
    ctx.crt = opts.crt_filter;
    ctx.fullscreen = opts.fullscreen;
    ctx.aspect = opts.aspect_ratio;

#if defined(DRAGON_HAVE_SDL3)
    if (!opts.headless) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
            std::fprintf(stderr, "nds_runtime: SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        const std::string title = "DragonRecomp NDS — " + (bus.cartridge().header().title.empty()
                                                               ? opts.rom_path.stem().string()
                                                               : bus.cartridge().header().title);
        const int w = static_cast<int>(Bus::kScreenWidth) * opts.scale;
        const int h = static_cast<int>(Bus::kScreenHeight) * opts.scale;
        ctx.window = SDL_CreateWindow(title.c_str(), w, h, SDL_WINDOW_RESIZABLE);
        if (ctx.window && opts.fullscreen) {
            SDL_SetWindowFullscreen(ctx.window, true);
        }
        ctx.renderer = ctx.window ? SDL_CreateRenderer(ctx.window, nullptr) : nullptr;
        if (ctx.renderer) {
            ctx.texture = SDL_CreateTexture(ctx.renderer, SDL_PIXELFORMAT_ABGR8888,
                                            SDL_TEXTUREACCESS_STREAMING, Bus::kScreenWidth,
                                            Bus::kScreenHeight);
            if (ctx.texture) {
                SDL_SetTextureScaleMode(ctx.texture, SDL_SCALEMODE_NEAREST);
            }
        }
        SDL_AudioSpec spec{};
        spec.freq = static_cast<int>(Spu::kSampleRate);
        spec.format = SDL_AUDIO_S16;
        spec.channels = 2;
        ctx.audio_stream =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (ctx.audio_stream) {
            SDL_ResumeAudioStreamDevice(ctx.audio_stream);
        }
    }
#endif

    bus.set_frame_callback(on_frame, &ctx);

    while (!ctx.quit && arm9.halted != 2U) {
        run_frame(arm9, arm7, bus);
    }

    if (!opts.dump_ppm_path.empty()) {
        write_framebuffer_ppm(opts.dump_ppm_path, bus);
    }
    if (!opts.save_path.empty() && bus.cartridge().save_dirty()) {
        bus.cartridge().save_to_file(opts.save_path);
    }
    if (!opts.save_state_path.empty()) {
        bus.save_state(arm9, arm7, opts.save_state_path);
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

}  // namespace dragon::nds
