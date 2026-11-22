#include <cstdio>

#include "ps1/frontend.h"

int main(int argc, char** argv) {
    dragon::ps1::RunnerOptions opts{};
    dragon::ps1::parse_runner_args(argc, argv, opts);
    if (opts.rom_path.empty()) {
        std::fprintf(
            stderr,
            "Usage: ps1_runner <game.cue|bin|iso|psexe|exe> [--recomp|--interp] "
            "[--headlessframes N] [--dump-ppm out.ppm] [--scale N] [--fullscreen] [--crt]\n");
        return 1;
    }
    return dragon::ps1::run_ps1_app(opts);
}
