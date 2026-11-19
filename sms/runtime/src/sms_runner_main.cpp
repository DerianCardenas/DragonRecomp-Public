#include "sms/frontend.h"

int main(int argc, char** argv) {
    dragon::sms::RunnerOptions opts;
    opts.mode = dragon::sms::RunMode::Interpreter;
    int exit_code = 0;
    if (!dragon::sms::parse_runner_args(argc, argv, opts, exit_code)) {
        return exit_code;
    }
    return dragon::sms::run_sms_app(opts);
}
