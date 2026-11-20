#include "pce/frontend.h"

int main(int argc, char** argv) {
    dragon::pce::RunnerOptions opts;
    opts.mode = dragon::pce::RunMode::Interpreter;
    int exit_code = 0;
    if (!dragon::pce::parse_runner_args(argc, argv, opts, exit_code)) {
        return exit_code;
    }
    return dragon::pce::run_pce_app(opts);
}
