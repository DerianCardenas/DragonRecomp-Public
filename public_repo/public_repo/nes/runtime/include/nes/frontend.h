#pragma once

// NES Frontend: SDL3 window + keyboard input, headless runner, lock-step verifier, and PPM dump.
// Reference: docs/SPEC.md §6, docs/NES_SPEC.md §5 (N2/N3).

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "nes/bus.h"
#include "nes/cpu_state.h"
#include "nes/ppu.h"

namespace dragon::nes {

enum class RunMode : uint8_t {
    Recompiled,
    Interpreter,
    Verify,
};

struct FrameStats {
    size_t non_backdrop_pixels = 0;
    size_t distinct_colors = 0;
    uint32_t backdrop_color = 0;
};

struct RunnerOptions {
    std::string window_title = "DragonRecomp NES";
    std::filesystem::path rom_path;
    std::filesystem::path save_path;
    std::filesystem::path load_state_path;
    std::filesystem::path save_state_path;
    int state_slot = 1;
    std::string expected_sha1;
    RunMode mode = RunMode::Recompiled;
    bool headless = false;
    uint64_t headless_frames = 0;
    uint64_t press_start_frame = 0;
    std::filesystem::path dump_ppm_path;
    int window_scale = 3;
    bool fullscreen = false;
    bool aspect_ratio = true;
    bool crt_filter = false;
    std::string palette;

    const RecompFuncEntry* dispatch_table = nullptr;
    size_t dispatch_count = 0;
    RecompFunc reset_func = nullptr;
};

// Analyze a 256x240 RGBA8888 framebuffer for distinct colors and non-backdrop pixels.
FrameStats analyze_framebuffer(
    const std::array<uint32_t, Ppu::kScreenWidth * Ppu::kScreenHeight>& fb) noexcept;

// Write a 256x240 RGBA8888 framebuffer as a binary P6 PPM image.
bool write_framebuffer_ppm(const std::filesystem::path& path,
                           const std::array<uint32_t, Ppu::kScreenWidth * Ppu::kScreenHeight>& fb);

// Parse standard CLI arguments (--recomp, --interp, --verify, --headlessframes N, --dump-ppm, etc).
// Returns false if execution should exit immediately (e.g. --help or invalid option).
bool parse_runner_args(int argc, char** argv, RunnerOptions& opts, int& exit_code);

// Run the NES application (SDL3 window or headless mode) according to `opts`.
int run_nes_app(const RunnerOptions& opts);

}  // namespace dragon::nes
