#include "gb/ppu.h"

#include <algorithm>
#include <cstring>

#include "gb/bus.h"

namespace dragon::gb {

namespace {

constexpr uint32_t kDmgGreyscale[4] = {
    0xFFFFFFFFU,  // White (0)
    0xFFAAAAAAU,  // Light Gray (1)
    0xFF555555U,  // Dark Gray (2)
    0xFF000000U   // Black (3)
};

constexpr uint32_t kDmgClassicGreen[4] = {
    0xFF9BBC0FU,  // Lightest Green (0)
    0xFF8BAC0FU,  // Light Green (1)
    0xFF306230U,  // Dark Green (2)
    0xFF0F380FU   // Darkest Green (3)
};

}  // namespace

Ppu::Ppu() noexcept {
    reset();
}

void Ppu::reset() noexcept {
    dot_ = 0;
    ly_ = 0;
    lyc_ = 0;
    lcdc_ = 0x91U;
    stat_ = 0x85U;
    scy_ = 0;
    scx_ = 0;
    wy_ = 0;
    wx_ = 0;
    window_line_ = 0;
    frame_complete_ = false;
    stat_irq_line_ = false;

    bgp_ = 0xFCU;
    obp0_ = 0xFFU;
    obp1_ = 0xFFU;

    bcps_ = 0;
    ocps_ = 0;
    cgb_bg_pal_.fill(0xFF);
    cgb_obj_pal_.fill(0xFF);

    framebuffer_.fill(0xFFFFFFFFU);
    bg_color_ids_.fill(0);
    bg_priority_flags_.fill(0);
}

uint32_t Ppu::color_to_rgba_dmg(uint8_t color_id, uint8_t palette_reg) const noexcept {
    uint8_t shade = (palette_reg >> (color_id * 2U)) & 0x03U;
    return classic_palette_ ? kDmgClassicGreen[shade] : kDmgGreyscale[shade];
}

uint32_t Ppu::color_to_rgba_cgb(uint8_t palette_idx, uint8_t color_id, bool is_obj) const noexcept {
    const auto& pal = is_obj ? cgb_obj_pal_ : cgb_bg_pal_;
    size_t offset = (palette_idx * 8U) + (color_id * 2U);
    uint16_t bgr =
        static_cast<uint16_t>(pal[offset] | (static_cast<uint16_t>(pal[offset + 1U]) << 8U));

    uint32_t r5 = bgr & 0x1FU;
    uint32_t g5 = (bgr >> 5U) & 0x1FU;
    uint32_t b5 = (bgr >> 10U) & 0x1FU;

    uint32_t r = (r5 * 255U) / 31U;
    uint32_t g = (g5 * 255U) / 31U;
    uint32_t b = (b5 * 255U) / 31U;

    return 0xFF000000U | (b << 16U) | (g << 8U) | r;
}

uint8_t Ppu::read_register(uint16_t addr) const noexcept {
    switch (addr) {
        case 0xFF40:
            return lcdc_;
        case 0xFF41:
            return stat_ | 0x80U;
        case 0xFF42:
            return scy_;
        case 0xFF43:
            return scx_;
        case 0xFF44:
            return ly_;
        case 0xFF45:
            return lyc_;
        case 0xFF47:
            return bgp_;
        case 0xFF48:
            return obp0_;
        case 0xFF49:
            return obp1_;
        case 0xFF4A:
            return wy_;
        case 0xFF4B:
            return wx_;
        case 0xFF68:
            return is_cgb_ ? (bcps_ | 0x40U) : 0xFFU;
        case 0xFF69:
            return is_cgb_ ? cgb_bg_pal_[bcps_ & 0x3FU] : 0xFFU;
        case 0xFF6A:
            return is_cgb_ ? (ocps_ | 0x40U) : 0xFFU;
        case 0xFF6B:
            return is_cgb_ ? cgb_obj_pal_[ocps_ & 0x3FU] : 0xFFU;
        default:
            return 0xFF;
    }
}

void Ppu::write_register(uint16_t addr, uint8_t value, Bus* bus) noexcept {
    switch (addr) {
        case 0xFF40: {
            bool was_on = (lcdc_ & 0x80U) != 0;
            bool is_on = (value & 0x80U) != 0;
            lcdc_ = value;
            if (was_on && !is_on) {
                // Turning LCD off resets LY to 0 and Mode to 0
                ly_ = 0;
                dot_ = 0;
                window_line_ = 0;
                stat_irq_line_ = false;
                stat_ &= ~0x03U;
                framebuffer_.fill(0xFFFFFFFFU);
            } else if (!was_on && is_on) {
                dot_ = 0;
                ly_ = 0;
                window_line_ = 0;
                stat_irq_line_ = false;
                stat_ = (stat_ & ~0x03U) | 0x02U;
                update_stat_interrupts(bus);
            }
            break;
        }
        case 0xFF41:
            // Bits 0-2 are read-only, bit 7 is unused (always 1)
            stat_ = (stat_ & 0x07U) | (value & 0x78U);
            update_stat_interrupts(bus);
            break;
        case 0xFF42:
            scy_ = value;
            break;
        case 0xFF43:
            scx_ = value;
            break;
        case 0xFF44:
            ly_ = 0;
            break;  // Writing to LY resets it
        case 0xFF45:
            lyc_ = value;
            update_stat_interrupts(bus);
            break;
        case 0xFF47:
            bgp_ = value;
            break;
        case 0xFF48:
            obp0_ = value;
            break;
        case 0xFF49:
            obp1_ = value;
            break;
        case 0xFF4A:
            wy_ = value;
            break;
        case 0xFF4B:
            wx_ = value;
            break;
        case 0xFF68:
            if (is_cgb_)
                bcps_ = value;
            break;
        case 0xFF69:
            if (is_cgb_) {
                cgb_bg_pal_[bcps_ & 0x3FU] = value;
                if (bcps_ & 0x80U) {  // Auto-increment
                    bcps_ = (bcps_ & 0x80U) | ((bcps_ + 1U) & 0x3FU);
                }
            }
            break;
        case 0xFF6A:
            if (is_cgb_)
                ocps_ = value;
            break;
        case 0xFF6B:
            if (is_cgb_) {
                cgb_obj_pal_[ocps_ & 0x3FU] = value;
                if (ocps_ & 0x80U) {  // Auto-increment
                    ocps_ = (ocps_ & 0x80U) | ((ocps_ + 1U) & 0x3FU);
                }
            }
            break;
        default:
            break;
    }
}

void Ppu::update_stat_interrupts(Bus* bus) noexcept {
    if (!bus)
        return;

    bool lyc_match = (ly_ == lyc_);
    if (lyc_match) {
        stat_ |= 0x04U;  // Coincidence flag
    } else {
        stat_ &= ~0x04U;
    }

    // Pan Docs (https://gbdev.io/pandocs/Interrupt_Sources.html#int-48--stat-interrupt):
    // The STAT interrupt is triggered only on a rising edge (0 -> 1) of the internal STAT line.
    uint8_t mode = stat_ & 0x03U;
    bool stat_line = false;

    if (lyc_match && (stat_ & 0x40U))
        stat_line = true;
    if (mode == 2 && (stat_ & 0x20U))
        stat_line = true;
    if (mode == 1 && (stat_ & 0x10U))
        stat_line = true;
    if (mode == 0 && (stat_ & 0x08U))
        stat_line = true;

    if (stat_line && !stat_irq_line_) {
        bus->request_interrupt(1);  // LCD STAT interrupt
    }
    stat_irq_line_ = stat_line;
}

void Ppu::advance(uint32_t t_cycles, Bus* bus) noexcept {
    if (!(lcdc_ & 0x80U)) {
        return;  // LCD is disabled
    }

    // Step through each PPU mode boundary (80, 252, 456) so large cycle deltas from
    // recompiled basic blocks never skip Mode 3 scanline rendering or Mode 0 HBlank/HDMA.
    uint32_t remaining = t_cycles;
    while (remaining > 0U) {
        uint16_t next_boundary = kDotsPerScanline;
        if (ly_ < 144U) {
            if (dot_ < 80U) {
                next_boundary = 80U;
            } else if (dot_ < 252U) {
                next_boundary = 252U;
            }
        }

        const uint32_t to_boundary = static_cast<uint32_t>(next_boundary - dot_);
        const uint32_t step = (remaining < to_boundary) ? remaining : to_boundary;
        dot_ = static_cast<uint16_t>(dot_ + step);
        remaining -= step;

        if (dot_ >= kDotsPerScanline) {
            dot_ = 0;
            ly_ = static_cast<uint8_t>((ly_ + 1U) % kScanlinesPerFrame);

            if (ly_ == 0U) {
                window_line_ = 0;
            }

            if (ly_ == 144U) {
                // Enter VBlank (Mode 1)
                stat_ = (stat_ & ~0x03U) | 0x01U;
                frame_complete_ = true;
                if (bus) {
                    bus->request_interrupt(0);  // VBlank interrupt
                }
                update_stat_interrupts(bus);
            } else if (ly_ < 144U) {
                // Visible scanline: Mode 2 (OAM Search)
                stat_ = (stat_ & ~0x03U) | 0x02U;
                update_stat_interrupts(bus);
            } else {
                // VBlank scanlines 145..153: still Mode 1, check LYC=LY
                update_stat_interrupts(bus);
            }
        } else if (ly_ < 144U) {
            if (dot_ == 80U) {
                stat_ = (stat_ & ~0x03U) | 0x03U;  // Mode 3: Drawing
                render_scanline(bus);
            } else if (dot_ == 252U) {
                stat_ = (stat_ & ~0x03U);  // Mode 0: HBlank
                if (bus) {
                    bus->step_hdma();
                }
                update_stat_interrupts(bus);
            }
        }
    }
}

void Ppu::render_scanline(Bus* bus) noexcept {
    if (!bus)
        return;

    bg_color_ids_.fill(0);
    bg_priority_flags_.fill(0);

    render_background_and_window(bus);
    render_sprites(bus);
}

void Ppu::render_background_and_window(Bus* bus) noexcept {
    uint32_t* fb_row = &framebuffer_[static_cast<size_t>(ly_) * kScreenWidth];

    bool bg_window_enable = (lcdc_ & 0x01U) != 0;
    bool window_enable = (lcdc_ & 0x20U) != 0;

    // In DMG, if bit 0 is 0, background and window are blank (white)
    if (!is_cgb_ && !bg_window_enable) {
        uint32_t blank_color = classic_palette_ ? kDmgClassicGreen[0] : kDmgGreyscale[0];
        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            fb_row[x] = blank_color;
            bg_color_ids_[x] = 0;
        }
        return;
    }

    uint16_t bg_map_base = (lcdc_ & 0x08U) ? 0x9C00U : 0x9800U;
    uint16_t win_map_base = (lcdc_ & 0x40U) ? 0x9C00U : 0x9800U;
    bool signed_tile_data = !(lcdc_ & 0x10U);

    bool window_active_on_line = window_enable && (ly_ >= wy_) && (wx_ <= 166);
    bool window_drawn = false;

    for (uint32_t x = 0; x < kScreenWidth; ++x) {
        bool in_window =
            window_active_on_line && (static_cast<int>(x) >= (static_cast<int>(wx_) - 7));
        uint16_t map_base;
        uint16_t tile_x;
        uint16_t tile_y;

        if (in_window) {
            map_base = win_map_base;
            tile_x = static_cast<uint16_t>(static_cast<int>(x) - (static_cast<int>(wx_) - 7));
            tile_y = window_line_;
            window_drawn = true;
        } else {
            map_base = bg_map_base;
            tile_x = (x + scx_) & 0xFFU;
            tile_y = (ly_ + scy_) & 0xFFU;
        }

        uint16_t map_addr = map_base + ((tile_y / 8U) * 32U) + (tile_x / 8U);
        uint8_t tile_idx = bus->read_vram(0, map_addr);

        uint8_t attr = is_cgb_ ? bus->read_vram(1, map_addr) : 0;

        uint8_t pal_num = is_cgb_ ? (attr & 0x07U) : 0;
        bool vram_bank1 = is_cgb_ && ((attr & 0x08U) != 0);
        bool x_flip = is_cgb_ && ((attr & 0x20U) != 0);
        bool y_flip = is_cgb_ && ((attr & 0x40U) != 0);
        bool priority = is_cgb_ && ((attr & 0x80U) != 0);

        uint16_t row_in_tile = tile_y % 8U;
        if (y_flip)
            row_in_tile = 7U - row_in_tile;

        uint16_t data_addr;
        if (signed_tile_data) {
            int8_t signed_idx = static_cast<int8_t>(tile_idx);
            data_addr = static_cast<uint16_t>(0x9000U + (signed_idx * 16));
        } else {
            data_addr = static_cast<uint16_t>(0x8000U + (tile_idx * 16U));
        }
        data_addr += row_in_tile * 2U;

        uint8_t byte0 = bus->read_vram(vram_bank1 ? 1 : 0, data_addr);
        uint8_t byte1 = bus->read_vram(vram_bank1 ? 1 : 0, static_cast<uint16_t>(data_addr + 1U));

        uint8_t bit_pos = 7U - (tile_x % 8U);
        if (x_flip)
            bit_pos = tile_x % 8U;

        uint8_t color_id =
            static_cast<uint8_t>(((byte0 >> bit_pos) & 1U) | (((byte1 >> bit_pos) & 1U) << 1U));
        bg_color_ids_[x] = color_id;
        bg_priority_flags_[x] = priority ? 1 : 0;

        if (is_cgb_) {
            fb_row[x] = color_to_rgba_cgb(pal_num, color_id, false);
        } else {
            fb_row[x] = color_to_rgba_dmg(color_id, bgp_);
        }
    }

    if (window_drawn) {
        ++window_line_;
    }
}

void Ppu::render_sprites(Bus* bus) noexcept {
    if (!(lcdc_ & 0x02U)) {
        return;  // Sprites disabled
    }

    uint32_t* fb_row = &framebuffer_[static_cast<size_t>(ly_) * kScreenWidth];
    uint8_t sprite_height = (lcdc_ & 0x04U) ? 16U : 8U;

    struct SpriteEntry {
        int y;
        int x;
        uint8_t tile;
        uint8_t attr;
        uint8_t oam_idx;
    };

    std::array<SpriteEntry, 10> visible_sprites{};
    size_t sprite_count = 0;

    // Scan OAM for sprites covering current scanline (max 10)
    for (uint8_t i = 0; i < 40 && sprite_count < 10; ++i) {
        uint16_t base = 0xFE00U + (i * 4U);
        int y = static_cast<int>(bus->peek(base)) - 16;
        int x = static_cast<int>(bus->peek(static_cast<uint16_t>(base + 1U))) - 8;
        uint8_t tile = bus->peek(static_cast<uint16_t>(base + 2U));
        uint8_t attr = bus->peek(static_cast<uint16_t>(base + 3U));

        if (static_cast<int>(ly_) >= y && static_cast<int>(ly_) < (y + sprite_height)) {
            visible_sprites[sprite_count++] = {y, x, tile, attr, i};
        }
    }

    // DMG: lower X coordinate has higher priority; if equal, lower OAM index wins.
    // CGB: lower OAM index always has higher priority.
    // In both cases, drawing back-to-front (lowest priority to highest priority) gives the correct
    // result.
    if (!is_cgb_) {
        std::stable_sort(visible_sprites.begin(), visible_sprites.begin() + sprite_count,
                         [](const SpriteEntry& a, const SpriteEntry& b) { return a.x > b.x; });
    } else {
        std::reverse(visible_sprites.begin(), visible_sprites.begin() + sprite_count);
    }

    for (size_t s = 0; s < sprite_count; ++s) {
        const auto& sp = visible_sprites[s];
        uint16_t row_in_tile = static_cast<uint16_t>(static_cast<int>(ly_) - sp.y);
        if (sp.attr & 0x40U) {  // Y-flip
            row_in_tile = (sprite_height - 1U) - row_in_tile;
        }

        uint8_t tile_idx = sp.tile;
        if (sprite_height == 16) {
            tile_idx &= 0xFEU;
        }

        uint16_t data_addr = 0x8000U + (tile_idx * 16U) + (row_in_tile * 2U);
        uint8_t sp_vram_bank = (is_cgb_ && (sp.attr & 0x08U)) ? 1 : 0;
        uint8_t byte0 = bus->read_vram(sp_vram_bank, data_addr);
        uint8_t byte1 = bus->read_vram(sp_vram_bank, static_cast<uint16_t>(data_addr + 1U));

        bool x_flip = (sp.attr & 0x20U) != 0;
        bool behind_bg = (sp.attr & 0x80U) != 0;
        uint8_t pal_num = sp.attr & 0x07U;
        uint8_t dmg_pal = (sp.attr & 0x10U) ? obp1_ : obp0_;

        for (int px = 0; px < 8; ++px) {
            int screen_x = sp.x + px;
            if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) {
                continue;
            }

            uint8_t bit_pos = 7U - px;
            if (x_flip)
                bit_pos = px;

            uint8_t color_id =
                static_cast<uint8_t>(((byte0 >> bit_pos) & 1U) | (((byte1 >> bit_pos) & 1U) << 1U));
            if (color_id == 0) {
                continue;  // Transparent pixel
            }

            // Priority evaluation
            bool bg_has_priority = false;
            if (is_cgb_) {
                if (!(lcdc_ & 0x01U)) {
                    bg_has_priority = false;  // In CGB, if LCDC bit 0 is 0, sprites always on top
                } else if (bg_priority_flags_[screen_x]) {
                    bg_has_priority = (bg_color_ids_[screen_x] != 0);
                } else if (behind_bg) {
                    bg_has_priority = (bg_color_ids_[screen_x] != 0);
                }
            } else {
                if (behind_bg && bg_color_ids_[screen_x] != 0) {
                    bg_has_priority = true;
                }
            }

            if (!bg_has_priority) {
                if (is_cgb_) {
                    fb_row[screen_x] = color_to_rgba_cgb(pal_num, color_id, true);
                } else {
                    fb_row[screen_x] = color_to_rgba_dmg(color_id, dmg_pal);
                }
            }
        }
    }
}

void Ppu::save_snapshot(Snapshot& snap) const noexcept {
    snap.is_cgb = is_cgb_;
    snap.classic_palette = classic_palette_;
    snap.frame_complete = frame_complete_;
    snap.dot = dot_;
    snap.ly = ly_;
    snap.lyc = lyc_;
    snap.lcdc = lcdc_;
    snap.stat = stat_;
    snap.scy = scy_;
    snap.scx = scx_;
    snap.wy = wy_;
    snap.wx = wx_;
    snap.window_line = window_line_;
    snap.bgp = bgp_;
    snap.obp0 = obp0_;
    snap.obp1 = obp1_;
    snap.bcps = bcps_;
    snap.ocps = ocps_;
    snap.cgb_bg_pal = cgb_bg_pal_;
    snap.cgb_obj_pal = cgb_obj_pal_;
}

void Ppu::load_snapshot(const Snapshot& snap) noexcept {
    is_cgb_ = snap.is_cgb;
    classic_palette_ = snap.classic_palette;
    frame_complete_ = snap.frame_complete;
    dot_ = snap.dot;
    ly_ = snap.ly;
    lyc_ = snap.lyc;
    lcdc_ = snap.lcdc;
    stat_ = snap.stat;
    scy_ = snap.scy;
    scx_ = snap.scx;
    wy_ = snap.wy;
    wx_ = snap.wx;
    window_line_ = snap.window_line;
    bgp_ = snap.bgp;
    obp0_ = snap.obp0;
    obp1_ = snap.obp1;
    bcps_ = snap.bcps;
    ocps_ = snap.ocps;
    cgb_bg_pal_ = snap.cgb_bg_pal;
    cgb_obj_pal_ = snap.cgb_obj_pal;
}

}  // namespace dragon::gb
