#include "studio/library.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <toml.hpp>

#include "dragon/rom_locator.h"

namespace dragon::studio {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// toml++ takes UTF-8; std::filesystem::path::string() is the ANSI code page on
// Windows, so go through u8string to keep non-ASCII ROM paths intact.
std::string path_to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path path_from_utf8(const std::string& s) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

}  // namespace

std::string platform_key(RomPlatform p) {
    switch (p) {
        case RomPlatform::NES:
            return "nes";
        case RomPlatform::SNES:
            return "snes";
        case RomPlatform::GameBoy:
            return "gb";
        case RomPlatform::GameBoyColor:
            return "gbc";
        case RomPlatform::GameBoyAdvance:
            return "gba";
        default:
            return "unknown";
    }
}

const char* game_mode_key(GameMode m) {
    switch (m) {
        case GameMode::Native:
            return "native";
        case GameMode::Interpreter:
            return "interpreter";
        default:
            return "unknown";
    }
}

GameMode game_mode_from_key(const std::string& s) {
    if (s == "native") {
        return GameMode::Native;
    }
    if (s == "interpreter") {
        return GameMode::Interpreter;
    }
    return GameMode::Unknown;
}

std::filesystem::path GameLibrary::default_path() {
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata && appdata[0] != '\0') {
        return std::filesystem::path(appdata) / "DragonRecomp" / "library.toml";
    }
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "dragonrecomp" / "library.toml";
    }
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "dragonrecomp" / "library.toml";
    }
#endif
    return std::filesystem::current_path() / "library.toml";
}

bool GameLibrary::load() {
    entries_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec)) {
        return true;
    }
    std::ifstream in(file_, std::ios::binary);
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

    const auto* games = root["game"].as_array();
    if (!games) {
        return true;
    }
    for (const auto& node : *games) {
        const auto* t = node.as_table();
        if (!t) {
            continue;
        }
        GameEntry e;
        e.sha1 = lower((*t)["sha1"].value_or(std::string{}));
        if (e.sha1.empty()) {
            continue;  // an entry without identity is unusable
        }
        e.title = (*t)["title"].value_or(std::string{});
        e.platform = (*t)["platform"].value_or(std::string("unknown"));
        e.rom_path = path_from_utf8((*t)["rom_path"].value_or(std::string{}));
        e.mode = game_mode_from_key((*t)["mode"].value_or(std::string{}));
        e.playtime_s = (*t)["playtime_s"].value_or(int64_t{0});
        e.last_played = (*t)["last_played"].value_or(int64_t{0});
        e.cover_path = path_from_utf8((*t)["cover"].value_or(std::string{}));
        e.settings.fullscreen = (*t)["fullscreen"].value_or(false);
        e.settings.crt_filter = (*t)["crt"].value_or(false);
        e.settings.scale = (*t)["scale"].value_or(std::string("int"));
        e.settings.volume =
            std::clamp(static_cast<int>((*t)["volume"].value_or(int64_t{100})), 0, 100);
        if (find(e.sha1) == nullptr) {
            entries_.push_back(std::move(e));
        }
    }
    return true;
}

bool GameLibrary::save() const {
    toml::table root;
    toml::array games;
    for (const auto& e : entries_) {
        toml::table t;
        t.insert("sha1", e.sha1);
        t.insert("title", e.title);
        t.insert("platform", e.platform);
        t.insert("rom_path", path_to_utf8(e.rom_path));
        t.insert("mode", game_mode_key(e.mode));
        t.insert("playtime_s", e.playtime_s);
        t.insert("last_played", e.last_played);
        t.insert("cover", path_to_utf8(e.cover_path));
        t.insert("fullscreen", e.settings.fullscreen);
        t.insert("crt", e.settings.crt_filter);
        t.insert("scale", e.settings.scale);
        t.insert("volume", static_cast<int64_t>(e.settings.volume));
        games.push_back(std::move(t));
    }
    root.insert("game", std::move(games));

    std::error_code ec;
    if (file_.has_parent_path()) {
        std::filesystem::create_directories(file_.parent_path(), ec);
    }
    // Write to a temp file then rename, so a crash never leaves a half-written
    // library (the user would lose their whole collection).
    const auto tmp = std::filesystem::path(file_.string() + ".tmp");
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
    std::filesystem::rename(tmp, file_, ec);
    if (ec) {
        std::filesystem::remove(file_, ec);
        ec.clear();
        std::filesystem::rename(tmp, file_, ec);
    }
    return !ec;
}

GameEntry* GameLibrary::add_rom(const RomInfo& info) {
    if (!info.is_valid() || info.sha1.empty()) {
        return nullptr;
    }
    const std::string sha = lower(info.sha1);
    GameEntry* existing = find(sha);
    if (existing) {
        existing->rom_path = info.file_path;
        if (existing->title.empty()) {
            existing->title = info.clean_name;
        }
        return existing;
    }
    GameEntry e;
    e.sha1 = sha;
    e.title = info.clean_name.empty() ? info.internal_title : info.clean_name;
    e.platform = platform_key(info.platform);
    e.rom_path = info.file_path;
    entries_.push_back(std::move(e));
    return &entries_.back();
}

bool GameLibrary::remove(const std::string& sha1) {
    const std::string sha = lower(sha1);
    const auto it = std::remove_if(entries_.begin(), entries_.end(),
                                   [&](const GameEntry& e) { return e.sha1 == sha; });
    if (it == entries_.end()) {
        return false;
    }
    entries_.erase(it, entries_.end());
    return true;
}

GameEntry* GameLibrary::find(const std::string& sha1) {
    const std::string sha = lower(sha1);
    for (auto& e : entries_) {
        if (e.sha1 == sha) {
            return &e;
        }
    }
    return nullptr;
}

const GameEntry* GameLibrary::find(const std::string& sha1) const {
    return const_cast<GameLibrary*>(this)->find(sha1);
}

void GameLibrary::touch_played(const std::string& sha1, int64_t seconds_played, int64_t now_unix) {
    if (GameEntry* e = find(sha1)) {
        e->playtime_s += std::max<int64_t>(0, seconds_played);
        e->last_played = now_unix;
    }
}

void GameLibrary::set_mode(const std::string& sha1, GameMode mode) {
    if (GameEntry* e = find(sha1)) {
        e->mode = mode;
    }
}

void GameLibrary::set_cover(const std::string& sha1, const std::filesystem::path& cover) {
    if (GameEntry* e = find(sha1)) {
        e->cover_path = cover;
    }
}

bool GameLibrary::relocate_rom(const std::string& sha1, const std::filesystem::path& search_root) {
    GameEntry* e = find(sha1);
    if (!e) {
        return false;
    }
    std::error_code ec;
    if (!e->rom_path.empty() && std::filesystem::exists(e->rom_path, ec)) {
        return true;
    }
    const auto found = find_rom_by_sha1(
        search_root, e->sha1, {".nes", ".sfc", ".smc", ".gb", ".gbc", ".gba"});
    if (found.empty()) {
        return false;
    }
    e->rom_path = found;
    return true;
}

std::vector<const GameEntry*> GameLibrary::shelf(const std::string& platform) const {
    std::vector<const GameEntry*> out;
    for (const auto& e : entries_) {
        if (e.platform == platform) {
            out.push_back(&e);
        }
    }
    std::sort(out.begin(), out.end(), [](const GameEntry* a, const GameEntry* b) {
        return lower(a->title) < lower(b->title);
    });
    return out;
}

const GameEntry* GameLibrary::last_played() const {
    const GameEntry* best = nullptr;
    for (const auto& e : entries_) {
        if (e.last_played > 0 && (!best || e.last_played > best->last_played)) {
            best = &e;
        }
    }
    return best;
}

}  // namespace dragon::studio
