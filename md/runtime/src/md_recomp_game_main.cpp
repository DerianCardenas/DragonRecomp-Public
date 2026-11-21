#include <cstdio>
#include <filesystem>

#include "md/cpu_state.h"
#include "md/frontend.h"

extern "C" {
extern const MdRecompFuncEntry md_recomp_functions[];
extern const size_t md_recomp_function_count;
extern const char* md_recomp_default_rom_path;
extern const char* md_recomp_expected_sha1;
}

int main(int argc, char** argv) {
    dragon::md::RunnerOptions opts;
    opts.mode = dragon::md::RunMode::Recompiled;
    if (md_recomp_default_rom_path && md_recomp_default_rom_path[0] != '\0') {
        opts.rom_path = md_recomp_default_rom_path;
    }
    if (md_recomp_expected_sha1 && md_recomp_expected_sha1[0] != '\0') {
        opts.expected_sha1 = md_recomp_expected_sha1;
    }
    opts.recomp_table = md_recomp_functions;
    opts.recomp_table_count = md_recomp_function_count;

    int exit_code = 0;
    if (!dragon::md::parse_runner_args(argc, argv, opts, exit_code)) {
        return exit_code;
    }

    if (opts.rom_path.empty() || !std::filesystem::exists(opts.rom_path)) {
        const auto local_roms = std::filesystem::current_path() / "roms";
        if (std::filesystem::exists(local_roms)) {
            for (const auto& entry : std::filesystem::directory_iterator(local_roms)) {
                if (entry.is_regular_file()) {
                    const auto ext = entry.path().extension().string();
                    if (ext == ".md" || ext == ".bin" || ext == ".gen" || ext == ".smd") {
                        opts.rom_path = entry.path();
                        break;
                    }
                }
            }
        }
    }

    return dragon::md::run_md_app(opts);
}
