#pragma once

// Game Boy Advance Frontend: SDL3 window, input handling, headless runner, and PPM dump.
// Reference: docs/GBA_SPEC.md, docs/SPEC.md.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "gba/bus.h"
#include "gba/cpu_state.h"
#include "gba/ppu.h"

namespace dragon::gba {

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
    std::string window_title = "DragonRecomp Game Boy Advance";
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
    int window_scale = 3;  // 240x160 * 3 = 720x480
    bool fullscreen = false;
    bool aspect_ratio = true;
    bool crt_filter = false;

    const RecompFuncEntry* dispatch_table = nullptr;
    size_t dispatch_count = 0;
    RecompFunc reset_func = nullptr;
};

FrameStats analyze_framebuffer(
    const std::array<uint32_t, Ppu::kScreenWidth * Ppu::kScreenHeight>& fb) noexcept;

bool write_framebuffer_ppm(const std::filesystem::path& path,
                           const std::array<uint32_t, Ppu::kScreenWidth * Ppu::kScreenHeight>& fb);

bool parse_runner_args(int argc, char** argv, RunnerOptions& opts, int& exit_code);

int run_gba_app(const RunnerOptions& opts);

}  // namespace dragon::gba
