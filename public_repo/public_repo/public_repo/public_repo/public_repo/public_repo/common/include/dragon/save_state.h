#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace dragon {

struct SaveStateHeader {
    char magic[4] = {'D', 'R', 'S', 'T'};   // Dragon Recomp STate
    char system[4] = {'N', 'O', 'N', 'E'};  // "NES ", "SNES", "GB  "
    uint32_t version = 1;
    uint32_t payload_size = 0;

    bool is_valid(std::string_view expected_sys, uint32_t expected_ver = 1) const noexcept {
        if (std::memcmp(magic, "DRST", 4) != 0) {
            return false;
        }
        if (expected_sys.size() == 4 && std::memcmp(system, expected_sys.data(), 4) != 0) {
            return false;
        }
        return version == expected_ver;
    }
};

inline std::filesystem::path slot_state_path(const std::filesystem::path& rom_path, int slot) {
    std::filesystem::path p = rom_path;
    p.replace_extension("");
    return p.string() + ".state" + std::to_string(slot);
}

}  // namespace dragon
