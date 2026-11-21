#include "md/vdp.h"

#include <algorithm>
#include <cstring>

#include "md/bus.h"

namespace dragon::md {

namespace {

// 3-bit Genesis color (0..7) -> 8-bit RGB ramps for Normal, Shadow (half), and Highlight
constexpr uint8_t kNormalLut[8] = {0, 52, 87, 116, 144, 172, 206, 255};
constexpr uint8_t kShadowLut[8] = {0, 29, 52, 70, 87, 101, 116, 130};
constexpr uint8_t kHighlightLut[8] = {130, 144, 158, 172, 187, 206, 228, 255};

inline uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return 0xFF000000U | (static_cast<uint32_t>(b) << 16U) | (static_cast<uint32_t>(g) << 8U) |
           static_cast<uint32_t>(r);
}

inline uint32_t plane_dim_tiles(uint8_t bits) noexcept {
    switch (bits & 0x03U) {
        case 0:
            return 32U;
        case 1:
            return 64U;
        case 3:
            return 128U;
        default:
            return 32U;
    }
}

}  // namespace

Vdp::Vdp() noexcept {
    reset(false);
}

void Vdp::reset(bool pal) noexcept {
    vram_.fill(0);
    cram_.fill(0);
    vsram_.fill(0);
    regs_.fill(0);
    regs_[0] = 0x04U;
    regs_[1] = 0x04U;   // Mode 5 enabled
    regs_[15] = 0x02U;  // Auto-increment default = 2
    for (uint8_t i = 0; i < 64; ++i) {
        update_cram_cache(i);
    }
    addr_reg_ = 0;
    code_reg_ = 0;
    pending_ctrl_ = false;
    dma_fill_pending_ = false;
    ctrl_latch_ = 0;
    scanline_ = 0;
    line_cycles_ = 0;
    hint_counter_ = 0;
    vint_pending_ = false;
    hint_pending_ = false;
    sprite_overflow_ = false;
    sprite_collision_ = false;
    frame_complete_ = false;
    pal_ = pal;
    framebuffer_.fill(0xFF000000U);
}

void Vdp::update_cram_cache(uint8_t index) noexcept {
    const uint16_t raw = cram_[index & 0x3FU];
    // Genesis CRAM format: 0000 BBB0 GGG0 RRR0
    const uint8_t r3 = static_cast<uint8_t>((raw >> 1U) & 0x07U);
    const uint8_t g3 = static_cast<uint8_t>((raw >> 5U) & 0x07U);
    const uint8_t b3 = static_cast<uint8_t>((raw >> 9U) & 0x07U);
    cram_rgba_normal_[index & 0x3FU] = pack_rgba(kNormalLut[r3], kNormalLut[g3], kNormalLut[b3]);
    cram_rgba_shadow_[index & 0x3FU] = pack_rgba(kShadowLut[r3], kShadowLut[g3], kShadowLut[b3]);
    cram_rgba_highlight_[index & 0x3FU] =
        pack_rgba(kHighlightLut[r3], kHighlightLut[g3], kHighlightLut[b3]);
}

uint32_t Vdp::cram_to_rgba(uint8_t index, uint8_t sh_mode) const noexcept {
    const uint8_t i = index & 0x3FU;
    if (sh_mode == 0U)
        return cram_rgba_shadow_[i];
    if (sh_mode == 2U)
        return cram_rgba_highlight_[i];
    return cram_rgba_normal_[i];
}

uint16_t Vdp::read_control() noexcept {
    pending_ctrl_ = false;
    const uint32_t act_h = active_height();
    const bool in_vblank = (scanline_ >= act_h) || ((regs_[1] & 0x40U) == 0U);
    const bool in_hblank = (line_cycles_ >= 380U);

    uint16_t status = 0x3400U;  // Unused upper bits + FIFO Empty (bit 9)
    if (pal_)
        status |= 0x0001U;
    if (in_hblank)
        status |= 0x0004U;
    if (in_vblank)
        status |= 0x0008U;
    if (sprite_collision_)
        status |= 0x0020U;
    if (sprite_overflow_)
        status |= 0x0040U;
    if (vint_pending_)
        status |= 0x0080U;
    status |= 0x0200U;  // FIFO empty

    sprite_collision_ = false;
    sprite_overflow_ = false;
    return status;
}

uint16_t Vdp::read_hv_counter() const noexcept {
    const uint32_t total_lines = pal_ ? kScanlinesPal : kScanlinesNtsc;
    uint32_t v = scanline_;
    if (!pal_ && v >= 0xEBU) {
        v = (v - total_lines + 0x100U) & 0xFFU;
    } else if (pal_ && v >= 0x103U) {
        v = (v - total_lines + 0x100U) & 0xFFU;
    }
    const uint8_t h = static_cast<uint8_t>((line_cycles_ * 256U) / kCyclesPerScanline);
    return static_cast<uint16_t>(((v & 0xFFU) << 8U) | h);
}

void Vdp::write_control(uint16_t value, Bus* bus) noexcept {
    if (!pending_ctrl_) {
        if ((value & 0xC000U) == 0x8000U) {
            // Register write: 100R RRRR DDDD DDDD
            const uint8_t reg_idx = static_cast<uint8_t>((value >> 8U) & 0x1FU);
            const uint8_t reg_val = static_cast<uint8_t>(value & 0xFFU);
            if (reg_idx < 24U) {
                regs_[reg_idx] = reg_val;
            }
            code_reg_ = 0;
            addr_reg_ = 0;
            pending_ctrl_ = false;
            return;
        }
        ctrl_latch_ = value;
        addr_reg_ = static_cast<uint16_t>((addr_reg_ & 0xC000U) | (value & 0x3FFFU));
        code_reg_ = static_cast<uint8_t>((code_reg_ & 0x3CU) | ((value >> 14U) & 0x03U));
        pending_ctrl_ = true;
    } else {
        pending_ctrl_ = false;
        addr_reg_ = static_cast<uint16_t>((addr_reg_ & 0x3FFFU) | ((value & 0x0003U) << 14U));
        code_reg_ = static_cast<uint8_t>((code_reg_ & 0x03U) | ((value >> 2U) & 0x3CU));

        if ((code_reg_ & 0x20U) && (regs_[1] & 0x10U)) {
            const uint8_t dma_mode = (regs_[23] >> 6U) & 0x03U;
            if (dma_mode < 2U) {
                execute_dma(bus);
            } else if (dma_mode == 2U) {
                dma_fill_pending_ = true;
            } else {
                execute_dma_copy();
            }
        }
    }
}

uint16_t Vdp::read_data() noexcept {
    pending_ctrl_ = false;
    uint16_t out = 0;
    const uint8_t target = code_reg_ & 0x0FU;
    if (target == 0x00U) {
        // VRAM read
        const uint16_t a = addr_reg_ & 0xFFFEU;
        out = static_cast<uint16_t>((static_cast<uint16_t>(vram_[a]) << 8U) | vram_[a + 1U]);
    } else if (target == 0x08U) {
        // CRAM read
        out = cram_[(addr_reg_ >> 1U) & 0x3FU];
    } else if (target == 0x04U) {
        // VSRAM read
        const uint8_t idx = static_cast<uint8_t>((addr_reg_ >> 1U) & 0x3FU);
        out = (idx < 40U) ? vsram_[idx] : 0;
    }
    addr_reg_ = static_cast<uint16_t>(addr_reg_ + regs_[15]);
    return out;
}

void Vdp::write_data(uint16_t value, Bus* /*bus*/) noexcept {
    pending_ctrl_ = false;

    const uint8_t target = code_reg_ & 0x0FU;
    if (target == 0x01U) {
        // VRAM write (if odd address, bytes are swapped per hardware spec)
        if (addr_reg_ & 1U) {
            const uint16_t a = addr_reg_ & 0xFFFEU;
            vram_[a] = static_cast<uint8_t>(value & 0xFFU);
            vram_[a + 1U] = static_cast<uint8_t>(value >> 8U);
        } else {
            vram_[addr_reg_] = static_cast<uint8_t>(value >> 8U);
            vram_[static_cast<uint16_t>(addr_reg_ + 1U)] = static_cast<uint8_t>(value & 0xFFU);
        }
    } else if (target == 0x03U) {
        // CRAM write
        const uint8_t idx = static_cast<uint8_t>((addr_reg_ >> 1U) & 0x3FU);
        cram_[idx] = value & 0x0EEEU;
        update_cram_cache(idx);
    } else if (target == 0x05U) {
        // VSRAM write
        const uint8_t idx = static_cast<uint8_t>((addr_reg_ >> 1U) & 0x3FU);
        if (idx < 40U) {
            vsram_[idx] = value & 0x03FFU;
        }
    }

    if (dma_fill_pending_) {
        dma_fill_pending_ = false;
        execute_dma_fill(value);
        return;
    }

    addr_reg_ = static_cast<uint16_t>(addr_reg_ + regs_[15]);
}

void Vdp::execute_dma(Bus* bus) noexcept {
    if (!bus)
        return;
    uint32_t len = static_cast<uint32_t>(regs_[19]) | (static_cast<uint32_t>(regs_[20]) << 8U);
    if (len == 0U)
        len = 0x10000U;

    const uint32_t src_high = static_cast<uint32_t>(regs_[23] & 0x7FU) << 17U;
    uint32_t src_low =
        (static_cast<uint32_t>(regs_[21]) << 1U) | (static_cast<uint32_t>(regs_[22]) << 9U);

    const uint8_t target = code_reg_ & 0x0FU;
    const uint16_t inc = regs_[15];

    for (uint32_t i = 0; i < len; ++i) {
        const uint32_t src_addr = src_high | (src_low & 0x1FFFFU);
        const uint16_t word = bus->read16(src_addr);
        src_low = (src_low + 2U) & 0x1FFFFU;

        if (target == 0x01U) {
            if (addr_reg_ & 1U) {
                const uint16_t a = addr_reg_ & 0xFFFEU;
                vram_[a] = static_cast<uint8_t>(word & 0xFFU);
                vram_[a + 1U] = static_cast<uint8_t>(word >> 8U);
            } else {
                vram_[addr_reg_] = static_cast<uint8_t>(word >> 8U);
                vram_[static_cast<uint16_t>(addr_reg_ + 1U)] = static_cast<uint8_t>(word & 0xFFU);
            }
        } else if (target == 0x03U) {
            const uint8_t idx = static_cast<uint8_t>((addr_reg_ >> 1U) & 0x3FU);
            cram_[idx] = word & 0x0EEEU;
            update_cram_cache(idx);
        } else if (target == 0x05U) {
            const uint8_t idx = static_cast<uint8_t>((addr_reg_ >> 1U) & 0x3FU);
            if (idx < 40U) {
                vsram_[idx] = word & 0x03FFU;
            }
        }
        addr_reg_ = static_cast<uint16_t>(addr_reg_ + inc);
    }

    regs_[19] = 0;
    regs_[20] = 0;
    regs_[21] = static_cast<uint8_t>((src_low >> 1U) & 0xFFU);
    regs_[22] = static_cast<uint8_t>((src_low >> 9U) & 0xFFU);
}

void Vdp::execute_dma_fill(uint16_t fill_word) noexcept {
    uint32_t len = static_cast<uint32_t>(regs_[19]) | (static_cast<uint32_t>(regs_[20]) << 8U);
    if (len == 0U)
        len = 0x10000U;
    const uint8_t fill_hi = static_cast<uint8_t>(fill_word >> 8U);
    const uint16_t inc = regs_[15];

    // The initial word write already wrote to addr_reg_; now fill `len` bytes with fill_hi
    for (uint32_t i = 0; i < len; ++i) {
        addr_reg_ = static_cast<uint16_t>(addr_reg_ + inc);
        vram_[addr_reg_ ^ 1U] = fill_hi;
    }
    regs_[19] = 0;
    regs_[20] = 0;
}

void Vdp::execute_dma_copy() noexcept {
    uint32_t len = static_cast<uint32_t>(regs_[19]) | (static_cast<uint32_t>(regs_[20]) << 8U);
    if (len == 0U)
        len = 0x10000U;
    uint16_t src = static_cast<uint16_t>(regs_[21]) | (static_cast<uint16_t>(regs_[22]) << 8U);
    const uint16_t inc = regs_[15];

    for (uint32_t i = 0; i < len; ++i) {
        vram_[addr_reg_ ^ 1U] = vram_[src ^ 1U];
        src = static_cast<uint16_t>(src + 1U);
        addr_reg_ = static_cast<uint16_t>(addr_reg_ + inc);
    }
    regs_[19] = 0;
    regs_[20] = 0;
    regs_[21] = static_cast<uint8_t>(src & 0xFFU);
    regs_[22] = static_cast<uint8_t>(src >> 8U);
}

void Vdp::step(uint32_t m68k_cycles, Bus* bus) noexcept {
    line_cycles_ += m68k_cycles;
    const uint32_t total_lines = pal_ ? kScanlinesPal : kScanlinesNtsc;
    const uint32_t act_h = active_height();

    while (line_cycles_ >= kCyclesPerScanline) {
        line_cycles_ -= kCyclesPerScanline;

        if (scanline_ < act_h) {
            render_scanline(scanline_);
            --hint_counter_;
            if (hint_counter_ < 0) {
                hint_counter_ = regs_[10];
                hint_pending_ = true;
            }
        } else {
            hint_counter_ = regs_[10];
        }

        ++scanline_;
        if (scanline_ == act_h) {
            vint_pending_ = true;
            frame_complete_ = true;
            if (bus) {
                bus->z80().assert_irq(true);
            }
        } else if (scanline_ == act_h + 1U) {
            if (bus) {
                bus->z80().assert_irq(false);
            }
        } else if (scanline_ >= total_lines) {
            scanline_ = 0;
        }
    }
}

void Vdp::render_scanline(uint32_t y) noexcept {
    if (y >= kMaxHeight)
        return;
    const uint32_t width = active_width();
    uint32_t* dst = &framebuffer_[y * kMaxWidth];
    const uint8_t bg_idx = regs_[7] & 0x3FU;
    const bool display_on = (regs_[1] & 0x40U) != 0U;

    if (!display_on) {
        const uint32_t bg = cram_rgba_normal_[bg_idx];
        for (uint32_t x = 0; x < width; ++x) {
            dst[x] = bg;
        }
        return;
    }

    const bool sh_enable = (regs_[12] & 0x08U) != 0U;
    const bool h40 = (width == 320U);

    // Per-pixel color index (0..63), priority/layer tag, and shadow/highlight mode
    // (0=sh,1=norm,2=hi)
    std::array<uint8_t, kMaxWidth> px_color{};
    std::array<uint8_t, kMaxWidth>
        px_prio{};  // 0=bg, 1=B_lo, 2=A_lo, 3=S_lo, 4=B_hi, 5=A_hi, 6=S_hi
    std::array<uint8_t, kMaxWidth> px_sh{};
    px_color.fill(bg_idx);
    px_prio.fill(0);
    px_sh.fill(sh_enable ? 0U : 1U);

    const uint32_t plane_w_tiles = plane_dim_tiles(regs_[16] & 0x03U);
    const uint32_t plane_h_tiles = plane_dim_tiles((regs_[16] >> 4U) & 0x03U);
    const uint32_t plane_w_px = plane_w_tiles * 8U;
    const uint32_t plane_h_px = plane_h_tiles * 8U;

    const uint16_t plane_a_base = static_cast<uint16_t>((regs_[2] & 0x38U) << 10U);
    const uint16_t plane_b_base = static_cast<uint16_t>((regs_[4] & 0x07U) << 13U);
    const uint16_t win_base = static_cast<uint16_t>((regs_[3] & (h40 ? 0x3CU : 0x3EU)) << 10U);
    const uint16_t hscroll_base = static_cast<uint16_t>((regs_[13] & 0x3FU) << 10U);

    const uint8_t hscroll_mode = regs_[11] & 0x03U;
    const bool vscroll_2cell = (regs_[11] & 0x04U) != 0U;

    uint32_t hscroll_row = 0;
    if (hscroll_mode == 2U) {
        hscroll_row = (y & ~7U) * 4U;
    } else if (hscroll_mode == 3U) {
        hscroll_row = y * 4U;
    }
    const uint16_t hscroll_a =
        static_cast<uint16_t>(
            (static_cast<uint16_t>(vram_[(hscroll_base + hscroll_row) & 0xFFFFU]) << 8U) |
            vram_[(hscroll_base + hscroll_row + 1U) & 0xFFFFU]) &
        0x03FFU;
    const uint16_t hscroll_b =
        static_cast<uint16_t>(
            (static_cast<uint16_t>(vram_[(hscroll_base + hscroll_row + 2U) & 0xFFFFU]) << 8U) |
            vram_[(hscroll_base + hscroll_row + 3U) & 0xFFFFU]) &
        0x03FFU;

    auto draw_scroll_plane = [&](uint16_t table_base, uint16_t hscroll, uint8_t vsram_off,
                                 uint8_t lo_prio, uint8_t hi_prio) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t col2 = vscroll_2cell ? ((x >> 4U) * 2U + vsram_off) : vsram_off;
            const uint16_t vscroll = (col2 < 40U) ? (vsram_[col2] & 0x03FFU) : 0U;

            const uint32_t sx = (x - hscroll) & (plane_w_px - 1U);
            const uint32_t sy = (y + vscroll) & (plane_h_px - 1U);

            const uint32_t tx = sx >> 3U;
            const uint32_t ty = sy >> 3U;
            const uint16_t ent_addr =
                static_cast<uint16_t>((table_base + (ty * plane_w_tiles + tx) * 2U) & 0xFFFFU);
            const uint16_t entry =
                static_cast<uint16_t>((static_cast<uint16_t>(vram_[ent_addr]) << 8U) |
                                      vram_[static_cast<uint16_t>(ent_addr + 1U)]);

            const bool prio = (entry & 0x8000U) != 0U;
            const uint8_t pal = static_cast<uint8_t>((entry >> 13U) & 0x03U);
            const bool vflip = (entry & 0x1000U) != 0U;
            const bool hflip = (entry & 0x0800U) != 0U;
            const uint16_t tile_idx = entry & 0x07FFU;

            const uint32_t row = vflip ? (7U - (sy & 7U)) : (sy & 7U);
            const uint32_t col = hflip ? (7U - (sx & 7U)) : (sx & 7U);
            const uint16_t tile_addr =
                static_cast<uint16_t>((tile_idx * 32U + row * 4U + (col >> 1U)) & 0xFFFFU);
            const uint8_t byte = vram_[tile_addr];
            const uint8_t pix = (col & 1U) ? (byte & 0x0FU) : (byte >> 4U);

            if (sh_enable && prio) {
                px_sh[x] = 1U;
            }
            if (pix != 0U) {
                const uint8_t layer_p = prio ? hi_prio : lo_prio;
                if (layer_p >= px_prio[x]) {
                    px_prio[x] = layer_p;
                    px_color[x] = static_cast<uint8_t>((pal << 4U) | pix);
                }
            }
        }
    };

    // 1. Plane B
    draw_scroll_plane(plane_b_base, hscroll_b, 1U, 1U, 4U);

    // 2. Plane A & Window
    const uint8_t win_h_pos = regs_[17] & 0x1FU;
    const bool win_right = (regs_[17] & 0x80U) != 0U;
    const uint8_t win_v_pos = regs_[18] & 0x1FU;
    const bool win_down = (regs_[18] & 0x80U) != 0U;
    const uint32_t win_y_line = static_cast<uint32_t>(win_v_pos) * 8U;
    const uint32_t win_x_px = static_cast<uint32_t>(win_h_pos) * 16U;
    const bool full_win_y = win_down ? (y >= win_y_line) : (y < win_y_line);
    const uint32_t win_stride = h40 ? 64U : 32U;

    for (uint32_t x = 0; x < width; ++x) {
        const bool in_win_x = win_right ? (x >= win_x_px) : (x < win_x_px);
        const bool in_window = full_win_y || in_win_x;

        uint16_t entry = 0;
        uint32_t px_in_tile_x = 0;
        uint32_t px_in_tile_y = 0;

        if (in_window) {
            const uint32_t tx = x >> 3U;
            const uint32_t ty = y >> 3U;
            const uint16_t ent_addr =
                static_cast<uint16_t>((win_base + (ty * win_stride + tx) * 2U) & 0xFFFFU);
            entry = static_cast<uint16_t>((static_cast<uint16_t>(vram_[ent_addr]) << 8U) |
                                          vram_[static_cast<uint16_t>(ent_addr + 1U)]);
            px_in_tile_x = x & 7U;
            px_in_tile_y = y & 7U;
        } else {
            const uint32_t col2 = vscroll_2cell ? ((x >> 4U) * 2U) : 0U;
            const uint16_t vscroll = (col2 < 40U) ? (vsram_[col2] & 0x03FFU) : 0U;
            const uint32_t sx = (x - hscroll_a) & (plane_w_px - 1U);
            const uint32_t sy = (y + vscroll) & (plane_h_px - 1U);
            const uint32_t tx = sx >> 3U;
            const uint32_t ty = sy >> 3U;
            const uint16_t ent_addr =
                static_cast<uint16_t>((plane_a_base + (ty * plane_w_tiles + tx) * 2U) & 0xFFFFU);
            entry = static_cast<uint16_t>((static_cast<uint16_t>(vram_[ent_addr]) << 8U) |
                                          vram_[static_cast<uint16_t>(ent_addr + 1U)]);
            px_in_tile_x = sx & 7U;
            px_in_tile_y = sy & 7U;
        }

        const bool prio = (entry & 0x8000U) != 0U;
        const uint8_t pal = static_cast<uint8_t>((entry >> 13U) & 0x03U);
        const bool vflip = (entry & 0x1000U) != 0U;
        const bool hflip = (entry & 0x0800U) != 0U;
        const uint16_t tile_idx = entry & 0x07FFU;

        const uint32_t row = vflip ? (7U - px_in_tile_y) : px_in_tile_y;
        const uint32_t col = hflip ? (7U - px_in_tile_x) : px_in_tile_x;
        const uint16_t tile_addr =
            static_cast<uint16_t>((tile_idx * 32U + row * 4U + (col >> 1U)) & 0xFFFFU);
        const uint8_t byte = vram_[tile_addr];
        const uint8_t pix = (col & 1U) ? (byte & 0x0FU) : (byte >> 4U);

        if (sh_enable && prio) {
            px_sh[x] = 1U;
        }
        if (pix != 0U) {
            const uint8_t layer_p = prio ? 5U : 2U;
            if (layer_p >= px_prio[x]) {
                px_prio[x] = layer_p;
                px_color[x] = static_cast<uint8_t>((pal << 4U) | pix);
            }
        }
    }

    // 3. Sprites (linked-list SAT traversal, max 80 total, max 20/line in H40, 16/line in H32)
    const uint16_t sat_base = static_cast<uint16_t>((regs_[5] & (h40 ? 0x7EU : 0x7FU)) << 9U);
    const uint32_t max_sprites = h40 ? 80U : 64U;
    const uint32_t max_per_line = h40 ? 20U : 16U;
    const uint32_t max_dots = width;

    std::array<uint8_t, kMaxWidth> spr_drawn{};
    uint32_t sprites_on_line = 0;
    uint32_t dots_drawn = 0;
    bool mask_active = false;
    bool seen_nonzero_x = false;

    uint32_t idx = 0;
    for (uint32_t count = 0; count < max_sprites; ++count) {
        const uint16_t base = static_cast<uint16_t>((sat_base + idx * 8U) & 0xFFFFU);
        const uint16_t w0 = static_cast<uint16_t>((static_cast<uint16_t>(vram_[base]) << 8U) |
                                                  vram_[(base + 1U) & 0xFFFFU]);
        const uint16_t w1 =
            static_cast<uint16_t>((static_cast<uint16_t>(vram_[(base + 2U) & 0xFFFFU]) << 8U) |
                                  vram_[(base + 3U) & 0xFFFFU]);
        const uint16_t w2 =
            static_cast<uint16_t>((static_cast<uint16_t>(vram_[(base + 4U) & 0xFFFFU]) << 8U) |
                                  vram_[(base + 5U) & 0xFFFFU]);
        const uint16_t w3 =
            static_cast<uint16_t>((static_cast<uint16_t>(vram_[(base + 6U) & 0xFFFFU]) << 8U) |
                                  vram_[(base + 7U) & 0xFFFFU]);

        const int32_t sy = static_cast<int32_t>(w0 & 0x03FFU) - 128;
        const uint32_t h_tiles = ((w1 >> 8U) & 0x03U) + 1U;
        const uint32_t w_tiles = ((w1 >> 10U) & 0x03U) + 1U;
        const uint32_t link = w1 & 0x7FU;

        if (static_cast<int32_t>(y) >= sy &&
            static_cast<int32_t>(y) < sy + static_cast<int32_t>(h_tiles * 8U)) {
            if (sprites_on_line >= max_per_line) {
                sprite_overflow_ = true;
                break;
            }
            ++sprites_on_line;

            const uint32_t raw_x = w3 & 0x01FFU;
            if (raw_x == 0U) {
                if (seen_nonzero_x) {
                    mask_active = true;
                }
            } else {
                seen_nonzero_x = true;
            }

            if (!mask_active && raw_x != 0U) {
                const int32_t sx = static_cast<int32_t>(raw_x) - 128;
                const bool prio = (w2 & 0x8000U) != 0U;
                const uint8_t pal = static_cast<uint8_t>((w2 >> 13U) & 0x03U);
                const bool vflip = (w2 & 0x1000U) != 0U;
                const bool hflip = (w2 & 0x0800U) != 0U;
                const uint16_t base_tile = w2 & 0x07FFU;

                const uint32_t py = static_cast<uint32_t>(static_cast<int32_t>(y) - sy);
                const uint32_t spr_y = vflip ? (h_tiles * 8U - 1U - py) : py;
                const uint32_t tile_y = spr_y >> 3U;
                const uint32_t row = spr_y & 7U;

                for (uint32_t px = 0; px < w_tiles * 8U; ++px) {
                    if (dots_drawn >= max_dots) {
                        sprite_overflow_ = true;
                        break;
                    }
                    ++dots_drawn;
                    const int32_t screen_x = sx + static_cast<int32_t>(px);
                    if (screen_x < 0 || screen_x >= static_cast<int32_t>(width)) {
                        continue;
                    }
                    const uint32_t spr_x = hflip ? (w_tiles * 8U - 1U - px) : px;
                    const uint32_t tile_x = spr_x >> 3U;
                    const uint32_t col = spr_x & 7U;
                    // Genesis sprites are stored in column-major tile order
                    const uint16_t tile_idx =
                        static_cast<uint16_t>((base_tile + tile_x * h_tiles + tile_y) & 0x07FFU);
                    const uint16_t taddr =
                        static_cast<uint16_t>((tile_idx * 32U + row * 4U + (col >> 1U)) & 0xFFFFU);
                    const uint8_t byte = vram_[taddr];
                    const uint8_t pix = (col & 1U) ? (byte & 0x0FU) : (byte >> 4U);
                    if (pix == 0U) {
                        continue;
                    }
                    const uint32_t ux = static_cast<uint32_t>(screen_x);
                    if (spr_drawn[ux]) {
                        sprite_collision_ = true;
                        continue;  // Earlier sprite in SAT wins
                    }
                    spr_drawn[ux] = 1U;

                    if (sh_enable && pal == 3U && (pix == 0x0EU || pix == 0x0FU)) {
                        // Shadow/Highlight operator pixels
                        if (pix == 0x0EU && px_sh[ux] < 2U) {
                            ++px_sh[ux];
                        } else if (pix == 0x0FU && px_sh[ux] > 0U) {
                            --px_sh[ux];
                        }
                        continue;
                    }

                    const uint8_t layer_p = prio ? 6U : 3U;
                    if (layer_p > px_prio[ux]) {
                        px_prio[ux] = layer_p;
                        px_color[ux] = static_cast<uint8_t>((pal << 4U) | pix);
                        if (sh_enable && (prio || (pal == 3U && pix == 0x0EU))) {
                            px_sh[ux] = 1U;
                        }
                    }
                }
            }
        }

        if (link == 0U || link >= max_sprites) {
            break;
        }
        idx = link;
    }

    // Leftmost 8-pixel column blanking (Reg $00 bit 5)
    const bool mask_left = (regs_[0] & 0x20U) != 0U;
    for (uint32_t x = 0; x < width; ++x) {
        if (mask_left && x < 8U) {
            dst[x] = cram_rgba_normal_[bg_idx];
        } else {
            dst[x] = cram_to_rgba(px_color[x], sh_enable ? px_sh[x] : 1U);
        }
    }
}

void Vdp::save_snapshot(Snapshot& s) const noexcept {
    s.vram = vram_;
    s.cram = cram_;
    s.vsram = vsram_;
    s.regs = regs_;
    s.addr_reg = addr_reg_;
    s.code_reg = code_reg_;
    s.pending_ctrl = pending_ctrl_ ? 1U : 0U;
    s.dma_fill_pending = dma_fill_pending_ ? 1U : 0U;
    s.ctrl_latch = ctrl_latch_;
    s.scanline = scanline_;
    s.line_cycles = line_cycles_;
    s.hint_counter = hint_counter_;
    s.vint_pending = vint_pending_ ? 1U : 0U;
    s.hint_pending = hint_pending_ ? 1U : 0U;
    s.sprite_overflow = sprite_overflow_ ? 1U : 0U;
    s.sprite_collision = sprite_collision_ ? 1U : 0U;
    s.pal = pal_ ? 1U : 0U;
}

void Vdp::load_snapshot(const Snapshot& s) noexcept {
    vram_ = s.vram;
    cram_ = s.cram;
    vsram_ = s.vsram;
    regs_ = s.regs;
    for (uint8_t i = 0; i < 64; ++i) {
        update_cram_cache(i);
    }
    addr_reg_ = s.addr_reg;
    code_reg_ = s.code_reg;
    pending_ctrl_ = s.pending_ctrl != 0U;
    dma_fill_pending_ = s.dma_fill_pending != 0U;
    ctrl_latch_ = s.ctrl_latch;
    scanline_ = s.scanline;
    line_cycles_ = s.line_cycles;
    hint_counter_ = s.hint_counter;
    vint_pending_ = s.vint_pending != 0U;
    hint_pending_ = s.hint_pending != 0U;
    sprite_overflow_ = s.sprite_overflow != 0U;
    sprite_collision_ = s.sprite_collision != 0U;
    pal_ = s.pal != 0U;
}

}  // namespace dragon::md
