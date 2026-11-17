#include "gba/ppu.h"

#include <algorithm>
#include <cstring>

namespace dragon::gba {

namespace {

constexpr uint16_t kDispstatVBlank = 1U << 0;
constexpr uint16_t kDispstatHBlank = 1U << 1;
constexpr uint16_t kDispstatVCount = 1U << 2;
constexpr uint16_t kDispstatVBlankIrq = 1U << 3;
constexpr uint16_t kDispstatHBlankIrq = 1U << 4;
constexpr uint16_t kDispstatVCountIrq = 1U << 5;

constexpr uint16_t kIrqVBlank = 1U << 0;
constexpr uint16_t kIrqHBlank = 1U << 1;
constexpr uint16_t kIrqVCount = 1U << 2;

int32_t sign_extend_28(uint32_t val) noexcept {
    val &= 0x0FFFFFFFU;
    if (val & 0x08000000U) {
        val |= 0xF0000000U;
    }
    return static_cast<int32_t>(val);
}

uint16_t blend_rgb555(uint16_t top, uint16_t bottom, uint32_t eva, uint32_t evb) noexcept {
    uint32_t r1 = top & 0x1FU;
    uint32_t g1 = (top >> 5U) & 0x1FU;
    uint32_t b1 = (top >> 10U) & 0x1FU;

    uint32_t r2 = bottom & 0x1FU;
    uint32_t g2 = (bottom >> 5U) & 0x1FU;
    uint32_t b2 = (bottom >> 10U) & 0x1FU;

    uint32_t r = std::min<uint32_t>(31U, (r1 * eva + r2 * evb) >> 4U);
    uint32_t g = std::min<uint32_t>(31U, (g1 * eva + g2 * evb) >> 4U);
    uint32_t b = std::min<uint32_t>(31U, (b1 * eva + b2 * evb) >> 4U);
    return static_cast<uint16_t>(r | (g << 5U) | (b << 10U));
}

uint16_t brighten_rgb555(uint16_t col, uint32_t evy) noexcept {
    uint32_t r = col & 0x1FU;
    uint32_t g = (col >> 5U) & 0x1FU;
    uint32_t b = (col >> 10U) & 0x1FU;

    r += ((31U - r) * evy) >> 4U;
    g += ((31U - g) * evy) >> 4U;
    b += ((31U - b) * evy) >> 4U;
    return static_cast<uint16_t>(r | (g << 5U) | (b << 10U));
}

uint16_t darken_rgb555(uint16_t col, uint32_t evy) noexcept {
    uint32_t r = col & 0x1FU;
    uint32_t g = (col >> 5U) & 0x1FU;
    uint32_t b = (col >> 10U) & 0x1FU;

    r -= (r * evy) >> 4U;
    g -= (g * evy) >> 4U;
    b -= (b * evy) >> 4U;
    return static_cast<uint16_t>(r | (g << 5U) | (b << 10U));
}

}  // namespace

Ppu::Ppu() {
    reset();
}

void Ppu::reset() {
    dispcnt_ = 0x0080U;
    dispstat_ = 0U;
    vcount_ = 0U;
    line_cycles_ = 0U;
    in_hblank_ = false;
    frame_complete_ = false;

    bgcnt_.fill(0U);
    bghofs_.fill(0U);
    bgvofs_.fill(0U);
    bgpa_ = {0x0100, 0x0100};
    bgpb_ = {0, 0};
    bgpc_ = {0, 0};
    bgpd_ = {0x0100, 0x0100};
    bgx_latch_ = {0, 0};
    bgy_latch_ = {0, 0};
    bgx_internal_ = {0, 0};
    bgy_internal_ = {0, 0};
    bgx_raw_ = {0, 0};
    bgy_raw_ = {0, 0};

    win0h_ = 0U;
    win1h_ = 0U;
    win0v_ = 0U;
    win1v_ = 0U;
    winin_ = 0U;
    winout_ = 0U;
    mosaic_ = 0U;
    bldcnt_ = 0U;
    bldalpha_ = 0U;
    bldy_ = 0U;

    palette_ram_.fill(0U);
    vram_.fill(0U);
    oam_.fill(0U);
    framebuffer_.fill(0xFF000000U);
}

uint32_t Ppu::rgb555_to_rgba32(uint16_t color) noexcept {
    const uint32_t r5 = color & 0x1FU;
    const uint32_t g5 = (color >> 5U) & 0x1FU;
    const uint32_t b5 = (color >> 10U) & 0x1FU;
    const uint32_t r8 = (r5 << 3U) | (r5 >> 2U);
    const uint32_t g8 = (g5 << 3U) | (g5 >> 2U);
    const uint32_t b8 = (b5 << 3U) | (b5 >> 2U);
    return r8 | (g8 << 8U) | (b8 << 16U) | 0xFF000000U;
}

uint32_t Ppu::map_vram_offset(uint32_t offset) noexcept {
    offset &= 0x1FFFFU;
    if (offset >= 0x18000U) {
        offset -= 0x8000U;
    }
    return offset;
}

uint16_t Ppu::read_palette16(uint32_t offset) const noexcept {
    offset &= 0x3FEU;
    return static_cast<uint16_t>(palette_ram_[offset] |
                                 (static_cast<uint16_t>(palette_ram_[offset + 1U]) << 8U));
}

void Ppu::write_palette16(uint32_t offset, uint16_t value) noexcept {
    offset &= 0x3FEU;
    palette_ram_[offset] = static_cast<uint8_t>(value & 0xFFU);
    palette_ram_[offset + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

uint8_t Ppu::read_vram8(uint32_t offset) const noexcept {
    return vram_[map_vram_offset(offset)];
}

uint16_t Ppu::read_vram16(uint32_t offset) const noexcept {
    const uint32_t mapped = map_vram_offset(offset & ~1U);
    return static_cast<uint16_t>(vram_[mapped] | (static_cast<uint16_t>(vram_[mapped + 1U]) << 8U));
}

void Ppu::write_vram16(uint32_t offset, uint16_t value) noexcept {
    const uint32_t mapped = map_vram_offset(offset & ~1U);
    vram_[mapped] = static_cast<uint8_t>(value & 0xFFU);
    vram_[mapped + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

uint16_t Ppu::read_oam16(uint32_t offset) const noexcept {
    offset &= 0x3FEU;
    return static_cast<uint16_t>(oam_[offset] | (static_cast<uint16_t>(oam_[offset + 1U]) << 8U));
}

void Ppu::write_oam16(uint32_t offset, uint16_t value) noexcept {
    offset &= 0x3FEU;
    oam_[offset] = static_cast<uint8_t>(value & 0xFFU);
    oam_[offset + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

uint8_t Ppu::read_io8(uint32_t offset) const noexcept {
    const uint16_t hw = read_io16(offset & ~1U);
    return (offset & 1U) ? static_cast<uint8_t>((hw >> 8U) & 0xFFU)
                         : static_cast<uint8_t>(hw & 0xFFU);
}

uint16_t Ppu::read_io16(uint32_t offset) const noexcept {
    switch (offset & 0xFFEU) {
        case 0x000:
            return dispcnt_;
        case 0x004:
            return dispstat_;
        case 0x006:
            return vcount_;
        case 0x008:
            return bgcnt_[0];
        case 0x00A:
            return bgcnt_[1];
        case 0x00C:
            return bgcnt_[2];
        case 0x00E:
            return bgcnt_[3];
        case 0x048:
            return winin_;
        case 0x04A:
            return winout_;
        case 0x050:
            return bldcnt_;
        case 0x052:
            return bldalpha_;
        default:
            return 0U;
    }
}

void Ppu::write_io8(uint32_t offset, uint8_t value) noexcept {
    const uint32_t aligned = offset & ~1U;
    uint16_t cur = read_io16(aligned);
    if (offset & 1U) {
        cur = static_cast<uint16_t>((cur & 0x00FFU) | (static_cast<uint16_t>(value) << 8U));
    } else {
        cur = static_cast<uint16_t>((cur & 0xFF00U) | value);
    }
    write_io16(aligned, cur);
}

void Ppu::write_io16(uint32_t offset, uint16_t value) noexcept {
    switch (offset & 0xFFEU) {
        case 0x000:
            dispcnt_ = value;
            break;
        case 0x004:
            dispstat_ = static_cast<uint16_t>((dispstat_ & 0x0007U) | (value & 0xFFF8U));
            break;
        case 0x008:
            bgcnt_[0] = value;
            break;
        case 0x00A:
            bgcnt_[1] = value;
            break;
        case 0x00C:
            bgcnt_[2] = value;
            break;
        case 0x00E:
            bgcnt_[3] = value;
            break;
        case 0x010:
            bghofs_[0] = value & 0x01FFU;
            break;
        case 0x012:
            bgvofs_[0] = value & 0x01FFU;
            break;
        case 0x014:
            bghofs_[1] = value & 0x01FFU;
            break;
        case 0x016:
            bgvofs_[1] = value & 0x01FFU;
            break;
        case 0x018:
            bghofs_[2] = value & 0x01FFU;
            break;
        case 0x01A:
            bgvofs_[2] = value & 0x01FFU;
            break;
        case 0x01C:
            bghofs_[3] = value & 0x01FFU;
            break;
        case 0x01E:
            bgvofs_[3] = value & 0x01FFU;
            break;
        case 0x020:
            bgpa_[0] = static_cast<int16_t>(value);
            break;
        case 0x022:
            bgpb_[0] = static_cast<int16_t>(value);
            break;
        case 0x024:
            bgpc_[0] = static_cast<int16_t>(value);
            break;
        case 0x026:
            bgpd_[0] = static_cast<int16_t>(value);
            break;
        case 0x028:
            bgx_raw_[0] = (bgx_raw_[0] & 0xFFFF0000U) | value;
            bgx_latch_[0] = sign_extend_28(bgx_raw_[0]);
            bgx_internal_[0] = bgx_latch_[0];
            break;
        case 0x02A:
            bgx_raw_[0] = (bgx_raw_[0] & 0x0000FFFFU) | (static_cast<uint32_t>(value) << 16U);
            bgx_latch_[0] = sign_extend_28(bgx_raw_[0]);
            bgx_internal_[0] = bgx_latch_[0];
            break;
        case 0x02C:
            bgy_raw_[0] = (bgy_raw_[0] & 0xFFFF0000U) | value;
            bgy_latch_[0] = sign_extend_28(bgy_raw_[0]);
            bgy_internal_[0] = bgy_latch_[0];
            break;
        case 0x02E:
            bgy_raw_[0] = (bgy_raw_[0] & 0x0000FFFFU) | (static_cast<uint32_t>(value) << 16U);
            bgy_latch_[0] = sign_extend_28(bgy_raw_[0]);
            bgy_internal_[0] = bgy_latch_[0];
            break;
        case 0x030:
            bgpa_[1] = static_cast<int16_t>(value);
            break;
        case 0x032:
            bgpb_[1] = static_cast<int16_t>(value);
            break;
        case 0x034:
            bgpc_[1] = static_cast<int16_t>(value);
            break;
        case 0x036:
            bgpd_[1] = static_cast<int16_t>(value);
            break;
        case 0x038:
            bgx_raw_[1] = (bgx_raw_[1] & 0xFFFF0000U) | value;
            bgx_latch_[1] = sign_extend_28(bgx_raw_[1]);
            bgx_internal_[1] = bgx_latch_[1];
            break;
        case 0x03A:
            bgx_raw_[1] = (bgx_raw_[1] & 0x0000FFFFU) | (static_cast<uint32_t>(value) << 16U);
            bgx_latch_[1] = sign_extend_28(bgx_raw_[1]);
            bgx_internal_[1] = bgx_latch_[1];
            break;
        case 0x03C:
            bgy_raw_[1] = (bgy_raw_[1] & 0xFFFF0000U) | value;
            bgy_latch_[1] = sign_extend_28(bgy_raw_[1]);
            bgy_internal_[1] = bgy_latch_[1];
            break;
        case 0x03E:
            bgy_raw_[1] = (bgy_raw_[1] & 0x0000FFFFU) | (static_cast<uint32_t>(value) << 16U);
            bgy_latch_[1] = sign_extend_28(bgy_raw_[1]);
            bgy_internal_[1] = bgy_latch_[1];
            break;
        case 0x040:
            win0h_ = value;
            break;
        case 0x042:
            win1h_ = value;
            break;
        case 0x044:
            win0v_ = value;
            break;
        case 0x046:
            win1v_ = value;
            break;
        case 0x048:
            winin_ = value & 0x3F3FU;
            break;
        case 0x04A:
            winout_ = value & 0x3F3FU;
            break;
        case 0x04C:
            mosaic_ = value;
            break;
        case 0x050:
            bldcnt_ = value & 0x3FFFU;
            break;
        case 0x052:
            bldalpha_ = value & 0x1F1FU;
            break;
        case 0x054:
            bldy_ = value & 0x001FU;
            break;
        default:
            break;
    }
}

Ppu::StepEvents Ppu::step(uint32_t cycles) {
    StepEvents ev;
    line_cycles_ += cycles;

    while (line_cycles_ >= 4U) {
        if (!in_hblank_ && line_cycles_ >= kHDrawCycles) {
            in_hblank_ = true;
            dispstat_ |= kDispstatHBlank;
            if (vcount_ < kScreenHeight) {
                render_scanline(vcount_);
                ev.entered_hblank = true;
            }
            if (dispstat_ & kDispstatHBlankIrq) {
                ev.irq_flags |= kIrqHBlank;
            }
        }

        if (line_cycles_ < kCyclesPerLine) {
            break;
        }

        line_cycles_ -= kCyclesPerLine;
        in_hblank_ = false;
        dispstat_ &= ~kDispstatHBlank;

        vcount_ = static_cast<uint16_t>((vcount_ + 1U) % kTotalLines);

        if (vcount_ == kScreenHeight) {
            dispstat_ |= kDispstatVBlank;
            ev.entered_vblank = true;
            frame_complete_ = true;
            bgx_internal_[0] = bgx_latch_[0];
            bgy_internal_[0] = bgy_latch_[0];
            bgx_internal_[1] = bgx_latch_[1];
            bgy_internal_[1] = bgy_latch_[1];
            if (dispstat_ & kDispstatVBlankIrq) {
                ev.irq_flags |= kIrqVBlank;
            }
        } else if (vcount_ == 227U) {
            dispstat_ &= ~kDispstatVBlank;
        }

        const uint16_t lyc = (dispstat_ >> 8U) & 0xFFU;
        if (vcount_ == lyc) {
            dispstat_ |= kDispstatVCount;
            if (dispstat_ & kDispstatVCountIrq) {
                ev.irq_flags |= kIrqVCount;
            }
        } else {
            dispstat_ &= ~kDispstatVCount;
        }
    }

    return ev;
}

void Ppu::render_regular_bg(int bg_idx, uint16_t line, uint16_t* line_color, uint8_t* line_prio,
                            uint8_t* line_layer) {
    const uint16_t cnt = bgcnt_[bg_idx];
    const uint8_t prio = static_cast<uint8_t>(cnt & 3U);
    const uint32_t char_base = ((cnt >> 2U) & 3U) * 0x4000U;
    const bool is_8bpp = (cnt & 0x80U) != 0U;
    const uint32_t screen_base = ((cnt >> 8U) & 0x1FU) * 0x800U;
    const uint32_t size_mode = (cnt >> 14U) & 3U;

    const uint32_t width_mask = (size_mode & 1U) ? 511U : 255U;
    const uint32_t height_mask = (size_mode & 2U) ? 511U : 255U;

    const uint32_t sy = (line + bgvofs_[bg_idx]) & height_mask;
    const uint32_t tile_y = sy >> 3U;
    const uint32_t fine_y = sy & 7U;

    for (uint32_t x = 0; x < kScreenWidth; ++x) {
        if (prio >= line_prio[x]) {
            continue;
        }

        const uint32_t sx = (x + bghofs_[bg_idx]) & width_mask;
        const uint32_t tile_x = sx >> 3U;
        const uint32_t fine_x = sx & 7U;

        uint32_t block_idx = 0U;
        if (size_mode == 1U) {
            block_idx = (tile_x >= 32U) ? 1U : 0U;
        } else if (size_mode == 2U) {
            block_idx = (tile_y >= 32U) ? 1U : 0U;
        } else if (size_mode == 3U) {
            block_idx = ((tile_x >= 32U) ? 1U : 0U) + ((tile_y >= 32U) ? 2U : 0U);
        }

        const uint32_t map_addr =
            screen_base + block_idx * 0x800U + ((tile_y & 31U) * 32U + (tile_x & 31U)) * 2U;
        if (map_addr >= 0x10000U) {
            continue;
        }
        const uint16_t entry = read_vram16(map_addr);
        const uint32_t tile_num = entry & 0x03FFU;
        const bool hflip = (entry & 0x0400U) != 0U;
        const bool vflip = (entry & 0x0800U) != 0U;
        const uint32_t pal_bank = (entry >> 12U) & 0x0FU;

        const uint32_t px = hflip ? (7U - fine_x) : fine_x;
        const uint32_t py = vflip ? (7U - fine_y) : fine_y;

        uint8_t color_idx = 0U;
        if (is_8bpp) {
            const uint32_t chr_addr = char_base + tile_num * 64U + py * 8U + px;
            if (chr_addr < 0x10000U) {
                color_idx = vram_[chr_addr];
            }
            if (color_idx != 0U) {
                line_color[x] = read_palette16(static_cast<uint32_t>(color_idx) * 2U);
                line_prio[x] = prio;
                line_layer[x] = static_cast<uint8_t>(bg_idx);
            }
        } else {
            const uint32_t chr_addr = char_base + tile_num * 32U + py * 4U + (px >> 1U);
            if (chr_addr < 0x10000U) {
                const uint8_t b = vram_[chr_addr];
                color_idx = (px & 1U) ? ((b >> 4U) & 0x0FU) : (b & 0x0FU);
            }
            if (color_idx != 0U) {
                line_color[x] = read_palette16((pal_bank * 16U + color_idx) * 2U);
                line_prio[x] = prio;
                line_layer[x] = static_cast<uint8_t>(bg_idx);
            }
        }
    }
}

void Ppu::render_affine_bg(int bg_idx, uint16_t /*line*/, uint16_t* line_color, uint8_t* line_prio,
                           uint8_t* line_layer) {
    const int aff_idx = bg_idx - 2;
    if (aff_idx < 0 || aff_idx > 1) {
        return;
    }

    const uint16_t cnt = bgcnt_[bg_idx];
    const uint8_t prio = static_cast<uint8_t>(cnt & 3U);
    const uint32_t char_base = ((cnt >> 2U) & 3U) * 0x4000U;
    const uint32_t screen_base = ((cnt >> 8U) & 0x1FU) * 0x800U;
    const bool wrap = (cnt & 0x2000U) != 0U;
    const uint32_t size_mode = (cnt >> 14U) & 3U;
    const int32_t dim = 128 << size_mode;  // 128, 256, 512, 1024 pixels
    const uint32_t tiles_per_row = static_cast<uint32_t>(dim >> 3);

    int32_t ref_x = bgx_internal_[aff_idx];
    int32_t ref_y = bgy_internal_[aff_idx];
    const int32_t pa = bgpa_[aff_idx];
    const int32_t pc = bgpc_[aff_idx];

    for (uint32_t x = 0; x < kScreenWidth; ++x) {
        int32_t tex_x = ref_x >> 8;
        int32_t tex_y = ref_y >> 8;
        ref_x += pa;
        ref_y += pc;

        if (prio >= line_prio[x]) {
            continue;
        }

        if (tex_x < 0 || tex_x >= dim || tex_y < 0 || tex_y >= dim) {
            if (!wrap) {
                continue;
            }
            tex_x &= (dim - 1);
            tex_y &= (dim - 1);
        }

        const uint32_t tile_x = static_cast<uint32_t>(tex_x) >> 3U;
        const uint32_t tile_y = static_cast<uint32_t>(tex_y) >> 3U;
        const uint32_t fine_x = static_cast<uint32_t>(tex_x) & 7U;
        const uint32_t fine_y = static_cast<uint32_t>(tex_y) & 7U;

        const uint32_t map_addr = screen_base + tile_y * tiles_per_row + tile_x;
        if (map_addr >= 0x10000U) {
            continue;
        }
        const uint8_t tile_num = vram_[map_addr];
        const uint32_t chr_addr =
            char_base + static_cast<uint32_t>(tile_num) * 64U + fine_y * 8U + fine_x;
        if (chr_addr >= 0x10000U) {
            continue;
        }
        const uint8_t color_idx = vram_[chr_addr];
        if (color_idx != 0U) {
            line_color[x] = read_palette16(static_cast<uint32_t>(color_idx) * 2U);
            line_prio[x] = prio;
            line_layer[x] = static_cast<uint8_t>(bg_idx);
        }
    }

    bgx_internal_[aff_idx] += bgpb_[aff_idx];
    bgy_internal_[aff_idx] += bgpd_[aff_idx];
}

void Ppu::render_sprites(uint16_t line, uint16_t* obj_color, uint8_t* obj_prio, bool* obj_semi,
                         bool* obj_win) {
    static constexpr uint8_t kObjWidths[3][4] = {
        {8, 16, 32, 64},   // Square
        {16, 32, 32, 64},  // Horizontal
        {8, 8, 16, 32}     // Vertical
    };
    static constexpr uint8_t kObjHeights[3][4] = {
        {8, 16, 32, 64},  // Square
        {8, 8, 16, 32},   // Horizontal
        {16, 32, 32, 64}  // Vertical
    };

    const bool mapping_1d = (dispcnt_ & 0x0040U) != 0U;
    const uint8_t video_mode = static_cast<uint8_t>(dispcnt_ & 7U);
    const uint32_t min_tile = (video_mode >= 3U) ? 512U : 0U;

    for (int i = 0; i < 128; ++i) {
        const uint32_t base = static_cast<uint32_t>(i) * 8U;
        const uint16_t attr0 = read_oam16(base + 0U);
        const uint16_t attr1 = read_oam16(base + 2U);
        const uint16_t attr2 = read_oam16(base + 4U);

        const uint8_t obj_mode = (attr0 >> 8U) & 3U;
        if (obj_mode == 2U) {
            continue;  // Disabled normal sprite
        }

        const uint8_t shape = (attr0 >> 14U) & 3U;
        if (shape == 3U) {
            continue;
        }
        const uint8_t size_idx = (attr1 >> 14U) & 3U;
        const int32_t w = kObjWidths[shape][size_idx];
        const int32_t h = kObjHeights[shape][size_idx];

        const bool is_affine = (attr0 & 0x0100U) != 0U;
        const bool double_size = is_affine && ((attr0 & 0x0200U) != 0U);
        const int32_t box_w = double_size ? (w * 2) : w;
        const int32_t box_h = double_size ? (h * 2) : h;

        int32_t sy = attr0 & 0xFFU;
        if (sy >= 160 && sy + box_h > 256) {
            sy -= 256;
        }
        const int32_t local_y = static_cast<int32_t>(line) - sy;
        if (local_y < 0 || local_y >= box_h) {
            continue;
        }

        int32_t sx = attr1 & 0x01FFU;
        if (sx >= 240) {
            sx -= 512;
        }

        const bool is_8bpp = (attr0 & 0x2000U) != 0U;
        const uint8_t gfx_mode = (attr0 >> 10U) & 3U;
        const uint32_t base_tile = attr2 & 0x03FFU;
        if (base_tile < min_tile) {
            continue;
        }
        const uint8_t prio = (attr2 >> 10U) & 3U;
        const uint32_t pal_bank = (attr2 >> 12U) & 0x0FU;

        int16_t pa = 0x0100, pb = 0, pc = 0, pd = 0x0100;
        if (is_affine) {
            const uint32_t aff_idx = (attr1 >> 9U) & 0x1FU;
            const uint32_t aff_base = aff_idx * 32U;
            pa = static_cast<int16_t>(read_oam16(aff_base + 0x06U));
            pb = static_cast<int16_t>(read_oam16(aff_base + 0x0EU));
            pc = static_cast<int16_t>(read_oam16(aff_base + 0x16U));
            pd = static_cast<int16_t>(read_oam16(aff_base + 0x1EU));
        }

        for (int32_t lx = 0; lx < box_w; ++lx) {
            const int32_t screen_x = sx + lx;
            if (screen_x < 0 || screen_x >= static_cast<int32_t>(kScreenWidth)) {
                continue;
            }

            int32_t tex_x = lx;
            int32_t tex_y = local_y;
            if (is_affine) {
                const int32_t cx = box_w / 2;
                const int32_t cy = box_h / 2;
                const int32_t dx = lx - cx;
                const int32_t dy = local_y - cy;
                tex_x = ((pa * dx + pb * dy) >> 8) + (w / 2);
                tex_y = ((pc * dx + pd * dy) >> 8) + (h / 2);
                if (tex_x < 0 || tex_x >= w || tex_y < 0 || tex_y >= h) {
                    continue;
                }
            } else {
                if (attr1 & 0x1000U) {
                    tex_x = (w - 1) - tex_x;
                }
                if (attr1 & 0x2000U) {
                    tex_y = (h - 1) - tex_y;
                }
            }

            const uint32_t tx = static_cast<uint32_t>(tex_x) >> 3U;
            const uint32_t ty = static_cast<uint32_t>(tex_y) >> 3U;
            const uint32_t fx = static_cast<uint32_t>(tex_x) & 7U;
            const uint32_t fy = static_cast<uint32_t>(tex_y) & 7U;

            const uint32_t tiles_w = static_cast<uint32_t>(w >> 3);
            uint32_t tile_idx = 0U;
            if (mapping_1d) {
                const uint32_t step = is_8bpp ? 2U : 1U;
                tile_idx = base_tile + (ty * tiles_w + tx) * step;
            } else {
                const uint32_t step = is_8bpp ? 2U : 1U;
                tile_idx = (base_tile & ~31U) + ty * 32U + ((base_tile & 31U) + tx * step);
            }
            tile_idx &= 0x03FFU;

            uint8_t color_idx = 0U;
            if (is_8bpp) {
                const uint32_t addr = 0x10000U + ((tile_idx * 32U + fy * 8U + fx) & 0x7FFFU);
                color_idx = vram_[addr];
            } else {
                const uint32_t addr =
                    0x10000U + ((tile_idx * 32U + fy * 4U + (fx >> 1U)) & 0x7FFFU);
                const uint8_t b = vram_[addr];
                color_idx = (fx & 1U) ? ((b >> 4U) & 0x0FU) : (b & 0x0FU);
            }

            if (color_idx == 0U) {
                continue;
            }

            if (gfx_mode == 2U) {
                obj_win[screen_x] = true;
            } else if (prio < obj_prio[screen_x]) {
                const uint32_t pal_offset =
                    0x200U + (is_8bpp ? color_idx : (pal_bank * 16U + color_idx)) * 2U;
                obj_color[screen_x] = read_palette16(pal_offset);
                obj_prio[screen_x] = prio;
                obj_semi[screen_x] = (gfx_mode == 1U);
            }
        }
    }
}

void Ppu::render_scanline(uint16_t line) {
    if (line >= kScreenHeight) {
        return;
    }

    uint32_t* dst = &framebuffer_[static_cast<size_t>(line) * kScreenWidth];

    // Forced blank (DISPCNT bit 7) renders white
    if (dispcnt_ & 0x0080U) {
        std::fill(dst, dst + kScreenWidth, 0xFFFFFFFFU);
        return;
    }

    const uint16_t backdrop = read_palette16(0U);
    const uint8_t mode = static_cast<uint8_t>(dispcnt_ & 7U);

    std::array<uint16_t, kScreenWidth> top_color;
    std::array<uint8_t, kScreenWidth> top_prio;
    std::array<uint8_t, kScreenWidth> top_layer;  // 0..3=BG, 4=OBJ, 5=BD
    top_color.fill(backdrop);
    top_prio.fill(4U);
    top_layer.fill(5U);

    if (mode == 0U) {
        for (int bg = 0; bg < 4; ++bg) {
            if (dispcnt_ & (1U << (8 + bg))) {
                render_regular_bg(bg, line, top_color.data(), top_prio.data(), top_layer.data());
            }
        }
    } else if (mode == 1U) {
        for (int bg = 0; bg < 2; ++bg) {
            if (dispcnt_ & (1U << (8 + bg))) {
                render_regular_bg(bg, line, top_color.data(), top_prio.data(), top_layer.data());
            }
        }
        if (dispcnt_ & (1U << 10)) {
            render_affine_bg(2, line, top_color.data(), top_prio.data(), top_layer.data());
        }
    } else if (mode == 2U) {
        if (dispcnt_ & (1U << 10)) {
            render_affine_bg(2, line, top_color.data(), top_prio.data(), top_layer.data());
        }
        if (dispcnt_ & (1U << 11)) {
            render_affine_bg(3, line, top_color.data(), top_prio.data(), top_layer.data());
        }
    } else if (mode == 3U) {
        if (dispcnt_ & (1U << 10)) {
            const uint8_t prio = static_cast<uint8_t>(bgcnt_[2] & 3U);
            const uint32_t row_base = static_cast<uint32_t>(line) * kScreenWidth * 2U;
            for (uint32_t x = 0; x < kScreenWidth; ++x) {
                top_color[x] = read_vram16(row_base + x * 2U);
                top_prio[x] = prio;
                top_layer[x] = 2U;
            }
        }
    } else if (mode == 4U) {
        if (dispcnt_ & (1U << 10)) {
            const uint8_t prio = static_cast<uint8_t>(bgcnt_[2] & 3U);
            const uint32_t frame_base = (dispcnt_ & 0x0010U) ? 0xA000U : 0x0000U;
            const uint32_t row_base = frame_base + static_cast<uint32_t>(line) * kScreenWidth;
            for (uint32_t x = 0; x < kScreenWidth; ++x) {
                const uint8_t idx = vram_[row_base + x];
                if (idx != 0U) {
                    top_color[x] = read_palette16(static_cast<uint32_t>(idx) * 2U);
                    top_prio[x] = prio;
                    top_layer[x] = 2U;
                }
            }
        }
    } else if (mode == 5U) {
        if ((dispcnt_ & (1U << 10)) && line < 128U) {
            const uint8_t prio = static_cast<uint8_t>(bgcnt_[2] & 3U);
            const uint32_t frame_base = (dispcnt_ & 0x0010U) ? 0xA000U : 0x0000U;
            const uint32_t row_base = frame_base + static_cast<uint32_t>(line) * 160U * 2U;
            for (uint32_t x = 0; x < 160U; ++x) {
                top_color[x] = read_vram16(row_base + x * 2U);
                top_prio[x] = prio;
                top_layer[x] = 2U;
            }
        }
    }

    std::array<uint16_t, kScreenWidth> obj_color{};
    std::array<uint8_t, kScreenWidth> obj_prio;
    std::array<bool, kScreenWidth> obj_semi{};
    std::array<bool, kScreenWidth> obj_win{};
    obj_prio.fill(4U);

    if (dispcnt_ & 0x1000U) {
        render_sprites(line, obj_color.data(), obj_prio.data(), obj_semi.data(), obj_win.data());
    }

    const uint32_t bld_mode = (bldcnt_ >> 6U) & 3U;
    const uint32_t eva = std::min<uint32_t>(16U, bldalpha_ & 0x1FU);
    const uint32_t evb = std::min<uint32_t>(16U, (bldalpha_ >> 8U) & 0x1FU);
    const uint32_t evy = std::min<uint32_t>(16U, bldy_ & 0x1FU);

    const bool win_any = (dispcnt_ & 0xE000U) != 0U;
    const bool win0_en = (dispcnt_ & 0x2000U) != 0U;
    const bool win1_en = (dispcnt_ & 0x4000U) != 0U;
    const bool winobj_en = (dispcnt_ & 0x8000U) != 0U;

    auto in_range = [](uint32_t pos, uint8_t start, uint8_t end) -> bool {
        if (start <= end) {
            return pos >= start && pos < end;
        }
        return pos >= start || pos < end;
    };

    const bool y_in_win0 =
        win0_en && in_range(line, static_cast<uint8_t>(win0v_ >> 8U), static_cast<uint8_t>(win0v_));
    const bool y_in_win1 =
        win1_en && in_range(line, static_cast<uint8_t>(win1v_ >> 8U), static_cast<uint8_t>(win1v_));

    for (uint32_t x = 0; x < kScreenWidth; ++x) {
        uint8_t win_mask = 0x3FU;
        if (win_any) {
            if (y_in_win0 &&
                in_range(x, static_cast<uint8_t>(win0h_ >> 8U), static_cast<uint8_t>(win0h_))) {
                win_mask = static_cast<uint8_t>(winin_ & 0x3FU);
            } else if (y_in_win1 && in_range(x, static_cast<uint8_t>(win1h_ >> 8U),
                                             static_cast<uint8_t>(win1h_))) {
                win_mask = static_cast<uint8_t>((winin_ >> 8U) & 0x3FU);
            } else if (winobj_en && obj_win[x]) {
                win_mask = static_cast<uint8_t>((winout_ >> 8U) & 0x3FU);
            } else {
                win_mask = static_cast<uint8_t>(winout_ & 0x3FU);
            }
        }

        uint16_t final_col = top_color[x];
        uint8_t first_layer = top_layer[x];
        uint16_t second_col = backdrop;
        uint8_t second_layer = 5U;

        if (first_layer < 4U && ((win_mask >> first_layer) & 1U) == 0U) {
            final_col = backdrop;
            first_layer = 5U;
        }

        const bool obj_visible = ((win_mask & 0x10U) != 0U) && (obj_prio[x] < 4U);
        const bool sfx_enabled = (win_mask & 0x20U) != 0U;

        if (obj_visible && (first_layer == 5U || obj_prio[x] <= top_prio[x])) {
            second_col = final_col;
            second_layer = first_layer;
            final_col = obj_color[x];
            first_layer = 4U;

            if (obj_semi[x] && ((bldcnt_ >> (8U + second_layer)) & 1U)) {
                final_col = blend_rgb555(final_col, second_col, eva, evb);
                dst[x] = rgb555_to_rgba32(final_col);
                continue;
            }
        }

        const bool is_first_target = ((bldcnt_ >> first_layer) & 1U) != 0U;
        if (sfx_enabled && is_first_target && bld_mode != 0U) {
            if (bld_mode == 1U && ((bldcnt_ >> (8U + second_layer)) & 1U)) {
                final_col = blend_rgb555(final_col, second_col, eva, evb);
            } else if (bld_mode == 2U) {
                final_col = brighten_rgb555(final_col, evy);
            } else if (bld_mode == 3U) {
                final_col = darken_rgb555(final_col, evy);
            }
        }

        dst[x] = rgb555_to_rgba32(final_col);
    }
}

void Ppu::save_state(StatePayload& p) const noexcept {
    p.dispcnt = dispcnt_;
    p.dispstat = dispstat_;
    p.vcount = vcount_;
    p.line_cycles = line_cycles_;
    p.in_hblank = in_hblank_ ? 1U : 0U;
    p.frame_complete = frame_complete_ ? 1U : 0U;
    p.bgcnt = bgcnt_;
    p.bghofs = bghofs_;
    p.bgvofs = bgvofs_;
    p.bgpa = bgpa_;
    p.bgpb = bgpb_;
    p.bgpc = bgpc_;
    p.bgpd = bgpd_;
    p.bgx_latch = bgx_latch_;
    p.bgy_latch = bgy_latch_;
    p.bgx_internal = bgx_internal_;
    p.bgy_internal = bgy_internal_;
    p.bgx_raw = bgx_raw_;
    p.bgy_raw = bgy_raw_;
    p.win0h = win0h_;
    p.win1h = win1h_;
    p.win0v = win0v_;
    p.win1v = win1v_;
    p.winin = winin_;
    p.winout = winout_;
    p.mosaic = mosaic_;
    p.bldcnt = bldcnt_;
    p.bldalpha = bldalpha_;
    p.bldy = bldy_;
}

void Ppu::load_state(const StatePayload& p) noexcept {
    dispcnt_ = p.dispcnt;
    dispstat_ = p.dispstat;
    vcount_ = p.vcount;
    line_cycles_ = p.line_cycles;
    in_hblank_ = (p.in_hblank != 0U);
    frame_complete_ = (p.frame_complete != 0U);
    bgcnt_ = p.bgcnt;
    bghofs_ = p.bghofs;
    bgvofs_ = p.bgvofs;
    bgpa_ = p.bgpa;
    bgpb_ = p.bgpb;
    bgpc_ = p.bgpc;
    bgpd_ = p.bgpd;
    bgx_latch_ = p.bgx_latch;
    bgy_latch_ = p.bgy_latch;
    bgx_internal_ = p.bgx_internal;
    bgy_internal_ = p.bgy_internal;
    bgx_raw_ = p.bgx_raw;
    bgy_raw_ = p.bgy_raw;
    win0h_ = p.win0h;
    win1h_ = p.win1h;
    win0v_ = p.win0v;
    win1v_ = p.win1v;
    winin_ = p.winin;
    winout_ = p.winout;
    mosaic_ = p.mosaic;
    bldcnt_ = p.bldcnt;
    bldalpha_ = p.bldalpha;
    bldy_ = p.bldy;
}

}  // namespace dragon::gba
