#pragma once

// Persistent game library for DragonRecomp Studio (plan: plan-studio-library-ui).
//
// Stores one entry per ROM, keyed by SHA-1, in a single `library.toml`. It holds
// only metadata (title, platform, mode, playtime, cover *path*, per-game
// settings). It never stores ROM bytes nor cover images: covers live as files
// in the user's own profile directory and are never distributed with the
// project.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "studio/rom_detector.h"

namespace dragon::studio {

enum class GameMode { Unknown, Native, Interpreter };

struct GameSettings {
    bool fullscreen = false;
    bool crt_filter = false;
    std::string scale = "int";  // "int" | "smooth"
    int volume = 100;           // 0..100
};

struct GameEntry {
    std::string sha1;
    std::string title;
    std::string platform;  // "nes" | "snes" | "gb" | "gbc" | "gba"
    std::filesystem::path rom_path;
    GameMode mode = GameMode::Unknown;
    int64_t playtime_s = 0;
    int64_t last_played = 0;  // unix seconds, 0 = never
    std::filesystem::path cover_path;
    GameSettings settings;
};

// Platform <-> string used in library.toml and for shelf grouping.
std::string platform_key(RomPlatform p);
const char* game_mode_key(GameMode m);
GameMode game_mode_from_key(const std::string& s);

class GameLibrary {
public:
    explicit GameLibrary(std::filesystem::path file) : file_(std::move(file)) {}

    // %APPDATA%/DragonRecomp/library.toml (Windows) or the XDG equivalent.
    static std::filesystem::path default_path();

    // Loads the file if present. A missing file is not an error (empty library);
    // a malformed one returns false and leaves the library empty.
    bool load();
    bool save() const;

    // Adds (or refreshes the path/title of) the game identified by `info.sha1`.
    // Returns nullptr if `info` is not a valid ROM.
    GameEntry* add_rom(const RomInfo& info);
    bool remove(const std::string& sha1);
    GameEntry* find(const std::string& sha1);
    const GameEntry* find(const std::string& sha1) const;

    void touch_played(const std::string& sha1, int64_t seconds_played, int64_t now_unix);
    void set_mode(const std::string& sha1, GameMode mode);
    void set_cover(const std::string& sha1, const std::filesystem::path& cover);

    // If the stored ROM path no longer exists, searches `search_root` for a file
    // with the same SHA-1 and updates the entry. Returns true if the ROM is
    // reachable afterwards.
    bool relocate_rom(const std::string& sha1, const std::filesystem::path& search_root);

    const std::vector<GameEntry>& entries() const noexcept { return entries_; }

    // Entries of one platform key, sorted by title (case-insensitive) for shelves.
    std::vector<const GameEntry*> shelf(const std::string& platform) const;
    // Most recently played entry, or nullptr when nothing was ever played.
    const GameEntry* last_played() const;

    const std::filesystem::path& file() const noexcept { return file_; }

private:
    std::filesystem::path file_;
    std::vector<GameEntry> entries_;
};

}  // namespace dragon::studio
