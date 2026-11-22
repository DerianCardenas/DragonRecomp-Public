#pragma once

#include <cstddef>
#include <filesystem>

#include "ps1/cpu_state.h"

namespace dragon::ps1 {

struct RunnerOptions {
    std::filesystem::path rom_path;
    std::filesystem::path dump_ppm_path;
    int headless_frames = 0;
    int scale = 3;  // Default 960x720 (4:3)
    bool fullscreen = false;
    bool crt_filter = false;
    bool recomp_mode = false;
    bool mute = false;
};

void parse_runner_args(int argc, char** argv, RunnerOptions& opts);

int run_ps1_app(const RunnerOptions& opts, const Ps1RecompFuncEntry* recomp_table = nullptr,
                size_t recomp_count = 0);

}  // namespace dragon::ps1
