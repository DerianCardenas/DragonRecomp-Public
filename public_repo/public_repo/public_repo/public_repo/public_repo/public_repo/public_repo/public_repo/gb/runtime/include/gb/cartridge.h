#pragma once

// Game Boy & Game Boy Color Cartridge loader & header parser.
// Reference: docs/GB_SPEC.md §1.1, https://gbdev.io/pandocs/The_Cartridge_Header.html

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dragon::gb {

enum class MbcType : uint8_t {
    RomOnly,
    Mbc1,
    Mbc2,
    Mbc3,
    Mbc5,
    Unsupported,
};

enum class CgbSupport : uint8_t {
    DmgOnly,        // Header $0143 != 0x80 and != 0xC0
    CgbCompatible,  // Header $0143 == 0x80 (runs on DMG and CGB)
    CgbOnly,        // Header $0143 == 0xC0 (requires CGB)
};

struct Cartridge {
    std::string title;
    std::string sha1;
    MbcType mbc = MbcType::RomOnly;
    CgbSupport cgb_mode = CgbSupport::DmgOnly;
    bool has_ram = false;
    bool has_battery = false;
    bool has_rtc = false;
    bool has_rumble = false;

    uint32_t rom_size = 0;
    uint32_t ram_size = 0;
    uint8_t header_checksum = 0;

    std::vector<uint8_t> rom;
};

Cartridge parse_gb_rom(std::span<const uint8_t> file_bytes, std::string_view expected_sha1 = {});
Cartridge load_gb_rom(const std::filesystem::path& path, std::string_view expected_sha1 = {});

}  // namespace dragon::gb
