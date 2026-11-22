#pragma once

#include <cstdint>
#include <filesystem>

#include "nds/bus.h"
#include "nds/cpu_state.h"

namespace dragon::nds {

struct RunnerOptions {
    std::filesystem::path rom_path;
    std::filesystem::path save_path;
    std::filesystem::path load_state_path;
    std::filesystem::path save_state_path;
    std::filesystem::path dump_ppm_path;
    bool recomp_mode = false;
    bool headless = false;
    uint64_t headless_frames = 0;
    uint64_t press_start_frame = 0;
    int scale = 2;
    int state_slot = 1;
    bool fullscreen = false;
    bool aspect_ratio = true;
    bool crt_filter = false;
};

struct FrameStats {
    uint32_t backdrop_color = 0;
    size_t non_backdrop_pixels = 0;
    size_t distinct_colors = 0;
};

FrameStats analyze_framebuffer(const Bus& bus) noexcept;
bool write_framebuffer_ppm(const std::filesystem::path& path, const Bus& bus);

bool parse_runner_args(int argc, char** argv, RunnerOptions& opts);
int run_nds_app(const RunnerOptions& opts, const NdsRecompFuncEntry* recomp_table = nullptr,
                size_t recomp_count = 0);

}  // namespace dragon::nds
