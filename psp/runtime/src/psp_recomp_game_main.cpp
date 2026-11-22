#include <cstdio>
#include <filesystem>

#include "psp/frontend.h"

extern "C" {
extern const PspRecompFuncEntry g_psp_recomp_functions[];
extern const size_t g_psp_recomp_function_count;
extern const char* g_psp_expected_rom_sha1;
extern const char* g_psp_default_rom_path;
}

int main(int argc, char** argv) {
    dragon::psp::RunnerOptions opts{};
    opts.recomp_mode = true;
    if (g_psp_default_rom_path && g_psp_default_rom_path[0] != '\0') {
        opts.rom_path = g_psp_default_rom_path;
    }
    dragon::psp::parse_runner_args(argc, argv, opts);

    if (opts.rom_path.empty() || !std::filesystem::exists(opts.rom_path)) {
        const auto local_roms = std::filesystem::current_path() / "roms";
        if (std::filesystem::exists(local_roms)) {
            for (const auto& entry : std::filesystem::directory_iterator(local_roms)) {
                if (entry.is_regular_file()) {
                    const auto ext = entry.path().extension().string();
                    if (ext == ".iso" || ext == ".ISO" || ext == ".cso" || ext == ".CSO" ||
                        ext == ".pbp" || ext == ".PBP" || ext == ".elf" || ext == ".ELF" ||
                        ext == ".prx" || ext == ".PRX") {
                        opts.rom_path = entry.path();
                        break;
                    }
                }
            }
        }
    }

    if (opts.rom_path.empty()) {
        std::fprintf(stderr,
                     "psp_recomp: PSP image (.iso/.cso/.pbp/.elf) not found "
                     "(pass --rom <path> or place in roms/)\n");
        return 1;
    }

    return dragon::psp::run_psp_app(opts, g_psp_recomp_functions, g_psp_recomp_function_count);
}
