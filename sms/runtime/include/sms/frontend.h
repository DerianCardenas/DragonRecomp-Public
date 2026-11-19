#pragma once

// Sega Master System & Game Gear Frontend: SDL3 window, audio, input, save states, headless runner.
// Reference: docs/SMS_SPEC.md §6, docs/SPEC.md.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "sms/bus.h"
#include "sms/cpu_state.h"
#include "sms/vdp.h"

namespace dragon::sms {

enum class RunMode : uint8_t {
    Recompiled,
    Interpreter,
};

struct FrameStats {
    size_t non_backdrop_pixels = 0;
    size_t distinct_colors = 0;
    uint32_t backdrop_color = 0;
};

struct RunnerOptions {
    std::string window_title = "DragonRecomp Sega Master System";
    std::filesystem::path rom_path;
    std::filesystem::path save_path;
    std::filesystem::path load_state_path;
    std::filesystem::path save_state_path;
    int state_slot = 1;
    std::string expected_sha1;
    RunMode mode = RunMode::Interpreter;
    bool headless = false;
    uint64_t headless_frames = 0;
    uint64_t press_start_frame = 0;
    std::filesystem::path dump_ppm_path;
    int window_scale = 3;  // 256x192 * 3 = 768x576
    bool fullscreen = false;
    bool aspect_ratio = true;
    bool crt_filter = false;

    const SmsRecompFuncEntry* dispatch_table = nullptr;
    size_t dispatch_count = 0;
    SmsRecompFunc reset_func = nullptr;
};

FrameStats analyze_framebuffer(const Vdp& vdp) noexcept;
bool write_framebuffer_ppm(const std::filesystem::path& path, const Vdp& vdp);
bool parse_runner_args(int argc, char** argv, RunnerOptions& opts, int& exit_code);
int run_sms_app(const RunnerOptions& opts);

}  // namespace dragon::sms
