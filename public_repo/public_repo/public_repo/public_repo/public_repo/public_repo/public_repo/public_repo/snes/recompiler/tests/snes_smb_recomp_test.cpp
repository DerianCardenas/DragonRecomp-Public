#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "dragon/config.h"
#include "dragon/rom_locator.h"
#include "snes/cartridge.h"
#include "snesrecomp/disassembler.h"
#include "snesrecomp/emitter.h"

int main() {
    const std::filesystem::path toml_path = "snes/games/smb_snes/smb_snes.toml";
    const dragon::GameConfig config = dragon::load_game_config(toml_path);

    const std::filesystem::path rom_path = dragon::resolve_rom_path(config.rom_path, config.sha1);
    if (rom_path.empty()) {
        if (dragon::roms_required()) {
            std::fprintf(stderr,
                         "snes_smb_recomp_test: ROM sha1=%s not found (DRAGON_REQUIRE_ROMS=1); set "
                         "DRAGON_ROM_DIR or place it at %s\n",
                         config.sha1.c_str(), config.rom_path.string().c_str());
            return 1;
        }
        std::printf("snes_smb_recomp_test: ROM sha1=%s not found; skipping.\n",
                    config.sha1.c_str());
        return 77;
    }

    const dragon::snes::Cartridge cart =
        dragon::snes::load_snes_rom(rom_path, config.sha1, config.map);

    assert(cart.map_mode == dragon::snes::MapMode::HiROM);
    assert(cart.has_fastrom);
    assert(cart.title == "SUPER MARIO BROS.SNES");
    assert(cart.native_vectors.nmi == 0xF000U);
    assert(cart.native_vectors.brk == 0xC000U);
    assert(cart.emu_vectors.reset == 0xFFB0U);

    const dragon::snesrecomp::ProgramCFG cfg = dragon::snesrecomp::analyze_snes_rom(cart, config);
    assert(cfg.functions.size() >= 6U);

    dragon::snesrecomp::EmitOptions opts;
    opts.enable_flag_opt = true;
    opts.split_every = config.split_every;

    const std::filesystem::path out_dir =
        std::filesystem::path(DRAGON_BINARY_DIR) / "snes_smb_recomp_test_gen";
    std::filesystem::remove_all(out_dir);

    const auto written = dragon::snesrecomp::write_generated_output(cfg, config, out_dir, opts);
    assert(!written.empty());

    // Compile every generated .c file as C99 with -Wall -Wextra -Wpedantic -Werror
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
        "snes_smb_recomp_test: compiled %zu functions (%zu blocks, %zu instructions) "
        "cleanly as C99\n",
        cfg.functions.size(), cfg.total_blocks, cfg.total_instructions);
    return 0;
}
