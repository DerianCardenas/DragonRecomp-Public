#pragma once

// SNES cartridge loader (LoROM / HiROM / ExHiROM + optional 512-byte SMC header).
// References:
//   https://snes.nesdev.org/wiki/ROM_header
//   https://problemkaputt.de/fullsnes.htm#snescartridgekinternalromheader

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dragon::snes {

enum class MapMode : uint8_t {
    LoROM,
    HiROM,
    ExHiROM,
};

struct InterruptVectors {
    uint16_t cop = 0;
    uint16_t brk = 0;
    uint16_t abort = 0;
    uint16_t nmi = 0;
    uint16_t reset = 0;
    uint16_t irq = 0;
};

struct Cartridge {
    std::string title;
    MapMode map_mode = MapMode::LoROM;
    uint8_t map_mode_byte = 0x20;
    uint8_t rom_type = 0;
    bool has_fastrom = false;
    bool has_smc_header = false;
    uint32_t rom_size_header = 0;
    uint32_t sram_size_bytes = 0;
    bool has_battery = false;
    uint16_t checksum_complement = 0;
    uint16_t checksum = 0;

    InterruptVectors native_vectors{};
    InterruptVectors emu_vectors{};

    std::string sha1;
    std::vector<uint8_t> rom;
};

// Map a 24-bit SNES address (bank:offset) to a byte index in ROM, or -1 if not mapped to ROM.
int64_t map_rom_address(MapMode mode, size_t rom_size, uint8_t bank, uint16_t offset) noexcept;

Cartridge parse_snes_rom(std::span<const uint8_t> file_bytes, std::string_view expected_sha1 = {},
                         std::string_view map_override = {});

Cartridge load_snes_rom(const std::filesystem::path& path, std::string_view expected_sha1 = {},
                        std::string_view map_override = {});

}  // namespace dragon::snes
