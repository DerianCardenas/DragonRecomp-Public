#include "studio/library.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <toml.hpp>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "dragon/rom_locator.h"
#include "dragon/sha1.h"

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

std::string read_text_file(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) {
        return {};
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim_ws(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    return std::string(sv);
}

bool is_hex_sha1(std::string_view s) {
    if (s.size() != 40) {
        return false;
    }
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

std::string extract_quoted_after(const std::string& text, std::string_view marker) {
    const size_t pos = text.find(marker);
    if (pos == std::string::npos) {
        return {};
    }
    const size_t start = pos + marker.size();
    const size_t end = text.find('"', start);
    if (end == std::string::npos) {
        return {};
    }
    return text.substr(start, end - start);
}

bool is_likely_linux_binary(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec) ||
        std::filesystem::file_size(p, ec) == 0) {
        return false;
    }
    const std::string fname = p.filename().string();
    if (fname == "run.sh" || fname == "README.txt" || fname == "LEEME.txt" ||
        fname == "CMakeLists.txt" || fname == "Makefile" || fname == "build.ninja") {
        return false;
    }
    const std::string ext = lower(p.extension().string());
    static const char* const kExcludedExts[] = {
        ".sh",  ".bat", ".desktop", ".txt",  ".md",  ".toml", ".cmake", ".so",
        ".dll", ".png", ".jpg",     ".jpeg", ".bmp", ".webp", ".nes",   ".sfc",
        ".smc", ".gb",  ".gbc",     ".gba",  ".c",   ".cpp",  ".h",     ".hpp",
        ".o",   ".obj", ".exe",     ".a",    ".lib", ".sav",  ".drst",  ".ini",
    };
    for (const char* ex : kExcludedExts) {
        if (ext == ex) {
            return false;
        }
    }
    if (fname.find(".so.") != std::string::npos) {
        return false;
    }

    std::ifstream in(p, std::ios::binary);
    char magic[4] = {0};
    if (in.read(magic, 4)) {
        // ELF magic: 0x7F 'E' 'L' 'F'
        if (static_cast<unsigned char>(magic[0]) == 0x7F && magic[1] == 'E' && magic[2] == 'L' &&
            magic[3] == 'F') {
            return true;
        }
        // Shebang script used as binary in tests or wrappers
        if (magic[0] == '#' && magic[1] == '!') {
            return true;
        }
    }
#if !defined(_WIN32)
    struct stat st{};
    if (stat(p.c_str(), &st) == 0 && (st.st_mode & 0111) != 0) {
        return true;
    }
#endif
    return false;
}

void scan_dir_for_executables(const std::filesystem::path& dir, std::filesystem::path& exec_linux,
                              std::filesystem::path& exec_windows) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return;
    }

    // 1. Check run.sh for target Linux binary name
    if (exec_linux.empty() && std::filesystem::exists(dir / "run.sh", ec)) {
        const std::string sh = read_text_file(dir / "run.sh");
        for (std::string_view marker :
             {std::string_view("exec \"$SCRIPT_DIR/"), std::string_view("exec \"${SCRIPT_DIR}/")}) {
            const std::string bin_name = extract_quoted_after(sh, marker);
            if (!bin_name.empty() && std::filesystem::exists(dir / bin_name, ec) &&
                std::filesystem::is_regular_file(dir / bin_name, ec)) {
                exec_linux = dir / bin_name;
                break;
            }
        }
    }

    // 2. Check run.bat for target Windows binary name
    if (exec_windows.empty() && std::filesystem::exists(dir / "run.bat", ec)) {
        const std::string bat = read_text_file(dir / "run.bat");
        const std::string bin_name = extract_quoted_after(bat, "start \"\" \"%SCRIPT_DIR%");
        if (!bin_name.empty() && std::filesystem::exists(dir / bin_name, ec)) {
            exec_windows = dir / bin_name;
        }
    }

    // 3. Scan directory entries directly
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const auto& p = entry.path();
        const std::string ext = lower(p.extension().string());
        if (ext == ".exe") {
            if (exec_windows.empty()) {
                exec_windows = p;
            }
        } else if (exec_linux.empty() && is_likely_linux_binary(p)) {
            exec_linux = p;
        }
    }
}

RomPlatform parse_platform_hint(const std::string& s) {
    const std::string l = lower(s);
    if (l.find("playstation portable") != std::string::npos ||
        l.find("allegrex") != std::string::npos || l == "psp") {
        return RomPlatform::PSP;
    }
    if (l.find("playstation 1") != std::string::npos || l.find("r3000a") != std::string::npos ||
        l == "ps1" || l == "psx") {
        return RomPlatform::PS1;
    }
    if (l.find("nintendo ds") != std::string::npos || l.find("arm9") != std::string::npos ||
        l == "nds") {
        return RomPlatform::NintendoDS;
    }
    if (l.find("genesis") != std::string::npos || l.find("mega drive") != std::string::npos ||
        l.find("68000") != std::string::npos || l == "md") {
        return RomPlatform::Genesis;
    }
    if (l.find("pc engine") != std::string::npos || l.find("turbografx") != std::string::npos ||
        l.find("huc6280") != std::string::npos || l == "pce") {
        return RomPlatform::PCEngine;
    }
    if (l.find("game gear") != std::string::npos || l == "gg") {
        return RomPlatform::GameGear;
    }
    if (l.find("master system") != std::string::npos || l.find("sms") != std::string::npos ||
        l.find("z80") != std::string::npos) {
        return RomPlatform::MasterSystem;
    }
    if (l.find("game boy advance") != std::string::npos || l.find("gba") != std::string::npos ||
        l.find("arm7tdmi") != std::string::npos) {
        return RomPlatform::GameBoyAdvance;
    }
    if (l.find("game boy color") != std::string::npos || l.find("gbc") != std::string::npos ||
        l.find("cgb") != std::string::npos) {
        return RomPlatform::GameBoyColor;
    }
    if (l.find("game boy") != std::string::npos || l.find("sm83") != std::string::npos ||
        l.find("dmg") != std::string::npos) {
        return RomPlatform::GameBoy;
    }
    if (l.find("snes") != std::string::npos || l.find("super nintendo") != std::string::npos ||
        l.find("65816") != std::string::npos) {
        return RomPlatform::SNES;
    }
    if (l.find("nes") != std::string::npos || l.find("6502") != std::string::npos) {
        return RomPlatform::NES;
    }
    return RomPlatform::Unknown;
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
        case RomPlatform::MasterSystem:
            return "sms";
        case RomPlatform::GameGear:
            return "gg";
        case RomPlatform::PCEngine:
            return "pce";
        case RomPlatform::Genesis:
            return "genesis";
        case RomPlatform::NintendoDS:
            return "nds";
        case RomPlatform::PSP:
            return "psp";
        case RomPlatform::PS1:
            return "ps1";
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
        e.package_dir = path_from_utf8((*t)["package_dir"].value_or(std::string{}));
        e.executable_linux = path_from_utf8((*t)["executable_linux"].value_or(std::string{}));
        e.executable_windows = path_from_utf8((*t)["executable_windows"].value_or(std::string{}));
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
        t.insert("package_dir", path_to_utf8(e.package_dir));
        t.insert("executable_linux", path_to_utf8(e.executable_linux));
        t.insert("executable_windows", path_to_utf8(e.executable_windows));
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
        if (!info.file_path.empty()) {
            existing->rom_path = info.file_path;
        }
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

GameEntry* GameLibrary::add_portable(const RomInfo& info, const std::filesystem::path& package_dir,
                                     const std::filesystem::path& exec_linux,
                                     const std::filesystem::path& exec_windows, GameMode mode) {
    GameEntry* e = add_rom(info);
    if (!e) {
        return nullptr;
    }
    if (!package_dir.empty()) {
        e->package_dir = package_dir;
    }
    if (!exec_linux.empty()) {
        e->executable_linux = exec_linux;
    }
    if (!exec_windows.empty()) {
        e->executable_windows = exec_windows;
    }
    if (mode != GameMode::Unknown) {
        e->mode = mode;
    }
    return e;
}

GameEntry* GameLibrary::add_manual_portable(const std::string& title,
                                            const std::string& platform_str,
                                            const std::filesystem::path& package_dir,
                                            const std::filesystem::path& exec_linux,
                                            const std::filesystem::path& exec_windows,
                                            const std::filesystem::path& rom_path,
                                            const std::string& sha1_hint) {
    std::string trimmed_title = trim_ws(title);
    if (trimmed_title.empty()) {
        return nullptr;
    }

    std::string sha = lower(trim_ws(sha1_hint));
    if (!rom_path.empty()) {
        RomInfo detected = detect_rom(rom_path);
        if (detected.is_valid() && !detected.sha1.empty()) {
            sha = lower(detected.sha1);
        }
    }
    if (sha.empty()) {
        const std::string seed =
            "manual:" + lower(platform_str) + ":" + trimmed_title + ":" + package_dir.string();
        sha = dragon::sha1_hex(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(seed.data()), seed.size()));
    }

    GameEntry* existing = find(sha);
    if (existing) {
        existing->title = trimmed_title;
        if (!platform_str.empty()) {
            existing->platform = lower(platform_str);
        }
        if (!package_dir.empty()) {
            existing->package_dir = package_dir;
        }
        if (!exec_linux.empty()) {
            existing->executable_linux = exec_linux;
        }
        if (!exec_windows.empty()) {
            existing->executable_windows = exec_windows;
        }
        if (!rom_path.empty()) {
            existing->rom_path = rom_path;
        }
        if (existing->mode == GameMode::Unknown) {
            existing->mode = GameMode::Native;
        }
        return existing;
    }

    GameEntry e;
    e.sha1 = sha;
    e.title = trimmed_title;
    e.platform = platform_str.empty() ? "nes" : lower(platform_str);
    e.package_dir = package_dir;
    e.executable_linux = exec_linux;
    e.executable_windows = exec_windows;
    e.rom_path = rom_path;
    e.mode = GameMode::Native;
    entries_.push_back(std::move(e));
    return &entries_.back();
}

PortableInspection GameLibrary::inspect_portable_folder(const std::filesystem::path& folder_path) {
    PortableInspection out;
    out.package_dir = folder_path;

    std::error_code ec;
    if (!std::filesystem::exists(folder_path, ec) ||
        !std::filesystem::is_directory(folder_path, ec)) {
        return out;
    }

    scan_dir_for_executables(folder_path, out.executable_linux, out.executable_windows);

    // If user selected a parent directory containing *_Linux and/or *_Windows subfolders:
    if (out.executable_linux.empty() || out.executable_windows.empty()) {
        for (const auto& sub : std::filesystem::directory_iterator(folder_path, ec)) {
            if (!sub.is_directory(ec)) {
                continue;
            }
            const std::string sub_name = sub.path().filename().string();
            if (sub_name.ends_with("_Linux") || sub_name.ends_with("_Windows")) {
                scan_dir_for_executables(sub.path(), out.executable_linux, out.executable_windows);
            }
        }
    }

    std::filesystem::path metadata_dir = folder_path;
    if (!std::filesystem::exists(metadata_dir / "README.txt", ec) &&
        !std::filesystem::exists(metadata_dir / "CMakeLists.txt", ec)) {
        if (!out.executable_linux.empty() &&
            std::filesystem::exists(out.executable_linux.parent_path() / "README.txt", ec)) {
            metadata_dir = out.executable_linux.parent_path();
        } else if (!out.executable_windows.empty() &&
                   std::filesystem::exists(out.executable_windows.parent_path() / "README.txt",
                                           ec)) {
            metadata_dir = out.executable_windows.parent_path();
        }
    }

    std::string hinted_rom_filename;

    // 1. Parse CMakeLists.txt hints if present
    const std::string cmake_txt = read_text_file(metadata_dir / "CMakeLists.txt");
    if (!cmake_txt.empty()) {
        const std::string t = extract_quoted_after(cmake_txt, "DRAGON_GAME_TITLE=\"");
        if (!t.empty()) {
            out.hinted_title = t;
        }
        const std::string s = extract_quoted_after(cmake_txt, "DRAGON_DEFAULT_ROM_SHA1=\"");
        if (is_hex_sha1(s)) {
            out.hinted_sha1 = lower(s);
        }
        const std::string rp = extract_quoted_after(cmake_txt, "DRAGON_DEFAULT_ROM_PATH=\"");
        if (!rp.empty()) {
            hinted_rom_filename = std::filesystem::path(rp).filename().string();
        }
    }

    // 2. Parse roms/LEEME.txt hints if present
    const std::string leeme_txt = read_text_file(metadata_dir / "roms" / "LEEME.txt");
    if (!leeme_txt.empty()) {
        if (out.hinted_title.empty()) {
            const std::string t =
                extract_quoted_after(leeme_txt, "Este paquete NO incluye el ROM de \"");
            if (!t.empty()) {
                out.hinted_title = t;
            }
        }
        if (out.hinted_sha1.empty()) {
            std::istringstream iss(leeme_txt);
            std::string line;
            while (std::getline(iss, line)) {
                const std::string cand = trim_ws(line);
                if (is_hex_sha1(cand)) {
                    out.hinted_sha1 = lower(cand);
                    break;
                }
            }
        }
    }

    // 3. Parse README.txt hints if present
    const std::string readme_txt = read_text_file(metadata_dir / "README.txt");
    if (!readme_txt.empty()) {
        std::istringstream iss(readme_txt);
        std::string line;
        while (std::getline(iss, line)) {
            const std::string trimmed = trim_ws(line);
            if (out.hinted_title.empty()) {
                const size_t dash_pos = trimmed.find(" \xE2\x80\x94 Recompilacion Nativa");
                if (dash_pos != std::string::npos && dash_pos > 0) {
                    out.hinted_title = trim_ws(trimmed.substr(0, dash_pos));
                }
            }
            if (trimmed.rfind("Plataforma original:", 0) == 0) {
                const std::string plat_val =
                    trim_ws(trimmed.substr(std::string("Plataforma original:").size()));
                out.hinted_platform = parse_platform_hint(plat_val);
            } else if (out.hinted_sha1.empty() && trimmed.rfind("Hash SHA-1:", 0) == 0) {
                const std::string sha_val =
                    trim_ws(trimmed.substr(std::string("Hash SHA-1:").size()));
                if (is_hex_sha1(sha_val)) {
                    out.hinted_sha1 = lower(sha_val);
                }
            }
        }
    }

    // Fallback title from folder name if still empty
    if (out.hinted_title.empty()) {
        std::string folder_name = folder_path.filename().string();
        if (folder_name.empty() && folder_path.has_parent_path()) {
            folder_name = folder_path.parent_path().filename().string();
        }
        if (folder_name.ends_with("_Linux") && folder_name.size() > 6) {
            folder_name.resize(folder_name.size() - 6);
        } else if (folder_name.ends_with("_Windows") && folder_name.size() > 8) {
            folder_name.resize(folder_name.size() - 8);
        }
        for (char& c : folder_name) {
            if (c == '_') {
                c = ' ';
            }
        }
        out.hinted_title = trim_ws(folder_name);
    }

    // 4. Search the `roms` directory inside the portable folder for the ROM
    std::vector<std::filesystem::path> roms_dirs = {folder_path / "roms"};
    if (metadata_dir != folder_path) {
        roms_dirs.push_back(metadata_dir / "roms");
    }

    for (const auto& roms_dir : roms_dirs) {
        if (!std::filesystem::exists(roms_dir, ec) ||
            !std::filesystem::is_directory(roms_dir, ec)) {
            continue;
        }

        // a) Check exact hinted ROM filename inside `roms/`
        if (!hinted_rom_filename.empty() &&
            std::filesystem::exists(roms_dir / hinted_rom_filename, ec)) {
            RomInfo info = detect_rom(roms_dir / hinted_rom_filename);
            if (info.is_valid()) {
                if (!out.hinted_title.empty()) {
                    info.clean_name = out.hinted_title;
                }
                out.found_rom_path = roms_dir / hinted_rom_filename;
                out.rom_info = std::move(info);
                return out;
            }
        }

        // b) Check by hinted SHA-1 inside `roms/`
        if (!out.hinted_sha1.empty()) {
            const auto by_sha = dragon::find_rom_by_sha1(
                roms_dir, out.hinted_sha1,
                {".nes", ".sfc", ".smc", ".gb", ".gbc", ".gba", ".sms", ".gg", ".pce", ".sgx"});
            if (!by_sha.empty()) {
                RomInfo info = detect_rom(by_sha);
                if (info.is_valid()) {
                    if (!out.hinted_title.empty()) {
                        info.clean_name = out.hinted_title;
                    }
                    out.found_rom_path = by_sha;
                    out.rom_info = std::move(info);
                    return out;
                }
            }
        }

        // c) Check any valid ROM file inside `roms/`
        for (const auto& entry : std::filesystem::directory_iterator(roms_dir, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const std::string fname = entry.path().filename().string();
            if (fname == "LEEME.txt" || fname == "README.txt") {
                continue;
            }
            RomInfo info = detect_rom(entry.path());
            if (info.is_valid()) {
                if (out.hinted_sha1.empty() || lower(info.sha1) == lower(out.hinted_sha1)) {
                    if (!out.hinted_title.empty()) {
                        info.clean_name = out.hinted_title;
                    }
                    out.found_rom_path = entry.path();
                    out.rom_info = std::move(info);
                    return out;
                }
            }
        }
    }

    return out;
}

std::filesystem::path GameLibrary::resolve_host_executable(const GameEntry& entry) {
    std::error_code ec;
#if defined(_WIN32)
    if (!entry.executable_windows.empty()) {
        if (entry.executable_windows.is_absolute() &&
            std::filesystem::exists(entry.executable_windows, ec)) {
            return entry.executable_windows;
        }
        if (!entry.package_dir.empty() &&
            std::filesystem::exists(entry.package_dir / entry.executable_windows, ec)) {
            return entry.package_dir / entry.executable_windows;
        }
    }
    if (!entry.package_dir.empty()) {
        const auto insp = inspect_portable_folder(entry.package_dir);
        if (!insp.executable_windows.empty() &&
            std::filesystem::exists(insp.executable_windows, ec)) {
            return insp.executable_windows;
        }
    }
#else
    if (!entry.executable_linux.empty()) {
        if (entry.executable_linux.is_absolute() &&
            std::filesystem::exists(entry.executable_linux, ec)) {
            return entry.executable_linux;
        }
        if (!entry.package_dir.empty() &&
            std::filesystem::exists(entry.package_dir / entry.executable_linux, ec)) {
            return entry.package_dir / entry.executable_linux;
        }
    }
    if (!entry.package_dir.empty()) {
        const auto insp = inspect_portable_folder(entry.package_dir);
        if (!insp.executable_linux.empty() && std::filesystem::exists(insp.executable_linux, ec)) {
            return insp.executable_linux;
        }
    }
#endif
    return {};
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
    const auto found =
        find_rom_by_sha1(search_root, e->sha1, {".nes", ".sfc", ".smc", ".gb", ".gbc", ".gba"});
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
