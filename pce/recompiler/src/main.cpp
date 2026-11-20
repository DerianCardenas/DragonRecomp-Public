#include <cstdio>
#include <filesystem>
#include <string>

#include "dragon/config.h"
#include "pce/cartridge.h"
#include "pcerecomp/disassembler.h"
#include "pcerecomp/emitter.h"

int main(int argc, char** argv) {
    std::filesystem::path toml_path;
    std::filesystem::path rom_override;
    std::filesystem::path out_dir;
    bool single_file = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            std::printf("pcerecomp 0.1.0 (DragonRecomp HuC6280 PCE/TG-16 static recompiler)\n");
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            std::printf("Usage: pcerecomp <game.toml> [--rom <file.pce>] [--out <dir>] [--single-file]\n");
            return 0;
        } else if (arg == "--rom" && i + 1 < argc) {
            rom_override = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--single-file") {
            single_file = true;
        } else if (!arg.empty() && arg[0] != '-' && toml_path.empty()) {
            toml_path = arg;
        }
    }

    if (toml_path.empty()) {
        std::fprintf(stderr, "error: missing game TOML path\n");
        return 1;
    }

    dragon::GameConfig cfg;
    try {
        cfg = dragon::load_game_config(toml_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error loading TOML: %s\n", e.what());
        return 1;
    }

    const std::filesystem::path rom_path = rom_override.empty() ? cfg.rom_path : rom_override;
    if (rom_path.empty()) {
        std::fprintf(stderr, "error: no ROM path specified in TOML or --rom\n");
        return 1;
    }

    dragon::pce::Cartridge cart;
    std::string err;
    if (!cart.load_from_file(rom_path, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    const auto disasm = dragon::pcerecomp::disassemble_rom(cart, cfg);

    dragon::pcerecomp::EmitOptions emit_opts;
    emit_opts.output_dir = out_dir.empty() ? (cfg.output_dir.empty() ? "generated" : cfg.output_dir) : out_dir;
    emit_opts.single_file = single_file;
    emit_opts.split_every = cfg.split_every > 0U ? cfg.split_every : 200U;

    const auto res = dragon::pcerecomp::emit_c99(disasm, emit_opts);
    if (!res.success) {
        std::fprintf(stderr, "error: %s\n", res.error.c_str());
        return 1;
    }

    std::printf("pcerecomp: emitted %zu functions (%zu instructions) into %zu file(s) at %s\n",
                res.functions_emitted, disasm.total_instructions, res.files_written,
                emit_opts.output_dir.string().c_str());
    return 0;
}
