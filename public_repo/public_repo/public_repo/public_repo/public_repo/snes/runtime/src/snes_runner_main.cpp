#include "snes/frontend.h"

int main(int argc, char** argv) {
    dragon::snes::RunnerOptions opts;
    opts.mode = dragon::snes::RunMode::Interpreter;
    int exit_code = 0;
    if (!dragon::snes::parse_runner_args(argc, argv, opts, exit_code)) {
        return exit_code;
    }
    return dragon::snes::run_snes_app(opts);
}
