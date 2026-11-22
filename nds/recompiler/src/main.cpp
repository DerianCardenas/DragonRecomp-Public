#include <cstdio>
#include <filesystem>
#include <string>

#include "dragon/config.h"
#include "dragon/file_io.h"
#include "dragon/sha1.h"
#include "nds/cartridge.h"
#include "ndsrecomp/disassembler.h"
#include "ndsrecomp/emitter.h"

int main(int argc, char** argv) {
    if (argc >= 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        std::printf("ndsrecomp 0.1.0 (DragonRecomp Nintendo DS ARM946E-S Static Recompiler)\n");
        return 0;
    }

    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <game.toml> [--rom <rom.nds>] [--out <dir>]\n", argv[0]);
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
        std::fprintf(stderr, "ndsrecomp: failed to load config: %s\n", e.what());
        return 1;
    }

    const std::filesystem::path rom_path = !rom_override.empty() ? rom_override : config.rom_path;
    const std::filesystem::path out_dir =
        has_cli_out
            ? out_override
            : (!config.output_dir.empty() ? config.output_dir : std::filesystem::path("generated"));

    dragon::nds::Cartridge cart;
    std::string err;
    if (!cart.load_from_file(rom_path, &err)) {
        std::fprintf(stderr, "ndsrecomp: %s\n", err.c_str());
        return 1;
    }

    const std::string sha1 = dragon::sha1_hex(cart.rom());
    const auto cfg = dragon::ndsrecomp::disassemble_nds(cart, config);
    const auto emitted =
        dragon::ndsrecomp::emit_nds_c99(cfg, config, out_dir, sha1, rom_path.string());

    std::printf("ndsrecomp: '%s' (%s)\n", cart.header().title.c_str(), rom_path.string().c_str());
    std::printf("  Game Code:          %s (Maker %s)\n", cart.header().game_code.c_str(),
                cart.header().maker_code.c_str());
    std::printf("  SHA-1:              %s\n", sha1.c_str());
    std::printf("  ARM9 RAM / Entry:   0x%08X (%u bytes) / 0x%08X\n", cart.header().arm9_ram_addr,
                cart.header().arm9_size, cart.header().arm9_entry_addr);
    std::printf("  ARM7 RAM / Entry:   0x%08X (%u bytes) / 0x%08X\n", cart.header().arm7_ram_addr,
                cart.header().arm7_size, cart.header().arm7_entry_addr);
    std::printf("  ARM9 Overlays:      %zu\n", cart.arm9_overlays().size());
    std::printf("  Functions:          %zu\n", emitted.functions_emitted);
    std::printf("  Instructions:       %zu\n", emitted.instructions_emitted);
    std::printf("  Output directory:   %s (%zu files written)\n", out_dir.string().c_str(),
                emitted.files_written);
    return 0;
}
