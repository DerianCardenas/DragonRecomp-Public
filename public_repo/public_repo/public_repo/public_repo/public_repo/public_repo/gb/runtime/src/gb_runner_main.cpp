#include "gb/frontend.h"

int main(int argc, char** argv) {
    dragon::gb::RunnerOptions opts;
    opts.mode = dragon::gb::RunMode::Interpreter;
    int exit_code = 0;
    if (!dragon::gb::parse_runner_args(argc, argv, opts, exit_code)) {
        return exit_code;
    }
    return dragon::gb::run_gb_app(opts);
}
