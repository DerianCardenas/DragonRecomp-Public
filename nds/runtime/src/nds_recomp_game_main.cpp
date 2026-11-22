#include <cstdio>
#include <filesystem>

#include "nds/frontend.h"

extern "C" {
extern const NdsRecompFuncEntry g_nds_recomp_functions[];
extern const size_t g_nds_recomp_function_count;
extern const char* g_nds_expected_rom_sha1;
extern const char* g_nds_default_rom_path;
}

int main(int argc, char** argv) {
    dragon::nds::RunnerOptions opts{};
    opts.recomp_mode = true;
    if (g_nds_default_rom_path && g_nds_default_rom_path[0] != '\0') {
        opts.rom_path = g_nds_default_rom_path;
    }
    dragon::nds::parse_runner_args(argc, argv, opts);

    if (opts.rom_path.empty() || !std::filesystem::exists(opts.rom_path)) {
        const auto local_roms = std::filesystem::current_path() / "roms";
        if (std::filesystem::exists(local_roms)) {
            for (const auto& entry : std::filesystem::directory_iterator(local_roms)) {
                if (entry.is_regular_file()) {
                    const auto ext = entry.path().extension().string();
                    if (ext == ".nds" || ext == ".NDS") {
                        opts.rom_path = entry.path();
                        break;
                    }
                }
            }
        }
    }

    if (opts.rom_path.empty()) {
        std::fprintf(stderr,
                     "nds_recomp: ROM .nds not found (pass --rom <path> or place in roms/)\n");
        return 1;
    }

    return dragon::nds::run_nds_app(opts, g_nds_recomp_functions, g_nds_recomp_function_count);
}
