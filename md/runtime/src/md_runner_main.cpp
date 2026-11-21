#include "md/frontend.h"

int main(int argc, char** argv) {
    dragon::md::RunnerOptions opts;
    int exit_code = 0;
    if (!dragon::md::parse_runner_args(argc, argv, opts, exit_code)) {
        return exit_code;
    }
    return dragon::md::run_md_app(opts);
}
