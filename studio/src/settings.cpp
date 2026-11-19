#include "studio/settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <toml.hpp>

namespace dragon::studio {

namespace {

std::string path_to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path path_from_utf8(const std::string& s) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

}  // namespace

const char* accent_color_key(AccentColor c) {
    switch (c) {
        case AccentColor::Cyan:
            return "cyan";
        case AccentColor::Purple:
            return "purple";
        case AccentColor::Green:
            return "green";
        case AccentColor::Orange:
        default:
            return "orange";
    }
}

AccentColor accent_color_from_key(const std::string& key) {
    if (key == "cyan")
        return AccentColor::Cyan;
    if (key == "purple")
        return AccentColor::Purple;
    if (key == "green")
        return AccentColor::Green;
    return AccentColor::Orange;
}

const char* opt_level_flag(CompileOptLevel level) {
    switch (level) {
        case CompileOptLevel::None:
            return "-O0";
        case CompileOptLevel::Fast:
            return "-O1";
        case CompileOptLevel::Balanced:
            return "-O2";
        case CompileOptLevel::Aggressive:
            return "-O3";
    }
    return "-O1";
}

CompileOptLevel opt_level_from_flag(const std::string& flag) {
    if (flag == "-O0")
        return CompileOptLevel::None;
    if (flag == "-O2")
        return CompileOptLevel::Balanced;
    if (flag == "-O3")
        return CompileOptLevel::Aggressive;
    return CompileOptLevel::Fast;
}

std::filesystem::path SettingsStore::default_path() {
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata && appdata[0] != '\0') {
        return std::filesystem::path(appdata) / "DragonRecomp" / "settings.toml";
    }
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "dragonrecomp" / "settings.toml";
    }
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "dragonrecomp" / "settings.toml";
    }
#endif
    return std::filesystem::current_path() / "settings.toml";
}

StudioSettings SettingsStore::defaults() {
    StudioSettings s;
    s.max_compile_threads = Pipeline::kDefaultCompileThreads;
    s.opt_level = CompileOptLevel::Fast;
    s.export_directory = std::filesystem::current_path() / "ExportedGames";
    s.dark_mode = true;
    s.accent_color = AccentColor::Orange;
    s.auto_save_state = true;
    s.default_crt_filter = false;
    return s;
}

bool SettingsStore::load(StudioSettings& out, const std::filesystem::path& path) {
    out = defaults();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return true;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::stringstream buf;
    buf << in.rdbuf();

    toml::table root;
    try {
        root = toml::parse(buf.str());
    } catch (const toml::parse_error&) {
        return false;
    }

    if (const auto* comp = root["compilation"].as_table()) {
        const int64_t t =
            (*comp)["max_threads"].value_or(static_cast<int64_t>(Pipeline::kDefaultCompileThreads));
        out.max_compile_threads = static_cast<unsigned int>(std::clamp<int64_t>(t, 1, 16));
        out.opt_level = opt_level_from_flag((*comp)["opt_level"].value_or(std::string("-O1")));
    }

    if (const auto* paths = root["paths"].as_table()) {
        const std::string exp = (*paths)["export_directory"].value_or(std::string{});
        if (!exp.empty()) {
            out.export_directory = path_from_utf8(exp);
        }
    }

    if (const auto* app = root["appearance"].as_table()) {
        out.dark_mode = (*app)["dark_mode"].value_or(true);
        out.accent_color =
            accent_color_from_key((*app)["accent_color"].value_or(std::string("orange")));
    }

    if (const auto* game = root["game"].as_table()) {
        out.auto_save_state = (*game)["auto_save_state"].value_or(true);
        out.default_crt_filter = (*game)["default_crt_filter"].value_or(false);
    }

    return true;
}

bool SettingsStore::save(const StudioSettings& s, const std::filesystem::path& path) {
    toml::table root;

    toml::table comp;
    comp.insert("max_threads", static_cast<int64_t>(std::clamp(s.max_compile_threads, 1U, 16U)));
    comp.insert("opt_level", opt_level_flag(s.opt_level));
    root.insert("compilation", std::move(comp));

    toml::table paths;
    paths.insert("export_directory", path_to_utf8(s.export_directory));
    root.insert("paths", std::move(paths));

    toml::table app;
    app.insert("dark_mode", s.dark_mode);
    app.insert("accent_color", accent_color_key(s.accent_color));
    root.insert("appearance", std::move(app));

    toml::table game;
    game.insert("auto_save_state", s.auto_save_state);
    game.insert("default_crt_filter", s.default_crt_filter);
    root.insert("game", std::move(game));

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    const auto tmp = std::filesystem::path(path.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << root;
        if (!out) {
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
    return !ec;
}

}  // namespace dragon::studio
