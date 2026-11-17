#include "gba/frontend.h"

int main(int argc, char** argv) {
    dragon::gba::RunnerOptions opts;
    opts.mode = dragon::gba::RunMode::Interpreter;
    int exit_code = 0;
    if (!dragon::gba::parse_runner_args(argc, argv, opts, exit_code)) {
        return exit_code;
    }
    return dragon::gba::run_gba_app(opts);
}
