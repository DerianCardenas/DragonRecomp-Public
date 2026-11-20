#include "pce/vdc.h"

#include <algorithm>
#include <cstring>

namespace dragon::pce {

namespace {

// Expand 3-bit component (0..7) to 8-bit (0..255)
constexpr uint8_t expand3(uint8_t c) noexcept {
    return static_cast<uint8_t>((c << 5U) | (c << 2U) | (c >> 1U));
}

}  // namespace

Vdc::Vdc() {
    reset();
}

uint32_t Vdc::vce_to_rgba32(uint16_t grb9) noexcept {
    // HuC6260 VCE palette format: bits 8-6 = Green, bits 5-3 = Red, bits 2-0 = Blue
    // Output: SDL_PIXELFORMAT_RGBA32 (little-endian: R in low byte, G, B, A=0xFF)
    const uint8_t b = expand3(static_cast<uint8_t>(grb9 & 0x07U));
    const uint8_t r = expand3(static_cast<uint8_t>((grb9 >> 3U) & 0x07U));
    const uint8_t g = expand3(static_cast<uint8_t>((grb9 >> 6U) & 0x07U));
    return 0xFF000000U | (static_cast<uint32_t>(b) << 16U) |
           (static_cast<uint32_t>(g) << 8U) | static_cast<uint32_t>(r);
}

void Vdc::reset() noexcept {
    st_ = {};
    // Sensible power-on defaults (256x224 display)
    st_.regs[0x09U] = 0x0000U;  // MWR: 32x32 tiles
    st_.regs[0x0AU] = 0x0202U;  // HSR
    st_.regs[0x0BU] = 0x031FU;  // HDR: HDW = 31 -> 32 tiles = 256 px
    st_.regs[0x0CU] = 0x0F02U;  // VPR
    st_.regs[0x0DU] = 0x00DFU;  // VDR: 223 -> 224 lines
    st_.regs[0x0EU] = 0x0003U;  // VCR
    vram_.fill(0x0000U);
    satb_.fill(0x0000U);
    palette_.fill(0x0000U);
    for (size_t i = 0; i < 512U; ++i) {
        palette_rgba_[i] = 0xFF000000U;
    }
    fb_.fill(0xFF000000U);
    vis_width_ = 256;
    vis_height_ = 224;
    frame_ready_ = false;
}

void Vdc::set_state(const VdcState& s) noexcept {
    st_ = s;
    for (size_t i = 0; i < 512U; ++i) {
        palette_rgba_[i] = vce_to_rgba32(palette_[i]);
    }
    update_visible_dimensions();
}

uint16_t Vdc::vram_inc_step() const noexcept {
    switch ((st_.regs[0x05U] >> 11U) & 0x03U) {
        case 0U: return 1U;
        case 1U: return 32U;
        case 2U: return 64U;
        case 3U: return 128U;
        default: return 1U;
    }
}

void Vdc::update_visible_dimensions() noexcept {
    const int hdw = static_cast<int>(st_.regs[0x0BU] & 0x7FU);
    int w = (hdw + 1) * 8;
    if (w < 128) w = 256;
    if (w > kMaxWidth) w = kMaxWidth;

    const int vdw = static_cast<int>(st_.regs[0x0DU] & 0x01FFU);
    int h = vdw + 1;
    if (h < 128) h = 224;
    if (h > 242) h = 242;

    vis_width_ = w;
    vis_height_ = h;
}

uint8_t Vdc::read_vdc(uint8_t port) noexcept {
    switch (port & 0x03U) {
        case 0U: {
            // Reading status clears bits 0..5 and deasserts IRQ1 line
            const uint8_t s = st_.status;
            st_.status &= static_cast<uint8_t>(~(kStatCollision | kStatOverflow | kStatRcr |
                                                 kStatSatbDma | kStatVramDma | kStatVblank));
            st_.irq_line = 0U;
            return s;
        }
        case 1U:
            return 0x00U;
        case 2U:
            if (st_.ar == 0x02U) {
                return static_cast<uint8_t>(st_.read_buffer & 0xFFU);
            }
            return static_cast<uint8_t>(st_.regs[st_.ar & 0x1FU] & 0xFFU);
        case 3U:
            if (st_.ar == 0x02U) {
                const uint8_t hi = static_cast<uint8_t>((st_.read_buffer >> 8U) & 0xFFU);
                // Reading high byte of VRR increments MARR and pre-fetches next word
                st_.regs[0x01U] = static_cast<uint16_t>(st_.regs[0x01U] + vram_inc_step());
                st_.read_buffer = vram_[st_.regs[0x01U] & 0x7FFFU];
                return hi;
            }
            return static_cast<uint8_t>((st_.regs[st_.ar & 0x1FU] >> 8U) & 0xFFU);
        default:
            return 0x00U;
    }
}

void Vdc::write_vdc(uint8_t port, uint8_t val) noexcept {
    switch (port & 0x03U) {
        case 0U:
            st_.ar = val & 0x1FU;
            break;
        case 1U:
            break;
        case 2U: {
            // Data Low byte
            const uint8_t r = st_.ar & 0x1FU;
            if (r == 0x02U) {
                st_.write_latch = static_cast<uint16_t>((st_.write_latch & 0xFF00U) | val);
            } else {
                st_.regs[r] = static_cast<uint16_t>((st_.regs[r] & 0xFF00U) | val);
                if (r == 0x08U) {
                    st_.byr_latch = st_.regs[0x08U] & 0x01FFU;
                } else if (r == 0x0FU) {
                    st_.satb_auto_dma = (st_.regs[0x0FU] & 0x0010U) ? 1U : 0U;
                }
            }
            break;
        }
        case 3U: {
            // Data High byte (commits VRAM write, MARR fetch, DMA trigger, BYR latch)
            const uint8_t r = st_.ar & 0x1FU;
            if (r == 0x02U) {
                st_.write_latch = static_cast<uint16_t>((st_.write_latch & 0x00FFU) |
                                                        (static_cast<uint16_t>(val) << 8U));
                const uint16_t addr = st_.regs[0x00U] & 0x7FFFU;
                vram_[addr] = st_.write_latch;
                st_.regs[0x00U] = static_cast<uint16_t>(st_.regs[0x00U] + vram_inc_step());
            } else {
                st_.regs[r] = static_cast<uint16_t>((st_.regs[r] & 0x00FFU) |
                                                    (static_cast<uint16_t>(val) << 8U));
                if (r == 0x01U) {
                    // Writing MARR high byte immediately prefetches VRAM word into read buffer
                    st_.read_buffer = vram_[st_.regs[0x01U] & 0x7FFFU];
                } else if (r == 0x08U) {
                    st_.byr_latch = st_.regs[0x08U] & 0x01FFU;
                } else if (r == 0x0BU || r == 0x0DU) {
                    update_visible_dimensions();
                } else if (r == 0x0FU) {
                    st_.satb_auto_dma = (st_.regs[0x0FU] & 0x0010U) ? 1U : 0U;
                } else if (r == 0x12U) {
                    // Writing LENR high byte triggers immediate VRAM-to-VRAM DMA
                    execute_vram_dma();
                } else if (r == 0x13U) {
                    // Writing DVSSR high byte schedules VRAM-to-SATB DMA at next VBlank
                    st_.satb_dma_pending = 1U;
                }
            }
            break;
        }
        default:
            break;
    }
}

uint8_t Vdc::read_vce(uint8_t port) noexcept {
    switch (port & 0x07U) {
        case 4U: {
            const uint16_t c = palette_[st_.vce_cta & 0x01FFU];
            return static_cast<uint8_t>(c & 0xFFU);
        }
        case 5U: {
            const uint16_t c = palette_[st_.vce_cta & 0x01FFU];
            st_.vce_cta = static_cast<uint16_t>((st_.vce_cta + 1U) & 0x01FFU);
            return static_cast<uint8_t>(0xFEU | ((c >> 8U) & 0x01U));
        }
        default:
            return 0xFFU;
    }
}

void Vdc::write_vce(uint8_t port, uint8_t val) noexcept {
    switch (port & 0x07U) {
        case 0U:
            st_.vce_ctrl = val;
            break;
        case 2U:
            st_.vce_cta = static_cast<uint16_t>((st_.vce_cta & 0x0100U) | val);
            break;
        case 3U:
            st_.vce_cta = static_cast<uint16_t>((st_.vce_cta & 0x00FFU) |
                                                (static_cast<uint16_t>(val & 0x01U) << 8U));
            break;
        case 4U: {
            const uint16_t idx = st_.vce_cta & 0x01FFU;
            palette_[idx] = static_cast<uint16_t>((palette_[idx] & 0x0100U) | val);
            palette_rgba_[idx] = vce_to_rgba32(palette_[idx]);
            break;
        }
        case 5U: {
            const uint16_t idx = st_.vce_cta & 0x01FFU;
            palette_[idx] = static_cast<uint16_t>((palette_[idx] & 0x00FFU) |
                                                  (static_cast<uint16_t>(val & 0x01U) << 8U));
            palette_rgba_[idx] = vce_to_rgba32(palette_[idx]);
            st_.vce_cta = static_cast<uint16_t>((st_.vce_cta + 1U) & 0x01FFU);
            break;
        }
        default:
            break;
    }
}

void Vdc::execute_vram_dma() noexcept {
    const uint16_t dcr = st_.regs[0x0FU];
    const int src_step = (dcr & 0x0004U) ? -1 : 1;
    const int dst_step = (dcr & 0x0008U) ? -1 : 1;
    uint16_t src = st_.regs[0x10U];
    uint16_t dst = st_.regs[0x11U];
    const uint32_t count = static_cast<uint32_t>(st_.regs[0x12U]) + 1U;

    for (uint32_t i = 0; i < count; ++i) {
        vram_[dst & 0x7FFFU] = vram_[src & 0x7FFFU];
        src = static_cast<uint16_t>(src + src_step);
        dst = static_cast<uint16_t>(dst + dst_step);
    }

    st_.regs[0x10U] = src;
    st_.regs[0x11U] = dst;
    st_.regs[0x12U] = 0xFFFFU;
    st_.status |= kStatVramDma;
    if (dcr & 0x0002U) {
        st_.irq_line = 1U;
    }
}

void Vdc::execute_satb_dma() noexcept {
    const uint16_t base = st_.regs[0x13U];
    for (uint16_t i = 0; i < 256U; ++i) {
        satb_[i] = vram_[(base + i) & 0x7FFFU];
    }
    st_.status |= kStatSatbDma;
    if (st_.regs[0x0FU] & 0x0001U) {
        st_.irq_line = 1U;
    }
}

void Vdc::render_scanline(int line) noexcept {
    if (line < 0 || line >= kMaxHeight) {
        return;
    }

    const int width = vis_width_;
    uint32_t* dst = &fb_[static_cast<size_t>(line) * kMaxWidth];
    const uint16_t cr = st_.regs[0x05U];
    const bool bg_on = (cr & 0x0080U) != 0U;
    const bool spr_on = (cr & 0x0040U) != 0U;

    // Color 0 of BG palette 0 (or overscan 0x100 when display disabled)
    const uint32_t backdrop = palette_rgba_[bg_on || spr_on ? 0U : 0x100U];
    std::array<uint8_t, kMaxWidth> bg_opaque{};
    for (int x = 0; x < width; ++x) {
        dst[x] = backdrop;
    }

    // 1. Background layer (8x8 planar 4bpp tiles)
    if (bg_on) {
        const uint16_t mwr = st_.regs[0x09U];
        int bat_w = 32;
        switch ((mwr >> 4U) & 0x03U) {
            case 0U: bat_w = 32; break;
            case 1U: bat_w = 64; break;
            default: bat_w = 128; break;
        }
        const int bat_h = (mwr & 0x0040U) ? 64 : 32;
        const int map_px_w = bat_w * 8;
        const int map_px_h = bat_h * 8;

        const int bxr = static_cast<int>(st_.regs[0x07U] & 0x03FFU);
        const int by = static_cast<int>(st_.byr_latch) % map_px_h;
        const int tile_y = by >> 3;
        const int fine_y = by & 7;

        for (int sx = 0; sx < width; ++sx) {
            const int bx = (sx + bxr) % map_px_w;
            const int tile_x = bx >> 3;
            const int fine_x = bx & 7;

            const uint16_t bat_addr = static_cast<uint16_t>((tile_y * bat_w + tile_x) & 0x7FFFU);
            const uint16_t entry = vram_[bat_addr];
            const uint16_t tile_idx = entry & 0x0FFFU;
            const uint8_t pal_bank = static_cast<uint8_t>((entry >> 12U) & 0x0FU);

            // Each 8x8 tile is 16 words: words 0..7 = planes 0 (lo) & 1 (hi), words 8..15 = planes 2 & 3
            const uint16_t tile_base = static_cast<uint16_t>((tile_idx << 4U) & 0x7FFFU);
            const uint16_t w01 = vram_[(tile_base + static_cast<uint16_t>(fine_y)) & 0x7FFFU];
            const uint16_t w23 = vram_[(tile_base + static_cast<uint16_t>(fine_y + 8)) & 0x7FFFU];

            const uint8_t bit = static_cast<uint8_t>(7 - fine_x);
            const uint8_t p0 = static_cast<uint8_t>((w01 >> bit) & 1U);
            const uint8_t p1 = static_cast<uint8_t>((w01 >> (bit + 8U)) & 1U);
            const uint8_t p2 = static_cast<uint8_t>((w23 >> bit) & 1U);
            const uint8_t p3 = static_cast<uint8_t>((w23 >> (bit + 8U)) & 1U);
            const uint8_t color = static_cast<uint8_t>(p0 | (p1 << 1U) | (p2 << 2U) | (p3 << 3U));

            if (color != 0U) {
                dst[sx] = palette_rgba_[static_cast<size_t>(pal_bank) * 16U + color];
                bg_opaque[static_cast<size_t>(sx)] = 1U;
            }
        }
    }

    // 2. Sprites (64 entries in internal SATB; max 16 x 16px cells per scanline)
    if (spr_on) {
        std::array<uint8_t, kMaxWidth> spr_owner{};
        spr_owner.fill(0xFFU);

        int cells_on_line = 0;
        for (int i = 0; i < 64; ++i) {
            const uint16_t w0 = satb_[static_cast<size_t>(i) * 4U + 0U];
            const uint16_t w1 = satb_[static_cast<size_t>(i) * 4U + 1U];
            const uint16_t w2 = satb_[static_cast<size_t>(i) * 4U + 2U];
            const uint16_t w3 = satb_[static_cast<size_t>(i) * 4U + 3U];

            const int sy = static_cast<int>(w0 & 0x03FFU) - 64;
            const int sx = static_cast<int>(w1 & 0x03FFU) - 32;

            const uint8_t cgy = static_cast<uint8_t>((w3 >> 12U) & 0x03U);
            int spr_h = 16;
            if (cgy == 1U) spr_h = 32;
            else if (cgy >= 2U) spr_h = 64;

            if (line < sy || line >= sy + spr_h) {
                continue;
            }

            const bool cgx = (w3 & 0x0100U) != 0U;
            const int spr_w = cgx ? 32 : 16;
            const int cell_cols = cgx ? 2 : 1;

            if (cells_on_line + cell_cols > 16) {
                st_.status |= kStatOverflow;
                if (cr & 0x0002U) {
                    st_.irq_line = 1U;
                }
                if (cells_on_line >= 16) {
                    break;
                }
            }

            const bool y_flip = (w3 & 0x8000U) != 0U;
            const bool x_flip = (w3 & 0x0800U) != 0U;
            const bool in_front = (w3 & 0x0080U) != 0U;
            const uint8_t pal_bank = static_cast<uint8_t>(w3 & 0x000FU);

            // Pattern index alignment: CGX masks bit 0, CGY=1 masks bit 1, CGY>=2 masks bits 1..2
            uint16_t pat_idx = static_cast<uint16_t>((w2 >> 1U) & 0x03FFU);
            if (cgx) pat_idx &= static_cast<uint16_t>(~0x01U);
            if (cgy == 1U) pat_idx &= static_cast<uint16_t>(~0x02U);
            else if (cgy >= 2U) pat_idx &= static_cast<uint16_t>(~0x06U);

            int local_y = line - sy;
            if (y_flip) {
                local_y = (spr_h - 1) - local_y;
            }
            const int cell_row = local_y >> 4;
            const int fine_y = local_y & 15;

            for (int col = 0; col < cell_cols; ++col) {
                if (cells_on_line >= 16) {
                    break;
                }
                ++cells_on_line;

                const int pat_col = x_flip ? (cell_cols - 1 - col) : col;
                const uint16_t cell_pat =
                    static_cast<uint16_t>(pat_idx + static_cast<uint16_t>(cell_row * 2 + pat_col));
                const uint16_t cell_base = static_cast<uint16_t>((cell_pat << 6U) & 0x7FFFU);

                // Each 16x16 sprite cell is 64 words: 16 words per bitplane (0..3)
                const uint16_t pl0 = vram_[(cell_base + 0U + static_cast<uint16_t>(fine_y)) & 0x7FFFU];
                const uint16_t pl1 = vram_[(cell_base + 16U + static_cast<uint16_t>(fine_y)) & 0x7FFFU];
                const uint16_t pl2 = vram_[(cell_base + 32U + static_cast<uint16_t>(fine_y)) & 0x7FFFU];
                const uint16_t pl3 = vram_[(cell_base + 48U + static_cast<uint16_t>(fine_y)) & 0x7FFFU];

                const int cell_screen_x = sx + col * 16;
                for (int px = 0; px < 16; ++px) {
                    const int screen_x = cell_screen_x + px;
                    if (screen_x < 0 || screen_x >= width) {
                        continue;
                    }
                    const int bit = x_flip ? px : (15 - px);
                    const uint8_t c = static_cast<uint8_t>(
                        ((pl0 >> bit) & 1U) | (((pl1 >> bit) & 1U) << 1U) |
                        (((pl2 >> bit) & 1U) << 2U) | (((pl3 >> bit) & 1U) << 3U));
                    if (c == 0U) {
                        continue;
                    }
                    if (spr_owner[static_cast<size_t>(screen_x)] == 0U && i > 0) {
                        st_.status |= kStatCollision;
                        if (cr & 0x0001U) {
                            st_.irq_line = 1U;
                        }
                    }
                    if (spr_owner[static_cast<size_t>(screen_x)] == 0xFFU) {
                        spr_owner[static_cast<size_t>(screen_x)] = static_cast<uint8_t>(i);
                        if (in_front || bg_opaque[static_cast<size_t>(screen_x)] == 0U) {
                            dst[screen_x] =
                                palette_rgba_[256U + static_cast<size_t>(pal_bank) * 16U + c];
                        }
                    }
                }
            }
            (void)spr_w;
        }
    }
}

void Vdc::enter_scanline(int line) noexcept {
    const uint16_t cr = st_.regs[0x05U];
    if (line == 0) {
        update_visible_dimensions();
        st_.byr_latch = st_.regs[0x08U] & 0x01FFU;
    }

    if (line < vis_height_) {
        // Raster Compare Register (RCR) matches against (active_line + 64) at the start of the
        // scanline so the CPU's RCR ISR can update BXR/BYR/CR/palettes before this line renders.
        const uint16_t rcr_target = st_.regs[0x06U] & 0x03FFU;
        if (static_cast<uint16_t>(line + 64) == rcr_target) {
            st_.status |= kStatRcr;
            if (cr & 0x0004U) {
                st_.irq_line = 1U;
            }
        }
    } else if (line == vis_height_) {
        // Start of VBlank!
        st_.status |= kStatVblank;
        if (cr & 0x0008U) {
            st_.irq_line = 1U;
        }
        if (st_.satb_dma_pending != 0U || st_.satb_auto_dma != 0U) {
            st_.satb_dma_pending = 0U;
            execute_satb_dma();
        }
        frame_ready_ = true;
    }
}

void Vdc::step(uint32_t cpu_cycles) noexcept {
    st_.line_cycles += cpu_cycles;
    while (st_.line_cycles >= kCyclesPerScanline) {
        st_.line_cycles -= kCyclesPerScanline;

        const int line = static_cast<int>(st_.scanline);
        if (line < vis_height_) {
            render_scanline(line);
            st_.byr_latch = static_cast<uint16_t>((st_.byr_latch + 1U) & 0x01FFU);
        }

        st_.scanline = static_cast<uint16_t>((st_.scanline + 1U) % kTotalScanlines);
        enter_scanline(static_cast<int>(st_.scanline));
    }
}

}  // namespace dragon::pce
