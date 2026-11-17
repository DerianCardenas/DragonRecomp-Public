#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "dragon/config.h"
#include "dragon/file_io.h"
#include "dragon/log.h"
#include "nes/cartridge.h"
#include "nesrecomp/disassembler.h"
#include "nesrecomp/emitter.h"

namespace {

void print_usage() {
    std::printf(
        "usage: nesrecomp <game.toml> [--rom <path>] [--out <dir>] [--no-flag-opt] "
        "[--single-file]\n"
        "       nesrecomp --version\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("nesrecomp 0.1.0\n");
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }

    std::filesystem::path config_path;
    std::filesystem::path rom_override;
    std::filesystem::path out_override;
    bool enable_flag_opt = true;
    bool single_file = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--no-flag-opt") {
            enable_flag_opt = false;
        } else if (arg == "--single-file") {
            single_file = true;
        } else if (arg == "--rom" && i + 1 < argc) {
            rom_override = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_override = argv[++i];
        } else if (!arg.empty() && arg[0] != '-' && config_path.empty()) {
            config_path = arg;
        } else {
            std::fprintf(stderr, "nesrecomp: unknown or incomplete option '%s'\n", arg.c_str());
            print_usage();
            return 1;
        }
    }

    if (config_path.empty()) {
        std::fprintf(stderr, "nesrecomp: missing <game.toml> path\n");
        print_usage();
        return 1;
    }

    try {
        dragon::GameConfig config = dragon::load_game_config(config_path);
        if (!rom_override.empty()) {
            config.rom_path = rom_override;
        }
        if (!out_override.empty()) {
            config.output_dir = out_override;
        }

        const dragon::nes::Cartridge cart =
            dragon::nes::load_ines_rom(config.rom_path, config.sha1);
        if (static_cast<int>(cart.mapper) != config.mapper) {
            std::fprintf(stderr,
                         "nesrecomp: mapper mismatch (config specifies %d, ROM header has %u)\n",
                         config.mapper, cart.mapper);
            return 1;
        }

        const dragon::nesrecomp::ProgramCFG cfg =
            dragon::nesrecomp::analyze_prg_rom(cart.prg_rom, config);

        for (uint16_t warn_pc : cfg.unofficial_warnings) {
            std::fprintf(stderr,
                         "[WARN] Unofficial opcode at $%04X treated as data boundary (fallback "
                         "emitted if reached)\n",
                         warn_pc);
        }

        dragon::nesrecomp::EmitOptions emit_opts;
        emit_opts.enable_flag_opt = enable_flag_opt;
        emit_opts.split_every = single_file ? 1000000U : config.split_every;

        const auto written_files =
            dragon::nesrecomp::write_generated_output(cfg, config, config.output_dir, emit_opts);

        size_t total_je_entries = 0;
        for (const auto& tbl : cfg.jump_engine_tables) {
            total_je_entries += tbl.entries.size();
        }

        size_t unresolved_indirect = 0;
        for (const auto& ind : cfg.indirect_jumps) {
            if (!ind.is_jump_engine) {
                ++unresolved_indirect;
            }
        }

        size_t total_funcs = cfg.functions.size();
        size_t total_blocks = cfg.blocks.size();
        size_t total_instrs = cfg.instructions.size();
        for (const auto& bcfg : cfg.bank_cfgs) {
            total_funcs += bcfg.functions.size();
            total_blocks += bcfg.blocks.size();
            total_instrs += bcfg.instructions.size();
        }

        std::printf("nesrecomp: '%s' (%s)\n", config.name.c_str(),
                    config.rom_path.string().c_str());
        std::printf("  SHA-1:              %s\n", cart.sha1.c_str());
        std::printf("  PRG ROM:            %zu bytes (mapper %u)\n", cart.prg_rom.size(),
                    cart.mapper);
        std::printf("  Vectors:            RESET=$%04X NMI=$%04X IRQ=$%04X\n", cfg.reset_addr,
                    cfg.nmi_addr, cfg.irq_addr);
        std::printf("  Functions:          %zu\n", total_funcs);
        std::printf("  Basic blocks:       %zu\n", total_blocks);
        std::printf("  Instructions:       %zu\n", total_instrs);
        std::printf("  JumpEngine tables:  %zu (%zu entries)\n", cfg.jump_engine_tables.size(),
                    total_je_entries);
        std::printf("  Indirect JMPs:      %zu total (%zu JumpEngine, %zu unresolved)\n",
                    cfg.indirect_jumps.size(), cfg.indirect_jumps.size() - unresolved_indirect,
                    unresolved_indirect);
        std::printf("  Output directory:   %s (%zu files written)\n",
                    config.output_dir.string().c_str(), written_files.size());

        // Per-game report (spec 003, §3.5): makes the recompiled-vs-interpreter
        // mode of this game machine-readable, instead of a single flat "done"
        // in docs/MILESTONES.md. "native" means config.interpreter_only is
        // false and 0 fallbacks were observed at --verify time; callers
        // (Studio, MILESTONES tooling) decide the label from these fields.
        char report_buf[1024];
        std::snprintf(report_buf, sizeof(report_buf),
                      "# Generated by nesrecomp. Do not edit; reflects the last recompilation.\n"
                      "[game]\n"
                      "name = \"%s\"\n"
                      "sha1 = \"%s\"\n"
                      "mapper = %u\n"
                      "\n"
                      "[recompiler]\n"
                      "interpreter_only = %s\n"
                      "functions = %zu\n"
                      "basic_blocks = %zu\n"
                      "instructions = %zu\n"
                      "jump_engine_tables = %zu\n"
                      "jump_engine_entries = %zu\n"
                      "indirect_jumps_total = %zu\n"
                      "indirect_jumps_unresolved = %zu\n"
                      "unofficial_opcode_warnings = %zu\n",
                      config.name.c_str(), cart.sha1.c_str(), cart.mapper,
                      config.interpreter_only ? "true" : "false", total_funcs, total_blocks,
                      total_instrs, cfg.jump_engine_tables.size(), total_je_entries,
                      cfg.indirect_jumps.size(), unresolved_indirect,
                      cfg.unofficial_warnings.size());
        dragon::write_text_file(config.output_dir / "report.toml", report_buf);

        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "nesrecomp error: %s\n", ex.what());
        return 1;
    }
}
