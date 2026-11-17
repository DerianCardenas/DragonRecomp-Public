#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "dragon/config.h"
#include "dragon/rom_locator.h"
#include "nes/cartridge.h"
#include "nesrecomp/disassembler.h"
#include "nesrecomp/emitter.h"

int main() {
    const std::filesystem::path toml_path = "nes/games/smb/smb.toml";
    const dragon::GameConfig config = dragon::load_game_config(toml_path);

    // Resolves the ROM against DRAGON_ROM_DIR by SHA-1 when it is not at the
    // declared `roms/nes/smb.nes` path (spec 003, ADR-007).
    const std::filesystem::path rom_path = dragon::resolve_rom_path(config.rom_path, config.sha1);
    if (rom_path.empty()) {
        if (dragon::roms_required()) {
            std::fprintf(stderr,
                         "nes_smb_recomp_test: ROM sha1=%s not found (DRAGON_REQUIRE_ROMS=1); set "
                         "DRAGON_ROM_DIR or place it at %s\n",
                         config.sha1.c_str(), config.rom_path.string().c_str());
            return 1;
        }
        std::printf("nes_smb_recomp_test: ROM sha1=%s not found; skipping.\n", config.sha1.c_str());
        return 77;
    }

    const dragon::nes::Cartridge cart = dragon::nes::load_ines_rom(rom_path, config.sha1);
    const dragon::nesrecomp::ProgramCFG cfg =
        dragon::nesrecomp::analyze_prg_rom(cart.prg_rom, config);

    assert(cfg.functions.size() >= 450);
    assert(cfg.jump_engine_tables.size() == 18);
    assert(cfg.unofficial_warnings.empty());

    size_t unresolved_indirect = 0;
    for (const auto& ind : cfg.indirect_jumps) {
        if (!ind.is_jump_engine) {
            ++unresolved_indirect;
        }
    }
    assert(unresolved_indirect == 0);

    dragon::nesrecomp::EmitOptions opts;
    opts.enable_flag_opt = true;
    opts.split_every = config.split_every;

    const std::filesystem::path out_dir =
        std::filesystem::path(DRAGON_BINARY_DIR) / "smb_recomp_test_gen";
    std::filesystem::remove_all(out_dir);

    const auto written = dragon::nesrecomp::write_generated_output(cfg, config, out_dir, opts);
    assert(!written.empty());

    // Compile every generated .c file as C99 with -Wall -Wextra -Wpedantic -Werror.
    for (const auto& path : written) {
        if (path.extension() != ".c") {
            continue;
        }
        const std::filesystem::path obj_path = out_dir / (path.stem().string() + ".o");
        const std::string cmd = std::string(DRAGON_C_COMPILER) +
                                " -std=c99 -Wall -Wextra -Wpedantic -Werror -O1 -c"
                                " -I\"" +
                                std::string(DRAGON_RUNTIME_INCLUDE_DIR) + "\" -I\"" +
                                out_dir.string() + "\" \"" + path.string() + "\" -o \"" +
                                obj_path.string() + "\"";
        const int rc = std::system(cmd.c_str());
        assert(rc == 0);
    }

    std::printf(
        "nes_smb_recomp_test: compiled %zu functions (%zu JumpEngine tables, %zu unresolved "
        "indirect jumps) cleanly as C99\n",
        cfg.functions.size(), cfg.jump_engine_tables.size(), unresolved_indirect);
    return 0;
}
