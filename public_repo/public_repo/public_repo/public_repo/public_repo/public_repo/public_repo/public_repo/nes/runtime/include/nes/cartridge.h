#pragma once

// iNES / NES 2.0 cartridge loader.
// Reference: https://www.nesdev.org/wiki/INES and https://www.nesdev.org/wiki/NES_2.0

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dragon::nes {

enum class Mirroring : uint8_t {
    Horizontal,
    Vertical,
    FourScreen,
    OneScreenLower,
    OneScreenUpper,
};

struct Cartridge {
    uint16_t mapper = 0;
    uint8_t submapper = 0;
    Mirroring mirroring = Mirroring::Horizontal;
    bool has_battery = false;
    bool has_trainer = false;
    bool is_nes2 = false;
    bool is_vs_system = false;

    std::string sha1;
    std::vector<uint8_t> trainer;
    std::vector<uint8_t> prg_rom;
    std::vector<uint8_t> chr_rom;
};

Cartridge parse_ines_rom(std::span<const uint8_t> file_bytes, std::string_view expected_sha1 = {});

Cartridge load_ines_rom(const std::filesystem::path& path, std::string_view expected_sha1 = {});

}  // namespace dragon::nes
