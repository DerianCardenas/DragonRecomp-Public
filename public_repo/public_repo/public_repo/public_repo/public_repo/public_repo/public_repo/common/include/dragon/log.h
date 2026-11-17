#pragma once

#include <string_view>

namespace dragon {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_log_level(LogLevel min_level);
LogLevel get_log_level();

void log(LogLevel level, std::string_view message);

inline void log_debug(std::string_view message) {
    log(LogLevel::Debug, message);
}
inline void log_info(std::string_view message) {
    log(LogLevel::Info, message);
}
inline void log_warn(std::string_view message) {
    log(LogLevel::Warn, message);
}
inline void log_error(std::string_view message) {
    log(LogLevel::Error, message);
}

}  // namespace dragon
