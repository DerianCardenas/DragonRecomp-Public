#pragma once

#include <filesystem>
#include <string>

#include "studio/pipeline.h"

namespace dragon::studio {

enum class AccentColor {
    Orange = 0,  // #F07C1E (default)
    Cyan = 1,    // #1EC8D4
    Purple = 2,  // #9B59B6
    Green = 3,   // #2ECC71
};

const char* accent_color_key(AccentColor c);
AccentColor accent_color_from_key(const std::string& key);

const char* opt_level_flag(CompileOptLevel level);
CompileOptLevel opt_level_from_flag(const std::string& flag);

struct StudioSettings {
    unsigned int max_compile_threads = Pipeline::kDefaultCompileThreads;  // 1..16
    CompileOptLevel opt_level = CompileOptLevel::Fast;                    // -O0 | -O1 | -O2 | -O3
    std::filesystem::path export_directory;  // Default folder for portable exports
    bool dark_mode = true;
    AccentColor accent_color = AccentColor::Orange;
    bool auto_save_state = true;
    bool default_crt_filter = false;
};

class SettingsStore {
public:
    static std::filesystem::path default_path();
    static StudioSettings defaults();

    static bool load(StudioSettings& out, const std::filesystem::path& path = default_path());
    static bool save(const StudioSettings& settings,
                     const std::filesystem::path& path = default_path());
};

}  // namespace dragon::studio
