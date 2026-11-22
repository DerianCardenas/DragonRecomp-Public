#include "ps1/gpu.h"

#include <algorithm>
#include <cstdlib>

namespace dragon::ps1 {

namespace {

inline int32_t sign_ext11(uint32_t v) noexcept {
    const int32_t s = static_cast<int32_t>(v & 0x7FFU);
    return (s & 0x400) ? (s - 0x800) : s;
}

inline uint16_t rgb888_to_bgr555(uint8_t r, uint8_t g, uint8_t b, bool stp) noexcept {
    const uint16_t r5 = static_cast<uint16_t>(r >> 3U);
    const uint16_t g5 = static_cast<uint16_t>(g >> 3U);
    const uint16_t b5 = static_cast<uint16_t>(b >> 3U);
    return static_cast<uint16_t>(r5 | (g5 << 5U) | (b5 << 10U) | (stp ? 0x8000U : 0U));
}

inline uint32_t bgr555_to_abgr8888(uint16_t c) noexcept {
    const uint32_t r = ((c & 0x1FU) * 255U) / 31U;
    const uint32_t g = (((c >> 5U) & 0x1FU) * 255U) / 31U;
    const uint32_t b = (((c >> 10U) & 0x1FU) * 255U) / 31U;
    return 0xFF000000U | (b << 16U) | (g << 8U) | r;
}

uint32_t gp0_command_length(uint8_t cmd) noexcept {
    if (cmd == 0x02U)
        return 3U;  // FillRect
    if (cmd >= 0x20U && cmd <= 0x3FU) {
        const bool quad = (cmd & 0x08U) != 0U;
        const bool shaded = (cmd & 0x10U) != 0U;
        const bool textured = (cmd & 0x04U) != 0U;
        const uint32_t verts = quad ? 4U : 3U;
        uint32_t words = 1U;  // command + color0
        words += verts;       // xy for each vertex
        if (textured)
            words += verts;
        if (shaded)
            words += (verts - 1U);
        return words;
    }
    if (cmd >= 0x40U && cmd <= 0x5FU) {
        const bool shaded = (cmd & 0x10U) != 0U;
        return shaded ? 4U : 3U;  // Polyline handled dynamically after first segment
    }
    if (cmd >= 0x60U && cmd <= 0x7FU) {
        const uint32_t sz_code = (cmd >> 3U) & 3U;
        const bool textured = (cmd & 0x04U) != 0U;
        uint32_t words = 2U;  // cmd+col, xy
        if (textured)
            words += 1U;
        if (sz_code == 0U)
            words += 1U;  // variable W/H
        return words;
    }
    if (cmd >= 0x80U && cmd <= 0x9FU)
        return 4U;  // VRAM->VRAM
    if (cmd >= 0xA0U && cmd <= 0xBFU)
        return 3U;  // CPU->VRAM
    if (cmd >= 0xC0U && cmd <= 0xDFU)
        return 3U;  // VRAM->CPU
    return 1U;      // Env commands & NOP
}

}  // namespace

Ps1Gpu::Ps1Gpu() : vram_(kVramPixels, 0U) {
    reset();
}

void Ps1Gpu::reset() {
    std::fill(vram_.begin(), vram_.end(), uint16_t{0});
    cmd_fifo_.fill(0U);
    cmd_words_received_ = 0;
    cmd_words_expected_ = 0;
    polyline_active_ = false;
    xfer_mode_ = TransferMode::None;
    gpuread_latch_ = 0;
    texpage_ = 0;
    tex_window_ = 0;
    draw_area_x1_ = 0;
    draw_area_y1_ = 0;
    draw_area_x2_ = 1023;
    draw_area_y2_ = 511;
    draw_offset_x_ = 0;
    draw_offset_y_ = 0;
    set_mask_bit_ = false;
    check_mask_bit_ = false;
    dither_ = false;
    draw_to_display_ = false;
    display_disabled_ = false;
    dma_dir_ = 0;
    disp_x_ = 0;
    disp_y_ = 0;
    disp_h_mode_ = 1;
    disp_h_368_ = false;
    disp_v_480_ = false;
    disp_24bit_ = false;
    disp_pal_ = false;
    odd_line_ = false;
    primitives_drawn_ = 0;
}

uint32_t Ps1Gpu::display_width() const noexcept {
    if (disp_h_368_)
        return 368U;
    static constexpr uint32_t kWidths[4] = {256U, 320U, 512U, 640U};
    return kWidths[disp_h_mode_ & 3U];
}

uint32_t Ps1Gpu::display_height() const noexcept {
    return disp_v_480_ ? 480U : 240U;
}

uint32_t Ps1Gpu::read_gpustat() const noexcept {
    uint32_t s = 0U;
    s |= (texpage_ & 0x7FFU);
    if (set_mask_bit_)
        s |= (1U << 11U);
    if (check_mask_bit_)
        s |= (1U << 12U);
    s |= (disp_h_368_ ? 1U : 0U) << 16U;
    s |= (disp_h_mode_ & 3U) << 17U;
    if (disp_v_480_)
        s |= (1U << 19U);
    if (disp_pal_)
        s |= (1U << 20U);
    if (disp_24bit_)
        s |= (1U << 21U);
    if (display_disabled_)
        s |= (1U << 23U);
    s |= (dma_dir_ & 3U) << 29U;
    // Always ready to receive commands, send VRAM, and accept DMA block
    s |= (1U << 26U) | (1U << 27U) | (1U << 28U);
    if (dma_dir_ != 0U)
        s |= (1U << 25U);
    if (odd_line_)
        s |= (1U << 31U);
    return s;
}

uint32_t Ps1Gpu::read_gpuread() noexcept {
    if (xfer_mode_ == TransferMode::VramToCpu) {
        auto next_pixel = [&]() -> uint16_t {
            const uint32_t px = (xfer_x_ + xfer_cur_x_) & 1023U;
            const uint32_t py = (xfer_y_ + xfer_cur_y_) & 511U;
            const uint16_t c = vram_[py * kVramWidth + px];
            if (++xfer_cur_x_ >= xfer_w_) {
                xfer_cur_x_ = 0;
                if (++xfer_cur_y_ >= xfer_h_) {
                    xfer_mode_ = TransferMode::None;
                }
            }
            return c;
        };
        const uint16_t lo = next_pixel();
        const uint16_t hi = (xfer_mode_ == TransferMode::VramToCpu) ? next_pixel() : 0U;
        gpuread_latch_ = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16U);
    }
    return gpuread_latch_;
}

void Ps1Gpu::write_gp1(uint32_t val) noexcept {
    const uint8_t cmd = static_cast<uint8_t>((val >> 24U) & 0x3FU);
    const uint32_t arg = val & 0x00FFFFFFU;
    switch (cmd) {
        case 0x00:  // Reset GPU
            cmd_words_received_ = 0;
            cmd_words_expected_ = 0;
            polyline_active_ = false;
            xfer_mode_ = TransferMode::None;
            texpage_ = 0;
            tex_window_ = 0;
            draw_area_x1_ = 0;
            draw_area_y1_ = 0;
            draw_area_x2_ = 1023;
            draw_area_y2_ = 511;
            draw_offset_x_ = 0;
            draw_offset_y_ = 0;
            display_disabled_ = true;
            dma_dir_ = 0;
            break;
        case 0x01:  // Reset command buffer
            cmd_words_received_ = 0;
            cmd_words_expected_ = 0;
            polyline_active_ = false;
            xfer_mode_ = TransferMode::None;
            break;
        case 0x02:
            break;  // Ack IRQ
        case 0x03:
            display_disabled_ = (arg & 1U) != 0U;
            break;
        case 0x04:
            dma_dir_ = arg & 3U;
            break;
        case 0x05:
            disp_x_ = arg & 0x3FEU;
            disp_y_ = (arg >> 10U) & 0x1FFU;
            break;
        case 0x06:
            break;  // Horizontal display range
        case 0x07:
            break;  // Vertical display range
        case 0x08:
            disp_h_mode_ = arg & 3U;
            disp_v_480_ = ((arg >> 2U) & 1U) != 0U;
            disp_pal_ = ((arg >> 3U) & 1U) != 0U;
            disp_24bit_ = ((arg >> 4U) & 1U) != 0U;
            disp_h_368_ = ((arg >> 6U) & 1U) != 0U;
            break;
        case 0x10:  // Get GPU Info
            switch (arg & 7U) {
                case 2:
                    gpuread_latch_ = tex_window_;
                    break;
                case 3:
                    gpuread_latch_ = static_cast<uint32_t>(draw_area_x1_ | (draw_area_y1_ << 10));
                    break;
                case 4:
                    gpuread_latch_ = static_cast<uint32_t>(draw_area_x2_ | (draw_area_y2_ << 10));
                    break;
                case 5:
                    gpuread_latch_ = static_cast<uint32_t>((draw_offset_x_ & 0x7FF) |
                                                           ((draw_offset_y_ & 0x7FF) << 11));
                    break;
                case 7:
                    gpuread_latch_ = 2U;
                    break;  // GPU v2
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

void Ps1Gpu::write_gp0(uint32_t val) noexcept {
    if (xfer_mode_ == TransferMode::CpuToVram) {
        auto put_pixel = [&](uint16_t px) {
            if (xfer_mode_ != TransferMode::CpuToVram)
                return;
            const uint32_t x = (xfer_x_ + xfer_cur_x_) & 1023U;
            const uint32_t y = (xfer_y_ + xfer_cur_y_) & 511U;
            vram_[y * kVramWidth + x] = px;
            if (++xfer_cur_x_ >= xfer_w_) {
                xfer_cur_x_ = 0;
                if (++xfer_cur_y_ >= xfer_h_) {
                    xfer_mode_ = TransferMode::None;
                }
            }
        };
        put_pixel(static_cast<uint16_t>(val & 0xFFFFU));
        put_pixel(static_cast<uint16_t>(val >> 16U));
        return;
    }

    if (polyline_active_) {
        if ((val & 0xF000F000U) == 0x50005000U) {
            polyline_active_ = false;
            return;
        }
        const bool shaded = (polyline_cmd_ & 0x10U) != 0U;
        if (shaded && polyline_expect_color_) {
            polyline_prev_.r = static_cast<uint8_t>(val & 0xFFU);
            polyline_prev_.g = static_cast<uint8_t>((val >> 8U) & 0xFFU);
            polyline_prev_.b = static_cast<uint8_t>((val >> 16U) & 0xFFU);
            polyline_expect_color_ = false;
            return;
        }
        Vertex next = polyline_prev_;
        next.x = sign_ext11(val & 0xFFFFU) + draw_offset_x_;
        next.y = sign_ext11(val >> 16U) + draw_offset_y_;
        // Bresenham line from polyline_prev_ to next
        int32_t x0 = polyline_prev_.x, y0 = polyline_prev_.y;
        const int32_t x1 = next.x, y1 = next.y;
        const int32_t dx = std::abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
        const int32_t dy = -std::abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
        int32_t err = dx + dy;
        const bool st = (polyline_cmd_ & 0x02U) != 0U;
        const uint8_t sm = static_cast<uint8_t>((texpage_ >> 5U) & 3U);
        for (int steps = 0; steps < 2048; ++steps) {
            plot_pixel(x0, y0, next.r, next.g, next.b, st, sm, set_mask_bit_);
            if (x0 == x1 && y0 == y1)
                break;
            const int32_t e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
        polyline_prev_ = next;
        polyline_expect_color_ = shaded;
        return;
    }

    if (cmd_words_received_ == 0U) {
        const uint8_t cmd = static_cast<uint8_t>(val >> 24U);
        cmd_words_expected_ = gp0_command_length(cmd);
    }
    if (cmd_words_received_ < cmd_fifo_.size()) {
        cmd_fifo_[cmd_words_received_++] = val;
    }
    if (cmd_words_received_ >= cmd_words_expected_) {
        dispatch_gp0_command();
        cmd_words_received_ = 0U;
        cmd_words_expected_ = 0U;
    }
}

void Ps1Gpu::dispatch_gp0_command() noexcept {
    const uint32_t w0 = cmd_fifo_[0];
    const uint8_t cmd = static_cast<uint8_t>(w0 >> 24U);

    if (cmd == 0x00U || cmd == 0x01U)
        return;  // NOP / Clear cache
    if (cmd == 0x02U) {
        cmd_fill_rect();
        return;
    }
    if (cmd >= 0x20U && cmd <= 0x3FU) {
        cmd_polygon(cmd);
        return;
    }
    if (cmd >= 0x40U && cmd <= 0x5FU) {
        cmd_line(cmd);
        return;
    }
    if (cmd >= 0x60U && cmd <= 0x7FU) {
        cmd_rect(cmd);
        return;
    }
    if (cmd >= 0x80U && cmd <= 0x9FU) {
        cmd_vram_to_vram();
        return;
    }
    if (cmd >= 0xA0U && cmd <= 0xBFU) {
        cmd_cpu_to_vram();
        return;
    }
    if (cmd >= 0xC0U && cmd <= 0xDFU) {
        cmd_vram_to_cpu();
        return;
    }

    switch (cmd) {
        case 0xE1:  // Draw Mode
            texpage_ = w0 & 0x00FFFFFFU;
            dither_ = ((w0 >> 9U) & 1U) != 0U;
            draw_to_display_ = ((w0 >> 10U) & 1U) != 0U;
            break;
        case 0xE2:
            tex_window_ = w0 & 0x000FFFFFU;
            break;
        case 0xE3:
            draw_area_x1_ = static_cast<int32_t>(w0 & 0x3FFU);
            draw_area_y1_ = static_cast<int32_t>((w0 >> 10U) & 0x1FFU);
            break;
        case 0xE4:
            draw_area_x2_ = static_cast<int32_t>(w0 & 0x3FFU);
            draw_area_y2_ = static_cast<int32_t>((w0 >> 10U) & 0x1FFU);
            break;
        case 0xE5:
            draw_offset_x_ = sign_ext11(w0 & 0x7FFU);
            draw_offset_y_ = sign_ext11((w0 >> 11U) & 0x7FFU);
            break;
        case 0xE6:
            set_mask_bit_ = (w0 & 1U) != 0U;
            check_mask_bit_ = (w0 & 2U) != 0U;
            break;
        default:
            break;
    }
}

void Ps1Gpu::cmd_fill_rect() noexcept {
    const uint8_t r = static_cast<uint8_t>(cmd_fifo_[0] & 0xFFU);
    const uint8_t g = static_cast<uint8_t>((cmd_fifo_[0] >> 8U) & 0xFFU);
    const uint8_t b = static_cast<uint8_t>((cmd_fifo_[0] >> 16U) & 0xFFU);
    const uint16_t color = rgb888_to_bgr555(r, g, b, false);

    const uint32_t x0 = cmd_fifo_[1] & 0x3F0U;
    const uint32_t y0 = (cmd_fifo_[1] >> 16U) & 0x1FFU;
    const uint32_t w = ((cmd_fifo_[2] & 0x3FFU) + 0x0FU) & ~0x0FU;
    const uint32_t h = (cmd_fifo_[2] >> 16U) & 0x1FFU;
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t py = (y0 + y) & 511U;
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t px = (x0 + x) & 1023U;
            vram_[py * kVramWidth + px] = color;
        }
    }
    ++primitives_drawn_;
}

void Ps1Gpu::plot_pixel(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b, bool semi_trans,
                        uint8_t semi_mode, bool mask_bit) noexcept {
    if (x < draw_area_x1_ || x > draw_area_x2_ || y < draw_area_y1_ || y > draw_area_y2_)
        return;
    if (x < 0 || x >= static_cast<int32_t>(kVramWidth) || y < 0 ||
        y >= static_cast<int32_t>(kVramHeight)) {
        return;
    }
    uint16_t& dst = vram_[static_cast<size_t>(y) * kVramWidth + static_cast<size_t>(x)];
    if (check_mask_bit_ && (dst & 0x8000U) != 0U)
        return;

    if (semi_trans) {
        const int32_t br = ((dst & 0x1FU) * 255) / 31;
        const int32_t bg = (((dst >> 5U) & 0x1FU) * 255) / 31;
        const int32_t bb = (((dst >> 10U) & 0x1FU) * 255) / 31;
        int32_t fr = r, fg = g, fb = b;
        switch (semi_mode & 3U) {
            case 0:  // 0.5*B + 0.5*F
                fr = (br + fr) >> 1;
                fg = (bg + fg) >> 1;
                fb = (bb + fb) >> 1;
                break;
            case 1:  // B + F
                fr = std::min(255, br + fr);
                fg = std::min(255, bg + fg);
                fb = std::min(255, bb + fb);
                break;
            case 2:  // B - F
                fr = std::max(0, br - fr);
                fg = std::max(0, bg - fg);
                fb = std::max(0, bb - fb);
                break;
            case 3:  // B + 0.25*F
                fr = std::min(255, br + (fr >> 2));
                fg = std::min(255, bg + (fg >> 2));
                fb = std::min(255, bb + (fb >> 2));
                break;
        }
        r = static_cast<uint8_t>(fr);
        g = static_cast<uint8_t>(fg);
        b = static_cast<uint8_t>(fb);
    }

    dst = rgb888_to_bgr555(r, g, b, mask_bit || set_mask_bit_);
}

bool Ps1Gpu::sample_texel(uint16_t tpage, uint16_t clut, uint32_t u, uint32_t v, uint8_t& out_r,
                          uint8_t& out_g, uint8_t& out_b, bool& out_stp) const noexcept {
    const uint32_t page_x = (tpage & 0x0FU) * 64U;
    const uint32_t page_y = ((tpage >> 4U) & 1U) * 256U;
    const uint32_t depth = (tpage >> 7U) & 3U;  // 0=4bpp, 1=8bpp, 2=15bpp
    const uint32_t clut_x = (clut & 0x3FU) * 16U;
    const uint32_t clut_y = (clut >> 6U) & 0x1FFU;

    u &= 0xFFU;
    v &= 0xFFU;
    uint16_t px = 0U;
    if (depth == 0U) {  // 4-bit CLUT
        const uint16_t word =
            vram_[((page_y + v) & 511U) * kVramWidth + ((page_x + (u >> 2U)) & 1023U)];
        const uint32_t idx = (word >> ((u & 3U) * 4U)) & 0x0FU;
        px = vram_[clut_y * kVramWidth + ((clut_x + idx) & 1023U)];
    } else if (depth == 1U) {  // 8-bit CLUT
        const uint16_t word =
            vram_[((page_y + v) & 511U) * kVramWidth + ((page_x + (u >> 1U)) & 1023U)];
        const uint32_t idx = (word >> ((u & 1U) * 8U)) & 0xFFU;
        px = vram_[clut_y * kVramWidth + ((clut_x + idx) & 1023U)];
    } else {  // 15-bit direct
        px = vram_[((page_y + v) & 511U) * kVramWidth + ((page_x + u) & 1023U)];
    }

    if (px == 0U)
        return false;  // Transparent texel (0x0000)
    out_r = static_cast<uint8_t>(((px & 0x1FU) * 255U) / 31U);
    out_g = static_cast<uint8_t>((((px >> 5U) & 0x1FU) * 255U) / 31U);
    out_b = static_cast<uint8_t>((((px >> 10U) & 0x1FU) * 255U) / 31U);
    out_stp = (px & 0x8000U) != 0U;
    return true;
}

void Ps1Gpu::rasterize_triangle(const Vertex& a, const Vertex& b, const Vertex& c, bool shaded,
                                bool textured, bool semi_trans, bool raw_tex, uint16_t clut,
                                uint16_t tpage) noexcept {
    auto edge = [](const Vertex& p0, const Vertex& p1, int32_t x, int32_t y) -> int64_t {
        return static_cast<int64_t>(p1.x - p0.x) * (y - p0.y) -
               static_cast<int64_t>(p1.y - p0.y) * (x - p0.x);
    };

    Vertex v0 = a, v1 = b, v2 = c;
    int64_t area = edge(v0, v1, v2.x, v2.y);
    if (area == 0)
        return;
    if (area < 0) {
        std::swap(v1, v2);
        area = -area;
    }

    const int32_t min_x = std::max(draw_area_x1_, std::min({v0.x, v1.x, v2.x}));
    const int32_t max_x = std::min(draw_area_x2_, std::max({v0.x, v1.x, v2.x}));
    const int32_t min_y = std::max(draw_area_y1_, std::min({v0.y, v1.y, v2.y}));
    const int32_t max_y = std::min(draw_area_y2_, std::max({v0.y, v1.y, v2.y}));

    if (min_x > max_x || min_y > max_y)
        return;
    // PS1 hardware skips polygons wider than 1023 or taller than 511
    if (max_x - min_x > 1023 || max_y - min_y > 511)
        return;

    const uint8_t semi_mode = static_cast<uint8_t>((tpage >> 5U) & 3U);

    for (int32_t y = min_y; y <= max_y; ++y) {
        for (int32_t x = min_x; x <= max_x; ++x) {
            const int64_t w0 = edge(v1, v2, x, y);
            const int64_t w1 = edge(v2, v0, x, y);
            const int64_t w2 = edge(v0, v1, x, y);
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;

            uint8_t cr = v0.r, cg = v0.g, cb = v0.b;
            if (shaded) {
                cr = static_cast<uint8_t>((w0 * v0.r + w1 * v1.r + w2 * v2.r) / area);
                cg = static_cast<uint8_t>((w0 * v0.g + w1 * v1.g + w2 * v2.g) / area);
                cb = static_cast<uint8_t>((w0 * v0.b + w1 * v1.b + w2 * v2.b) / area);
            }

            bool stp = false;
            bool apply_semi = semi_trans;
            if (textured) {
                const uint32_t u =
                    static_cast<uint32_t>((w0 * v0.u + w1 * v1.u + w2 * v2.u) / area);
                const uint32_t v =
                    static_cast<uint32_t>((w0 * v0.v + w1 * v1.v + w2 * v2.v) / area);
                uint8_t tr = 0, tg = 0, tb = 0;
                if (!sample_texel(tpage, clut, u, v, tr, tg, tb, stp))
                    continue;
                if (raw_tex) {
                    cr = tr;
                    cg = tg;
                    cb = tb;
                } else {
                    cr = static_cast<uint8_t>(
                        std::min<uint32_t>(255U, (static_cast<uint32_t>(tr) * cr) >> 7U));
                    cg = static_cast<uint8_t>(
                        std::min<uint32_t>(255U, (static_cast<uint32_t>(tg) * cg) >> 7U));
                    cb = static_cast<uint8_t>(
                        std::min<uint32_t>(255U, (static_cast<uint32_t>(tb) * cb) >> 7U));
                }
                apply_semi = semi_trans && stp;
            }
            plot_pixel(x, y, cr, cg, cb, apply_semi, semi_mode, stp);
        }
    }
    ++primitives_drawn_;
}

void Ps1Gpu::cmd_polygon(uint8_t cmd) noexcept {
    const bool quad = (cmd & 0x08U) != 0U;
    const bool shaded = (cmd & 0x10U) != 0U;
    const bool textured = (cmd & 0x04U) != 0U;
    const bool semi_trans = (cmd & 0x02U) != 0U;
    const bool raw_tex = (cmd & 0x01U) != 0U;
    const uint32_t count = quad ? 4U : 3U;

    Vertex v[4];
    uint16_t clut = 0;
    uint16_t tpage = static_cast<uint16_t>(texpage_ & 0xFFFFU);
    uint32_t idx = 0;

    for (uint32_t i = 0; i < count; ++i) {
        if (i == 0U || shaded) {
            const uint32_t col = cmd_fifo_[idx++];
            v[i].r = static_cast<uint8_t>(col & 0xFFU);
            v[i].g = static_cast<uint8_t>((col >> 8U) & 0xFFU);
            v[i].b = static_cast<uint8_t>((col >> 16U) & 0xFFU);
        } else {
            v[i].r = v[0].r;
            v[i].g = v[0].g;
            v[i].b = v[0].b;
        }
        const uint32_t xy = cmd_fifo_[idx++];
        v[i].x = sign_ext11(xy & 0xFFFFU) + draw_offset_x_;
        v[i].y = sign_ext11(xy >> 16U) + draw_offset_y_;
        if (textured) {
            const uint32_t uv = cmd_fifo_[idx++];
            v[i].u = static_cast<uint8_t>(uv & 0xFFU);
            v[i].v = static_cast<uint8_t>((uv >> 8U) & 0xFFU);
            if (i == 0U)
                clut = static_cast<uint16_t>(uv >> 16U);
            else if (i == 1U) {
                tpage = static_cast<uint16_t>(uv >> 16U);
                texpage_ = (texpage_ & ~0x1FFU) | (tpage & 0x1FFU);
            }
        }
    }

    rasterize_triangle(v[0], v[1], v[2], shaded, textured, semi_trans, raw_tex, clut, tpage);
    if (quad) {
        rasterize_triangle(v[1], v[2], v[3], shaded, textured, semi_trans, raw_tex, clut, tpage);
    }
}

void Ps1Gpu::cmd_line(uint8_t cmd) noexcept {
    const bool shaded = (cmd & 0x10U) != 0U;
    const bool poly = (cmd & 0x08U) != 0U;
    const bool semi_trans = (cmd & 0x02U) != 0U;
    const uint8_t sm = static_cast<uint8_t>((texpage_ >> 5U) & 3U);

    Vertex a{}, b{};
    uint32_t idx = 0;
    const uint32_t c0 = cmd_fifo_[idx++];
    a.r = static_cast<uint8_t>(c0 & 0xFFU);
    a.g = static_cast<uint8_t>((c0 >> 8U) & 0xFFU);
    a.b = static_cast<uint8_t>((c0 >> 16U) & 0xFFU);
    const uint32_t p0 = cmd_fifo_[idx++];
    a.x = sign_ext11(p0 & 0xFFFFU) + draw_offset_x_;
    a.y = sign_ext11(p0 >> 16U) + draw_offset_y_;
    if (shaded) {
        const uint32_t c1 = cmd_fifo_[idx++];
        b.r = static_cast<uint8_t>(c1 & 0xFFU);
        b.g = static_cast<uint8_t>((c1 >> 8U) & 0xFFU);
        b.b = static_cast<uint8_t>((c1 >> 16U) & 0xFFU);
    } else {
        b.r = a.r;
        b.g = a.g;
        b.b = a.b;
    }
    const uint32_t p1 = cmd_fifo_[idx++];
    b.x = sign_ext11(p1 & 0xFFFFU) + draw_offset_x_;
    b.y = sign_ext11(p1 >> 16U) + draw_offset_y_;

    int32_t x0 = a.x, y0 = a.y;
    const int32_t dx = std::abs(b.x - x0), sx = (x0 < b.x) ? 1 : -1;
    const int32_t dy = -std::abs(b.y - y0), sy = (y0 < b.y) ? 1 : -1;
    int32_t err = dx + dy;
    for (int steps = 0; steps < 2048; ++steps) {
        plot_pixel(x0, y0, a.r, a.g, a.b, semi_trans, sm, set_mask_bit_);
        if (x0 == b.x && y0 == b.y)
            break;
        const int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
    ++primitives_drawn_;

    if (poly) {
        polyline_active_ = true;
        polyline_cmd_ = cmd;
        polyline_prev_ = b;
        polyline_expect_color_ = shaded;
    }
}

void Ps1Gpu::cmd_rect(uint8_t cmd) noexcept {
    const uint32_t sz_code = (cmd >> 3U) & 3U;
    const bool textured = (cmd & 0x04U) != 0U;
    const bool semi_trans = (cmd & 0x02U) != 0U;
    const bool raw_tex = (cmd & 0x01U) != 0U;

    uint32_t idx = 0;
    const uint32_t col = cmd_fifo_[idx++];
    const uint8_t r = static_cast<uint8_t>(col & 0xFFU);
    const uint8_t g = static_cast<uint8_t>((col >> 8U) & 0xFFU);
    const uint8_t b = static_cast<uint8_t>((col >> 16U) & 0xFFU);
    const uint32_t xy = cmd_fifo_[idx++];
    const int32_t x0 = sign_ext11(xy & 0xFFFFU) + draw_offset_x_;
    const int32_t y0 = sign_ext11(xy >> 16U) + draw_offset_y_;

    uint32_t u0 = 0, v0 = 0;
    uint16_t clut = 0;
    if (textured) {
        const uint32_t uv = cmd_fifo_[idx++];
        u0 = uv & 0xFFU;
        v0 = (uv >> 8U) & 0xFFU;
        clut = static_cast<uint16_t>(uv >> 16U);
    }

    int32_t w = 1, h = 1;
    if (sz_code == 0U) {
        const uint32_t wh = cmd_fifo_[idx++];
        w = static_cast<int32_t>(wh & 0x3FFU);
        h = static_cast<int32_t>((wh >> 16U) & 0x1FFU);
    } else if (sz_code == 1U) {
        w = 1;
        h = 1;
    } else if (sz_code == 2U) {
        w = 8;
        h = 8;
    } else {
        w = 16;
        h = 16;
    }

    const uint16_t tpage = static_cast<uint16_t>(texpage_ & 0xFFFFU);
    const uint8_t semi_mode = static_cast<uint8_t>((tpage >> 5U) & 3U);

    for (int32_t dy = 0; dy < h; ++dy) {
        for (int32_t dx = 0; dx < w; ++dx) {
            uint8_t cr = r, cg = g, cb = b;
            bool stp = false;
            bool apply_semi = semi_trans;
            if (textured) {
                uint8_t tr = 0, tg = 0, tb = 0;
                if (!sample_texel(tpage, clut, u0 + static_cast<uint32_t>(dx),
                                  v0 + static_cast<uint32_t>(dy), tr, tg, tb, stp)) {
                    continue;
                }
                if (raw_tex) {
                    cr = tr;
                    cg = tg;
                    cb = tb;
                } else {
                    cr = static_cast<uint8_t>(
                        std::min<uint32_t>(255U, (static_cast<uint32_t>(tr) * r) >> 7U));
                    cg = static_cast<uint8_t>(
                        std::min<uint32_t>(255U, (static_cast<uint32_t>(tg) * g) >> 7U));
                    cb = static_cast<uint8_t>(
                        std::min<uint32_t>(255U, (static_cast<uint32_t>(tb) * b) >> 7U));
                }
                apply_semi = semi_trans && stp;
            }
            plot_pixel(x0 + dx, y0 + dy, cr, cg, cb, apply_semi, semi_mode, stp);
        }
    }
    ++primitives_drawn_;
}

void Ps1Gpu::cmd_vram_to_vram() noexcept {
    const uint32_t sx = cmd_fifo_[1] & 0x3FFU;
    const uint32_t sy = (cmd_fifo_[1] >> 16U) & 0x1FFU;
    const uint32_t dx = cmd_fifo_[2] & 0x3FFU;
    const uint32_t dy = (cmd_fifo_[2] >> 16U) & 0x1FFU;
    uint32_t w = cmd_fifo_[3] & 0x3FFU;
    uint32_t h = (cmd_fifo_[3] >> 16U) & 0x1FFU;
    if (w == 0U)
        w = 1024U;
    if (h == 0U)
        h = 512U;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint16_t px = vram_[((sy + y) & 511U) * kVramWidth + ((sx + x) & 1023U)];
            vram_[((dy + y) & 511U) * kVramWidth + ((dx + x) & 1023U)] = px;
        }
    }
}

void Ps1Gpu::cmd_cpu_to_vram() noexcept {
    xfer_x_ = cmd_fifo_[1] & 0x3FFU;
    xfer_y_ = (cmd_fifo_[1] >> 16U) & 0x1FFU;
    xfer_w_ = cmd_fifo_[2] & 0x3FFU;
    xfer_h_ = (cmd_fifo_[2] >> 16U) & 0x1FFU;
    if (xfer_w_ == 0U)
        xfer_w_ = 1024U;
    if (xfer_h_ == 0U)
        xfer_h_ = 512U;
    xfer_cur_x_ = 0U;
    xfer_cur_y_ = 0U;
    xfer_mode_ = TransferMode::CpuToVram;
}

void Ps1Gpu::cmd_vram_to_cpu() noexcept {
    xfer_x_ = cmd_fifo_[1] & 0x3FFU;
    xfer_y_ = (cmd_fifo_[1] >> 16U) & 0x1FFU;
    xfer_w_ = cmd_fifo_[2] & 0x3FFU;
    xfer_h_ = (cmd_fifo_[2] >> 16U) & 0x1FFU;
    if (xfer_w_ == 0U)
        xfer_w_ = 1024U;
    if (xfer_h_ == 0U)
        xfer_h_ = 512U;
    xfer_cur_x_ = 0U;
    xfer_cur_y_ = 0U;
    xfer_mode_ = TransferMode::VramToCpu;
}

void Ps1Gpu::present_frame(std::vector<uint32_t>& out_abgr, uint32_t& out_w,
                           uint32_t& out_h) const {
    out_w = display_width();
    out_h = display_height();
    out_abgr.resize(static_cast<size_t>(out_w) * out_h);

    if (display_disabled_) {
        std::fill(out_abgr.begin(), out_abgr.end(), 0xFF000000U);
        return;
    }

    if (!disp_24bit_) {
        for (uint32_t y = 0; y < out_h; ++y) {
            const uint32_t vy = (disp_y_ + y) & 511U;
            const uint16_t* row = vram_.data() + static_cast<size_t>(vy) * kVramWidth;
            uint32_t* dst = out_abgr.data() + static_cast<size_t>(y) * out_w;
            for (uint32_t x = 0; x < out_w; ++x) {
                dst[x] = bgr555_to_abgr8888(row[(disp_x_ + x) & 1023U]);
            }
        }
    } else {
        // 24-bit RGB display mode (3 bytes per pixel packed into 16-bit VRAM words)
        const uint8_t* vbytes = reinterpret_cast<const uint8_t*>(vram_.data());
        for (uint32_t y = 0; y < out_h; ++y) {
            const uint32_t vy = (disp_y_ + y) & 511U;
            const size_t row_off = (static_cast<size_t>(vy) * kVramWidth + disp_x_) * 2U;
            uint32_t* dst = out_abgr.data() + static_cast<size_t>(y) * out_w;
            for (uint32_t x = 0; x < out_w; ++x) {
                const size_t o = (row_off + static_cast<size_t>(x) * 3U) % (kVramPixels * 2U);
                const uint32_t r = vbytes[o];
                const uint32_t g = vbytes[(o + 1U) % (kVramPixels * 2U)];
                const uint32_t b = vbytes[(o + 2U) % (kVramPixels * 2U)];
                dst[x] = 0xFF000000U | (b << 16U) | (g << 8U) | r;
            }
        }
    }
}

}  // namespace dragon::ps1
