#include "nds/ppu2d.h"

#include <algorithm>

#include "nds/bus.h"

namespace dragon::nds {

uint32_t Engine2D::bgr555_to_rgba(uint16_t c) noexcept {
    const uint8_t r = static_cast<uint8_t>(((c & 0x1FU) << 3U) | ((c & 0x1FU) >> 2U));
    const uint8_t g =
        static_cast<uint8_t>((((c >> 5U) & 0x1FU) << 3U) | (((c >> 5U) & 0x1FU) >> 2U));
    const uint8_t b =
        static_cast<uint8_t>((((c >> 10U) & 0x1FU) << 3U) | (((c >> 10U) & 0x1FU) >> 2U));
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8U) |
           (static_cast<uint32_t>(b) << 16U) | 0xFF000000U;
}

uint32_t Engine2D::apply_master_bright(uint32_t rgba) const noexcept {
    const uint32_t mode = (master_bright_ >> 14U) & 3U;
    if (mode == 0U || mode == 3U) {
        return rgba;
    }
    const uint32_t factor = std::min<uint32_t>(16U, master_bright_ & 0x1FU);
    if (factor == 0U) {
        return rgba;
    }
    uint32_t r = rgba & 0xFFU;
    uint32_t g = (rgba >> 8U) & 0xFFU;
    uint32_t b = (rgba >> 16U) & 0xFFU;
    if (mode == 1U) {  // Brighten toward white
        r = r + (((255U - r) * factor) >> 4U);
        g = g + (((255U - g) * factor) >> 4U);
        b = b + (((255U - b) * factor) >> 4U);
    } else {  // Darken toward black
        r = r - ((r * factor) >> 4U);
        g = g - ((g * factor) >> 4U);
        b = b - ((b * factor) >> 4U);
    }
    return (r & 0xFFU) | ((g & 0xFFU) << 8U) | ((b & 0xFFU) << 16U) | 0xFF000000U;
}

void Engine2D::reset() noexcept {
    dispcnt_ = 0;
    bgcnt_.fill(0);
    bghofs_.fill(0);
    bgvofs_.fill(0);
    bg_pa_ = {0x100, 0x100};
    bg_pb_ = {0, 0};
    bg_pc_ = {0, 0};
    bg_pd_ = {0x100, 0x100};
    bg_x_ = {0, 0};
    bg_y_ = {0, 0};
    bg_ref_x_ = {0, 0};
    bg_ref_y_ = {0, 0};
    bldcnt_ = 0;
    bldalpha_ = 0;
    bldy_ = 0;
    master_bright_ = 0;
    fb_.fill(0xFF000000U);
}

uint32_t Engine2D::read32(uint32_t offset) const noexcept {
    switch (offset) {
        case 0x00U:
            return dispcnt_;
        case 0x08U:
            return bgcnt_[0] | (static_cast<uint32_t>(bgcnt_[1]) << 16U);
        case 0x0CU:
            return bgcnt_[2] | (static_cast<uint32_t>(bgcnt_[3]) << 16U);
        case 0x50U:
            return bldcnt_ | (static_cast<uint32_t>(bldalpha_) << 16U);
        case 0x54U:
            return bldy_;
        default:
            return 0U;
    }
}

void Engine2D::write32(uint32_t offset, uint32_t val, uint32_t mask) noexcept {
    auto write16 = [&](uint16_t& dst, uint16_t v, bool hi) {
        if (hi ? (mask & 0xFFFF0000U) : (mask & 0x0000FFFFU)) {
            dst = v;
        }
    };
    switch (offset) {
        case 0x00U:
            dispcnt_ = (dispcnt_ & ~mask) | (val & mask);
            break;
        case 0x08U:
            write16(bgcnt_[0], static_cast<uint16_t>(val & 0xFFFFU), false);
            write16(bgcnt_[1], static_cast<uint16_t>(val >> 16U), true);
            break;
        case 0x0CU:
            write16(bgcnt_[2], static_cast<uint16_t>(val & 0xFFFFU), false);
            write16(bgcnt_[3], static_cast<uint16_t>(val >> 16U), true);
            break;
        case 0x10U:
            write16(bghofs_[0], static_cast<uint16_t>(val & 0x1FFU), false);
            write16(bgvofs_[0], static_cast<uint16_t>((val >> 16U) & 0x1FFU), true);
            break;
        case 0x14U:
            write16(bghofs_[1], static_cast<uint16_t>(val & 0x1FFU), false);
            write16(bgvofs_[1], static_cast<uint16_t>((val >> 16U) & 0x1FFU), true);
            break;
        case 0x18U:
            write16(bghofs_[2], static_cast<uint16_t>(val & 0x1FFU), false);
            write16(bgvofs_[2], static_cast<uint16_t>((val >> 16U) & 0x1FFU), true);
            break;
        case 0x1CU:
            write16(bghofs_[3], static_cast<uint16_t>(val & 0x1FFU), false);
            write16(bgvofs_[3], static_cast<uint16_t>((val >> 16U) & 0x1FFU), true);
            break;
        case 0x20U:
            if (mask & 0xFFFFU)
                bg_pa_[0] = static_cast<int16_t>(val & 0xFFFFU);
            if (mask & 0xFFFF0000U)
                bg_pb_[0] = static_cast<int16_t>(val >> 16U);
            break;
        case 0x24U:
            if (mask & 0xFFFFU)
                bg_pc_[0] = static_cast<int16_t>(val & 0xFFFFU);
            if (mask & 0xFFFF0000U)
                bg_pd_[0] = static_cast<int16_t>(val >> 16U);
            break;
        case 0x28U: {
            uint32_t cur = static_cast<uint32_t>(bg_x_[0]);
            cur = (cur & ~mask) | (val & mask);
            if (cur & 0x08000000U)
                cur |= 0xF0000000U;
            bg_x_[0] = static_cast<int32_t>(cur);
            bg_ref_x_[0] = bg_x_[0];
            break;
        }
        case 0x2CU: {
            uint32_t cur = static_cast<uint32_t>(bg_y_[0]);
            cur = (cur & ~mask) | (val & mask);
            if (cur & 0x08000000U)
                cur |= 0xF0000000U;
            bg_y_[0] = static_cast<int32_t>(cur);
            bg_ref_y_[0] = bg_y_[0];
            break;
        }
        case 0x30U:
            if (mask & 0xFFFFU)
                bg_pa_[1] = static_cast<int16_t>(val & 0xFFFFU);
            if (mask & 0xFFFF0000U)
                bg_pb_[1] = static_cast<int16_t>(val >> 16U);
            break;
        case 0x34U:
            if (mask & 0xFFFFU)
                bg_pc_[1] = static_cast<int16_t>(val & 0xFFFFU);
            if (mask & 0xFFFF0000U)
                bg_pd_[1] = static_cast<int16_t>(val >> 16U);
            break;
        case 0x38U: {
            uint32_t cur = static_cast<uint32_t>(bg_x_[1]);
            cur = (cur & ~mask) | (val & mask);
            if (cur & 0x08000000U)
                cur |= 0xF0000000U;
            bg_x_[1] = static_cast<int32_t>(cur);
            bg_ref_x_[1] = bg_x_[1];
            break;
        }
        case 0x3CU: {
            uint32_t cur = static_cast<uint32_t>(bg_y_[1]);
            cur = (cur & ~mask) | (val & mask);
            if (cur & 0x08000000U)
                cur |= 0xF0000000U;
            bg_y_[1] = static_cast<int32_t>(cur);
            bg_ref_y_[1] = bg_y_[1];
            break;
        }
        case 0x50U:
            write16(bldcnt_, static_cast<uint16_t>(val & 0xFFFFU), false);
            write16(bldalpha_, static_cast<uint16_t>(val >> 16U), true);
            break;
        case 0x54U:
            write16(bldy_, static_cast<uint16_t>(val & 0x1FU), false);
            break;
        default:
            break;
    }
}

void Engine2D::render_text_bg(uint32_t y, int bg, const Bus& bus, uint32_t* line_col,
                              uint8_t* line_prio) noexcept {
    const uint16_t cnt = bgcnt_[bg];
    const uint8_t prio = static_cast<uint8_t>(cnt & 3U);
    const bool is_8bpp = (cnt & (1U << 7)) != 0U;
    const uint32_t char_step = is_engine_a_ ? (((dispcnt_ >> 24U) & 7U) * 0x10000U) : 0U;
    const uint32_t scr_step = is_engine_a_ ? (((dispcnt_ >> 27U) & 7U) * 0x10000U) : 0U;
    const uint32_t char_base = char_step + (((cnt >> 2U) & 0x0FU) * 0x4000U);
    const uint32_t scr_base = scr_step + (((cnt >> 8U) & 0x1FU) * 0x0800U);

    const uint32_t sz = (cnt >> 14U) & 3U;
    const uint32_t map_w = (sz & 1U) ? 512U : 256U;
    const uint32_t map_h = (sz & 2U) ? 512U : 256U;

    const uint32_t sy = (y + bgvofs_[bg]) & (map_h - 1U);
    const uint32_t ty = sy >> 3U;
    const uint32_t py = sy & 7U;

    auto read_vram = [&](uint32_t off) -> uint8_t {
        return is_engine_a_ ? bus.read_vram_bg_a(off) : bus.read_vram_bg_b(off);
    };

    for (uint32_t x = 0; x < kWidth; ++x) {
        if (prio > line_prio[x])
            continue;
        const uint32_t sx = (x + bghofs_[bg]) & (map_w - 1U);
        const uint32_t tx = sx >> 3U;
        const uint32_t px = sx & 7U;

        uint32_t block = 0U;
        if (tx >= 32U)
            block += 1U;
        if (ty >= 32U)
            block += (map_w == 512U) ? 2U : 1U;

        const uint32_t entry_off = scr_base + block * 0x800U + ((ty & 31U) * 32U + (tx & 31U)) * 2U;
        const uint16_t entry = static_cast<uint16_t>(
            read_vram(entry_off) | (static_cast<uint16_t>(read_vram(entry_off + 1U)) << 8U));
        const uint32_t tile = entry & 0x03FFU;
        const bool hflip = (entry & 0x0400U) != 0U;
        const bool vflip = (entry & 0x0800U) != 0U;
        const uint32_t pal = (entry >> 12U) & 0x0FU;

        const uint32_t fx = hflip ? (7U - px) : px;
        const uint32_t fy = vflip ? (7U - py) : py;

        uint8_t color_idx = 0;
        if (is_8bpp) {
            color_idx = read_vram(char_base + tile * 64U + fy * 8U + fx);
            if (color_idx == 0U)
                continue;
            const bool ext_pal = (dispcnt_ & (1U << 30)) != 0U;
            if (ext_pal) {
                const int slot = (bg < 2 && ((cnt & (1U << 13)) != 0U)) ? (bg + 2) : bg;
                line_col[x] =
                    bgr555_to_rgba(bus.read_bg_ext_pal16(is_engine_a_, slot, pal, color_idx));
            } else {
                line_col[x] = bgr555_to_rgba(bus.read_pal16(is_engine_a_, color_idx * 2U));
            }
        } else {
            const uint8_t b = read_vram(char_base + tile * 32U + fy * 4U + (fx >> 1U));
            color_idx = (fx & 1U) ? (b >> 4U) : (b & 0x0FU);
            if (color_idx == 0U)
                continue;
            line_col[x] =
                bgr555_to_rgba(bus.read_pal16(is_engine_a_, (pal * 16U + color_idx) * 2U));
        }
        line_prio[x] = prio;
    }
}

void Engine2D::render_affine_or_ext_bg(uint32_t y, int bg, const Bus& bus, uint32_t* line_col,
                                       uint8_t* line_prio) noexcept {
    (void)y;
    const int idx = bg - 2;
    const uint16_t cnt = bgcnt_[bg];
    const uint8_t prio = static_cast<uint8_t>(cnt & 3U);
    const bool wrap = (cnt & (1U << 13)) != 0U;
    const uint32_t bg_mode = dispcnt_ & 7U;
    const bool is_ext = (bg_mode == 3U && bg == 3) || (bg_mode == 4U && bg == 3) || (bg_mode == 5U);
    const bool is_direct_bmp = is_ext && ((cnt & (1U << 7)) != 0U) && ((cnt & (1U << 2)) != 0U);
    const bool is_256_bmp = is_ext && ((cnt & (1U << 7)) != 0U) && ((cnt & (1U << 2)) == 0U);

    const uint32_t sz = (cnt >> 14U) & 3U;
    uint32_t w = 128U << sz;
    uint32_t h = 128U << sz;
    if (is_direct_bmp || is_256_bmp) {
        static constexpr uint32_t kBmpW[4] = {128U, 256U, 512U, 512U};
        static constexpr uint32_t kBmpH[4] = {128U, 256U, 256U, 512U};
        w = kBmpW[sz];
        h = kBmpH[sz];
    }

    auto read_vram = [&](uint32_t off) -> uint8_t {
        return is_engine_a_ ? bus.read_vram_bg_a(off) : bus.read_vram_bg_b(off);
    };

    const uint32_t bmp_base = ((cnt >> 8U) & 0x1FU) * 0x4000U;
    const uint32_t char_step = is_engine_a_ ? (((dispcnt_ >> 24U) & 7U) * 0x10000U) : 0U;
    const uint32_t scr_step = is_engine_a_ ? (((dispcnt_ >> 27U) & 7U) * 0x10000U) : 0U;
    const uint32_t char_base = char_step + (((cnt >> 2U) & 0x0FU) * 0x4000U);
    const uint32_t scr_base = scr_step + (((cnt >> 8U) & 0x1FU) * 0x0800U);

    int32_t rx = bg_ref_x_[idx];
    int32_t ry = bg_ref_y_[idx];

    for (uint32_t x = 0; x < kWidth; ++x) {
        int32_t tx = rx >> 8;
        int32_t ty = ry >> 8;
        rx += bg_pa_[idx];
        ry += bg_pc_[idx];

        if (prio > line_prio[x])
            continue;

        if (wrap) {
            tx &= static_cast<int32_t>(w - 1U);
            ty &= static_cast<int32_t>(h - 1U);
        } else if (tx < 0 || ty < 0 || tx >= static_cast<int32_t>(w) ||
                   ty >= static_cast<int32_t>(h)) {
            continue;
        }

        if (is_direct_bmp) {
            const uint32_t off =
                bmp_base + (static_cast<uint32_t>(ty) * w + static_cast<uint32_t>(tx)) * 2U;
            const uint16_t c = static_cast<uint16_t>(
                read_vram(off) | (static_cast<uint16_t>(read_vram(off + 1U)) << 8U));
            if (c & 0x8000U) {
                line_col[x] = bgr555_to_rgba(c);
                line_prio[x] = prio;
            }
        } else if (is_256_bmp) {
            const uint8_t ci =
                read_vram(bmp_base + static_cast<uint32_t>(ty) * w + static_cast<uint32_t>(tx));
            if (ci != 0U) {
                line_col[x] = bgr555_to_rgba(bus.read_pal16(is_engine_a_, ci * 2U));
                line_prio[x] = prio;
            }
        } else if (is_ext) {
            const uint32_t tile_x = static_cast<uint32_t>(tx) >> 3U;
            const uint32_t tile_y = static_cast<uint32_t>(ty) >> 3U;
            const uint32_t entry_off = scr_base + (tile_y * (w >> 3U) + tile_x) * 2U;
            const uint16_t entry = static_cast<uint16_t>(
                read_vram(entry_off) | (static_cast<uint16_t>(read_vram(entry_off + 1U)) << 8U));
            const uint32_t tile = entry & 0x03FFU;
            const uint32_t px = (entry & 0x0400U) ? (7U - (static_cast<uint32_t>(tx) & 7U))
                                                  : (static_cast<uint32_t>(tx) & 7U);
            const uint32_t py = (entry & 0x0800U) ? (7U - (static_cast<uint32_t>(ty) & 7U))
                                                  : (static_cast<uint32_t>(ty) & 7U);
            const uint32_t pal = (entry >> 12U) & 0x0FU;
            const uint8_t ci = read_vram(char_base + tile * 64U + py * 8U + px);
            if (ci != 0U) {
                const bool ext_pal = (dispcnt_ & (1U << 30)) != 0U;
                line_col[x] =
                    bgr555_to_rgba(ext_pal ? bus.read_bg_ext_pal16(is_engine_a_, bg, pal, ci)
                                           : bus.read_pal16(is_engine_a_, ci * 2U));
                line_prio[x] = prio;
            }
        } else {
            const uint32_t tile_x = static_cast<uint32_t>(tx) >> 3U;
            const uint32_t tile_y = static_cast<uint32_t>(ty) >> 3U;
            const uint32_t tile = read_vram(scr_base + tile_y * (w >> 3U) + tile_x);
            const uint8_t ci =
                read_vram(char_base + tile * 64U + (static_cast<uint32_t>(ty) & 7U) * 8U +
                          (static_cast<uint32_t>(tx) & 7U));
            if (ci != 0U) {
                line_col[x] = bgr555_to_rgba(bus.read_pal16(is_engine_a_, ci * 2U));
                line_prio[x] = prio;
            }
        }
    }

    bg_ref_x_[idx] += bg_pb_[idx];
    bg_ref_y_[idx] += bg_pd_[idx];
}

void Engine2D::render_sprites(uint32_t y, const Bus& bus, uint32_t* line_col,
                              uint8_t* line_prio) noexcept {
    static constexpr uint32_t kObjW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static constexpr uint32_t kObjH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

    const bool mapping_1d = (dispcnt_ & (1U << 4)) != 0U;
    const uint32_t tile_step = mapping_1d ? (32U << ((dispcnt_ >> 20U) & 3U)) : 32U;

    auto read_obj = [&](uint32_t off) -> uint8_t {
        return is_engine_a_ ? bus.read_vram_obj_a(off) : bus.read_vram_obj_b(off);
    };

    for (int i = 127; i >= 0; --i) {
        const uint16_t attr0 = bus.read_oam16(is_engine_a_, static_cast<uint32_t>(i * 8 + 0));
        const uint16_t attr1 = bus.read_oam16(is_engine_a_, static_cast<uint32_t>(i * 8 + 2));
        const uint16_t attr2 = bus.read_oam16(is_engine_a_, static_cast<uint32_t>(i * 8 + 4));

        const uint32_t rot_flag = (attr0 >> 8U) & 3U;
        if (rot_flag == 2U)
            continue;  // Hidden
        const uint32_t obj_mode = (attr0 >> 10U) & 3U;
        if (obj_mode == 2U)
            continue;  // OBJ Window mask (not directly rendered)
        const uint32_t shape = (attr0 >> 14U) & 3U;
        if (shape == 3U)
            continue;
        const uint32_t size = (attr1 >> 14U) & 3U;
        const uint32_t w = kObjW[shape][size];
        const uint32_t h = kObjH[shape][size];

        int32_t sy = static_cast<int32_t>(attr0 & 0xFFU);
        if (sy >= 192)
            sy -= 256;
        if (static_cast<int32_t>(y) < sy || static_cast<int32_t>(y) >= sy + static_cast<int32_t>(h))
            continue;

        int32_t sx = static_cast<int32_t>(attr1 & 0x1FFU);
        if (sx >= 256)
            sx -= 512;

        const bool is_8bpp = (attr0 & (1U << 13)) != 0U;
        const bool hflip = (rot_flag == 0U) && ((attr1 & (1U << 12)) != 0U);
        const bool vflip = (rot_flag == 0U) && ((attr1 & (1U << 13)) != 0U);
        const uint32_t tile_base = attr2 & 0x03FFU;
        const uint8_t prio = static_cast<uint8_t>((attr2 >> 10U) & 3U);
        const uint32_t pal = (attr2 >> 12U) & 0x0FU;

        const uint32_t ly =
            vflip ? (h - 1U - (y - static_cast<uint32_t>(sy))) : (y - static_cast<uint32_t>(sy));
        const uint32_t tile_row = ly >> 3U;
        const uint32_t py = ly & 7U;

        if (obj_mode == 3U) {
            // Direct-color 16-bit RGBA5551 Bitmap OBJ (GBATEK)
            if (pal == 0U)
                continue;  // Alpha == 0 -> transparent
            const bool bmp_1d = (dispcnt_ & (1U << 6)) != 0U;
            const uint32_t bmp_step = (dispcnt_ & (1U << 22)) ? 256U : 128U;
            const uint32_t bmp_stride = (dispcnt_ & (1U << 5)) ? 256U : 128U;
            const uint32_t tiles_per_row = bmp_stride >> 3U;
            const uint32_t bx = (tile_base & (tiles_per_row - 1U)) * 8U;
            const uint32_t by = (tile_base / tiles_per_row) * 8U;
            for (uint32_t lx = 0; lx < w; ++lx) {
                const int32_t px_scr = sx + static_cast<int32_t>(lx);
                if (px_scr < 0 || px_scr >= static_cast<int32_t>(kWidth))
                    continue;
                if (prio > line_prio[px_scr])
                    continue;
                const uint32_t fx = hflip ? (w - 1U - lx) : lx;
                const uint32_t off = bmp_1d ? (tile_base * bmp_step + (ly * w + fx) * 2U)
                                            : (((by + ly) * bmp_stride + bx + fx) * 2U);
                const uint16_t c = static_cast<uint16_t>(
                    read_obj(off) | (static_cast<uint16_t>(read_obj(off + 1U)) << 8U));
                if ((c & 0x8000U) == 0U)
                    continue;
                line_col[px_scr] = bgr555_to_rgba(c);
                line_prio[px_scr] = prio;
            }
            continue;
        }

        for (uint32_t lx = 0; lx < w; ++lx) {
            const int32_t px_scr = sx + static_cast<int32_t>(lx);
            if (px_scr < 0 || px_scr >= static_cast<int32_t>(kWidth))
                continue;
            if (prio > line_prio[px_scr])
                continue;

            const uint32_t fx = hflip ? (w - 1U - lx) : lx;
            const uint32_t tile_col = fx >> 3U;
            const uint32_t px = fx & 7U;

            uint32_t off = 0;
            if (is_8bpp) {
                if (mapping_1d) {
                    off = tile_base * tile_step + (tile_row * (w >> 3U) + tile_col) * 64U +
                          py * 8U + px;
                } else {
                    off = ((tile_base & ~1U) + tile_row * 32U + tile_col * 2U) * 32U + py * 8U + px;
                }
                const uint8_t ci = read_obj(off);
                if (ci == 0U)
                    continue;
                const bool obj_ext_pal = (dispcnt_ & (1U << 31)) != 0U;
                line_col[px_scr] =
                    bgr555_to_rgba(obj_ext_pal ? bus.read_obj_ext_pal16(is_engine_a_, pal, ci)
                                               : bus.read_pal16(is_engine_a_, 0x200U + ci * 2U));
            } else {
                if (mapping_1d) {
                    off = tile_base * tile_step + (tile_row * (w >> 3U) + tile_col) * 32U +
                          py * 4U + (px >> 1U);
                } else {
                    off = (tile_base + tile_row * 32U + tile_col) * 32U + py * 4U + (px >> 1U);
                }
                const uint8_t b = read_obj(off);
                const uint8_t ci = (px & 1U) ? (b >> 4U) : (b & 0x0FU);
                if (ci == 0U)
                    continue;
                line_col[px_scr] =
                    bgr555_to_rgba(bus.read_pal16(is_engine_a_, 0x200U + (pal * 16U + ci) * 2U));
            }
            line_prio[px_scr] = prio;
        }
    }
}

void Engine2D::render_scanline(uint32_t y, const Bus& bus, const uint32_t* gx3d_fb) noexcept {
    if (y >= kHeight)
        return;
    if (y == 0U) {
        bg_ref_x_[0] = bg_x_[0];
        bg_ref_y_[0] = bg_y_[0];
        bg_ref_x_[1] = bg_x_[1];
        bg_ref_y_[1] = bg_y_[1];
    }

    uint32_t* dst = &fb_[static_cast<size_t>(y) * kWidth];
    const uint32_t disp_mode = (dispcnt_ >> 16U) & 3U;

    if (disp_mode == 0U) {
        // Display OFF -> white screen (real NDS LCD behavior)
        std::fill_n(dst, kWidth, 0xFFFFFFFFU);
        return;
    }

    if (disp_mode == 2U && is_engine_a_) {
        // VRAM Display Mode: direct 16-bit bitmap from VRAM bank (A..D)
        const uint32_t bank = (dispcnt_ >> 18U) & 3U;
        const uint32_t base = bank * 0x20000U + y * kWidth * 2U;
        for (uint32_t x = 0; x < kWidth; ++x) {
            const uint16_t c = static_cast<uint16_t>(
                bus.read_vram_bg_a(base + x * 2U) |
                (static_cast<uint16_t>(bus.read_vram_bg_a(base + x * 2U + 1U)) << 8U));
            dst[x] = apply_master_bright(bgr555_to_rgba(c));
        }
        return;
    }

    // Normal Graphics Display (disp_mode == 1)
    const uint32_t backdrop = bgr555_to_rgba(bus.read_pal16(is_engine_a_, 0U));
    std::array<uint32_t, kWidth> line_col{};
    std::array<uint8_t, kWidth> line_prio{};
    line_col.fill(backdrop);
    line_prio.fill(4U);

    const uint32_t bg_mode = dispcnt_ & 7U;
    const bool bg0_is_3d = is_engine_a_ && ((dispcnt_ & (1U << 3)) != 0U);

    // Render BGs from 3 down to 0 so lower BG index wins on equal priority
    for (int bg = 3; bg >= 0; --bg) {
        if ((dispcnt_ & (1U << (8 + bg))) == 0U)
            continue;
        if (bg == 0 && bg0_is_3d) {
            if (gx3d_fb) {
                const uint8_t prio = static_cast<uint8_t>(bgcnt_[0] & 3U);
                const uint32_t* src3d = &gx3d_fb[static_cast<size_t>(y) * kWidth];
                for (uint32_t x = 0; x < kWidth; ++x) {
                    if (prio <= line_prio[x] && ((src3d[x] >> 24U) != 0U)) {
                        line_col[x] = src3d[x] | 0xFF000000U;
                        line_prio[x] = prio;
                    }
                }
            }
            continue;
        }
        const bool is_text =
            (bg_mode == 0U) || (bg < 2) || (bg_mode == 1U && bg == 2) || (bg_mode == 3U && bg == 2);
        if (is_text) {
            render_text_bg(y, bg, bus, line_col.data(), line_prio.data());
        } else {
            render_affine_or_ext_bg(y, bg, bus, line_col.data(), line_prio.data());
        }
    }

    if (dispcnt_ & (1U << 12)) {
        render_sprites(y, bus, line_col.data(), line_prio.data());
    }

    for (uint32_t x = 0; x < kWidth; ++x) {
        dst[x] = apply_master_bright(line_col[x]);
    }
}

}  // namespace dragon::nds
