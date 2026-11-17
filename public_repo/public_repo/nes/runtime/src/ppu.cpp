#include "nes/ppu.h"

#include <algorithm>
#include <cstring>

namespace dragon::nes {

namespace {

// Standard 2C02 64-color NTSC palette in RGBA8888 (0xRRGGBBFF) format.
// Reference: https://www.nesdev.org/wiki/PPU_palettes#2C02
constexpr std::array<uint32_t, 64> kNesPaletteRgba = {
    0x545454FFU, 0x001E74FFU, 0x081090FFU, 0x300088FFU, 0x440064FFU, 0x5C0030FFU, 0x540400FFU,
    0x3C1800FFU, 0x202A00FFU, 0x083A00FFU, 0x004000FFU, 0x003C00FFU, 0x00323CFFU, 0x000000FFU,
    0x000000FFU, 0x000000FFU, 0x989698FFU, 0x084CC4FFU, 0x3032ECFFU, 0x5C1EE4FFU, 0x8814B0FFU,
    0xA01464FFU, 0x982220FFU, 0x783C00FFU, 0x545A00FFU, 0x287200FFU, 0x087C00FFU, 0x007628FFU,
    0x006678FFU, 0x000000FFU, 0x000000FFU, 0x000000FFU, 0xECEEECFFU, 0x4C9AECFFU, 0x787CECFFU,
    0xB062ECFFU, 0xE454ECFFU, 0xEC58B4FFU, 0xEC6A64FFU, 0xD48820FFU, 0xA0AA00FFU, 0x74C400FFU,
    0x4CD020FFU, 0x38CC6CFFU, 0x38B4CCFFU, 0x3C3C3CFFU, 0x000000FFU, 0x000000FFU, 0xECEEECFFU,
    0xA8CCECFFU, 0xBCBCECFFU, 0xD4B2ECFFU, 0xECAEECFFU, 0xECAED4FFU, 0xECB4B0FFU, 0xE4C490FFU,
    0xCCD278FFU, 0xB4DE78FFU, 0xA8E290FFU, 0x98E2B4FFU, 0xA0D6E4FFU, 0xA0A2A0FFU, 0x000000FFU,
    0x000000FFU,
};

// Nintendo Vs. System Arcade PPUs (RP2C04 scrambled & RP2C03 RGB)
// Reference: https://www.nesdev.org/wiki/PPU_palettes#RP2C04
constexpr std::array<uint32_t, 64> kVs0001PaletteRgba = {
    0xFFB6B6FFU, 0xDA6DFFFFU, 0xFF0000FFU, 0x9191FFFFU, 0x009191FFU, 0x244800FFU, 0x484848FFU,
    0xFF0091FFU, 0xFFFFFFFFU, 0x6D6D6DFFU, 0xFFB600FFU, 0xB6006DFFU, 0x91006DFFU, 0xDADA00FFU,
    0x6D4800FFU, 0xFFFFFFFFU, 0x6DB6FFFFU, 0xDAB66DFFU, 0x6D2400FFU, 0x6DDA00FFU, 0x91DAFFFFU,
    0xDAB6FFFFU, 0xFFDA91FFU, 0x0048FFFFU, 0xFFDA00FFU, 0x48FFDAFFU, 0x000000FFU, 0x480000FFU,
    0xDADADAFFU, 0x919191FFU, 0xFF00FFFFU, 0x002491FFU, 0x00006DFFU, 0xB6DAFFFFU, 0xFFB6FFFFU,
    0x00FF00FFU, 0x00FFFFFFU, 0x004848FFU, 0x00B66DFFU, 0xB600FFFFU, 0x000000FFU, 0x914800FFU,
    0xFF91FFFFU, 0xB62400FFU, 0x9100FFFFU, 0x0000DAFFU, 0xFF9100FFU, 0x000000FFU, 0x000000FFU,
    0x249100FFU, 0xB6B6B6FFU, 0x006D24FFU, 0xB6FF48FFU, 0x6D48DAFFU, 0xFFFF00FFU, 0xDA6D00FFU,
    0x004800FFU, 0x006DDAFFU, 0x009100FFU, 0x242424FFU, 0xFFFF6DFFU, 0xFF6DFFFFU, 0x916D00FFU,
    0x91FF6DFFU,
};

constexpr std::array<uint32_t, 64> kVs0002PaletteRgba = {
    0x000000FFU, 0xFFB600FFU, 0x916D00FFU, 0xB6FF48FFU, 0x91FF6DFFU, 0xFF6DFFFFU, 0x009191FFU,
    0xB6DAFFFFU, 0xFF0000FFU, 0x9100FFFFU, 0xFFFF6DFFU, 0xFF91FFFFU, 0xFFFFFFFFU, 0xDA6DFFFFU,
    0x91DAFFFFU, 0x009100FFU, 0x004800FFU, 0x6DB6FFFFU, 0xB62400FFU, 0xDADADAFFU, 0x00B66DFFU,
    0x6DDA00FFU, 0x480000FFU, 0x9191FFFFU, 0x484848FFU, 0xFF00FFFFU, 0x00006DFFU, 0x48FFDAFFU,
    0xDAB6FFFFU, 0x6D4800FFU, 0x000000FFU, 0x6D48DAFFU, 0x91006DFFU, 0xFFDA91FFU, 0xFF9100FFU,
    0xFFB6FFFFU, 0x006DDAFFU, 0x6D2400FFU, 0xB6B6B6FFU, 0x0000DAFFU, 0xB600FFFFU, 0xFFDA00FFU,
    0x6D6D6DFFU, 0x244800FFU, 0x0048FFFFU, 0x000000FFU, 0xDADA00FFU, 0xFFFFFFFFU, 0xDAB66DFFU,
    0x242424FFU, 0x00FF00FFU, 0xDA6D00FFU, 0x004848FFU, 0x002491FFU, 0xFF0091FFU, 0x249100FFU,
    0x000000FFU, 0x00FFFFFFU, 0x914800FFU, 0xFFFF00FFU, 0xFFB6B6FFU, 0xB6006DFFU, 0x006D24FFU,
    0x919191FFU,
};

constexpr std::array<uint32_t, 64> kVs0003PaletteRgba = {
    0xB600FFFFU, 0xFF6DFFFFU, 0x91FF6DFFU, 0xB6B6B6FFU, 0x009100FFU, 0xFFFFFFFFU, 0xB6DAFFFFU,
    0x244800FFU, 0x002491FFU, 0x000000FFU, 0xFFDA91FFU, 0x6D4800FFU, 0xFF0091FFU, 0xDADADAFFU,
    0xDAB66DFFU, 0x91DAFFFFU, 0x9191FFFFU, 0x009191FFU, 0xB6006DFFU, 0x0048FFFFU, 0x249100FFU,
    0x916D00FFU, 0xDA6D00FFU, 0x00B66DFFU, 0x6D6D6DFFU, 0x6D48DAFFU, 0x000000FFU, 0x0000DAFFU,
    0xFF0000FFU, 0xB62400FFU, 0xFF91FFFFU, 0xFFB6B6FFU, 0xDA6DFFFFU, 0x004800FFU, 0x00006DFFU,
    0xFFFF00FFU, 0x242424FFU, 0xFFB600FFU, 0xFF9100FFU, 0xFFFFFFFFU, 0x6DDA00FFU, 0x91006DFFU,
    0x6DB6FFFFU, 0xFF00FFFFU, 0x006DDAFFU, 0x919191FFU, 0x000000FFU, 0x6D2400FFU, 0x00FFFFFFU,
    0x480000FFU, 0xB6FF48FFU, 0xFFB6FFFFU, 0x914800FFU, 0x00FF00FFU, 0xDADA00FFU, 0x484848FFU,
    0x006D24FFU, 0x000000FFU, 0xDAB6FFFFU, 0xFFFF6DFFU, 0x9100FFFFU, 0x48FFDAFFU, 0xFFDA00FFU,
    0x004848FFU,
};

constexpr std::array<uint32_t, 64> kVs0004PaletteRgba = {
    0x916D00FFU, 0x6D48DAFFU, 0x009191FFU, 0xDADA00FFU, 0x000000FFU, 0xFFB6B6FFU, 0x002491FFU,
    0xDA6D00FFU, 0xB6B6B6FFU, 0x6D2400FFU, 0x00FF00FFU, 0x00006DFFU, 0xFFDA91FFU, 0xFFFF00FFU,
    0x009100FFU, 0xB6FF48FFU, 0xFF6DFFFFU, 0x480000FFU, 0x0048FFFFU, 0xFF91FFFFU, 0x000000FFU,
    0x484848FFU, 0xB62400FFU, 0xFF9100FFU, 0xDAB66DFFU, 0x00B66DFFU, 0x9191FFFFU, 0x249100FFU,
    0x91006DFFU, 0x000000FFU, 0x91FF6DFFU, 0x6DB6FFFFU, 0xB6006DFFU, 0x006D24FFU, 0x914800FFU,
    0x0000DAFFU, 0x9100FFFFU, 0xB600FFFFU, 0x6D6D6DFFU, 0xFF0091FFU, 0x004848FFU, 0xDADADAFFU,
    0x006DDAFFU, 0x004800FFU, 0x242424FFU, 0xFFFF6DFFU, 0x919191FFU, 0xFF00FFFFU, 0xFFB6FFFFU,
    0xFFFFFFFFU, 0x6D4800FFU, 0xFF0000FFU, 0xFFDA00FFU, 0x48FFDAFFU, 0xFFFFFFFFU, 0x91DAFFFFU,
    0x000000FFU, 0xFFB600FFU, 0xDA6DFFFFU, 0xB6DAFFFFU, 0x6DDA00FFU, 0xDAB6FFFFU, 0x00FFFFFFU,
    0x244800FFU,
};

constexpr std::array<uint32_t, 64> kVs2C03PaletteRgba = {
    0x6D6D6DFFU, 0x002491FFU, 0x0000DAFFU, 0x6D48DAFFU, 0x91006DFFU, 0xB6006DFFU, 0xB62400FFU,
    0x914800FFU, 0x6D4800FFU, 0x244800FFU, 0x006D24FFU, 0x009100FFU, 0x004848FFU, 0x000000FFU,
    0x000000FFU, 0x000000FFU, 0xB6B6B6FFU, 0x006DDAFFU, 0x0048FFFFU, 0x9100FFFFU, 0xB600FFFFU,
    0xFF0091FFU, 0xFF0000FFU, 0xDA6D00FFU, 0x916D00FFU, 0x249100FFU, 0x009100FFU, 0x00B66DFFU,
    0x009191FFU, 0x000000FFU, 0x000000FFU, 0x000000FFU, 0xFFFFFFFFU, 0x6DB6FFFFU, 0x9191FFFFU,
    0xDA6DFFFFU, 0xFF00FFFFU, 0xFF6DFFFFU, 0xFF9100FFU, 0xFFB600FFU, 0xDADA00FFU, 0x6DDA00FFU,
    0x00FF00FFU, 0x48FFDAFFU, 0x00FFFFFFU, 0x000000FFU, 0x000000FFU, 0x000000FFU, 0xFFFFFFFFU,
    0xB6DAFFFFU, 0xDAB6FFFFU, 0xFFB6FFFFU, 0xFF91FFFFU, 0xFFB6B6FFU, 0xFFDA91FFU, 0xFFFF48FFU,
    0xFFFF6DFFU, 0xB6FF48FFU, 0x91FF6DFFU, 0x48FFDAFFU, 0x91DAFFFFU, 0x000000FFU, 0x000000FFU,
    0x000000FFU,
};

constexpr std::array<uint32_t, 64> kSmoothFbxPaletteRgba = {
    0x6A6D6AFFU, 0x002490FFU, 0x001098FFU, 0x380090FFU, 0x580070FFU, 0x700020FFU, 0x700000FFU,
    0x501000FFU, 0x282000FFU, 0x003000FFU, 0x003800FFU, 0x003018FFU, 0x002848FFU, 0x000000FFU,
    0x000000FFU, 0x000000FFU, 0xB6B6B6FFU, 0x1860E8FFU, 0x4038F8FFU, 0x7820F0FFU, 0xA810B8FFU,
    0xC01060FFU, 0xB82000FFU, 0x884000FFU, 0x605800FFU, 0x207000FFU, 0x007800FFU, 0x007038FFU,
    0x006078FFU, 0x000000FFU, 0x000000FFU, 0x000000FFU, 0xFFFFFFFFU, 0x60B0FFFFU, 0x8890FFFFU,
    0xC080FFFFU, 0xF070FFFFU, 0xFF68B8FFU, 0xFF7858FFU, 0xE09818FFU, 0xB8B000FFU, 0x70C800FFU,
    0x38D820FFU, 0x20D870FFU, 0x20C0C8FFU, 0x484848FFU, 0x000000FFU, 0x000000FFU, 0xFFFFFFFFU,
    0xC0E0FFFFU, 0xD0D0FFFFU, 0xE8C8FFFFU, 0xF8C0FFFFU, 0xFFC0E8FFU, 0xFFC8B8FFU, 0xF0D898FFU,
    0xE0E880FFU, 0xC0F080FFU, 0xA8F898FFU, 0x90F8C0FFU, 0x90E8F0FFU, 0xA8A8A8FFU, 0x000000FFU,
    0x000000FFU,
};

constexpr uint8_t reverse_bits8(uint8_t b) noexcept {
    b = static_cast<uint8_t>(((b & 0xF0U) >> 4U) | ((b & 0x0FU) << 4U));
    b = static_cast<uint8_t>(((b & 0xCCU) >> 2U) | ((b & 0x33U) << 2U));
    b = static_cast<uint8_t>(((b & 0xAAU) >> 1U) | ((b & 0x55U) << 1U));
    return b;
}

}  // namespace

Ppu::Ppu() noexcept {
    chr_data_ = chr_ram_.data();
    chr_size_ = static_cast<uint32_t>(chr_ram_.size());
    reset();
}

void Ppu::reset() noexcept {
    ciram_.fill(0U);
    palette_ram_.fill(0U);
    oam_.fill(0xFFU);
    framebuffer_.fill(kNesPaletteRgba[0]);

    if (!chr_data_) {
        chr_data_ = chr_ram_.data();
        chr_size_ = static_cast<uint32_t>(chr_ram_.size());
    }
    for (uint8_t i = 0U; i < 8U; ++i) {
        chr_bank_offsets_[i] = static_cast<uint32_t>(i) * 0x0400U;
    }

    ctrl_ = 0U;
    mask_ = 0U;
    status_ = 0U;
    oam_addr_ = 0U;
    read_buffer_ = 0U;
    open_bus_ = 0U;

    v_ = 0U;
    t_ = 0U;
    x_ = 0U;
    w_ = 0U;

    scanline_ = 0U;
    dot_ = 0U;
    frame_count_ = 0ULL;
    total_dots_ = 0ULL;

    nmi_pending_ = false;
    frame_complete_ = false;
    suppress_vbl_ = false;
    suppress_nmi_ = false;
    sprite_zero_hit_latched_ = false;

    bg_next_tile_id_ = 0U;
    bg_next_tile_attr_ = 0U;
    bg_next_tile_lsb_ = 0U;
    bg_next_tile_msb_ = 0U;
    bg_shift_pat_lo_ = 0U;
    bg_shift_pat_hi_ = 0U;
    bg_shift_attr_lo_ = 0U;
    bg_shift_attr_hi_ = 0U;

    active_sprites_ = {};
    active_sprite_count_ = 0U;
}

void Ppu::load_chr(std::span<const uint8_t> chr_rom, bool is_ram) noexcept {
    chr_ram_.fill(0U);
    if (!chr_rom.empty()) {
        if (chr_rom.size() <= chr_ram_.size()) {
            std::memcpy(chr_ram_.data(), chr_rom.data(), chr_rom.size());
            chr_data_ = chr_ram_.data();
            chr_size_ = static_cast<uint32_t>(chr_ram_.size());
        } else {
            chr_rom_storage_.assign(chr_rom.begin(), chr_rom.end());
            chr_data_ = chr_rom_storage_.data();
            chr_size_ = static_cast<uint32_t>(chr_rom_storage_.size());
        }
        chr_is_ram_ = is_ram;
    } else {
        chr_data_ = chr_ram_.data();
        chr_size_ = static_cast<uint32_t>(chr_ram_.size());
        chr_is_ram_ = true;
    }
    set_chr_bank_8k(0U);
}

void Ppu::set_chr_bank_1k(uint8_t slot_1k, uint32_t bank_1k) noexcept {
    if (slot_1k >= 8U || chr_size_ < 0x0400U) {
        return;
    }
    const uint32_t num_1k_banks = chr_size_ >> 10U;
    const uint32_t wrapped = (num_1k_banks > 0U) ? (bank_1k % num_1k_banks) : 0U;
    chr_bank_offsets_[slot_1k] = wrapped << 10U;
}

void Ppu::set_chr_bank_2k(uint8_t slot_1k_base, uint32_t bank_1k_aligned) noexcept {
    const uint32_t base = bank_1k_aligned & ~1U;
    set_chr_bank_1k(slot_1k_base, base);
    set_chr_bank_1k(static_cast<uint8_t>(slot_1k_base + 1U), base + 1U);
}

void Ppu::set_chr_bank_4k(uint8_t slot_4k, uint32_t bank_4k) noexcept {
    const uint8_t base_slot = static_cast<uint8_t>((slot_4k & 0x01U) * 4U);
    const uint32_t base_1k = bank_4k * 4U;
    for (uint8_t i = 0U; i < 4U; ++i) {
        set_chr_bank_1k(static_cast<uint8_t>(base_slot + i), base_1k + i);
    }
}

void Ppu::set_chr_bank_8k(uint32_t bank_8k) noexcept {
    const uint32_t base_1k = bank_8k * 8U;
    for (uint8_t i = 0U; i < 8U; ++i) {
        set_chr_bank_1k(i, base_1k + i);
    }
}

uint32_t Ppu::palette_rgba(uint8_t color_id, uint8_t emphasis_bits, PpuPalette pal) noexcept {
    const uint8_t idx = color_id & 0x3FU;
    const std::array<uint32_t, 64>* tbl = &kNesPaletteRgba;
    switch (pal) {
        case PpuPalette::Ntsc2C02:
            tbl = &kNesPaletteRgba;
            break;
        case PpuPalette::Vs0001:
            tbl = &kVs0001PaletteRgba;
            break;
        case PpuPalette::Vs0002:
            tbl = &kVs0002PaletteRgba;
            break;
        case PpuPalette::Vs0003:
            tbl = &kVs0003PaletteRgba;
            break;
        case PpuPalette::Vs0004:
            tbl = &kVs0004PaletteRgba;
            break;
        case PpuPalette::Vs2C03:
            tbl = &kVs2C03PaletteRgba;
            break;
        case PpuPalette::SmoothFbx:
            tbl = &kSmoothFbxPaletteRgba;
            break;
    }
    const uint32_t base = (*tbl)[idx];
    if (emphasis_bits == 0U || (idx & 0x0FU) >= 0x0EU) {
        return base;
    }

    uint32_t r = (base >> 24U) & 0xFFU;
    uint32_t g = (base >> 16U) & 0xFFU;
    uint32_t b = (base >> 8U) & 0xFFU;

    // Bit 0: emphasize red (attenuate G/B); Bit 1: green (attenuate R/B); Bit 2: blue (attenuate
    // R/G).
    if (emphasis_bits & 0x01U) {
        g = (g * 3U) / 4U;
        b = (b * 3U) / 4U;
    }
    if (emphasis_bits & 0x02U) {
        r = (r * 3U) / 4U;
        b = (b * 3U) / 4U;
    }
    if (emphasis_bits & 0x04U) {
        r = (r * 3U) / 4U;
        g = (g * 3U) / 4U;
    }

    return (r << 24U) | (g << 16U) | (b << 8U) | 0xFFU;
}

uint16_t Ppu::map_nametable_addr(uint16_t addr) const noexcept {
    const uint16_t offset = addr & 0x0FFFU;
    switch (mirroring_) {
        case Mirroring::Horizontal:
            // $2000 & $2400 -> first 1 KiB; $2800 & $2C00 -> second 1 KiB
            return static_cast<uint16_t>(((offset >> 1U) & 0x0400U) | (offset & 0x03FFU));
        case Mirroring::Vertical:
            // $2000 & $2800 -> first 1 KiB; $2400 & $2C00 -> second 1 KiB
            return static_cast<uint16_t>(offset & 0x07FFU);
        case Mirroring::FourScreen:
            return offset;
        case Mirroring::OneScreenLower:
            return static_cast<uint16_t>(offset & 0x03FFU);
        case Mirroring::OneScreenUpper:
            return static_cast<uint16_t>(0x0400U | (offset & 0x03FFU));
    }
    return static_cast<uint16_t>(offset & 0x07FFU);
}

uint8_t Ppu::map_palette_addr(uint16_t addr) noexcept {
    uint8_t idx = static_cast<uint8_t>(addr & 0x001FU);
    // $3F10/$3F14/$3F18/$3F1C mirror $3F00/$3F04/$3F08/$3F0C
    if ((idx & 0x13U) == 0x10U) {
        idx &= ~0x10U;
    }
    return idx;
}

uint8_t Ppu::read_vram(uint16_t addr) const noexcept {
    const uint16_t masked = addr & 0x3FFFU;
    if (masked < 0x2000U) {
        const uint8_t slot = static_cast<uint8_t>((masked >> 10U) & 0x07U);
        return chr_data_[chr_bank_offsets_[slot] + (masked & 0x03FFU)];
    }
    if (masked < 0x3F00U) {
        return ciram_[map_nametable_addr(masked)];
    }
    return palette_ram_[map_palette_addr(masked)];
}

void Ppu::write_vram(uint16_t addr, uint8_t value) noexcept {
    const uint16_t masked = addr & 0x3FFFU;
    if (masked < 0x2000U) {
        if (chr_is_ram_) {
            const uint8_t slot = static_cast<uint8_t>((masked >> 10U) & 0x07U);
            chr_data_[chr_bank_offsets_[slot] + (masked & 0x03FFU)] = value;
        }
        return;
    }
    if (masked < 0x3F00U) {
        ciram_[map_nametable_addr(masked)] = value;
        return;
    }
    palette_ram_[map_palette_addr(masked)] = static_cast<uint8_t>(value & 0x3FU);
}

uint8_t Ppu::read_register(uint16_t reg) noexcept {
    switch (reg & 0x0007U) {
        case 2U: {
            // $2002 PPUSTATUS: reading at (241, 0) suppresses VBlank & NMI for that frame;
            // reading at (241, 1..2) suppresses NMI.
            if (scanline_ == 241U) {
                if (dot_ == 0U) {
                    suppress_vbl_ = true;
                    suppress_nmi_ = true;
                } else if (dot_ == 1U || dot_ == 2U) {
                    nmi_pending_ = false;
                }
            }
            const uint8_t res = static_cast<uint8_t>((status_ & 0xE0U) | (open_bus_ & 0x1FU));
            open_bus_ = res;
            status_ &= ~0x80U;
            w_ = 0U;
            return res;
        }
        case 4U: {
            // $2004 OAMDATA
            uint8_t val = oam_[oam_addr_];
            if ((oam_addr_ & 0x03U) == 2U) {
                val &= 0xE3U;
            }
            open_bus_ = val;
            return val;
        }
        case 7U: {
            // $2007 PPUDATA: buffered read except for palette RAM ($3F00-$3FFF)
            const uint16_t addr = v_ & 0x3FFFU;
            uint8_t res = 0U;
            if (addr < 0x3F00U) {
                res = read_buffer_;
                read_buffer_ = read_vram(addr);
                open_bus_ = res;
            } else {
                const uint8_t pal_mask = (mask_ & 0x01U) ? 0x30U : 0x3FU;
                res = static_cast<uint8_t>((read_vram(addr) & pal_mask) | (open_bus_ & 0xC0U));
                // Underlying nametable at $2F00-$2FFF still loads into the read buffer
                read_buffer_ = read_vram(static_cast<uint16_t>(addr & 0x2FFFU));
                open_bus_ = res;
            }
            const uint16_t inc = (ctrl_ & 0x04U) ? 32U : 1U;
            v_ = static_cast<uint16_t>((v_ + inc) & 0x7FFFU);
            return res;
        }
        default:
            return open_bus_;
    }
}

void Ppu::write_register(uint16_t reg, uint8_t value) noexcept {
    open_bus_ = value;
    switch (reg & 0x0007U) {
        case 0U: {
            // $2000 PPUCTRL
            const bool old_nmi = (ctrl_ & 0x80U) != 0U;
            ctrl_ = value;
            const bool new_nmi = (ctrl_ & 0x80U) != 0U;
            // t: ...GH.. ........ <- d: ......GH
            t_ = static_cast<uint16_t>((t_ & 0x73FFU) |
                                       (static_cast<uint16_t>(value & 0x03U) << 10U));
            // Rising edge on NMI enable while VBlank flag is set triggers immediate NMI
            if (!old_nmi && new_nmi && (status_ & 0x80U) != 0U) {
                if (!(scanline_ == 261U && dot_ == 0U)) {
                    nmi_pending_ = true;
                }
            }
            if (!new_nmi && scanline_ == 241U && (dot_ == 1U || dot_ == 2U)) {
                nmi_pending_ = false;
            }
            break;
        }
        case 1U:
            // $2001 PPUMASK
            mask_ = value;
            break;
        case 2U:
            // $2002 PPUSTATUS (read-only)
            break;
        case 3U:
            // $2003 OAMADDR
            oam_addr_ = value;
            break;
        case 4U:
            // $2004 OAMDATA
            if ((oam_addr_ & 0x03U) == 2U) {
                value &= 0xE3U;
            }
            oam_[oam_addr_] = value;
            oam_addr_ = static_cast<uint8_t>(oam_addr_ + 1U);
            break;
        case 5U:
            // $2005 PPUSCROLL
            if (w_ == 0U) {
                // First write: t: ....... ...ABCDE <- d: ABCDE...; x <- d: .....FGH; w <- 1
                t_ = static_cast<uint16_t>((t_ & 0x7FE0U) | (value >> 3U));
                x_ = static_cast<uint8_t>(value & 0x07U);
                w_ = 1U;
            } else {
                // Second write: t: FGH..AB CDE..... <- d: ABCDEFGH; w <- 0
                t_ = static_cast<uint16_t>((t_ & 0x0C1FU) |
                                           (static_cast<uint16_t>(value & 0x07U) << 12U) |
                                           (static_cast<uint16_t>(value & 0xF8U) << 2U));
                w_ = 0U;
            }
            break;
        case 6U:
            // $2006 PPUADDR
            if (w_ == 0U) {
                // First write: t: 0CDEFGH ........ <- d: ..CDEFGH (bit 14 cleared); w <- 1
                t_ = static_cast<uint16_t>((t_ & 0x00FFU) |
                                           (static_cast<uint16_t>(value & 0x3FU) << 8U));
                w_ = 1U;
            } else {
                // Second write: t: ....... ABCDEFGH <- d: ABCDEFGH; v <- t; w <- 0
                t_ = static_cast<uint16_t>((t_ & 0x7F00U) | value);
                v_ = t_;
                w_ = 0U;
            }
            break;
        case 7U: {
            // $2007 PPUDATA
            write_vram(v_ & 0x3FFFU, value);
            const uint16_t inc = (ctrl_ & 0x04U) ? 32U : 1U;
            v_ = static_cast<uint16_t>((v_ + inc) & 0x7FFFU);
            break;
        }
    }
}

void Ppu::write_oam_dma(uint8_t offset, uint8_t value) noexcept {
    const uint8_t dst = static_cast<uint8_t>(oam_addr_ + offset);
    if ((dst & 0x03U) == 2U) {
        value &= 0xE3U;
    }
    oam_[dst] = value;
    open_bus_ = value;
}

void Ppu::increment_scroll_x() noexcept {
    if (!rendering_enabled()) {
        return;
    }
    if ((v_ & 0x001FU) == 31U) {
        v_ &= ~0x001FU;
        v_ ^= 0x0400U;
    } else {
        v_ = static_cast<uint16_t>(v_ + 1U);
    }
}

void Ppu::increment_scroll_y() noexcept {
    if (!rendering_enabled()) {
        return;
    }
    if ((v_ & 0x7000U) != 0x7000U) {
        v_ = static_cast<uint16_t>(v_ + 0x1000U);
    } else {
        v_ &= ~0x7000U;
        uint16_t y = static_cast<uint16_t>((v_ & 0x03E0U) >> 5U);
        if (y == 29U) {
            y = 0U;
            v_ ^= 0x0800U;
        } else if (y == 31U) {
            y = 0U;
        } else {
            y = static_cast<uint16_t>(y + 1U);
        }
        v_ = static_cast<uint16_t>((v_ & ~0x03E0U) | (y << 5U));
    }
}

void Ppu::transfer_address_x() noexcept {
    if (!rendering_enabled()) {
        return;
    }
    v_ = static_cast<uint16_t>((v_ & ~0x041FU) | (t_ & 0x041FU));
}

void Ppu::transfer_address_y() noexcept {
    if (!rendering_enabled()) {
        return;
    }
    v_ = static_cast<uint16_t>((v_ & ~0x7BE0U) | (t_ & 0x7BE0U));
}

void Ppu::load_background_shifters() noexcept {
    bg_shift_pat_lo_ = static_cast<uint16_t>((bg_shift_pat_lo_ & 0xFF00U) | bg_next_tile_lsb_);
    bg_shift_pat_hi_ = static_cast<uint16_t>((bg_shift_pat_hi_ & 0xFF00U) | bg_next_tile_msb_);
    bg_shift_attr_lo_ = static_cast<uint16_t>((bg_shift_attr_lo_ & 0xFF00U) |
                                              ((bg_next_tile_attr_ & 0x01U) ? 0x00FFU : 0x0000U));
    bg_shift_attr_hi_ = static_cast<uint16_t>((bg_shift_attr_hi_ & 0xFF00U) |
                                              ((bg_next_tile_attr_ & 0x02U) ? 0x00FFU : 0x0000U));
}

void Ppu::update_background_shifters() noexcept {
    if ((mask_ & 0x08U) != 0U) {
        bg_shift_pat_lo_ <<= 1U;
        bg_shift_pat_hi_ <<= 1U;
        bg_shift_attr_lo_ <<= 1U;
        bg_shift_attr_hi_ <<= 1U;
    }
}

void Ppu::run_background_fetch_step(uint16_t phase) noexcept {
    switch (phase) {
        case 1U:
            bg_next_tile_id_ = read_vram(static_cast<uint16_t>(0x2000U | (v_ & 0x0FFFU)));
            break;
        case 3U: {
            const uint16_t attr_addr = static_cast<uint16_t>(
                0x23C0U | (v_ & 0x0C00U) | ((v_ >> 4U) & 0x38U) | ((v_ >> 2U) & 0x07U));
            uint8_t attr = read_vram(attr_addr);
            if ((v_ & 0x0040U) != 0U) {
                attr >>= 4U;
            }
            if ((v_ & 0x0002U) != 0U) {
                attr >>= 2U;
            }
            bg_next_tile_attr_ = static_cast<uint8_t>(attr & 0x03U);
            break;
        }
        case 5U: {
            const uint16_t bg_table = (ctrl_ & 0x10U) ? 0x1000U : 0x0000U;
            const uint16_t fine_y = (v_ >> 12U) & 0x07U;
            const uint16_t addr = static_cast<uint16_t>(
                bg_table + (static_cast<uint16_t>(bg_next_tile_id_) << 4U) + fine_y);
            bg_next_tile_lsb_ = read_vram(addr);
            break;
        }
        case 7U: {
            const uint16_t bg_table = (ctrl_ & 0x10U) ? 0x1000U : 0x0000U;
            const uint16_t fine_y = (v_ >> 12U) & 0x07U;
            const uint16_t addr = static_cast<uint16_t>(
                bg_table + (static_cast<uint16_t>(bg_next_tile_id_) << 4U) + fine_y + 8U);
            bg_next_tile_msb_ = read_vram(addr);
            load_background_shifters();
            increment_scroll_x();
            break;
        }
        default:
            break;
    }
}

void Ppu::evaluate_sprites_for_next_scanline(uint16_t eval_scanline) noexcept {
    active_sprite_count_ = 0U;
    if (!rendering_enabled() || eval_scanline >= 239U) {
        return;
    }

    const int sprite_height = (ctrl_ & 0x20U) ? 16 : 8;
    uint8_t found = 0U;

    for (uint8_t i = 0U; i < 64U; ++i) {
        const uint8_t spr_y = oam_[static_cast<size_t>(i) * 4U + 0U];
        if (spr_y >= 0xEFU) {
            continue;
        }
        const int row_in_sprite = static_cast<int>(eval_scanline) - static_cast<int>(spr_y);
        if (row_in_sprite < 0 || row_in_sprite >= sprite_height) {
            continue;
        }

        if (found < 8U) {
            const uint8_t tile_id = oam_[static_cast<size_t>(i) * 4U + 1U];
            const uint8_t attr =
                static_cast<uint8_t>(oam_[static_cast<size_t>(i) * 4U + 2U] & 0xE3U);
            const uint8_t spr_x = oam_[static_cast<size_t>(i) * 4U + 3U];

            const bool flip_h = (attr & 0x40U) != 0U;
            const bool flip_v = (attr & 0x80U) != 0U;

            uint16_t pat_addr = 0U;
            if (sprite_height == 8) {
                const uint16_t table = (ctrl_ & 0x08U) ? 0x1000U : 0x0000U;
                const int row = flip_v ? (7 - row_in_sprite) : row_in_sprite;
                pat_addr = static_cast<uint16_t>(table + (static_cast<uint16_t>(tile_id) << 4U) +
                                                 static_cast<uint16_t>(row));
            } else {
                const uint16_t table = (tile_id & 0x01U) ? 0x1000U : 0x0000U;
                const int row = flip_v ? (15 - row_in_sprite) : row_in_sprite;
                const uint8_t tile_num =
                    static_cast<uint8_t>((tile_id & 0xFEU) + ((row >= 8) ? 1U : 0U));
                pat_addr = static_cast<uint16_t>(table + (static_cast<uint16_t>(tile_num) << 4U) +
                                                 static_cast<uint16_t>(row & 0x07));
            }

            uint8_t pat_lo = read_vram(pat_addr);
            uint8_t pat_hi = read_vram(static_cast<uint16_t>(pat_addr + 8U));
            if (flip_h) {
                pat_lo = reverse_bits8(pat_lo);
                pat_hi = reverse_bits8(pat_hi);
            }

            ActiveSprite& sp = active_sprites_[found];
            sp.x = spr_x;
            sp.pat_lo = pat_lo;
            sp.pat_hi = pat_hi;
            sp.palette = static_cast<uint8_t>((attr & 0x03U) + 4U);
            sp.priority = static_cast<uint8_t>((attr & 0x20U) ? 1U : 0U);
            sp.is_sprite_zero = (i == 0U);
            ++found;
        } else {
            status_ |= 0x20U;  // Sprite overflow flag
            break;
        }
    }

    active_sprite_count_ = found;
}

void Ppu::render_pixel() noexcept {
    const uint16_t x = static_cast<uint16_t>(dot_ - 1U);

    if (sprite_zero_hit_latched_ && dot_ >= 2U) {
        status_ |= 0x40U;
        sprite_zero_hit_latched_ = false;
    }

    uint8_t bg_pixel = 0U;
    uint8_t bg_palette = 0U;

    if ((mask_ & 0x08U) != 0U) {
        if (x >= 8U || (mask_ & 0x02U) != 0U) {
            const uint16_t mux = static_cast<uint16_t>(0x8000U >> x_);
            const uint8_t p0 = (bg_shift_pat_lo_ & mux) ? 1U : 0U;
            const uint8_t p1 = (bg_shift_pat_hi_ & mux) ? 2U : 0U;
            bg_pixel = static_cast<uint8_t>(p0 | p1);

            const uint8_t a0 = (bg_shift_attr_lo_ & mux) ? 1U : 0U;
            const uint8_t a1 = (bg_shift_attr_hi_ & mux) ? 2U : 0U;
            bg_palette = static_cast<uint8_t>(a0 | a1);
        }
    }

    uint8_t fg_pixel = 0U;
    uint8_t fg_palette = 0U;
    uint8_t fg_priority = 0U;
    bool sprite_zero_rendered = false;

    if ((mask_ & 0x10U) != 0U) {
        if (x >= 8U || (mask_ & 0x04U) != 0U) {
            for (uint8_t s = 0U; s < active_sprite_count_; ++s) {
                const ActiveSprite& sp = active_sprites_[s];
                const int diff = static_cast<int>(x) - static_cast<int>(sp.x);
                if (diff >= 0 && diff < 8) {
                    const uint8_t shift = static_cast<uint8_t>(7 - diff);
                    const uint8_t p0 = static_cast<uint8_t>((sp.pat_lo >> shift) & 0x01U);
                    const uint8_t p1 = static_cast<uint8_t>(((sp.pat_hi >> shift) & 0x01U) << 1U);
                    const uint8_t sp_pixel = static_cast<uint8_t>(p0 | p1);
                    if (sp_pixel != 0U) {
                        if (sp.is_sprite_zero) {
                            sprite_zero_rendered = true;
                        }
                        if (fg_pixel == 0U) {
                            fg_pixel = sp_pixel;
                            fg_palette = sp.palette;
                            fg_priority = sp.priority;
                            break;
                        }
                    }
                }
            }
        }
    }

    // Sprite-0 hit check: opaque BG and opaque Sprite-0 overlap, never at X=255.
    // Left-8px clipping is already enforced above on bg_pixel and fg_pixel.
    if (bg_pixel != 0U && fg_pixel != 0U && sprite_zero_rendered && x != 255U &&
        (mask_ & 0x18U) == 0x18U) {
        if (dot_ >= 2U) {
            status_ |= 0x40U;
        } else {
            // Earliest sprite-0 hit is dot 2
            sprite_zero_hit_latched_ = true;
        }
    }

    uint8_t palette_index = 0U;
    if (bg_pixel == 0U && fg_pixel == 0U) {
        palette_index = 0U;
    } else if (bg_pixel == 0U && fg_pixel != 0U) {
        palette_index = static_cast<uint8_t>((fg_palette << 2U) | fg_pixel);
    } else if (bg_pixel != 0U && fg_pixel == 0U) {
        palette_index = static_cast<uint8_t>((bg_palette << 2U) | bg_pixel);
    } else {
        if (fg_priority == 0U) {
            palette_index = static_cast<uint8_t>((fg_palette << 2U) | fg_pixel);
        } else {
            palette_index = static_cast<uint8_t>((bg_palette << 2U) | bg_pixel);
        }
    }

    uint8_t color_id = palette_ram_[map_palette_addr(palette_index)];
    if ((mask_ & 0x01U) != 0U) {
        color_id &= 0x30U;
    }

    const uint8_t emphasis = static_cast<uint8_t>((mask_ >> 5U) & 0x07U);
    framebuffer_[static_cast<size_t>(scanline_) * kScreenWidth + x] =
        palette_rgba(color_id, emphasis, palette_);
}

void Ppu::step_dot() noexcept {
    // Advance (scanline_, dot_) by 1 dot, handling odd-frame pre-render skip.
    if (scanline_ == 261U && dot_ == 339U && (frame_count_ & 1ULL) != 0ULL && rendering_enabled()) {
        scanline_ = 0U;
        dot_ = 0U;
        ++frame_count_;
    } else {
        ++dot_;
        if (dot_ >= kDotsPerScanline) {
            dot_ = 0U;
            ++scanline_;
            if (scanline_ >= kScanlinesPerFrame) {
                scanline_ = 0U;
                ++frame_count_;
            }
        }
    }
    ++total_dots_;

    if (scanline_ < 240U) {
        // Visible scanlines (0..239)
        if (dot_ >= 1U && dot_ <= 256U) {
            render_pixel();
            if (rendering_enabled()) {
                update_background_shifters();
                run_background_fetch_step(static_cast<uint16_t>((dot_ - 1U) & 7U));
                if (dot_ == 256U) {
                    increment_scroll_y();
                }
            }
        } else if (dot_ == 257U) {
            if (rendering_enabled()) {
                transfer_address_x();
                evaluate_sprites_for_next_scanline(scanline_);
                oam_addr_ = 0U;
            } else {
                active_sprite_count_ = 0U;
            }
        } else if (dot_ >= 258U && dot_ <= 320U) {
            if (rendering_enabled()) {
                oam_addr_ = 0U;
                if (dot_ == 260U && uses_pattern_table_1000() && scanline_irq_cb_) {
                    scanline_irq_cb_(scanline_irq_user_);
                }
            }
        } else if (dot_ >= 321U && dot_ <= 336U) {
            if (rendering_enabled()) {
                update_background_shifters();
                run_background_fetch_step(static_cast<uint16_t>((dot_ - 1U) & 7U));
            }
        } else if (dot_ == 338U || dot_ == 340U) {
            if (rendering_enabled()) {
                bg_next_tile_id_ = read_vram(static_cast<uint16_t>(0x2000U | (v_ & 0x0FFFU)));
            }
        }
    } else if (scanline_ == 241U && dot_ == 1U) {
        // Start of VBlank
        frame_complete_ = true;
        if (!suppress_vbl_) {
            status_ |= 0x80U;
            if ((ctrl_ & 0x80U) != 0U && !suppress_nmi_) {
                nmi_pending_ = true;
            }
        }
        suppress_vbl_ = false;
        suppress_nmi_ = false;
    } else if (scanline_ == 261U) {
        // Pre-render scanline (261)
        if (dot_ == 1U) {
            status_ &= ~0xE0U;
            nmi_pending_ = false;
            suppress_vbl_ = false;
            suppress_nmi_ = false;
            sprite_zero_hit_latched_ = false;
        }
        if (rendering_enabled()) {
            if (dot_ >= 1U && dot_ <= 256U) {
                update_background_shifters();
                run_background_fetch_step(static_cast<uint16_t>((dot_ - 1U) & 7U));
                if (dot_ == 256U) {
                    increment_scroll_y();
                }
            } else if (dot_ == 257U) {
                transfer_address_x();
                active_sprite_count_ = 0U;
                oam_addr_ = 0U;
            } else if (dot_ >= 258U && dot_ <= 320U) {
                oam_addr_ = 0U;
                if (dot_ >= 280U && dot_ <= 304U) {
                    transfer_address_y();
                }
            } else if (dot_ >= 321U && dot_ <= 336U) {
                update_background_shifters();
                run_background_fetch_step(static_cast<uint16_t>((dot_ - 1U) & 7U));
            } else if (dot_ == 338U || dot_ == 340U) {
                bg_next_tile_id_ = read_vram(static_cast<uint16_t>(0x2000U | (v_ & 0x0FFFU)));
            }
        }
    }
}

void Ppu::step_dots(uint64_t dots) noexcept {
    for (uint64_t i = 0ULL; i < dots; ++i) {
        step_dot();
    }
}

uint32_t Ppu::dots_until_next_event() const noexcept {
    if (nmi_pending_ || frame_complete_) {
        return 0U;
    }

    // If MMC3 scanline IRQ watch is active during visible rendering, wake up at dot 260.
    if (scanline_irq_watch_ && rendering_enabled() && uses_pattern_table_1000()) {
        if (scanline_ < 240U) {
            if (dot_ < 260U) {
                return static_cast<uint32_t>(260U - dot_);
            }
            if (scanline_ < 239U) {
                return static_cast<uint32_t>((kDotsPerScanline - dot_) + 260U);
            }
        } else if (scanline_ == 261U && dot_ > 1U) {
            return static_cast<uint32_t>((kDotsPerScanline - dot_) + 260U);
        }
    }

    const uint32_t cur = static_cast<uint32_t>(scanline_) * kDotsPerScanline + dot_;
    constexpr uint32_t kVBlankDot = 241U * kDotsPerScanline + 1U;
    constexpr uint32_t kPreRenderDot = 261U * kDotsPerScanline + 1U;
    constexpr uint32_t kFrameDots = 262U * kDotsPerScanline;

    if (cur < kVBlankDot) {
        return kVBlankDot - cur;
    }
    if (cur < kPreRenderDot) {
        return kPreRenderDot - cur;
    }
    return (kFrameDots - cur) + 1U;
}

void Ppu::save_snapshot(Snapshot& snap) const noexcept {
    snap.ctrl = ctrl_;
    snap.mask = mask_;
    snap.status = status_;
    snap.oam_addr = oam_addr_;
    snap.read_buffer = read_buffer_;
    snap.open_bus = open_bus_;
    snap.v = v_;
    snap.t = t_;
    snap.x = x_;
    snap.w = w_;
    snap.scanline = scanline_;
    snap.dot = dot_;
    snap.frame_count = frame_count_;
    snap.total_dots = total_dots_;
    snap.nmi_pending = nmi_pending_;
    snap.frame_complete = frame_complete_;
    snap.suppress_vbl = suppress_vbl_;
    snap.suppress_nmi = suppress_nmi_;
    snap.sprite_zero_hit_latched = sprite_zero_hit_latched_;
    snap.bg_next_tile_id = bg_next_tile_id_;
    snap.bg_next_tile_attr = bg_next_tile_attr_;
    snap.bg_next_tile_lsb = bg_next_tile_lsb_;
    snap.bg_next_tile_msb = bg_next_tile_msb_;
    snap.bg_shift_pat_lo = bg_shift_pat_lo_;
    snap.bg_shift_pat_hi = bg_shift_pat_hi_;
    snap.bg_shift_attr_lo = bg_shift_attr_lo_;
    snap.bg_shift_attr_hi = bg_shift_attr_hi_;
    snap.mirroring = mirroring_;
    snap.chr_is_ram = chr_is_ram_;
    snap.chr_bank_offsets = chr_bank_offsets_;
    snap.ciram = ciram_;
    snap.palette_ram = palette_ram_;
    snap.oam = oam_;
    snap.chr_ram = chr_ram_;
}

void Ppu::load_snapshot(const Snapshot& snap) noexcept {
    ctrl_ = snap.ctrl;
    mask_ = snap.mask;
    status_ = snap.status;
    oam_addr_ = snap.oam_addr;
    read_buffer_ = snap.read_buffer;
    open_bus_ = snap.open_bus;
    v_ = snap.v;
    t_ = snap.t;
    x_ = snap.x;
    w_ = snap.w;
    scanline_ = snap.scanline;
    dot_ = snap.dot;
    frame_count_ = snap.frame_count;
    total_dots_ = snap.total_dots;
    nmi_pending_ = snap.nmi_pending;
    frame_complete_ = snap.frame_complete;
    suppress_vbl_ = snap.suppress_vbl;
    suppress_nmi_ = snap.suppress_nmi;
    sprite_zero_hit_latched_ = snap.sprite_zero_hit_latched;
    bg_next_tile_id_ = snap.bg_next_tile_id;
    bg_next_tile_attr_ = snap.bg_next_tile_attr;
    bg_next_tile_lsb_ = snap.bg_next_tile_lsb;
    bg_next_tile_msb_ = snap.bg_next_tile_msb;
    bg_shift_pat_lo_ = snap.bg_shift_pat_lo;
    bg_shift_pat_hi_ = snap.bg_shift_pat_hi;
    bg_shift_attr_lo_ = snap.bg_shift_attr_lo;
    bg_shift_attr_hi_ = snap.bg_shift_attr_hi;
    mirroring_ = snap.mirroring;
    chr_is_ram_ = snap.chr_is_ram;
    chr_bank_offsets_ = snap.chr_bank_offsets;
    ciram_ = snap.ciram;
    palette_ram_ = snap.palette_ram;
    oam_ = snap.oam;
    chr_ram_ = snap.chr_ram;
}

}  // namespace dragon::nes
