#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dragon::gba {

enum class SaveType {
    None,
    Sram,
    Flash64K,
    Flash128K,
    Eeprom,
};

struct Cartridge {
    std::vector<uint8_t> rom;
    std::string title;
    std::string game_code;
    std::string maker_code;
    std::string sha1;
    uint32_t rom_size = 0;
    uint8_t header_checksum = 0;
    bool checksum_valid = false;
    SaveType save_type = SaveType::Sram;
};

Cartridge load_gba_rom(const std::filesystem::path& path, std::string_view expected_sha1 = "");
Cartridge parse_gba_rom_bytes(std::vector<uint8_t> data, std::string_view fallback_title = "");
const char* save_type_to_string(SaveType type) noexcept;

}  // namespace dragon::gba
