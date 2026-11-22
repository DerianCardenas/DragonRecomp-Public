#include "ps1/frontend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "dragon/save_state.h"
#include "ps1/runtime.h"

#ifdef DRAGON_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

namespace dragon::ps1 {

void parse_runner_args(int argc, char** argv, RunnerOptions& opts) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--rom" || arg == "-r") && i + 1 < argc) {
            opts.rom_path = argv[++i];
        } else if (arg == "--dump-ppm" && i + 1 < argc) {
            opts.dump_ppm_path = argv[++i];
        } else if (arg == "--headlessframes" && i + 1 < argc) {
            opts.headless_frames = std::atoi(argv[++i]);
        } else if (arg == "--scale" && i + 1 < argc) {
            opts.scale = std::clamp(std::atoi(argv[++i]), 1, 6);
        } else if (arg == "--fullscreen") {
            opts.fullscreen = true;
        } else if (arg == "--crt") {
            opts.crt_filter = true;
        } else if (arg == "--recomp") {
            opts.recomp_mode = true;
        } else if (arg == "--interp") {
            opts.recomp_mode = false;
        } else if (arg == "--mute") {
            opts.mute = true;
        } else if (!arg.empty() && arg[0] != '-' && opts.rom_path.empty()) {
            opts.rom_path = arg;
        }
    }
}

int run_ps1_app(const RunnerOptions& opts, const Ps1RecompFuncEntry* recomp_table,
                size_t recomp_count) {
    Ps1Runtime rt;
    if (recomp_table && recomp_count > 0) {
        rt.register_recomp_functions(recomp_table, recomp_count);
    }
    rt.set_recomp_mode(opts.recomp_mode && recomp_table && recomp_count > 0);

    std::string err;
    if (!rt.load_file(opts.rom_path, err)) {
        std::fprintf(stderr, "ps1_runtime: failed to load '%s': %s\n",
                     opts.rom_path.string().c_str(), err.c_str());
        return 1;
    }

    if (opts.headless_frames > 0) {
        for (int f = 0; f < opts.headless_frames; ++f) {
            rt.run_frame();
        }
        if (!opts.dump_ppm_path.empty()) {
            if (!rt.dump_ppm(opts.dump_ppm_path)) {
                std::fprintf(stderr, "ps1_runtime: failed to write PPM '%s'\n",
                             opts.dump_ppm_path.string().c_str());
                return 1;
            }
        }
        return 0;
    }

#ifndef DRAGON_HAVE_SDL3
    std::fprintf(stderr, "ps1_runtime: SDL3 not available; pass --headlessframes <N>\n");
    return 1;
#else
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "ps1_runtime: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const int win_w = 320 * opts.scale;
    const int win_h = 240 * opts.scale;
    const std::string title =
        "DragonRecomp PS1 — " +
        (rt.image().title.empty() ? opts.rom_path.stem().string() : rt.image().title);

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if (opts.fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;

    SDL_Window* window = SDL_CreateWindow(title.c_str(), win_w, win_h, flags);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!window || !renderer) {
        std::fprintf(stderr, "ps1_runtime: SDL window/renderer failed: %s\n", SDL_GetError());
        if (renderer)
            SDL_DestroyRenderer(renderer);
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    uint32_t tex_w = 320U;
    uint32_t tex_h = 240U;
    SDL_Texture* tex =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                          static_cast<int>(tex_w), static_cast<int>(tex_h));
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    SDL_AudioStream* audio_stream = nullptr;
    if (!opts.mute) {
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_S16;
        spec.channels = 2;
        spec.freq = static_cast<int>(Ps1Spu::kSampleRate);
        audio_stream =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audio_stream)
            SDL_ResumeAudioStreamDevice(audio_stream);
    }

    SDL_Gamepad* gamepad = nullptr;
    int num_joys = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&num_joys)) {
        if (num_joys > 0)
            gamepad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }

    bool running = true;
    bool fullscreen = opts.fullscreen;
    bool crt_filter = opts.crt_filter;
    int save_slot = 0;
    std::vector<int16_t> audio_buf(735U * 2U);

    using Clock = std::chrono::steady_clock;
    constexpr auto kFrameTime = std::chrono::microseconds(16667);

    while (running) {
        const auto frame_start = Clock::now();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (ev.type == SDL_EVENT_GAMEPAD_ADDED && !gamepad) {
                gamepad = SDL_OpenGamepad(ev.gdevice.which);
            } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED && gamepad &&
                       SDL_GetGamepadID(gamepad) == ev.gdevice.which) {
                SDL_CloseGamepad(gamepad);
                gamepad = nullptr;
            } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                const SDL_Keycode key = ev.key.key;
                if (key == SDLK_ESCAPE)
                    running = false;
                else if (key == SDLK_F11) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(window, fullscreen);
                } else if (key == SDLK_F9) {
                    crt_filter = !crt_filter;
                } else if (key == SDLK_F5) {
                    rt.save_state(dragon::slot_state_path(opts.rom_path, save_slot));
                } else if (key == SDLK_F7) {
                    rt.load_state(dragon::slot_state_path(opts.rom_path, save_slot));
                } else if (key >= SDLK_0 && key <= SDLK_9) {
                    save_slot = static_cast<int>(key - SDLK_0);
                }
            }
        }

        // Poll keyboard & gamepad
        uint16_t btns = 0U;
        const bool* ks = SDL_GetKeyboardState(nullptr);
        if (ks[SDL_SCANCODE_UP] || ks[SDL_SCANCODE_W])
            btns |= kPs1PadUp;
        if (ks[SDL_SCANCODE_DOWN] || ks[SDL_SCANCODE_S])
            btns |= kPs1PadDown;
        if (ks[SDL_SCANCODE_LEFT] || ks[SDL_SCANCODE_A])
            btns |= kPs1PadLeft;
        if (ks[SDL_SCANCODE_RIGHT] || ks[SDL_SCANCODE_D])
            btns |= kPs1PadRight;
        if (ks[SDL_SCANCODE_Z] || ks[SDL_SCANCODE_J])
            btns |= kPs1PadCross;
        if (ks[SDL_SCANCODE_X] || ks[SDL_SCANCODE_K])
            btns |= kPs1PadCircle;
        if (ks[SDL_SCANCODE_C] || ks[SDL_SCANCODE_U])
            btns |= kPs1PadSquare;
        if (ks[SDL_SCANCODE_V] || ks[SDL_SCANCODE_I])
            btns |= kPs1PadTriangle;
        if (ks[SDL_SCANCODE_Q])
            btns |= kPs1PadL1;
        if (ks[SDL_SCANCODE_E])
            btns |= kPs1PadR1;
        if (ks[SDL_SCANCODE_1])
            btns |= kPs1PadL2;
        if (ks[SDL_SCANCODE_3])
            btns |= kPs1PadR2;
        if (ks[SDL_SCANCODE_RETURN])
            btns |= kPs1PadStart;
        if (ks[SDL_SCANCODE_RSHIFT] || ks[SDL_SCANCODE_BACKSPACE])
            btns |= kPs1PadSelect;

        if (gamepad) {
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP))
                btns |= kPs1PadUp;
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))
                btns |= kPs1PadDown;
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
                btns |= kPs1PadLeft;
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
                btns |= kPs1PadRight;
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH))
                btns |= kPs1PadCross;
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST))
                btns |= kPs1PadCircle;
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST))
                btns |= kPs1PadSquare;
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH))
                btns |= kPs1PadTriangle;
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))
                btns |= kPs1PadL1;
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))
                btns |= kPs1PadR1;
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START))
                btns |= kPs1PadStart;
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK))
                btns |= kPs1PadSelect;
            if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000)
                btns |= kPs1PadL2;
            if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000)
                btns |= kPs1PadR2;
        }
        rt.bus().set_pad_buttons(btns);

        rt.run_frame();

        if (audio_stream) {
            const int queued = SDL_GetAudioStreamQueued(audio_stream);
            if (queued < static_cast<int>(Ps1Spu::kSampleRate / 4U) * 4) {
                rt.bus().spu().pop_stereo(audio_buf.data(), 735U);
                SDL_PutAudioStreamData(audio_stream, audio_buf.data(),
                                       static_cast<int>(audio_buf.size() * sizeof(int16_t)));
            }
        }

        // Recreate texture if game changes resolution (e.g., 320x240 <-> 640x480)
        if (rt.fb_width() != tex_w || rt.fb_height() != tex_h) {
            tex_w = rt.fb_width();
            tex_h = rt.fb_height();
            SDL_DestroyTexture(tex);
            tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                    static_cast<int>(tex_w), static_cast<int>(tex_h));
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        }

        SDL_UpdateTexture(tex, nullptr, rt.framebuffer(),
                          static_cast<int>(tex_w * sizeof(uint32_t)));
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        int out_w = win_w, out_h = win_h;
        SDL_GetRenderOutputSize(renderer, &out_w, &out_h);
        constexpr float kAspect = 4.0f / 3.0f;
        float dst_w = static_cast<float>(out_w);
        float dst_h = dst_w / kAspect;
        if (dst_h > static_cast<float>(out_h)) {
            dst_h = static_cast<float>(out_h);
            dst_w = dst_h * kAspect;
        }
        SDL_FRect dst{(static_cast<float>(out_w) - dst_w) * 0.5f,
                      (static_cast<float>(out_h) - dst_h) * 0.5f, dst_w, dst_h};
        SDL_RenderTexture(renderer, tex, nullptr, &dst);

        if (crt_filter) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 56);
            for (float y = dst.y; y < dst.y + dst.h; y += 3.0f) {
                SDL_RenderLine(renderer, dst.x, y, dst.x + dst.w, y);
            }
        }

        SDL_RenderPresent(renderer);

        const auto elapsed = Clock::now() - frame_start;
        if (elapsed < kFrameTime) {
            std::this_thread::sleep_for(kFrameTime - elapsed);
        }
    }

    if (gamepad)
        SDL_CloseGamepad(gamepad);
    if (audio_stream)
        SDL_DestroyAudioStream(audio_stream);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
#endif
}

}  // namespace dragon::ps1
