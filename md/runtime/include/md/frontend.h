#pragma once

// Sega Genesis / Mega Drive SDL3 + headless frontend runner.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

#include "md/bus.h"
#include "md/cpu_state.h"

namespace dragon::md {

enum class RunMode {
    Interpreter,
    Recompiled,
};

struct RunnerOptions {
    std::filesystem::path rom_path;
    std::string expected_sha1;
    std::filesystem::path save_path;
    std::filesystem::path load_state_path;
    std::filesystem::path save_state_path;
    std::filesystem::path dump_ppm_path;
    RunMode mode = RunMode::Interpreter;
    bool headless = false;
    uint64_t headless_frames = 0;
    uint64_t press_start_frame = 0;
    int window_scale = 3;
    int state_slot = 1;
    bool fullscreen = false;
    bool aspect_ratio = true;
    bool crt_filter = false;
    const MdRecompFuncEntry* recomp_table = nullptr;
    size_t recomp_table_count = 0;
};

struct FrameStats {
    uint32_t backdrop_color = 0;
    size_t non_backdrop_pixels = 0;
    size_t distinct_colors = 0;
};

FrameStats analyze_framebuffer(const Vdp& vdp) noexcept;
bool write_framebuffer_ppm(const std::filesystem::path& path, const Vdp& vdp);
bool parse_runner_args(int argc, char** argv, RunnerOptions& opts, int& exit_code);
int run_md_app(const RunnerOptions& opts);

}  // namespace dragon::md
