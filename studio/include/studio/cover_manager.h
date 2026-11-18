#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL.h>

#include "studio/library.h"

namespace dragon::studio {

class CoverManager {
public:
    CoverManager();
    ~CoverManager();

    // Directory where downloaded and cached covers live (%APPDATA%/DragonRecomp/covers)
    static std::filesystem::path covers_dir();

    // Returns the SDL_Texture* for this game's cover, or nullptr if unavailable.
    // Textures are cached in memory and owned by the CoverManager.
    SDL_Texture* get_texture(SDL_Renderer* renderer, const std::string& sha1,
                             const std::filesystem::path& cover_path);

    // Asynchronously downloads a cover from libretro-thumbnails for the given game.
    // Calls on_complete(success, downloaded_path) on completion.
    bool download_cover_async(
        const GameEntry& entry,
        std::function<void(bool success, const std::filesystem::path& path)> on_complete = nullptr);

    // Copies a local user image file to the covers directory and updates entry.
    std::filesystem::path assign_manual_cover(const std::string& sha1,
                                             const std::filesystem::path& source_path);

    // Invalidate memory cache so next get_texture reloads from disk.
    void invalidate(const std::string& sha1);

    // Checks if a download is in progress for this game.
    bool is_downloading(const std::string& sha1) const;

    // Helper to generate candidate names for libretro-thumbnails.
    static std::vector<std::string> get_candidate_names(const GameEntry& entry);
    static std::string get_system_name(const std::string& platform_key);

    // Checks if a file path has an image extension (.png, .jpg, .jpeg, .bmp, .webp).
    static bool is_image_file(const std::filesystem::path& path);

private:
    bool download_http_to_file(const std::string& url, const std::filesystem::path& dest);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, SDL_Texture*> textures_;
    std::unordered_set<std::string> downloading_;
};

}  // namespace dragon::studio
