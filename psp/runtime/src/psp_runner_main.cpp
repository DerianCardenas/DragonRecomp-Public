#include <cstdio>

#include "psp/frontend.h"

int main(int argc, char** argv) {
    dragon::psp::RunnerOptions opts{};
    dragon::psp::parse_runner_args(argc, argv, opts);
    if (opts.rom_path.empty()) {
        std::fprintf(
            stderr,
            "Usage: psp_runner <game.iso|cso|pbp|elf> [--recomp|--interp] "
            "[--headlessframes N] [--dump-ppm out.ppm] [--scale N] [--fullscreen] [--crt]\n");
        return 1;
    }
    return dragon::psp::run_psp_app(opts);
}
