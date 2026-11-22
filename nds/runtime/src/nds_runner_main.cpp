#include "nds/frontend.h"

int main(int argc, char** argv) {
    dragon::nds::RunnerOptions opts{};
    if (!dragon::nds::parse_runner_args(argc, argv, opts)) {
        return 1;
    }
    return dragon::nds::run_nds_app(opts);
}
