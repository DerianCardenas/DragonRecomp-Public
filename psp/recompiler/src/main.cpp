#include <cstdio>
#include <filesystem>
#include <string>

#include "dragon/config.h"
#include "psp/loader.h"
#include "psprecomp/disassembler.h"
#include "psprecomp/emitter.h"

int main(int argc, char** argv) {
    if (argc >= 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        std::printf(
            "psprecomp 0.1.0 (DragonRecomp PlayStation Portable Allegrex Static Recompiler)\n");
        return 0;
    }

    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <game.toml> [--rom <game.iso|cso|pbp|elf>] [--out <dir>]\n",
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
        std::fprintf(stderr, "psprecomp: failed to load config: %s\n", e.what());
        return 1;
    }

    const std::filesystem::path rom_path = !rom_override.empty() ? rom_override : config.rom_path;
    const std::filesystem::path out_dir =
        has_cli_out
            ? out_override
            : (!config.output_dir.empty() ? config.output_dir : std::filesystem::path("generated"));

    dragon::psp::LoadedPspImage img;
    std::string err;
    if (!dragon::psp::load_psp_image(rom_path, img, err)) {
        std::fprintf(stderr, "psprecomp: %s\n", err.c_str());
        return 1;
    }

    const auto cfg = dragon::psprecomp::disassemble_psp(img, config);
    const auto emitted =
        dragon::psprecomp::emit_psp_c99(cfg, config, out_dir, img.sha1_hex, rom_path.string());

    std::printf("psprecomp: '%s' (%s)\n", img.title.c_str(), rom_path.string().c_str());
    std::printf("  Disc ID:            %s\n", img.disc_id.empty() ? "(none)" : img.disc_id.c_str());
    std::printf("  SHA-1:              %s\n", img.sha1_hex.c_str());
    std::printf("  Entry / GP:         0x%08X / 0x%08X (%s)\n", img.entry_point, img.gp_value,
                img.is_prx ? "PRX" : "ELF");
    std::printf("  NID Stubs Patched:  %zu\n", img.imports.size());
    std::printf("  Functions:          %zu\n", emitted.functions_emitted);
    std::printf("  Instructions:       %zu\n", emitted.instructions_emitted);
    std::printf("  Output directory:   %s (%zu files written)\n", out_dir.string().c_str(),
                emitted.files_written);
    return 0;
}
