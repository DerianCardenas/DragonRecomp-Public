#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "psp/cpu_state.h"

namespace dragon::psp {

struct RunnerOptions {
    std::filesystem::path rom_path;
    std::filesystem::path dump_ppm_path;
    int headless_frames = 0;
    int scale = 2;  // Default 960x544 window
    bool fullscreen = false;
    bool crt_filter = false;
    bool recomp_mode = false;
    bool mute = false;
};

void parse_runner_args(int argc, char** argv, RunnerOptions& opts);

int run_psp_app(const RunnerOptions& opts, const PspRecompFuncEntry* recomp_table = nullptr,
                size_t recomp_count = 0);

}  // namespace dragon::psp
