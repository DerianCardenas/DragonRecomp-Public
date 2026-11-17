#pragma once

// Game Boy / Game Boy Color Frontend: SDL3 window, input handling, headless runner, and PPM dump.
// Reference: docs/GB_SPEC.md §5, docs/SPEC.md.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "gb/bus.h"
#include "gb/cpu_state.h"
#include "gb/ppu.h"

namespace dragon::gb {

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
    std::string window_title = "DragonRecomp Game Boy";
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
    int window_scale = 4;  // 160x144 * 4 = 640x576
    bool fullscreen = false;
    bool aspect_ratio = true;
    bool crt_filter = false;
    bool classic_palette = false;

    const RecompFuncEntry* dispatch_table = nullptr;
    size_t dispatch_count = 0;
    RecompFunc reset_func = nullptr;
};

FrameStats analyze_framebuffer(
    const std::array<uint32_t, Ppu::kScreenWidth * Ppu::kScreenHeight>& fb) noexcept;

bool write_framebuffer_ppm(const std::filesystem::path& path,
                           const std::array<uint32_t, Ppu::kScreenWidth * Ppu::kScreenHeight>& fb);

bool parse_runner_args(int argc, char** argv, RunnerOptions& opts, int& exit_code);

int run_gb_app(const RunnerOptions& opts);

}  // namespace dragon::gb
