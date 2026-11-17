#include "dragon/rom_locator.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include "dragon/file_io.h"
#include "dragon/sha1.h"

namespace dragon {

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool extension_allowed(const std::filesystem::path& path,
                       std::initializer_list<std::string_view> extensions) {
    if (extensions.size() == 0) {
        return true;
    }
    const std::string ext = to_lower(path.extension().string());
    for (const std::string_view allowed : extensions) {
        if (ext == allowed) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::filesystem::path find_rom_by_sha1(const std::filesystem::path& root,
                                       std::string_view expected_sha1_hex,
                                       std::initializer_list<std::string_view> extensions) {
    if (expected_sha1_hex.empty()) {
        return {};
    }
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return {};
    }

    const std::string expected_lower = to_lower(expected_sha1_hex);

    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        const std::filesystem::directory_entry& entry = *it;
        std::error_code status_ec;
        if (!entry.is_regular_file(status_ec) || status_ec) {
            continue;
        }
        const std::filesystem::path& candidate = entry.path();
        if (!extension_allowed(candidate, extensions)) {
            continue;
        }

        const std::vector<uint8_t> bytes = try_read_binary_file(candidate);
        if (bytes.empty()) {
            continue;
        }
        if (to_lower(sha1_hex(std::span<const uint8_t>(bytes))) == expected_lower) {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path resolve_rom_path(const std::filesystem::path& declared_path,
                                       std::string_view expected_sha1,
                                       const char* search_root_env) {
    std::error_code ec;
    if (!declared_path.empty() && std::filesystem::exists(declared_path, ec)) {
        return declared_path;
    }

    std::filesystem::path search_root = "roms";
    if (search_root_env != nullptr) {
        if (const char* env_value = std::getenv(search_root_env);
            env_value != nullptr && env_value[0] != '\0') {
            search_root = env_value;
        }
    }

    return find_rom_by_sha1(search_root, expected_sha1,
                            {".nes", ".sfc", ".smc", ".gb", ".gbc", ".gba"});
}

bool roms_required() noexcept {
    const char* value = std::getenv("DRAGON_REQUIRE_ROMS");
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    return !(value[0] == '0' && value[1] == '\0');
}

}  // namespace dragon
