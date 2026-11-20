#pragma once

// PC Engine / TurboGrafx-16 SDL3 & Headless Frontend.
// Reference: docs/PCE_SPEC.md §5.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "pce/bus.h"
#include "pce/cpu_state.h"

namespace dragon::pce {

enum class RunMode : uint8_t {
    Interpreter = 0,
    Recompiled = 1,
};

struct RunnerOptions {
    std::string window_title = "DragonRecomp PC Engine / TurboGrafx-16";
    std::filesystem::path rom_path;
    std::string expected_sha1;
    std::filesystem::path save_path;
    std::filesystem::path save_state_path;
    std::filesystem::path load_state_path;
    std::filesystem::path dump_ppm_path;

    RunMode mode = RunMode::Interpreter;
    const PceRecompFuncEntry* dispatch_table = nullptr;
    size_t dispatch_count = 0;

    bool headless = false;
    uint64_t headless_frames = 0;
    uint64_t press_start_frame = 0;

    int window_scale = 3;
    int state_slot = 0;
    bool fullscreen = false;
    bool aspect_ratio = true;
    bool crt_filter = false;
};

struct FrameStats {
    size_t non_backdrop_pixels = 0;
    size_t distinct_colors = 0;
    uint32_t backdrop_color = 0;
};

FrameStats analyze_framebuffer(const Vdc& vdc) noexcept;
bool write_framebuffer_ppm(const std::filesystem::path& path, const Vdc& vdc);

bool parse_runner_args(int argc, char** argv, RunnerOptions& opts, int& exit_code);
int run_pce_app(const RunnerOptions& opts);

}  // namespace dragon::pce
