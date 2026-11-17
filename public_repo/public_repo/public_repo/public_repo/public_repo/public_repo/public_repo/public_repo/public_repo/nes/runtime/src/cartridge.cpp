#include "nes/cartridge.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

#include "dragon/file_io.h"
#include "dragon/sha1.h"

namespace dragon::nes {

Cartridge parse_ines_rom(std::span<const uint8_t> file_bytes, std::string_view expected_sha1) {
    if (file_bytes.size() < 16) {
        throw std::runtime_error("ROM file too small for 16-byte iNES header");
    }
    if (file_bytes[0] != 'N' || file_bytes[1] != 'E' || file_bytes[2] != 'S' ||
        file_bytes[3] != 0x1A) {
        throw std::runtime_error("Invalid iNES magic (expected 'NES\\x1A')");
    }

    Cartridge cart;
    cart.sha1 = dragon::sha1_hex(file_bytes);

    if (!expected_sha1.empty()) {
        std::string exp(expected_sha1);
        std::transform(exp.begin(), exp.end(), exp.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (cart.sha1 != exp) {
            throw std::runtime_error("ROM SHA-1 mismatch: expected " + exp + ", got " + cart.sha1);
        }
    }

    const uint8_t flags6 = file_bytes[6];
    const uint8_t flags7 = file_bytes[7];

    cart.has_battery = (flags6 & 0x02U) != 0;
    cart.has_trainer = (flags6 & 0x04U) != 0;
    if (flags6 & 0x08U) {
        cart.mirroring = Mirroring::FourScreen;
    } else if (flags6 & 0x01U) {
        cart.mirroring = Mirroring::Vertical;
    } else {
        cart.mirroring = Mirroring::Horizontal;
    }

    cart.is_nes2 = ((flags7 & 0x0CU) == 0x08U);

    size_t prg_size = 0;
    size_t chr_size = 0;

    if (cart.is_nes2) {
        cart.mapper = static_cast<uint16_t>((flags6 >> 4U) | (flags7 & 0xF0U) |
                                            (static_cast<uint16_t>(file_bytes[8] & 0x0FU) << 8U));
        cart.submapper = static_cast<uint8_t>(file_bytes[8] >> 4U);

        const uint16_t prg_msb = static_cast<uint16_t>(file_bytes[9] & 0x0FU);
        const uint16_t chr_msb = static_cast<uint16_t>(file_bytes[9] >> 4U);
        prg_size = static_cast<size_t>((prg_msb << 8U) | file_bytes[4]) * 16384ULL;
        chr_size = static_cast<size_t>((chr_msb << 8U) | file_bytes[5]) * 8192ULL;
    } else {
        // Check if bytes 12..15 are zero before trusting upper mapper nybble (archaic iNES check).
        bool clean_header = (file_bytes[12] == 0 && file_bytes[13] == 0 && file_bytes[14] == 0 &&
                             file_bytes[15] == 0);
        cart.mapper = static_cast<uint16_t>((flags6 >> 4U) | (clean_header ? (flags7 & 0xF0U) : 0));
        prg_size = static_cast<size_t>(file_bytes[4]) * 16384ULL;
        chr_size = static_cast<size_t>(file_bytes[5]) * 8192ULL;
    }

    cart.is_vs_system = ((flags7 & 0x01U) != 0U) || (cart.mapper == 99U);

    size_t offset = 16;
    if (cart.has_trainer) {
        if (offset + 512 > file_bytes.size()) {
            throw std::runtime_error("ROM truncated inside 512-byte trainer");
        }
        cart.trainer.assign(file_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                            file_bytes.begin() + static_cast<std::ptrdiff_t>(offset + 512));
        offset += 512;
    }

    if (offset + prg_size > file_bytes.size()) {
        throw std::runtime_error("ROM truncated inside PRG ROM data");
    }
    cart.prg_rom.assign(file_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                        file_bytes.begin() + static_cast<std::ptrdiff_t>(offset + prg_size));
    offset += prg_size;

    if (chr_size > 0) {
        if (offset + chr_size > file_bytes.size()) {
            throw std::runtime_error("ROM truncated inside CHR ROM data");
        }
        cart.chr_rom.assign(file_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                            file_bytes.begin() + static_cast<std::ptrdiff_t>(offset + chr_size));
    }

    return cart;
}

Cartridge load_ines_rom(const std::filesystem::path& path, std::string_view expected_sha1) {
    const std::vector<uint8_t> bytes = dragon::read_binary_file(path);
    return parse_ines_rom(bytes, expected_sha1);
}

}  // namespace dragon::nes
