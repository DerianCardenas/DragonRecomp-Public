#include "dragon/log.h"

#include <cstdio>

namespace dragon {

namespace {
LogLevel g_min_level = LogLevel::Info;
}  // namespace

void set_log_level(LogLevel min_level) {
    g_min_level = min_level;
}

LogLevel get_log_level() {
    return g_min_level;
}

void log(LogLevel level, std::string_view message) {
    if (static_cast<int>(level) < static_cast<int>(g_min_level)) {
        return;
    }
    static constexpr const char* kNames[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    std::fprintf(stderr, "[%s] %.*s\n", kNames[static_cast<int>(level)],
                 static_cast<int>(message.size()), message.data());
}

}  // namespace dragon
