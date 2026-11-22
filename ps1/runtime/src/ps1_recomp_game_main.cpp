#include <cstdio>
#include <filesystem>

#include "ps1/frontend.h"

extern "C" {
extern const Ps1RecompFuncEntry g_ps1_recomp_functions[];
extern const size_t g_ps1_recomp_function_count;
extern const char* g_ps1_expected_rom_sha1;
extern const char* g_ps1_default_rom_path;
}

int main(int argc, char** argv) {
    dragon::ps1::RunnerOptions opts{};
    opts.recomp_mode = true;
    if (g_ps1_default_rom_path && g_ps1_default_rom_path[0] != '\0') {
        opts.rom_path = g_ps1_default_rom_path;
    }
    dragon::ps1::parse_runner_args(argc, argv, opts);

    if (opts.rom_path.empty() || !std::filesystem::exists(opts.rom_path)) {
        const auto local_roms = std::filesystem::current_path() / "roms";
        if (std::filesystem::exists(local_roms)) {
            for (const auto& entry : std::filesystem::directory_iterator(local_roms)) {
                if (entry.is_regular_file()) {
                    const auto ext = entry.path().extension().string();
                    if (ext == ".cue" || ext == ".CUE" || ext == ".bin" || ext == ".BIN" ||
                        ext == ".iso" || ext == ".ISO" || ext == ".psexe" || ext == ".psx" ||
                        ext == ".exe" || ext == ".EXE") {
                        opts.rom_path = entry.path();
                        break;
                    }
                }
            }
        }
    }

    if (opts.rom_path.empty()) {
        std::fprintf(stderr,
                     "ps1_recomp: PlayStation 1 image (.cue/.bin/.iso/.psexe) not found "
                     "(pass --rom <path> or place in roms/)\n");
        return 1;
    }

    return dragon::ps1::run_ps1_app(opts, g_ps1_recomp_functions, g_ps1_recomp_function_count);
}
