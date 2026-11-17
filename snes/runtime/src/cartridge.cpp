#include "snes/cartridge.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

#include "dragon/file_io.h"
#include "dragon/sha1.h"

namespace dragon::snes {

namespace {

uint16_t read_u16_le(std::span<const uint8_t> buf, size_t offset) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(buf[offset + 1]) << 8U) | buf[offset]);
}

bool is_common_boot_opcode(uint8_t op) noexcept {
    switch (op) {
        case 0x78:  // SEI
        case 0x18:  // CLC
        case 0x4C:  // JMP abs
        case 0x5C:  // JML long
        case 0x20:  // JSR abs
        case 0x22:  // JSL long
        case 0xA2:  // LDX #imm
        case 0xA9:  // LDA #imm
        case 0xC2:  // REP #imm
        case 0xE2:  // SEP #imm
        case 0xD8:  // CLD
        case 0x80:  // BRA
        case 0x82:  // BRL
        case 0x9C:  // STZ abs
        case 0xEA:  // NOP
            return true;
        default:
            return false;
    }
}

int score_header(std::span<const uint8_t> rom, size_t header_offset, MapMode mode) noexcept {
    if (rom.size() < header_offset + 0x40U) {
        return -1000;
    }

    int score = 0;
    const uint8_t map_byte = rom[header_offset + 0x15U];
    const uint8_t rom_size_code = rom[header_offset + 0x17U];
    const uint8_t sram_size_code = rom[header_offset + 0x18U];
    const uint16_t comp = read_u16_le(rom, header_offset + 0x1CU);
    const uint16_t chk = read_u16_le(rom, header_offset + 0x1EU);
    const uint16_t reset_vec = read_u16_le(rom, header_offset + 0x3CU);

    if (reset_vec < 0x8000U) {
        return -500;
    }
    score += 2;

    if (static_cast<uint16_t>(comp + chk) == 0xFFFFU && chk != 0U) {
        score += 4;
    }

    const uint8_t masked_mode = static_cast<uint8_t>(map_byte & 0xEFU);
    if (mode == MapMode::LoROM) {
        if (masked_mode == 0x20U || masked_mode == 0x23U) {
            score += 3;
        } else if ((map_byte & 0x01U) == 0U && (map_byte & 0x20U) != 0U) {
            score += 2;
        }
    } else if (mode == MapMode::HiROM) {
        if (masked_mode == 0x21U) {
            score += 3;
        } else if ((map_byte & 0x01U) == 1U && (map_byte & 0x20U) != 0U) {
            score += 2;
        }
    } else if (mode == MapMode::ExHiROM) {
        if (masked_mode == 0x25U) {
            score += 4;
        }
    }

    if (rom_size_code >= 0x05U && rom_size_code <= 0x0DU) {
        score += 1;
    }
    if (sram_size_code <= 0x08U) {
        score += 1;
    }

    int printable = 0;
    for (size_t i = 0; i < 21U; ++i) {
        const uint8_t c = rom[header_offset + i];
        if (c >= 0x20U && c <= 0x7EU) {
            ++printable;
        }
    }
    if (printable >= 15) {
        score += 2;
    }

    const int64_t reset_rom_off = map_rom_address(mode, rom.size(), 0x00U, reset_vec);
    if (reset_rom_off >= 0 && static_cast<size_t>(reset_rom_off) < rom.size()) {
        if (is_common_boot_opcode(rom[static_cast<size_t>(reset_rom_off)])) {
            score += 2;
        }
    }

    return score;
}

std::string normalize_lower(std::string_view sv) {
    std::string s(sv);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

int64_t map_rom_address(MapMode mode, size_t rom_size, uint8_t bank, uint16_t offset) noexcept {
    if (rom_size == 0U) {
        return -1;
    }
    switch (mode) {
        case MapMode::LoROM: {
            if (offset >= 0x8000U) {
                if (bank == 0x7EU || bank == 0x7FU) {
                    return -1;
                }
                const size_t idx = (static_cast<size_t>(bank & 0x7FU) * 0x8000U) +
                                   static_cast<size_t>(offset & 0x7FFFU);
                return static_cast<int64_t>(idx % rom_size);
            }
            if ((bank >= 0x40U && bank <= 0x6FU) || (bank >= 0xC0U && bank <= 0xEFU)) {
                const size_t idx = (static_cast<size_t>(bank & 0x7FU) * 0x8000U) +
                                   static_cast<size_t>(offset & 0x7FFFU);
                return static_cast<int64_t>(idx % rom_size);
            }
            return -1;
        }
        case MapMode::HiROM: {
            if ((bank >= 0x40U && bank <= 0x7DU) || bank >= 0xC0U) {
                const size_t idx =
                    (static_cast<size_t>(bank & 0x3FU) << 16U) | static_cast<size_t>(offset);
                return static_cast<int64_t>(idx % rom_size);
            }
            if (offset >= 0x8000U && (bank <= 0x3FU || (bank >= 0x80U && bank <= 0xBFU))) {
                const size_t idx =
                    (static_cast<size_t>(bank & 0x3FU) << 16U) | static_cast<size_t>(offset);
                return static_cast<int64_t>(idx % rom_size);
            }
            return -1;
        }
        case MapMode::ExHiROM: {
            if (bank >= 0xC0U || (bank >= 0x80U && bank <= 0xBFU && offset >= 0x8000U)) {
                const size_t idx =
                    (static_cast<size_t>(bank & 0x3FU) << 16U) | static_cast<size_t>(offset);
                return static_cast<int64_t>(idx % rom_size);
            }
            if ((bank >= 0x40U && bank <= 0x7DU) || (bank <= 0x3FU && offset >= 0x8000U)) {
                const size_t idx = 0x400000U + ((static_cast<size_t>(bank & 0x3FU) << 16U) |
                                                static_cast<size_t>(offset));
                return static_cast<int64_t>(idx % rom_size);
            }
            return -1;
        }
    }
    return -1;
}

Cartridge parse_snes_rom(std::span<const uint8_t> file_bytes, std::string_view expected_sha1,
                         std::string_view map_override) {
    if (file_bytes.size() < 0x8000U) {
        throw std::runtime_error("SNES ROM image is smaller than 32 KiB");
    }

    Cartridge cart;
    cart.sha1 = sha1_hex(file_bytes);

    std::span<const uint8_t> rom_span = file_bytes;
    if ((file_bytes.size() % 1024U) == 512U && file_bytes.size() >= 0x8200U) {
        cart.has_smc_header = true;
        rom_span = file_bytes.subspan(512U);
    } else if ((file_bytes.size() % 0x8000U) == 1024U && file_bytes.size() >= 0x8400U) {
        const int raw_best = std::max({score_header(file_bytes, 0x7FC0U, MapMode::LoROM),
                                       score_header(file_bytes, 0xFFC0U, MapMode::HiROM),
                                       score_header(file_bytes, 0x40FFC0U, MapMode::ExHiROM)});
        const auto stripped1024 = file_bytes.subspan(1024U);
        const int strip_best = std::max({score_header(stripped1024, 0x7FC0U, MapMode::LoROM),
                                         score_header(stripped1024, 0xFFC0U, MapMode::HiROM),
                                         score_header(stripped1024, 0x40FFC0U, MapMode::ExHiROM)});
        if (strip_best > raw_best) {
            cart.has_smc_header = true;
            rom_span = stripped1024;
        }
    }

    if (!expected_sha1.empty()) {
        const std::string exp = normalize_lower(expected_sha1);
        const std::string stripped_sha1 = cart.has_smc_header ? sha1_hex(rom_span) : cart.sha1;
        if (cart.sha1 != exp && stripped_sha1 != exp) {
            throw std::runtime_error("SNES ROM SHA-1 mismatch: expected " + exp + ", got " +
                                     cart.sha1);
        }
    }

    cart.rom.assign(rom_span.begin(), rom_span.end());

    const int score_lorom = score_header(rom_span, 0x7FC0U, MapMode::LoROM);
    const int score_hirom = score_header(rom_span, 0xFFC0U, MapMode::HiROM);
    const int score_exhirom = score_header(rom_span, 0x40FFC0U, MapMode::ExHiROM);

    MapMode selected_mode = MapMode::LoROM;
    if (score_exhirom > score_hirom && score_exhirom > score_lorom) {
        selected_mode = MapMode::ExHiROM;
    } else if (score_hirom > score_lorom) {
        selected_mode = MapMode::HiROM;
    } else {
        selected_mode = MapMode::LoROM;
    }

    if (!map_override.empty()) {
        const std::string ov = normalize_lower(map_override);
        if (ov == "lorom") {
            selected_mode = MapMode::LoROM;
        } else if (ov == "hirom") {
            selected_mode = MapMode::HiROM;
        } else if (ov == "exhirom") {
            selected_mode = MapMode::ExHiROM;
        } else {
            throw std::runtime_error("Unknown SNES map mode override: '" + std::string(ov) + "'");
        }
    }

    cart.map_mode = selected_mode;
    size_t hdr_off = 0x7FC0U;
    if (selected_mode == MapMode::HiROM && rom_span.size() >= 0x10000U) {
        hdr_off = 0xFFC0U;
    } else if (selected_mode == MapMode::ExHiROM && rom_span.size() >= 0x410000U) {
        hdr_off = 0x40FFC0U;
    }

    std::string raw_title(reinterpret_cast<const char*>(rom_span.data() + hdr_off), 21U);
    while (!raw_title.empty() && (raw_title.back() == ' ' || raw_title.back() == '\0')) {
        raw_title.pop_back();
    }
    cart.title = std::move(raw_title);

    cart.map_mode_byte = rom_span[hdr_off + 0x15U];
    cart.rom_type = rom_span[hdr_off + 0x16U];
    cart.has_fastrom = (cart.map_mode_byte & 0x10U) != 0U;

    const uint8_t rom_sz_code = rom_span[hdr_off + 0x17U];
    cart.rom_size_header = (rom_sz_code <= 16U) ? (1024U << rom_sz_code) : 0U;

    const uint8_t sram_sz_code = rom_span[hdr_off + 0x18U];
    cart.sram_size_bytes =
        (sram_sz_code > 0U && sram_sz_code <= 12U) ? (1024U << sram_sz_code) : 0U;
    cart.has_battery =
        (cart.rom_type == 0x02U || cart.rom_type == 0x05U || cart.rom_type == 0x06U ||
         (cart.sram_size_bytes > 0U && cart.rom_type != 0x00U));

    cart.checksum_complement = read_u16_le(rom_span, hdr_off + 0x1CU);
    cart.checksum = read_u16_le(rom_span, hdr_off + 0x1EU);

    cart.native_vectors.cop = read_u16_le(rom_span, hdr_off + 0x24U);
    cart.native_vectors.brk = read_u16_le(rom_span, hdr_off + 0x26U);
    cart.native_vectors.abort = read_u16_le(rom_span, hdr_off + 0x28U);
    cart.native_vectors.nmi = read_u16_le(rom_span, hdr_off + 0x2AU);
    cart.native_vectors.reset = read_u16_le(rom_span, hdr_off + 0x3CU);
    cart.native_vectors.irq = read_u16_le(rom_span, hdr_off + 0x2EU);

    cart.emu_vectors.cop = read_u16_le(rom_span, hdr_off + 0x34U);
    cart.emu_vectors.brk = read_u16_le(rom_span, hdr_off + 0x3EU);
    cart.emu_vectors.abort = read_u16_le(rom_span, hdr_off + 0x38U);
    cart.emu_vectors.nmi = read_u16_le(rom_span, hdr_off + 0x3AU);
    cart.emu_vectors.reset = read_u16_le(rom_span, hdr_off + 0x3CU);
    cart.emu_vectors.irq = read_u16_le(rom_span, hdr_off + 0x3EU);

    return cart;
}

Cartridge load_snes_rom(const std::filesystem::path& path, std::string_view expected_sha1,
                        std::string_view map_override) {
    const std::vector<uint8_t> bytes = read_binary_file(path);
    return parse_snes_rom(bytes, expected_sha1, map_override);
}

}  // namespace dragon::snes
