#pragma once

// SNES Frontend: SDL3 window (768x672) + 48 kHz audio stream, keyboard input,
// headless runner, lock-step verifier, and PPM dump.
// Reference: docs/SPEC.md §6, docs/SNES_SPEC.md §6 (S2/S3/S4).

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "snes/bus.h"
#include "snes/cpu_state.h"
#include "snes/ppu.h"

namespace dragon::snes {

constexpr uint16_t kButtonB = 0x8000U;
constexpr uint16_t kButtonY = 0x4000U;
constexpr uint16_t kButtonSelect = 0x2000U;
constexpr uint16_t kButtonStart = 0x1000U;
constexpr uint16_t kButtonUp = 0x0800U;
constexpr uint16_t kButtonDown = 0x0400U;
constexpr uint16_t kButtonLeft = 0x0200U;
constexpr uint16_t kButtonRight = 0x0100U;
constexpr uint16_t kButtonA = 0x0080U;
constexpr uint16_t kButtonX = 0x0040U;
constexpr uint16_t kButtonL = 0x0020U;
constexpr uint16_t kButtonR = 0x0010U;

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
    std::string window_title = "DragonRecomp SNES";
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

    const RecompFuncEntry* dispatch_table = nullptr;
    size_t dispatch_count = 0;

    // Config-driven stack-trick / jump-engine dispatch targets (see Bus).
    const uint32_t* stack_trick_targets = nullptr;
    size_t stack_trick_count = 0;
};

// Analyze a 256x224 RGBA8888 framebuffer for distinct colors and non-backdrop pixels.
FrameStats analyze_framebuffer(
    const std::array<uint32_t, Ppu::kScreenWidth * Ppu::kScreenHeight>& fb) noexcept;

// Write a 256x224 RGBA8888 framebuffer as a binary P6 PPM image.
bool write_framebuffer_ppm(const std::filesystem::path& path,
                           const std::array<uint32_t, Ppu::kScreenWidth * Ppu::kScreenHeight>& fb);

// Parse standard CLI arguments (--recomp, --interp, --verify, --headless-frames N, --press-start N,
// --dump-ppm, etc). Returns false if execution should exit immediately (e.g. --help or invalid
// option).
bool parse_runner_args(int argc, char** argv, RunnerOptions& opts, int& exit_code);

// Run the SNES application according to `opts`.
int run_snes_app(const RunnerOptions& opts);

}  // namespace dragon::snes
