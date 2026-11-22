#include <cstdio>
#include <filesystem>
#include <string>

#include "dragon/config.h"
#include "ps1/loader.h"
#include "psxrecomp/disassembler.h"
#include "psxrecomp/emitter.h"

int main(int argc, char** argv) {
    if (argc >= 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        std::printf("psxrecomp 0.1.0 (DragonRecomp PlayStation 1 R3000A Static Recompiler)\n");
        return 0;
    }

    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <game.toml> [--rom <game.cue|bin|iso|psexe>] [--out <dir>]\n",
                     argv[0]);
        return 1;
    }

    std::filesystem::path toml_path = argv[1];
    std::filesystem::path rom_override;
    std::filesystem::path out_override;
    bool has_cli_out = false;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--rom" && i + 1 < argc) {
            rom_override = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_override = argv[++i];
            has_cli_out = true;
        }
    }

    dragon::GameConfig config{};
    try {
        config = dragon::load_game_config(toml_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "psxrecomp: failed to load config: %s\n", e.what());
        return 1;
    }

    const std::filesystem::path rom_path = !rom_override.empty() ? rom_override : config.rom_path;
    const std::filesystem::path out_dir =
        has_cli_out
            ? out_override
            : (!config.output_dir.empty() ? config.output_dir : std::filesystem::path("generated"));

    dragon::ps1::LoadedPs1Image img;
    std::string err;
    if (!dragon::ps1::load_ps1_image(rom_path, img, err)) {
        std::fprintf(stderr, "psxrecomp: %s\n", err.c_str());
        return 1;
    }

    const auto cfg = dragon::psxrecomp::disassemble_ps1(img, config);
    const auto emitted =
        dragon::psxrecomp::emit_ps1_c99(cfg, config, out_dir, img.sha1_hex, rom_path.string());

    std::printf("psxrecomp: '%s' (%s)\n", img.title.c_str(), rom_path.string().c_str());
    std::printf("  Boot ID:            %s\n",
                img.boot_id.empty() ? "(PS-X EXE)" : img.boot_id.c_str());
    std::printf("  SHA-1:              %s\n", img.sha1_hex.c_str());
    std::printf("  Load / Entry:       0x%08X (%u bytes) / 0x%08X\n", img.load_addr, img.load_size,
                img.initial_pc);
    std::printf("  Functions:          %zu\n", emitted.functions_emitted);
    std::printf("  Instructions:       %zu\n", emitted.instructions_emitted);
    std::printf("  Output directory:   %s (%zu files written)\n", out_dir.string().c_str(),
                emitted.files_written);
    return 0;
}
