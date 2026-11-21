#include "md/cartridge.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#include "dragon/sha1.h"

namespace dragon::md {

namespace {

std::string trim_ascii(const uint8_t* data, size_t len) {
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = data[i];
        if (c >= 0x20U && c < 0x7FU) {
            s.push_back(static_cast<char>(c));
        } else if (c == 0) {
            break;
        }
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    return s.substr(start);
}

uint16_t be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8U) | p[1]);
}

uint32_t be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24U) | (static_cast<uint32_t>(p[1]) << 16U) |
           (static_cast<uint32_t>(p[2]) << 8U) | static_cast<uint32_t>(p[3]);
}

bool is_smd_format(const std::vector<uint8_t>& raw) noexcept {
    if (raw.size() < 512U + 16384U) {
        return false;
    }
    if (((raw.size() - 512U) % 16384U) != 0U) {
        return false;
    }
    // Super Magic Drive magic: bytes 8 == 0xAA, 9 == 0xBB, byte 10 == 0x06 (or 0x00)
    if (raw[8] == 0xAAU && raw[9] == 0xBBU) {
        return true;
    }
    // Check if flat header has "SEGA" at $100; if not, check de-interleaved first block
    if (raw.size() >= 0x104U && std::memcmp(&raw[0x100], "SEGA", 4) == 0) {
        return false;
    }
    return false;
}

std::vector<uint8_t> deinterleave_smd(const std::vector<uint8_t>& raw) {
    const size_t blocks = (raw.size() - 512U) / 16384U;
    std::vector<uint8_t> out(blocks * 16384U, 0);
    for (size_t b = 0; b < blocks; ++b) {
        const uint8_t* src = &raw[512U + b * 16384U];
        uint8_t* dst = &out[b * 16384U];
        for (size_t i = 0; i < 8192U; ++i) {
            dst[i * 2U + 1U] = src[i];          // First 8 KiB = odd bytes
            dst[i * 2U + 0U] = src[8192U + i];  // Second 8 KiB = even bytes
        }
    }
    return out;
}

}  // namespace

bool Cartridge::load_from_file(const std::filesystem::path& path, std::string* err) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        if (err)
            *err = "Cannot open ROM file: " + path.string();
        return false;
    }
    const auto sz = in.tellg();
    if (sz <= 0 || sz > 16 * 1024 * 1024) {
        if (err)
            *err = "Invalid Genesis ROM size";
        return false;
    }
    std::vector<uint8_t> raw(static_cast<size_t>(sz));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(raw.data()), sz)) {
        if (err)
            *err = "Failed reading ROM bytes";
        return false;
    }
    return load_from_bytes(raw, err);
}

bool Cartridge::load_from_bytes(const std::vector<uint8_t>& raw, std::string* err) {
    if (raw.size() < 512U) {
        if (err)
            *err = "ROM too small (< 512 bytes)";
        return false;
    }

    if (is_smd_format(raw)) {
        rom_ = deinterleave_smd(raw);
    } else {
        rom_ = raw;
    }

    // Pad ROM to at least 64 KiB and even size for safe 16-bit fetches
    if (rom_.size() & 1U) {
        rom_.push_back(0xFFU);
    }

    sha1_ = dragon::sha1_hex(rom_);
    parse_header();

    uses_ssf2_ = (rom_.size() > 4U * 1024U * 1024U);
    reset();
    return true;
}

void Cartridge::parse_header() {
    header_ = CartridgeHeader{};
    if (rom_.size() < 0x200U) {
        return;
    }
    header_.console_name = trim_ascii(&rom_[0x100], 16);
    header_.domestic_title = trim_ascii(&rom_[0x120], 48);
    header_.overseas_title = trim_ascii(&rom_[0x150], 48);
    header_.serial = trim_ascii(&rom_[0x180], 14);
    header_.checksum = be16(&rom_[0x18E]);
    header_.rom_start = be32(&rom_[0x1A0]);
    header_.rom_end = be32(&rom_[0x1A4]);
    header_.region = trim_ascii(&rom_[0x1F0], 16);

    // PAL-only detection: 'E' present without 'U' or 'J'
    const bool has_e = header_.region.find('E') != std::string::npos;
    const bool has_u = header_.region.find('U') != std::string::npos;
    const bool has_j = header_.region.find('J') != std::string::npos;
    header_.pal_only = has_e && !has_u && !has_j;

    // SRAM header ($01B0-$01BF)
    if (rom_[0x1B0] == 'R' && rom_[0x1B1] == 'A') {
        const uint8_t sram_type = rom_[0x1B2];
        // Bit 6 = battery backup, Bits 4-3 = byte width (00=both, 10=even, 11=odd)
        if ((sram_type & 0xA0U) == 0xA0U || (sram_type & 0xF8U) == 0xF8U ||
            (sram_type & 0xF8U) == 0xE0U || (sram_type & 0xF8U) == 0xB8U) {
            uint32_t s_start = be32(&rom_[0x1B4]) & 0x00FFFFFFU;
            uint32_t s_end = be32(&rom_[0x1B8]) & 0x00FFFFFFU;
            if (s_end >= s_start && (s_end - s_start) < 0x100000U) {
                header_.has_sram = true;
                header_.sram_battery = (sram_type & 0x40U) != 0U;
                const uint8_t width_bits = (sram_type >> 3U) & 0x03U;
                if (width_bits == 0x03U) {
                    header_.sram_mode = SramMode::OddBytes;
                } else if (width_bits == 0x02U) {
                    header_.sram_mode = SramMode::EvenBytes;
                } else {
                    header_.sram_mode = SramMode::BothBytes;
                }
                header_.sram_start = s_start & ~1U;
                header_.sram_end = s_end | 1U;
                const size_t span = static_cast<size_t>(header_.sram_end - header_.sram_start) + 1U;
                sram_.assign(std::clamp<size_t>(span, 0x2000U, 0x20000U), 0xFFU);
            }
        }
    }
}

void Cartridge::reset() noexcept {
    ssf2_banks_ = {0, 1, 2, 3, 4, 5, 6, 7};
    // If ROM overlaps SRAM range (ROM > 2 MiB and SRAM at $200000), SRAM starts disabled until
    // $A130F1 bit 0 is written.
    if (header_.has_sram && header_.sram_start < rom_.size()) {
        sram_enabled_ = false;
    } else {
        sram_enabled_ = header_.has_sram;
    }
    sram_write_protect_ = false;
}

bool Cartridge::is_sram_addr(uint32_t addr) const noexcept {
    return header_.has_sram && sram_enabled_ && !sram_.empty() && addr >= header_.sram_start &&
           addr <= header_.sram_end;
}

uint32_t Cartridge::map_rom_addr(uint32_t addr) const noexcept {
    const uint32_t a = addr & 0x003FFFFFU;
    if (uses_ssf2_) {
        const uint32_t slot = (a >> 19U) & 0x07U;
        const uint32_t bank = ssf2_banks_[slot];
        return (bank << 19U) | (a & 0x0007FFFFU);
    }
    return a;
}

uint8_t Cartridge::read8(uint32_t addr) const noexcept {
    if (is_sram_addr(addr)) {
        const size_t off = static_cast<size_t>(addr - header_.sram_start) % sram_.size();
        return sram_[off];
    }
    if (rom_.empty()) {
        return 0xFFU;
    }
    const uint32_t mapped = map_rom_addr(addr);
    if (mapped < rom_.size()) {
        return rom_[mapped];
    }
    return 0xFFU;
}

uint16_t Cartridge::read16(uint32_t addr) const noexcept {
    const uint8_t hi = read8(addr);
    const uint8_t lo = read8(addr + 1U);
    return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
}

void Cartridge::write8(uint32_t addr, uint8_t value) noexcept {
    if (!is_sram_addr(addr) || sram_write_protect_) {
        return;
    }
    if (header_.sram_mode == SramMode::OddBytes && (addr & 1U) == 0U) {
        return;
    }
    if (header_.sram_mode == SramMode::EvenBytes && (addr & 1U) != 0U) {
        return;
    }
    const size_t off = static_cast<size_t>(addr - header_.sram_start) % sram_.size();
    sram_[off] = value;
}

void Cartridge::write16(uint32_t addr, uint16_t value) noexcept {
    write8(addr, static_cast<uint8_t>(value >> 8U));
    write8(addr + 1U, static_cast<uint8_t>(value & 0xFFU));
}

void Cartridge::write_time_reg(uint32_t addr, uint8_t value) noexcept {
    const uint32_t reg = addr & 0xFFU;
    if (reg == 0xF1U) {
        sram_enabled_ = (value & 0x01U) != 0U;
        sram_write_protect_ = (value & 0x02U) != 0U;
    } else if (uses_ssf2_ && reg >= 0xF3U && reg <= 0xFFU && (reg & 1U) != 0U) {
        const size_t slot = (reg - 0xF1U) / 2U;  // $F3->1 .. $FF->7
        if (slot < 8U) {
            ssf2_banks_[slot] = value & 0x3FU;
        }
    }
}

bool Cartridge::load_sram(const std::filesystem::path& path) {
    if (sram_.empty() || !std::filesystem::exists(path)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.read(reinterpret_cast<char*>(sram_.data()), static_cast<std::streamsize>(sram_.size()));
    return true;
}

bool Cartridge::save_sram(const std::filesystem::path& path) const {
    if (sram_.empty() || !header_.sram_battery) {
        return false;
    }
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(sram_.data()),
              static_cast<std::streamsize>(sram_.size()));
    return out.good();
}

}  // namespace dragon::md
