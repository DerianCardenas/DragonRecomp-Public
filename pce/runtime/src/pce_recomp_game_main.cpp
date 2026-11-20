#include <cstdio>
#include <filesystem>

#include "dragon/rom_locator.h"
#include "pce/frontend.h"
#include "recomp_functions.h"

#ifndef DRAGON_GAME_TITLE
#define DRAGON_GAME_TITLE "DragonRecomp PC Engine / TurboGrafx-16 Game"
#endif

#ifndef DRAGON_DEFAULT_ROM_PATH
#define DRAGON_DEFAULT_ROM_PATH ""
#endif

#ifndef DRAGON_DEFAULT_ROM_SHA1
#define DRAGON_DEFAULT_ROM_SHA1 ""
#endif

int main(int argc, char** argv) {
    dragon::pce::RunnerOptions opts;
    opts.window_title = DRAGON_GAME_TITLE;
    opts.rom_path = DRAGON_DEFAULT_ROM_PATH;
    opts.expected_sha1 = DRAGON_DEFAULT_ROM_SHA1;
    opts.mode = dragon::pce::RunMode::Recompiled;
    opts.dispatch_table = kRecompDispatchTable;
    opts.dispatch_count = kRecompDispatchTableCount;

    int exit_code = 0;
    if (!dragon::pce::parse_runner_args(argc, argv, opts, exit_code)) {
        return exit_code;
    }

    const std::filesystem::path resolved_rom =
        dragon::resolve_rom_path(opts.rom_path, opts.expected_sha1);
    if (resolved_rom.empty()) {
        std::fprintf(stderr,
                     "No se encontro el ROM original.\n"
                     "Coloca tu copia legal del ROM (sha1 esperado: %s) en la carpeta "
                     "'roms/' junto al ejecutable, o indica su ruta con --rom <archivo>.\n",
                     opts.expected_sha1.c_str());
        return 1;
    }
    opts.rom_path = resolved_rom;

    return dragon::pce::run_pce_app(opts);
}
