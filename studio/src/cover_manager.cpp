#include "studio/cover_manager.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <urlmon.h>
#include <windows.h>
#include <wininet.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace dragon::studio {

namespace {

std::string url_encode(std::string_view s) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            escaped << c;
        } else if (c == ' ') {
            escaped << "%20";
        } else if (c == '(') {
            escaped << "%28";
        } else if (c == ')') {
            escaped << "%29";
        } else if (c == ',') {
            escaped << "%2C";
        } else {
            escaped << '%' << std::setw(2) << static_cast<int>(static_cast<unsigned char>(c));
        }
    }
    return escaped.str();
}

}  // namespace

CoverManager::CoverManager() = default;

CoverManager::~CoverManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [sha, tex] : textures_) {
        if (tex) {
            SDL_DestroyTexture(tex);
        }
    }
    textures_.clear();
}

std::filesystem::path CoverManager::covers_dir() {
    return GameLibrary::default_path().parent_path() / "covers";
}

bool CoverManager::is_image_file(const std::filesystem::path& path) {
    const std::string ext = path.extension().string();
    std::string lower_ext;
    for (char c : ext)
        lower_ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return (lower_ext == ".png" || lower_ext == ".jpg" || lower_ext == ".jpeg" ||
            lower_ext == ".bmp" || lower_ext == ".webp");
}

std::string CoverManager::get_system_name(const std::string& platform_key) {
    if (platform_key == "nes")
        return "Nintendo - Nintendo Entertainment System";
    if (platform_key == "snes")
        return "Nintendo - Super Nintendo Entertainment System";
    if (platform_key == "gb")
        return "Nintendo - Game Boy";
    if (platform_key == "gbc")
        return "Nintendo - Game Boy Color";
    if (platform_key == "gba")
        return "Nintendo - Game Boy Advance";
    if (platform_key == "sms")
        return "Sega - Master System - Mark III";
    if (platform_key == "gg")
        return "Sega - Game Gear";
    if (platform_key == "pce")
        return "NEC - PC Engine - TurboGrafx 16";
    if (platform_key == "genesis")
        return "Sega - Mega Drive - Genesis";
    return "";
}

std::vector<std::string> CoverManager::get_candidate_names(const GameEntry& entry) {
    std::vector<std::string> list;
    auto add_unique = [&](const std::string& name) {
        if (!name.empty() && std::find(list.begin(), list.end(), name) == list.end()) {
            list.push_back(name);
        }
    };

    if (!entry.rom_path.empty()) {
        add_unique(entry.rom_path.stem().string());
    }
    add_unique(entry.title);

    const char* regions[] = {
        " (World)",  " (USA)",   " (World) (Rev A)", " (USA, Europe)",
        " (Europe)", " (Japan)", " (Japan) (En)",
    };
    for (const char* reg : regions) {
        add_unique(entry.title + reg);
        if (!entry.rom_path.empty()) {
            add_unique(entry.rom_path.stem().string() + reg);
        }
    }

    std::string upper_title = entry.title;
    for (char& c : upper_title)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (upper_title.find("POKEMON") != std::string::npos ||
        upper_title.find("POKÉMON") != std::string::npos) {
        if (upper_title.find("FIRE") != std::string::npos) {
            add_unique("Pokemon - FireRed Version (USA)");
            add_unique("Pokemon - FireRed Version (USA, Europe)");
        } else if (upper_title.find("LEAF") != std::string::npos) {
            add_unique("Pokemon - LeafGreen Version (USA)");
            add_unique("Pokemon - LeafGreen Version (USA, Europe)");
        } else if (upper_title.find("EMER") != std::string::npos) {
            add_unique("Pokemon - Emerald Version (USA, Europe)");
        } else if (upper_title.find("RUBY") != std::string::npos) {
            add_unique("Pokemon - Ruby Version (USA, Europe)");
        } else if (upper_title.find("SAPP") != std::string::npos) {
            add_unique("Pokemon - Sapphire Version (USA, Europe)");
        } else if (upper_title.find("RED") != std::string::npos) {
            add_unique("Pokemon - Red Version (USA, Europe)");
        } else if (upper_title.find("BLUE") != std::string::npos) {
            add_unique("Pokemon - Blue Version (USA, Europe)");
        } else if (upper_title.find("YELLOW") != std::string::npos) {
            add_unique("Pokemon - Yellow Version - Special Pikachu Edition (USA, Europe)");
        } else if (upper_title.find("GOLD") != std::string::npos) {
            add_unique("Pokemon - Gold Version (USA, Europe)");
        } else if (upper_title.find("SILVER") != std::string::npos) {
            add_unique("Pokemon - Silver Version (USA, Europe)");
        } else if (upper_title.find("CRYSTAL") != std::string::npos) {
            add_unique("Pokemon - Crystal Version (USA, Europe)");
        }
    }
    if (upper_title.find("TETRIS") != std::string::npos) {
        add_unique("Tetris (World) (Rev A)");
        add_unique("Tetris (Japan) (En)");
        add_unique("Tetris (USA)");
    }
    if (upper_title.find("SUPER MARIO BROS") != std::string::npos || upper_title == "SMB") {
        add_unique("Super Mario Bros. (World)");
        add_unique("Super Mario Bros. (USA)");
        add_unique("Super Mario Bros. (Europe)");
        add_unique("Super Mario Bros. 2 (USA)");
        add_unique("Super Mario Bros. 3 (USA)");
    }
    if (upper_title.find("SUPER MARIO WORLD") != std::string::npos || upper_title == "SMW") {
        add_unique("Super Mario World (USA)");
        add_unique("Super Mario World (Europe)");
    }
    if (upper_title.find("ZELDA") != std::string::npos) {
        add_unique("Legend of Zelda, The - A Link to the Past (USA)");
        add_unique("Legend of Zelda, The (USA)");
        add_unique("Legend of Zelda, The - Link's Awakening (USA, Europe)");
        add_unique("Legend of Zelda, The - The Minish Cap (USA)");
    }
    if (upper_title.find("DONKEY KONG") != std::string::npos || upper_title == "DK") {
        add_unique("Donkey Kong (World) (Rev A)");
        add_unique("Donkey Kong (USA)");
        add_unique("Donkey Kong Country (USA)");
    }

    return list;
}

bool CoverManager::download_http_to_file(const std::string& url,
                                         const std::filesystem::path& dest) {
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);

#if defined(_WIN32)
    std::wstring wide_url(url.begin(), url.end());
    std::wstring wide_dest = dest.wstring();
    DeleteUrlCacheEntryW(wide_url.c_str());

    HRESULT hr = URLDownloadToFileW(NULL, wide_url.c_str(), wide_dest.c_str(), 0, NULL);
    if (SUCCEEDED(hr)) {
        if (std::filesystem::exists(dest) && std::filesystem::file_size(dest, ec) > 0) {
            return true;
        }
    }
    // Fallback to curl.exe
    std::string cmd = "curl.exe -s -f -L -o \"" + dest.string() + "\" \"" + url + "\"";
    int ret = std::system(cmd.c_str());
    return (ret == 0) && std::filesystem::exists(dest) &&
           (std::filesystem::file_size(dest, ec) > 0);
#else
    std::string cmd = "curl -s -f -L -o \"" + dest.string() + "\" \"" + url + "\" >/dev/null 2>&1";
    int ret = std::system(cmd.c_str());
    return (ret == 0) && std::filesystem::exists(dest) &&
           (std::filesystem::file_size(dest, ec) > 0);
#endif
}

bool CoverManager::download_cover_async(
    const GameEntry& entry,
    std::function<void(bool success, const std::filesystem::path& path)> on_complete) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (downloading_.count(entry.sha1) > 0) {
            return false;
        }
        downloading_.insert(entry.sha1);
    }

    std::thread([this, entry, on_complete]() {
        const std::string system = get_system_name(entry.platform);
        if (system.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            downloading_.erase(entry.sha1);
            if (on_complete)
                on_complete(false, {});
            return;
        }

        const auto candidates = get_candidate_names(entry);
        const auto target_file = covers_dir() / (entry.sha1 + ".png");

        bool success = false;
        for (const auto& cand : candidates) {
            std::string url =
                "https://raw.githubusercontent.com/libretro/libretro-thumbnails/master/" +
                url_encode(system) + "/Named_Boxarts/" + url_encode(cand) + ".png";
            if (download_http_to_file(url, target_file)) {
                success = true;
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            downloading_.erase(entry.sha1);
            if (success) {
                auto it = textures_.find(entry.sha1);
                if (it != textures_.end()) {
                    if (it->second) {
                        SDL_DestroyTexture(it->second);
                    }
                    textures_.erase(it);
                }
            }
        }

        if (on_complete) {
            on_complete(success, success ? target_file : std::filesystem::path{});
        }
    }).detach();

    return true;
}

std::filesystem::path CoverManager::assign_manual_cover(const std::string& sha1,
                                                        const std::filesystem::path& source_path) {
    if (!std::filesystem::exists(source_path) || !is_image_file(source_path)) {
        return {};
    }
    const auto dir = covers_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::string ext = source_path.extension().string();
    if (ext.empty())
        ext = ".png";
    const auto target = dir / (sha1 + ext);

    std::filesystem::copy_file(source_path, target,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return {};
    }

    invalidate(sha1);
    return target;
}

void CoverManager::invalidate(const std::string& sha1) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(sha1);
    if (it != textures_.end()) {
        if (it->second) {
            SDL_DestroyTexture(it->second);
        }
        textures_.erase(it);
    }
}

bool CoverManager::is_downloading(const std::string& sha1) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return downloading_.count(sha1) > 0;
}

SDL_Texture* CoverManager::get_texture(SDL_Renderer* renderer, const std::string& sha1,
                                       const std::filesystem::path& cover_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = textures_.find(sha1);
    if (it != textures_.end()) {
        return it->second;
    }

    std::filesystem::path file_to_load;
    std::error_code ec;
    if (!cover_path.empty() && std::filesystem::exists(cover_path, ec)) {
        file_to_load = cover_path;
    } else {
        const auto cdir = covers_dir();
        const std::string exts[] = {".png", ".jpg", ".jpeg", ".bmp", ".webp"};
        for (const auto& ext : exts) {
            const auto candidate = cdir / (sha1 + ext);
            if (std::filesystem::exists(candidate, ec)) {
                file_to_load = candidate;
                break;
            }
        }
    }

    if (file_to_load.empty()) {
        textures_[sha1] = nullptr;
        return nullptr;
    }

    std::ifstream stream(file_to_load, std::ios::binary | std::ios::ate);
    if (!stream) {
        textures_[sha1] = nullptr;
        return nullptr;
    }
    const std::streamsize size = stream.tellg();
    if (size <= 0) {
        textures_[sha1] = nullptr;
        return nullptr;
    }
    stream.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(buffer.data()), size)) {
        textures_[sha1] = nullptr;
        return nullptr;
    }

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels =
        stbi_load_from_memory(buffer.data(), static_cast<int>(buffer.size()), &w, &h, &channels, 4);
    if (!pixels) {
        textures_[sha1] = nullptr;
        return nullptr;
    }

    SDL_Texture* tex =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) {
        SDL_UpdateTexture(tex, nullptr, pixels, w * 4);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    }
    stbi_image_free(pixels);

    textures_[sha1] = tex;
    return tex;
}

}  // namespace dragon::studio
