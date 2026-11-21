#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "dragon/config.h"
#include "md/cartridge.h"
#include "mdrecomp/disassembler.h"
#include "mdrecomp/emitter.h"

int main(int argc, char** argv) {
    if (argc >= 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        std::printf("mdrecomp 0.1.0 (DragonRecomp Sega Genesis / Mega Drive M68000 -> C99)\n");
        return 0;
    }

    std::filesystem::path rom_path;
    std::filesystem::path config_path;
    std::filesystem::path output_dir = "generated";
    bool has_cli_out = false;
    bool interpreter_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--rom" || arg == "-r") && i + 1 < argc) {
            rom_path = argv[++i];
        } else if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if ((arg == "--out" || arg == "-o") && i + 1 < argc) {
            output_dir = argv[++i];
            has_cli_out = true;
        } else if (arg == "--interpreter-only") {
            interpreter_only = true;
        } else if (arg == "--single-file") {
            // Accepted for Studio CLI compatibility
        } else if (!arg.empty() && arg[0] != '-') {
            const std::filesystem::path p(arg);
            if (p.extension() == ".toml" && config_path.empty()) {
                config_path = p;
            } else if (rom_path.empty()) {
                rom_path = p;
            }
        }
    }

    std::vector<uint32_t> extra_entries;
    std::string game_name;
    if (!config_path.empty() && std::filesystem::exists(config_path)) {
        try {
            const auto cfg = dragon::load_game_config(config_path);
            if (rom_path.empty() && !cfg.rom_path.empty()) {
                rom_path = cfg.rom_path;
            }
            if (!has_cli_out && !cfg.output_dir.empty()) {
                output_dir = cfg.output_dir;
            }
            if (cfg.interpreter_only) {
                interpreter_only = true;
            }
            game_name = cfg.name;
            for (const auto& ep : cfg.entry_points) {
                extra_entries.push_back(ep.addr);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "mdrecomp: config error: %s\n", e.what());
            return 1;
        }
    }

    if (rom_path.empty()) {
        std::fprintf(stderr, "usage: mdrecomp [--config game.toml] [--rom game.md] [--out dir]\n");
        return 1;
    }

    dragon::md::Cartridge cart;
    std::string err;
    if (!cart.load_from_file(rom_path, &err)) {
        std::fprintf(stderr, "mdrecomp: %s\n", err.c_str());
        return 1;
    }

    if (game_name.empty()) {
        game_name = !cart.header().overseas_title.empty() ? cart.header().overseas_title
                                                          : rom_path.stem().string();
    }

    const auto prog_cfg = dragon::mdrecomp::build_cfg(cart, extra_entries);

    dragon::mdrecomp::EmitOptions emit_opts;
    emit_opts.output_dir = output_dir;
    emit_opts.rom_path = rom_path;
    emit_opts.game_name = game_name;
    emit_opts.interpreter_only = interpreter_only;

    const auto res = dragon::mdrecomp::emit_c99(cart, prog_cfg, emit_opts);
    if (!res.success) {
        std::fprintf(stderr, "mdrecomp: emit failed: %s\n", res.error_message.c_str());
        return 1;
    }

    std::printf("mdrecomp: '%s' (%s)\n", game_name.c_str(), rom_path.string().c_str());
    std::printf("  SHA-1:        %s\n", cart.sha1().c_str());
    std::printf("  ROM size:     %u bytes\n", cart.rom_size());
    std::printf("  Functions:    %zu\n", interpreter_only ? 0U : prog_cfg.functions.size());
    std::printf("  Basic blocks: %zu\n", interpreter_only ? 0U : prog_cfg.total_blocks);
    std::printf("  Instructions: %zu\n", interpreter_only ? 0U : prog_cfg.total_instructions);
    std::printf("  Output dir:   %s (%zu files written)\n", output_dir.string().c_str(),
                res.files_written);
    return 0;
}
