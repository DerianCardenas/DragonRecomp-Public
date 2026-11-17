#include "nes/frontend.h"

int main(int argc, char** argv) {
    dragon::nes::RunnerOptions opts;
    opts.mode = dragon::nes::RunMode::Interpreter;
    int exit_code = 0;
    if (!dragon::nes::parse_runner_args(argc, argv, opts, exit_code)) {
        return exit_code;
    }
    return dragon::nes::run_nes_app(opts);
}
