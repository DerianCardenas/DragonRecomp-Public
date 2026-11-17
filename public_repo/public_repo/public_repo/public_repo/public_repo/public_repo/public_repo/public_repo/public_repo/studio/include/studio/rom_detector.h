#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace dragon::studio {

enum class RomPlatform {
    Unknown,
    NES,
    SNES,
    GameBoy,
    GameBoyColor,
    GameBoyAdvance,
};

struct RomInfo {
    RomPlatform platform = RomPlatform::Unknown;
    std::filesystem::path file_path;
    std::string internal_title;
    std::string clean_name;
    std::string sha1;
    size_t file_size = 0;

    // NES details
    int mapper = 0;
    size_t prg_size_kb = 0;
    size_t chr_size_kb = 0;
    bool battery = false;

    // SNES details
    std::string map_mode_str;  // "lorom", "hirom", "exhirom"
    uint16_t checksum = 0;
    bool fast_rom = false;
    bool smc_header = false;

    // Game Boy / Color details
    std::string mbc_type_str;
    size_t rom_size_kb = 0;
    size_t ram_size_kb = 0;
    bool cgb_flag = false;

    // Game Boy Advance details
    std::string game_code;
    std::string save_type_str;

    bool is_valid() const noexcept { return platform != RomPlatform::Unknown; }

    std::string platform_name() const {
        switch (platform) {
            case RomPlatform::NES:
                return "NES (6502)";
            case RomPlatform::SNES:
                return "SNES (65816)";
            case RomPlatform::GameBoy:
                return "Game Boy (SM83)";
            case RomPlatform::GameBoyColor:
                return "Game Boy Color (SM83)";
            case RomPlatform::GameBoyAdvance:
                return "Game Boy Advance (ARM7TDMI)";
            default:
                return "Desconocida";
        }
    }
};

RomInfo detect_rom(const std::filesystem::path& path);

}  // namespace dragon::studio
