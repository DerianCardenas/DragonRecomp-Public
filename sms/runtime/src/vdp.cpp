#include "sms/vdp.h"

#include <algorithm>

namespace dragon::sms {

namespace {

inline uint32_t pack_rgba32(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return 0xFF000000U | (static_cast<uint32_t>(b) << 16U) |
           (static_cast<uint32_t>(g) << 8U) | static_cast<uint32_t>(r);
}

}  // namespace

Vdp::Vdp() {
    reset(false);
}

void Vdp::reset(bool game_gear_mode) noexcept {
    st_ = {};
    game_gear_ = game_gear_mode;
    frame_ready_ = false;
    st_.line_counter = 0xFFU;

    // Standard post-BIOS VDP register defaults so games boot without an external BIOS
    st_.regs[0] = 0x36U;
    st_.regs[1] = 0xA0U;
    st_.regs[2] = 0xFFU;
    st_.regs[3] = 0xFFU;
    st_.regs[4] = 0xFFU;
    st_.regs[5] = 0xFFU;
    st_.regs[6] = 0xFBU;
    st_.regs[7] = 0x00U;
    st_.regs[8] = 0x00U;
    st_.regs[9] = 0x00U;
    st_.regs[10] = 0xFFU;

    fb_.fill(pack_rgba32(0, 0, 0));
}

bool Vdp::is_224_mode() const noexcept {
    // 224-line Mode 4 on SMS2: M4 (reg0 bit 2) && M2 (reg0 bit 1) && M1 (reg1 bit 4)
    return !game_gear_ && ((st_.regs[0] & 0x06U) == 0x06U) && ((st_.regs[1] & 0x10U) != 0U) &&
           ((st_.regs[1] & 0x08U) == 0U);
}

int Vdp::visible_height() const noexcept {
    if (game_gear_) {
        return kGgHeight;
    }
    return is_224_mode() ? kMaxHeight : kSmsHeight;
}

uint32_t Vdp::cram_rgba32(uint8_t color_index) const noexcept {
    const uint8_t idx = color_index & 0x1FU;
    if (game_gear_) {
        // 12-bit GG CRAM: byte 0 = GGGGRRRR, byte 1 = ----BBBB
        const uint8_t lo = st_.cram[static_cast<size_t>(idx) * 2U];
        const uint8_t hi = st_.cram[static_cast<size_t>(idx) * 2U + 1U];
        const uint8_t r = static_cast<uint8_t>((lo & 0x0FU) * 17U);
        const uint8_t g = static_cast<uint8_t>(((lo >> 4U) & 0x0FU) * 17U);
        const uint8_t b = static_cast<uint8_t>((hi & 0x0FU) * 17U);
        return pack_rgba32(r, g, b);
    }
    // 6-bit SMS CRAM: --BBGGRR
    const uint8_t v = st_.cram[idx];
    const uint8_t r = static_cast<uint8_t>((v & 0x03U) * 85U);
    const uint8_t g = static_cast<uint8_t>(((v >> 2U) & 0x03U) * 85U);
    const uint8_t b = static_cast<uint8_t>(((v >> 4U) & 0x03U) * 85U);
    return pack_rgba32(r, g, b);
}

uint8_t Vdp::read_data() noexcept {
    st_.second_byte = 0U;
    const uint8_t ret = st_.read_buffer;
    st_.read_buffer = st_.vram[st_.addr & 0x3FFFU];
    st_.addr = static_cast<uint16_t>((st_.addr + 1U) & 0x3FFFU);
    return ret;
}

void Vdp::write_data(uint8_t value) noexcept {
    st_.second_byte = 0U;
    st_.read_buffer = value;

    if (st_.code == 3U) {
        if (game_gear_) {
            // Game Gear CRAM write latch (https://www.smspower.org/Development/GameGear):
            // Even addresses latch low byte; odd addresses commit 12-bit pair to CRAM.
            if ((st_.addr & 1U) == 0U) {
                st_.cram_latch = value;
            } else {
                const size_t base = static_cast<size_t>(st_.addr & 0x3EU);
                st_.cram[base] = st_.cram_latch;
                st_.cram[base + 1U] = static_cast<uint8_t>(value & 0x0FU);
            }
        } else {
            st_.cram[st_.addr & 0x1FU] = static_cast<uint8_t>(value & 0x3FU);
        }
    } else {
        st_.vram[st_.addr & 0x3FFFU] = value;
    }

    st_.addr = static_cast<uint16_t>((st_.addr + 1U) & 0x3FFFU);
}

uint8_t Vdp::read_status() noexcept {
    const uint8_t ret = st_.status;
    st_.status = static_cast<uint8_t>(st_.status & 0x1FU);  // Clear Frame INT, Overflow, Collision
    st_.line_irq_pending = 0U;
    st_.second_byte = 0U;
    return ret;
}

void Vdp::write_control(uint8_t value) noexcept {
    if (st_.second_byte == 0U) {
        st_.addr_latch = value;
        st_.addr = static_cast<uint16_t>((st_.addr & 0x3F00U) | value);
        st_.second_byte = 1U;
    } else {
        st_.second_byte = 0U;
        st_.code = static_cast<uint8_t>((value >> 6U) & 0x03U);
        st_.addr =
            static_cast<uint16_t>((static_cast<uint16_t>(value & 0x3FU) << 8U) | st_.addr_latch);

        if (st_.code == 0U) {
            // Code 0: VRAM Read pre-fetch + address auto-increment
            st_.read_buffer = st_.vram[st_.addr & 0x3FFFU];
            st_.addr = static_cast<uint16_t>((st_.addr + 1U) & 0x3FFFU);
        } else if (st_.code == 2U) {
            // Code 2: VDP Register write
            const uint8_t reg = static_cast<uint8_t>(value & 0x0FU);
            st_.regs[reg] = st_.addr_latch;
        }
    }
}

uint8_t Vdp::read_v_counter() const noexcept {
    // NTSC V-Counter jump table (msvdp.txt §8)
    const uint16_t line = st_.scanline;
    if (is_224_mode()) {
        return (line <= 0xEAU) ? static_cast<uint8_t>(line)
                               : static_cast<uint8_t>(line - 6U);
    }
    return (line <= 0xDAU) ? static_cast<uint8_t>(line)
                           : static_cast<uint8_t>(line - 6U);
}

uint8_t Vdp::read_h_counter() const noexcept {
    return st_.h_counter;
}

bool Vdp::irq_asserted() const noexcept {
    const bool frame_irq = ((st_.status & 0x80U) != 0U) && ((st_.regs[1] & 0x20U) != 0U);
    const bool line_irq = (st_.line_irq_pending != 0U) && ((st_.regs[0] & 0x10U) != 0U);
    return frame_irq || line_irq;
}

uint16_t Vdp::nametable_base() const noexcept {
    if (is_224_mode()) {
        return static_cast<uint16_t>(((st_.regs[2] & 0x0CU) << 10U) | 0x0700U);
    }
    return static_cast<uint16_t>((st_.regs[2] & 0x0EU) << 10U);
}

void Vdp::step(uint32_t cycles) noexcept {
    st_.line_cycles += cycles;
    st_.h_counter = static_cast<uint8_t>((st_.line_cycles * 256U) / kCyclesPerScanline);

    while (st_.line_cycles >= kCyclesPerScanline) {
        st_.line_cycles -= kCyclesPerScanline;

        const int active_h = is_224_mode() ? kMaxHeight : kSmsHeight;

        if (st_.scanline == 0U) {
            // Vertical scroll (Reg 9) is latched once at the start of the active frame
            st_.latched_vscroll = st_.regs[9];
        }

        if (st_.scanline < static_cast<uint16_t>(active_h)) {
            render_scanline(static_cast<int>(st_.scanline));
        }

        // Line interrupt counter: decrements during active display AND line == active_h
        if (st_.scanline <= static_cast<uint16_t>(active_h)) {
            if (st_.line_counter == 0U) {
                st_.line_counter = st_.regs[10];
                st_.line_irq_pending = 1U;
            } else {
                st_.line_counter = static_cast<uint8_t>(st_.line_counter - 1U);
            }
        } else {
            st_.line_counter = st_.regs[10];
        }

        // Frame interrupt fires when entering VBlank (line == active_h)
        if (st_.scanline == static_cast<uint16_t>(active_h)) {
            st_.status |= 0x80U;
            frame_ready_ = true;
        }

        st_.scanline = static_cast<uint16_t>((st_.scanline + 1U) % kTotalScanlines);
    }
}

void Vdp::render_scanline(int line) noexcept {
    uint32_t* row = fb_.data() + static_cast<size_t>(line) * kMaxWidth;
    const uint32_t backdrop = cram_rgba32(static_cast<uint8_t>(16U + (st_.regs[7] & 0x0FU)));

    // Display blanked (Reg 1 bit 6 == 0)
    if ((st_.regs[1] & 0x40U) == 0U) {
        std::fill_n(row, kMaxWidth, backdrop);
        return;
    }

    // Per-pixel background priority & color index tracking
    std::array<uint8_t, kMaxWidth> bg_priority{};
    std::array<uint8_t, kMaxWidth> bg_color_idx{};

    const bool lock_top_hscroll = (st_.regs[0] & 0x40U) != 0U;
    const bool lock_right_vscroll = (st_.regs[0] & 0x80U) != 0U;
    const bool mask_col0 = (st_.regs[0] & 0x20U) != 0U;

    const uint8_t hscroll = (lock_top_hscroll && line < 16) ? 0U : st_.regs[8];
    const int map_height_px = is_224_mode() ? 256 : 224;
    const uint16_t nt_base = nametable_base();

    // 1. Render Background (Mode 4, 4bpp planar tiles)
    for (int x = 0; x < kMaxWidth; ++x) {
        const int col = x >> 3;
        const int vscroll = (lock_right_vscroll && col >= 24) ? 0 : static_cast<int>(st_.latched_vscroll);

        const int scrolled_x = (x - static_cast<int>(hscroll)) & 0xFF;
        int scrolled_y = line + vscroll;
        if (scrolled_y >= map_height_px) {
            scrolled_y -= map_height_px;
        }

        const int tile_x = scrolled_x >> 3;
        const int tile_y = scrolled_y >> 3;
        const uint16_t nt_addr =
            static_cast<uint16_t>((nt_base + static_cast<uint16_t>((tile_y * 32 + tile_x) * 2)) & 0x3FFFU);
        const uint16_t entry = static_cast<uint16_t>(
            st_.vram[nt_addr] | (static_cast<uint16_t>(st_.vram[(nt_addr + 1U) & 0x3FFFU]) << 8U));

        const uint16_t tile_idx = entry & 0x01FFU;
        const bool hflip = (entry & 0x0200U) != 0U;
        const bool vflip = (entry & 0x0400U) != 0U;
        const uint8_t pal_bank = (entry & 0x0800U) ? 16U : 0U;
        const bool prio = (entry & 0x1000U) != 0U;

        const int fine_x = hflip ? (scrolled_x & 7) : (7 - (scrolled_x & 7));
        const int fine_y = vflip ? (7 - (scrolled_y & 7)) : (scrolled_y & 7);

        const uint16_t pat_addr = static_cast<uint16_t>((tile_idx * 32U + static_cast<uint16_t>(fine_y * 4)) & 0x3FFFU);
        const uint8_t b0 = (st_.vram[pat_addr] >> fine_x) & 1U;
        const uint8_t b1 = (st_.vram[(pat_addr + 1U) & 0x3FFFU] >> fine_x) & 1U;
        const uint8_t b2 = (st_.vram[(pat_addr + 2U) & 0x3FFFU] >> fine_x) & 1U;
        const uint8_t b3 = (st_.vram[(pat_addr + 3U) & 0x3FFFU] >> fine_x) & 1U;
        const uint8_t color = static_cast<uint8_t>(b0 | (b1 << 1U) | (b2 << 2U) | (b3 << 3U));

        bg_color_idx[static_cast<size_t>(x)] = color;
        bg_priority[static_cast<size_t>(x)] = (prio && color != 0U) ? 1U : 0U;
        row[x] = cram_rgba32(static_cast<uint8_t>(pal_bank + color));
    }

    // 2. Render Sprites (up to 8 visible per scanline, SAT 64 entries)
    const uint16_t sat_base = static_cast<uint16_t>((st_.regs[5] & 0x7EU) << 7U);
    const uint16_t spr_pat_base = static_cast<uint16_t>((st_.regs[6] & 0x04U) << 11U);
    const bool tall_sprites = (st_.regs[1] & 0x02U) != 0U;
    const bool zoomed_sprites = (st_.regs[1] & 0x01U) != 0U;
    const int spr_h = (tall_sprites ? 16 : 8) * (zoomed_sprites ? 2 : 1);
    const int spr_w = 8 * (zoomed_sprites ? 2 : 1);
    const int shift_left = (st_.regs[0] & 0x08U) ? 8 : 0;

    std::array<uint8_t, kMaxWidth> sprite_drawn{};
    int sprites_on_line = 0;

    for (int i = 0; i < 64; ++i) {
        const uint8_t raw_y = st_.vram[(sat_base + static_cast<uint16_t>(i)) & 0x3FFFU];
        if (!is_224_mode() && raw_y == 0xD0U) {
            break;  // End of active sprite list in 192-line mode
        }

        int spr_y = static_cast<int>(raw_y) + 1;
        if (spr_y > 240) {
            spr_y -= 256;  // Wrap top edge cleanly
        }

        if (line < spr_y || line >= spr_y + spr_h) {
            continue;
        }

        if (sprites_on_line >= 8) {
            st_.status |= 0x40U;  // Sprite overflow flag
            break;
        }
        ++sprites_on_line;

        const uint16_t xt_addr = static_cast<uint16_t>((sat_base + 128U + static_cast<uint16_t>(i * 2)) & 0x3FFFU);
        const int spr_x = static_cast<int>(st_.vram[xt_addr]) - shift_left;
        uint8_t tile = st_.vram[(xt_addr + 1U) & 0x3FFFU];
        if (tall_sprites) {
            tile &= 0xFEU;  // Bit 0 ignored for 8x16 sprites
        }

        int row_in_spr = line - spr_y;
        if (zoomed_sprites) {
            row_in_spr >>= 1;
        }

        const uint16_t pat_addr = static_cast<uint16_t>(
            (spr_pat_base + static_cast<uint16_t>(tile) * 32U + static_cast<uint16_t>(row_in_spr) * 4U) & 0x3FFFU);
        const uint8_t p0 = st_.vram[pat_addr];
        const uint8_t p1 = st_.vram[(pat_addr + 1U) & 0x3FFFU];
        const uint8_t p2 = st_.vram[(pat_addr + 2U) & 0x3FFFU];
        const uint8_t p3 = st_.vram[(pat_addr + 3U) & 0x3FFFU];

        for (int px = 0; px < spr_w; ++px) {
            const int dst_x = spr_x + px;
            if (dst_x < 0 || dst_x >= kMaxWidth) {
                continue;
            }
            const int bit = 7 - (zoomed_sprites ? (px >> 1) : px);
            const uint8_t color = static_cast<uint8_t>(
                ((p0 >> bit) & 1U) | (((p1 >> bit) & 1U) << 1U) |
                (((p2 >> bit) & 1U) << 2U) | (((p3 >> bit) & 1U) << 3U));
            if (color == 0U) {
                continue;  // Color 0 is transparent for sprites
            }

            if (sprite_drawn[static_cast<size_t>(dst_x)] != 0U) {
                st_.status |= 0x20U;  // Sprite collision flag
                continue;             // Higher-priority (lower index) sprite wins
            }
            sprite_drawn[static_cast<size_t>(dst_x)] = 1U;

            if (bg_priority[static_cast<size_t>(dst_x)] == 0U) {
                row[dst_x] = cram_rgba32(static_cast<uint8_t>(16U + color));
            }
        }
    }

    // 3. Column 0 mask (Reg 0 bit 5): covers leftmost 8 pixels with backdrop color
    if (mask_col0) {
        std::fill_n(row, 8, backdrop);
    }
}

}  // namespace dragon::sms
