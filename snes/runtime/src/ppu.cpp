#include "snes/ppu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace dragon::snes {

namespace {

struct SpriteSize {
    uint8_t w;
    uint8_t h;
};

// Size configurations determined by $2101 OBSEL bits 5-7.
// Each entry provides {small_size, large_size}.
constexpr std::array<std::pair<SpriteSize, SpriteSize>, 8> kSpriteSizes = {{
    {{8U, 8U}, {16U, 16U}},    // 0: 8x8, 16x16
    {{8U, 8U}, {32U, 32U}},    // 1: 8x8, 32x32
    {{8U, 8U}, {64U, 64U}},    // 2: 8x8, 64x64
    {{16U, 16U}, {32U, 32U}},  // 3: 16x16, 32x32
    {{16U, 16U}, {64U, 64U}},  // 4: 16x16, 64x64
    {{32U, 32U}, {64U, 64U}},  // 5: 32x32, 64x64
    {{16U, 32U}, {32U, 64U}},  // 6: 16x32, 32x64
    {{16U, 32U}, {32U, 32U}},  // 7: 16x32, 32x32
}};

inline uint8_t clamp5(int32_t val) noexcept {
    if (val < 0) {
        return 0U;
    }
    if (val > 31) {
        return 31U;
    }
    return static_cast<uint8_t>(val);
}

}  // namespace

Ppu::Ppu() noexcept {
    reset();
}

void Ppu::reset() noexcept {
    vram_.fill(0U);
    cgram_.fill(0U);
    oam_.fill(0U);
    framebuffer_.fill(0xFF000000U);

    vmain_ = 0U;
    vram_addr_ = 0U;
    vram_prefetch_ = 0U;

    cgram_addr_ = 0U;
    cgram_latch_ = 0U;
    cgram_second_write_ = false;
    cgram_read_second_ = false;

    obsel_ = 0U;
    oam_reload_ = 0U;
    oam_addr_ = 0U;
    oam_latch_ = 0U;
    oam_priority_rotation_ = false;

    inidisp_ = 0x80U;
    bgmode_ = 0U;
    mosaic_ = 0U;
    bg_sc_.fill(0U);
    bg12nba_ = 0U;
    bg34nba_ = 0U;
    bg_hofs_.fill(0U);
    bg_vofs_.fill(0U);
    bg_ofs_latch_ = 0U;
    bg_hofs_latch_ = 0U;

    m7sel_ = 0U;
    m7_latch_ = 0U;
    m7a_ = 0;
    m7b_ = 0;
    m7c_ = 0;
    m7d_ = 0;
    m7x_ = 0;
    m7y_ = 0;
    m7_hofs_ = 0;
    m7_vofs_ = 0;
    mpy_result_ = 0;

    w12sel_ = 0U;
    w34sel_ = 0U;
    wobjsel_ = 0U;
    wh0_ = 0U;
    wh1_ = 0U;
    wh2_ = 0U;
    wh3_ = 0U;
    wbglog_ = 0U;
    wobjlog_ = 0U;

    tm_ = 0U;
    ts_ = 0U;
    tmw_ = 0U;
    tsw_ = 0U;
    cgwsel_ = 0U;
    cgadsub_ = 0U;
    setini_ = 0U;
    fixed_r_ = 0U;
    fixed_g_ = 0U;
    fixed_b_ = 0U;

    latched_h_ = 0U;
    latched_v_ = 0U;
    ophct_second_ = false;
    opvct_second_ = false;
    stat77_ = 0x01U;
    stat78_ = 0x01U;
}

uint16_t Ppu::translate_vram_addr(uint16_t word_addr) const noexcept {
    const uint8_t remap = (vmain_ >> 2U) & 0x03U;
    switch (remap) {
        case 1:  // 8-bit translation: aaaaaaaBBBccccc -> aaaaaaacccccBBB
            return static_cast<uint16_t>((word_addr & ~0x00FFU) | ((word_addr & 0x001FU) << 3U) |
                                         ((word_addr >> 5U) & 0x0007U));
        case 2:  // 16-bit translation: aaaaaaBBBcccccc -> aaaaaaccccccBBB
            return static_cast<uint16_t>((word_addr & ~0x01FFU) | ((word_addr & 0x003FU) << 3U) |
                                         ((word_addr >> 6U) & 0x0007U));
        case 3:  // 32-bit translation: aaaaaBBBccccccc -> aaaaacccccccBBB
            return static_cast<uint16_t>((word_addr & ~0x03FFU) | ((word_addr & 0x007FU) << 3U) |
                                         ((word_addr >> 7U) & 0x0007U));
        default:
            return static_cast<uint16_t>(word_addr & 0x7FFFU);
    }
}

uint16_t Ppu::vram_step() const noexcept {
    switch (vmain_ & 0x03U) {
        case 0:
            return 1U;
        case 1:
            return 32U;
        default:
            return 128U;
    }
}

void Ppu::on_vblank_start() noexcept {
    if ((inidisp_ & 0x80U) == 0U) {
        oam_addr_ = static_cast<uint16_t>((oam_reload_ << 1U) % 544U);
    }
}

void Ppu::latch_hv_counter(uint16_t h_dot, uint16_t v_line) noexcept {
    latched_h_ = h_dot;
    latched_v_ = v_line;
}

uint8_t Ppu::read_register(uint8_t reg) noexcept {
    switch (reg) {
        case 0x34U:  // MPYL
            return static_cast<uint8_t>(mpy_result_ & 0xFF);
        case 0x35U:  // MPYM
            return static_cast<uint8_t>((mpy_result_ >> 8) & 0xFF);
        case 0x36U:  // MPYH
            return static_cast<uint8_t>((mpy_result_ >> 16) & 0xFF);
        case 0x37U:  // SLHV: latch H/V counters
            latched_h_ = 0U;
            latched_v_ = 0U;
            return 0U;
        case 0x38U: {  // OAMDATAREAD
            const uint8_t val = oam_[oam_addr_ % 544U];
            oam_addr_ = static_cast<uint16_t>((oam_addr_ + 1U) % 544U);
            return val;
        }
        case 0x39U: {  // VMDATALREAD
            const uint8_t val = static_cast<uint8_t>(vram_prefetch_ & 0xFFU);
            if ((vmain_ & 0x80U) == 0U) {
                const uint16_t addr = translate_vram_addr(vram_addr_);
                vram_prefetch_ = vram_[addr & 0x7FFFU];
                vram_addr_ = static_cast<uint16_t>(vram_addr_ + vram_step());
            }
            return val;
        }
        case 0x3AU: {  // VMDATAHREAD
            const uint8_t val = static_cast<uint8_t>((vram_prefetch_ >> 8U) & 0xFFU);
            if ((vmain_ & 0x80U) != 0U) {
                const uint16_t addr = translate_vram_addr(vram_addr_);
                vram_prefetch_ = vram_[addr & 0x7FFFU];
                vram_addr_ = static_cast<uint16_t>(vram_addr_ + vram_step());
            }
            return val;
        }
        case 0x3BU: {  // CGDATAREAD
            if (!cgram_read_second_) {
                cgram_read_second_ = true;
                return static_cast<uint8_t>(cgram_[cgram_addr_] & 0xFFU);
            }
            cgram_read_second_ = false;
            const uint8_t val = static_cast<uint8_t>((cgram_[cgram_addr_] >> 8U) & 0x7FU);
            cgram_addr_++;
            return val;
        }
        case 0x3CU: {  // OPHCT
            const uint8_t val = ophct_second_ ? static_cast<uint8_t>((latched_h_ >> 8U) & 0x01U)
                                              : static_cast<uint8_t>(latched_h_ & 0xFFU);
            ophct_second_ = !ophct_second_;
            return val;
        }
        case 0x3DU: {  // OPVCT
            const uint8_t val = opvct_second_ ? static_cast<uint8_t>((latched_v_ >> 8U) & 0x01U)
                                              : static_cast<uint8_t>(latched_v_ & 0xFFU);
            opvct_second_ = !opvct_second_;
            return val;
        }
        case 0x3EU:  // STAT77
            return stat77_;
        case 0x3FU:  // STAT78
            ophct_second_ = false;
            opvct_second_ = false;
            return stat78_;
        default:
            return 0U;
    }
}

void Ppu::write_register(uint8_t reg, uint8_t value) noexcept {
    switch (reg) {
        case 0x00U:  // INIDISP
            inidisp_ = value;
            break;
        case 0x01U:  // OBSEL
            obsel_ = value;
            break;
        case 0x02U:  // OAMADDL
            oam_reload_ = static_cast<uint16_t>((oam_reload_ & 0x0100U) | value);
            oam_addr_ = static_cast<uint16_t>((oam_reload_ << 1U) % 544U);
            break;
        case 0x03U:  // OAMADDH
            oam_reload_ = static_cast<uint16_t>((oam_reload_ & 0x00FFU) | ((value & 0x01U) << 8U));
            oam_priority_rotation_ = (value & 0x80U) != 0U;
            oam_addr_ = static_cast<uint16_t>((oam_reload_ << 1U) % 544U);
            break;
        case 0x04U:  // OAMDATA
            if (oam_addr_ < 512U) {
                if ((oam_addr_ & 1U) == 0U) {
                    oam_latch_ = value;
                } else {
                    oam_[oam_addr_ - 1U] = oam_latch_;
                    oam_[oam_addr_] = value;
                }
            } else if (oam_addr_ < 544U) {
                oam_[oam_addr_] = value;
            }
            oam_addr_ = static_cast<uint16_t>((oam_addr_ + 1U) % 544U);
            break;
        case 0x05U:  // BGMODE
            bgmode_ = value;
            break;
        case 0x06U:  // MOSAIC
            mosaic_ = value;
            break;
        case 0x07U:  // BG1SC
        case 0x08U:  // BG2SC
        case 0x09U:  // BG3SC
        case 0x0AU:  // BG4SC
            bg_sc_[reg - 0x07U] = value;
            break;
        case 0x0BU:  // BG12NBA
            bg12nba_ = value;
            break;
        case 0x0CU:  // BG34NBA
            bg34nba_ = value;
            break;
        case 0x0DU:  // BG1HOFS / M7HOFS
            bg_hofs_[0] = static_cast<uint16_t>((static_cast<uint16_t>(value) << 8U) |
                                                (bg_hofs_latch_ & ~7U) | (bg_ofs_latch_ & 7U));
            bg_hofs_latch_ = value;
            bg_ofs_latch_ = value;
            m7_hofs_ = static_cast<int16_t>((static_cast<uint16_t>(value) << 8U) | m7_latch_);
            m7_latch_ = value;
            break;
        case 0x0EU:  // BG1VOFS / M7VOFS
            bg_vofs_[0] =
                static_cast<uint16_t>((static_cast<uint16_t>(value) << 8U) | bg_ofs_latch_);
            bg_ofs_latch_ = value;
            m7_vofs_ = static_cast<int16_t>((static_cast<uint16_t>(value) << 8U) | m7_latch_);
            m7_latch_ = value;
            break;
        case 0x0FU:  // BG2HOFS
            bg_hofs_[1] = static_cast<uint16_t>((static_cast<uint16_t>(value) << 8U) |
                                                (bg_hofs_latch_ & ~7U) | (bg_ofs_latch_ & 7U));
            bg_hofs_latch_ = value;
            bg_ofs_latch_ = value;
            break;
        case 0x10U:  // BG2VOFS
            bg_vofs_[1] =
                static_cast<uint16_t>((static_cast<uint16_t>(value) << 8U) | bg_ofs_latch_);
            bg_ofs_latch_ = value;
            break;
        case 0x11U:  // BG3HOFS
            bg_hofs_[2] = static_cast<uint16_t>((static_cast<uint16_t>(value) << 8U) |
                                                (bg_hofs_latch_ & ~7U) | (bg_ofs_latch_ & 7U));
            bg_hofs_latch_ = value;
            bg_ofs_latch_ = value;
            break;
        case 0x12U:  // BG3VOFS
            bg_vofs_[2] =
                static_cast<uint16_t>((static_cast<uint16_t>(value) << 8U) | bg_ofs_latch_);
            bg_ofs_latch_ = value;
            break;
        case 0x13U:  // BG4HOFS
            bg_hofs_[3] = static_cast<uint16_t>((static_cast<uint16_t>(value) << 8U) |
                                                (bg_hofs_latch_ & ~7U) | (bg_ofs_latch_ & 7U));
            bg_hofs_latch_ = value;
            bg_ofs_latch_ = value;
            break;
        case 0x14U:  // BG4VOFS
            bg_vofs_[3] =
                static_cast<uint16_t>((static_cast<uint16_t>(value) << 8U) | bg_ofs_latch_);
            bg_ofs_latch_ = value;
            break;
        case 0x15U:  // VMAIN
            vmain_ = value;
            break;
        case 0x16U:  // VMADDL
            vram_addr_ = static_cast<uint16_t>((vram_addr_ & 0xFF00U) | value);
            vram_prefetch_ = vram_[translate_vram_addr(vram_addr_) & 0x7FFFU];
            break;
        case 0x17U:  // VMADDH
            vram_addr_ = static_cast<uint16_t>((vram_addr_ & 0x00FFU) |
                                               (static_cast<uint16_t>(value) << 8U));
            vram_prefetch_ = vram_[translate_vram_addr(vram_addr_) & 0x7FFFU];
            break;
        case 0x18U: {  // VMDATAL
            const uint16_t addr = translate_vram_addr(vram_addr_) & 0x7FFFU;
            vram_[addr] = static_cast<uint16_t>((vram_[addr] & 0xFF00U) | value);
            if ((vmain_ & 0x80U) == 0U) {
                vram_addr_ = static_cast<uint16_t>(vram_addr_ + vram_step());
            }
            break;
        }
        case 0x19U: {  // VMDATAH
            const uint16_t addr = translate_vram_addr(vram_addr_) & 0x7FFFU;
            vram_[addr] = static_cast<uint16_t>((vram_[addr] & 0x00FFU) |
                                                (static_cast<uint16_t>(value) << 8U));
            if ((vmain_ & 0x80U) != 0U) {
                vram_addr_ = static_cast<uint16_t>(vram_addr_ + vram_step());
            }
            break;
        }
        case 0x1AU:  // M7SEL
            m7sel_ = value;
            break;
        case 0x1BU:  // M7A
            m7a_ = static_cast<int16_t>((static_cast<uint16_t>(value) << 8U) | m7_latch_);
            m7_latch_ = value;
            mpy_result_ =
                static_cast<int32_t>(m7a_) * static_cast<int32_t>(static_cast<int8_t>(m7b_ >> 8U));
            break;
        case 0x1CU:  // M7B
            m7b_ = static_cast<int16_t>((static_cast<uint16_t>(value) << 8U) | m7_latch_);
            m7_latch_ = value;
            mpy_result_ =
                static_cast<int32_t>(m7a_) * static_cast<int32_t>(static_cast<int8_t>(value));
            break;
        case 0x1DU:  // M7C
            m7c_ = static_cast<int16_t>((static_cast<uint16_t>(value) << 8U) | m7_latch_);
            m7_latch_ = value;
            break;
        case 0x1EU:  // M7D
            m7d_ = static_cast<int16_t>((static_cast<uint16_t>(value) << 8U) | m7_latch_);
            m7_latch_ = value;
            break;
        case 0x1FU:  // M7X
            m7x_ = static_cast<int16_t>((static_cast<uint16_t>(value) << 8U) | m7_latch_);
            m7_latch_ = value;
            break;
        case 0x20U:  // M7Y
            m7y_ = static_cast<int16_t>((static_cast<uint16_t>(value) << 8U) | m7_latch_);
            m7_latch_ = value;
            break;
        case 0x21U:  // CGADD
            cgram_addr_ = value;
            cgram_second_write_ = false;
            cgram_read_second_ = false;
            break;
        case 0x22U:  // CGDATA
            if (!cgram_second_write_) {
                cgram_latch_ = value;
                cgram_second_write_ = true;
            } else {
                cgram_[cgram_addr_] = static_cast<uint16_t>(
                    ((static_cast<uint16_t>(value) & 0x7FU) << 8U) | cgram_latch_);
                cgram_addr_++;
                cgram_second_write_ = false;
            }
            break;
        case 0x23U:  // W12SEL
            w12sel_ = value;
            break;
        case 0x24U:  // W34SEL
            w34sel_ = value;
            break;
        case 0x25U:  // WOBJSEL
            wobjsel_ = value;
            break;
        case 0x26U:  // WH0
            wh0_ = value;
            break;
        case 0x27U:  // WH1
            wh1_ = value;
            break;
        case 0x28U:  // WH2
            wh2_ = value;
            break;
        case 0x29U:  // WH3
            wh3_ = value;
            break;
        case 0x2AU:  // WBGLOG
            wbglog_ = value;
            break;
        case 0x2BU:  // WOBJLOG
            wobjlog_ = value;
            break;
        case 0x2CU:  // TM
            tm_ = value;
            break;
        case 0x2DU:  // TS
            ts_ = value;
            break;
        case 0x2EU:  // TMW
            tmw_ = value;
            break;
        case 0x2FU:  // TSW
            tsw_ = value;
            break;
        case 0x30U:  // CGWSEL
            cgwsel_ = value;
            break;
        case 0x31U:  // CGADSUB
            cgadsub_ = value;
            break;
        case 0x32U:  // COLDATA
            if ((value & 0x20U) != 0U) {
                fixed_r_ = value & 0x1FU;
            }
            if ((value & 0x40U) != 0U) {
                fixed_g_ = value & 0x1FU;
            }
            if ((value & 0x80U) != 0U) {
                fixed_b_ = value & 0x1FU;
            }
            break;
        case 0x33U:  // SETINI
            setini_ = value;
            break;
        default:
            break;
    }
}

uint32_t Ppu::bgr555_to_rgba8888(uint16_t bgr555, uint8_t brightness) noexcept {
    if (brightness > 15U) {
        brightness = 15U;
    }
    const uint8_t r5 = static_cast<uint8_t>(bgr555 & 0x1FU);
    const uint8_t g5 = static_cast<uint8_t>((bgr555 >> 5U) & 0x1FU);
    const uint8_t b5 = static_cast<uint8_t>((bgr555 >> 10U) & 0x1FU);

    const uint8_t r_scaled = static_cast<uint8_t>((r5 * brightness) / 15U);
    const uint8_t g_scaled = static_cast<uint8_t>((g5 * brightness) / 15U);
    const uint8_t b_scaled = static_cast<uint8_t>((b5 * brightness) / 15U);

    const uint8_t r8 = static_cast<uint8_t>((r_scaled << 3U) | (r_scaled >> 2U));
    const uint8_t g8 = static_cast<uint8_t>((g_scaled << 3U) | (g_scaled >> 2U));
    const uint8_t b8 = static_cast<uint8_t>((b_scaled << 3U) | (b_scaled >> 2U));

    return (static_cast<uint32_t>(r8) << 24U) | (static_cast<uint32_t>(g8) << 16U) |
           (static_cast<uint32_t>(b8) << 8U) | 0xFFU;
}

bool Ppu::eval_layer_window(uint8_t layer_idx, uint16_t x) const noexcept {
    uint8_t wsel = 0U;
    uint8_t wlog = 0U;
    if (layer_idx < 2U) {
        wsel = (layer_idx == 0U) ? (w12sel_ & 0x0FU) : ((w12sel_ >> 4U) & 0x0FU);
        wlog = (layer_idx == 0U) ? (wbglog_ & 0x03U) : ((wbglog_ >> 2U) & 0x03U);
    } else if (layer_idx < 4U) {
        wsel = (layer_idx == 2U) ? (w34sel_ & 0x0FU) : ((w34sel_ >> 4U) & 0x0FU);
        wlog = (layer_idx == 2U) ? ((wbglog_ >> 4U) & 0x03U) : ((wbglog_ >> 6U) & 0x03U);
    } else {
        wsel = wobjsel_ & 0x0FU;
        wlog = wobjlog_ & 0x03U;
    }

    const bool w1_en = (wsel & 0x01U) != 0U;
    const bool w1_inv = (wsel & 0x02U) != 0U;
    const bool w2_en = (wsel & 0x04U) != 0U;
    const bool w2_inv = (wsel & 0x08U) != 0U;

    if (!w1_en && !w2_en) {
        return false;
    }

    bool in_w1 = (x >= wh0_ && x <= wh1_);
    if (w1_inv) {
        in_w1 = !in_w1;
    }

    bool in_w2 = (x >= wh2_ && x <= wh3_);
    if (w2_inv) {
        in_w2 = !in_w2;
    }

    if (w1_en && !w2_en) {
        return in_w1;
    }
    if (!w1_en && w2_en) {
        return in_w2;
    }

    switch (wlog) {
        case 0:  // OR
            return in_w1 || in_w2;
        case 1:  // AND
            return in_w1 && in_w2;
        case 2:  // XOR
            return in_w1 ^ in_w2;
        case 3:  // XNOR
            return !(in_w1 ^ in_w2);
        default:
            return false;
    }
}

bool Ppu::eval_color_window(uint16_t x) const noexcept {
    const uint8_t wsel = (wobjsel_ >> 4U) & 0x0FU;
    const uint8_t wlog = (wobjlog_ >> 2U) & 0x03U;

    const bool w1_en = (wsel & 0x01U) != 0U;
    const bool w1_inv = (wsel & 0x02U) != 0U;
    const bool w2_en = (wsel & 0x04U) != 0U;
    const bool w2_inv = (wsel & 0x08U) != 0U;

    if (!w1_en && !w2_en) {
        return false;
    }

    bool in_w1 = (x >= wh0_ && x <= wh1_);
    if (w1_inv) {
        in_w1 = !in_w1;
    }

    bool in_w2 = (x >= wh2_ && x <= wh3_);
    if (w2_inv) {
        in_w2 = !in_w2;
    }

    if (w1_en && !w2_en) {
        return in_w1;
    }
    if (!w1_en && w2_en) {
        return in_w2;
    }

    switch (wlog) {
        case 0:
            return in_w1 || in_w2;
        case 1:
            return in_w1 && in_w2;
        case 2:
            return in_w1 ^ in_w2;
        case 3:
            return !(in_w1 ^ in_w2);
        default:
            return false;
    }
}

void Ppu::evaluate_sprites_on_scanline(uint16_t y,
                                       std::array<LayerPixel, kScreenWidth>& out_obj) noexcept {
    const uint8_t size_sel = (obsel_ >> 5U) & 0x07U;
    const auto& [small_sz, large_sz] = kSpriteSizes[size_sel];
    const uint16_t name_base = static_cast<uint16_t>((obsel_ & 0x07U) << 13U);
    const uint16_t second_table_offset =
        static_cast<uint16_t>((((obsel_ >> 3U) & 0x03U) + 1U) << 12U);

    const size_t start_idx = oam_priority_rotation_ ? ((oam_reload_ >> 1U) & 0x7FU) : 0U;

    for (size_t step = 0; step < 128U; ++step) {
        const size_t i = (start_idx + step) & 0x7FU;
        const uint8_t high_byte = oam_[512U + (i >> 2U)];
        const uint8_t bits = (high_byte >> ((i & 3U) << 1U)) & 0x03U;

        int16_t x_pos = oam_[i * 4U];
        if ((bits & 1U) != 0U) {
            x_pos |= 0x0100U;
        }
        if (x_pos >= 256) {
            x_pos -= 512;
        }

        const uint8_t y_pos = oam_[i * 4U + 1U];
        const uint8_t tile = oam_[i * 4U + 2U];
        const uint8_t attr = oam_[i * 4U + 3U];
        const bool is_large = (bits & 2U) != 0U;

        const uint8_t spr_w = is_large ? large_sz.w : small_sz.w;
        const uint8_t spr_h = is_large ? large_sz.h : small_sz.h;

        const uint16_t row_in_sprite = static_cast<uint16_t>((y - y_pos) & 0xFFU);
        if (row_in_sprite >= spr_h) {
            continue;
        }

        const bool h_flip = (attr & 0x40U) != 0U;
        const bool v_flip = (attr & 0x80U) != 0U;
        const uint8_t priority = (attr >> 4U) & 0x03U;
        const uint8_t palette = (attr >> 1U) & 0x07U;
        const bool second_table = (attr & 0x01U) != 0U;
        const uint16_t table_base =
            second_table ? static_cast<uint16_t>(name_base + second_table_offset) : name_base;

        const uint16_t sy =
            v_flip ? static_cast<uint16_t>(spr_h - 1U - row_in_sprite) : row_in_sprite;
        const uint16_t tile_y_offset = sy / 8U;
        const uint16_t line_in_tile = sy % 8U;

        const uint8_t base_tile_x = tile & 0x0FU;
        const uint8_t base_tile_y = (tile >> 4U) & 0x0FU;

        for (uint16_t px = 0; px < spr_w; ++px) {
            const int16_t screen_x = static_cast<int16_t>(x_pos + px);
            if (screen_x < 0 || screen_x >= static_cast<int16_t>(kScreenWidth)) {
                continue;
            }
            if (out_obj[static_cast<size_t>(screen_x)].valid) {
                continue;  // Already drawn by higher priority sprite
            }

            const uint16_t sx = h_flip ? static_cast<uint16_t>(spr_w - 1U - px) : px;
            const uint16_t tile_x_offset = sx / 8U;
            const uint16_t col_in_tile = sx % 8U;

            const uint16_t final_tile_x =
                static_cast<uint16_t>((base_tile_x + tile_x_offset) & 0x0FU);
            const uint16_t final_tile_y =
                static_cast<uint16_t>((base_tile_y + tile_y_offset) & 0x0FU);
            const uint16_t tile_index = static_cast<uint16_t>((final_tile_y * 16U) + final_tile_x);

            // 4bpp tile = 16 words per 8x8 tile
            const uint16_t tile_word_addr =
                static_cast<uint16_t>(table_base + tile_index * 16U + line_in_tile);
            const uint16_t w0 = vram_[tile_word_addr & 0x7FFFU];
            const uint16_t w1 = vram_[(tile_word_addr + 8U) & 0x7FFFU];

            const uint8_t shift = static_cast<uint8_t>(7U - col_in_tile);
            const uint8_t bit0 = static_cast<uint8_t>((w0 >> shift) & 1U);
            const uint8_t bit1 = static_cast<uint8_t>((w0 >> (shift + 8U)) & 1U);
            const uint8_t bit2 = static_cast<uint8_t>((w1 >> shift) & 1U);
            const uint8_t bit3 = static_cast<uint8_t>((w1 >> (shift + 8U)) & 1U);
            const uint8_t color_idx =
                static_cast<uint8_t>((bit3 << 3U) | (bit2 << 2U) | (bit1 << 1U) | bit0);

            if (color_idx != 0U) {
                LayerPixel& out = out_obj[static_cast<size_t>(screen_x)];
                out.color = cgram_[128U + palette * 16U + color_idx];
                out.priority = priority;
                out.palette = palette;
                out.valid = true;
            }
        }
    }
}

void Ppu::render_bg_scanline(uint8_t bg_idx, uint8_t bpp, uint16_t y,
                             std::array<LayerPixel, kScreenWidth>& out_bg) const noexcept {
    const uint16_t sc_reg = bg_sc_[bg_idx];
    const uint16_t sc_base = static_cast<uint16_t>((sc_reg & 0xFCU) << 8U);
    const uint8_t sc_layout = sc_reg & 0x03U;

    uint16_t char_base = 0U;
    if (bg_idx == 0U) {
        char_base = static_cast<uint16_t>((bg12nba_ & 0x0FU) << 12U);
    } else if (bg_idx == 1U) {
        char_base = static_cast<uint16_t>(((bg12nba_ >> 4U) & 0x0FU) << 12U);
    } else if (bg_idx == 2U) {
        char_base = static_cast<uint16_t>((bg34nba_ & 0x0FU) << 12U);
    } else {
        char_base = static_cast<uint16_t>(((bg34nba_ >> 4U) & 0x0FU) << 12U);
    }

    const bool tile_16x16 = (bgmode_ & (0x10U << bg_idx)) != 0U;
    const uint16_t tile_size = tile_16x16 ? 16U : 8U;
    const uint16_t words_per_tile = static_cast<uint16_t>(bpp * 4U);

    for (uint16_t x = 0; x < kScreenWidth; ++x) {
        const uint16_t fine_x = static_cast<uint16_t>((x + bg_hofs_[bg_idx]) & 0x03FFU);
        const uint16_t fine_y = static_cast<uint16_t>((y + bg_vofs_[bg_idx]) & 0x03FFU);

        const uint16_t tile_x = fine_x / tile_size;
        const uint16_t tile_y = fine_y / tile_size;

        uint16_t screen_offset = 0U;
        switch (sc_layout) {
            case 1:  // 64x32
                screen_offset = static_cast<uint16_t>(((tile_x >> 5U) & 1U) * 1024U);
                break;
            case 2:  // 32x64
                screen_offset = static_cast<uint16_t>(((tile_y >> 5U) & 1U) * 1024U);
                break;
            case 3:  // 64x64
                screen_offset = static_cast<uint16_t>(
                    ((((tile_y >> 5U) & 1U) << 1U) | ((tile_x >> 5U) & 1U)) * 1024U);
                break;
            default:
                break;
        }

        const uint16_t map_addr = static_cast<uint16_t>(sc_base + screen_offset +
                                                        ((tile_y & 31U) * 32U) + (tile_x & 31U));
        const uint16_t tile_data = vram_[map_addr & 0x7FFFU];

        uint16_t char_num = tile_data & 0x03FFU;
        const uint8_t palette = static_cast<uint8_t>((tile_data >> 10U) & 0x07U);
        const uint8_t priority = static_cast<uint8_t>((tile_data >> 13U) & 0x01U);
        const bool h_flip = (tile_data & 0x4000U) != 0U;
        const bool v_flip = (tile_data & 0x8000U) != 0U;

        uint16_t px = fine_x % tile_size;
        uint16_t py = fine_y % tile_size;

        if (h_flip) {
            px = tile_size - 1U - px;
        }
        if (v_flip) {
            py = tile_size - 1U - py;
        }

        if (tile_16x16) {
            const uint16_t sub_x = px / 8U;
            const uint16_t sub_y = py / 8U;
            char_num = static_cast<uint16_t>(char_num + sub_x + sub_y * 16U);
            px %= 8U;
            py %= 8U;
        }

        const uint16_t tile_addr =
            static_cast<uint16_t>(char_base + char_num * words_per_tile + py);

        uint8_t color_idx = 0U;
        const uint8_t shift = static_cast<uint8_t>(7U - px);

        if (bpp == 2U) {
            const uint16_t w0 = vram_[tile_addr & 0x7FFFU];
            const uint8_t bit0 = static_cast<uint8_t>((w0 >> shift) & 1U);
            const uint8_t bit1 = static_cast<uint8_t>((w0 >> (shift + 8U)) & 1U);
            color_idx = static_cast<uint8_t>((bit1 << 1U) | bit0);
        } else if (bpp == 4U) {
            const uint16_t w0 = vram_[tile_addr & 0x7FFFU];
            const uint16_t w1 = vram_[(tile_addr + 8U) & 0x7FFFU];
            const uint8_t bit0 = static_cast<uint8_t>((w0 >> shift) & 1U);
            const uint8_t bit1 = static_cast<uint8_t>((w0 >> (shift + 8U)) & 1U);
            const uint8_t bit2 = static_cast<uint8_t>((w1 >> shift) & 1U);
            const uint8_t bit3 = static_cast<uint8_t>((w1 >> (shift + 8U)) & 1U);
            color_idx = static_cast<uint8_t>((bit3 << 3U) | (bit2 << 2U) | (bit1 << 1U) | bit0);
        } else if (bpp == 8U) {
            const uint16_t w0 = vram_[tile_addr & 0x7FFFU];
            const uint16_t w1 = vram_[(tile_addr + 8U) & 0x7FFFU];
            const uint16_t w2 = vram_[(tile_addr + 16U) & 0x7FFFU];
            const uint16_t w3 = vram_[(tile_addr + 24U) & 0x7FFFU];
            const uint8_t bit0 = static_cast<uint8_t>((w0 >> shift) & 1U);
            const uint8_t bit1 = static_cast<uint8_t>((w0 >> (shift + 8U)) & 1U);
            const uint8_t bit2 = static_cast<uint8_t>((w1 >> shift) & 1U);
            const uint8_t bit3 = static_cast<uint8_t>((w1 >> (shift + 8U)) & 1U);
            const uint8_t bit4 = static_cast<uint8_t>((w2 >> shift) & 1U);
            const uint8_t bit5 = static_cast<uint8_t>((w2 >> (shift + 8U)) & 1U);
            const uint8_t bit6 = static_cast<uint8_t>((w3 >> shift) & 1U);
            const uint8_t bit7 = static_cast<uint8_t>((w3 >> (shift + 8U)) & 1U);
            color_idx =
                static_cast<uint8_t>((bit7 << 7U) | (bit6 << 6U) | (bit5 << 5U) | (bit4 << 4U) |
                                     (bit3 << 3U) | (bit2 << 2U) | (bit1 << 1U) | bit0);
        }

        if (color_idx != 0U) {
            uint16_t cgram_idx = 0U;
            if (bgmode_ == 0U) {
                cgram_idx = static_cast<uint16_t>(bg_idx * 32U + palette * 4U + color_idx);
            } else if (bpp == 2U) {
                cgram_idx = static_cast<uint16_t>(palette * 4U + color_idx);
            } else if (bpp == 4U) {
                cgram_idx = static_cast<uint16_t>(palette * 16U + color_idx);
            } else {
                cgram_idx = color_idx;
            }

            LayerPixel& out = out_bg[x];
            out.color = cgram_[cgram_idx & 0xFFU];
            out.priority = priority;
            out.palette = palette;
            out.valid = true;
        }
    }
}

void Ppu::render_mode7_scanline(uint16_t y,
                                std::array<LayerPixel, kScreenWidth>& out_bg1) const noexcept {
    const int32_t y_screen =
        (m7sel_ & 0x02U) ? (255 - static_cast<int32_t>(y)) : static_cast<int32_t>(y);
    const int32_t y_diff = y_screen + m7_vofs_ - m7y_;
    const uint8_t repeat_mode = (m7sel_ >> 6U) & 0x03U;

    for (uint16_t x = 0; x < kScreenWidth; ++x) {
        const int32_t x_screen =
            (m7sel_ & 0x01U) ? (255 - static_cast<int32_t>(x)) : static_cast<int32_t>(x);
        const int32_t x_diff = x_screen + m7_hofs_ - m7x_;

        const int32_t x_map = ((static_cast<int32_t>(m7a_) * x_diff) >> 8) +
                              ((static_cast<int32_t>(m7b_) * y_diff) >> 8) + m7x_;
        const int32_t y_map = ((static_cast<int32_t>(m7c_) * x_diff) >> 8) +
                              ((static_cast<int32_t>(m7d_) * y_diff) >> 8) + m7y_;

        const bool out_of_bounds = (x_map < 0 || x_map >= 1024 || y_map < 0 || y_map >= 1024);

        if (out_of_bounds && repeat_mode == 2U) {
            continue;  // Transparent / empty space
        }

        uint16_t tile_x = static_cast<uint16_t>((x_map >> 3) & 0x7FU);
        uint16_t tile_y = static_cast<uint16_t>((y_map >> 3) & 0x7FU);
        if (out_of_bounds && repeat_mode == 3U) {
            tile_x = 0U;
            tile_y = 0U;
        }

        const uint16_t map_addr = static_cast<uint16_t>(tile_y * 128U + tile_x);
        const uint8_t tile_idx = static_cast<uint8_t>(vram_[map_addr & 0x3FFFU] & 0xFFU);

        const uint16_t px = static_cast<uint16_t>(x_map & 7);
        const uint16_t py = static_cast<uint16_t>(y_map & 7);
        const uint16_t char_word_addr =
            static_cast<uint16_t>(static_cast<uint16_t>(tile_idx) * 64U + py * 8U + px);
        const uint8_t color_idx =
            static_cast<uint8_t>((vram_[char_word_addr & 0x3FFFU] >> 8U) & 0xFFU);

        if (color_idx != 0U) {
            LayerPixel& out = out_bg1[x];
            out.color = cgram_[color_idx];
            out.priority = 0U;
            out.palette = 0U;
            out.valid = true;
        }
    }
}

void Ppu::render_scanline(uint16_t y) noexcept {
    if (y >= kScreenHeight) {
        return;
    }

    if ((inidisp_ & 0x80U) != 0U) {
        // Forced blank: full line is black
        std::fill_n(&framebuffer_[static_cast<size_t>(y) * kScreenWidth], kScreenWidth,
                    0xFF000000U);
        return;
    }

    std::array<std::array<LayerPixel, kScreenWidth>, 4> bgs{};
    std::array<LayerPixel, kScreenWidth> objs{};

    const uint8_t mode = bgmode_ & 0x07U;

    // Render BGs according to mode
    if (mode == 0U) {
        render_bg_scanline(0U, 2U, y, bgs[0]);
        render_bg_scanline(1U, 2U, y, bgs[1]);
        render_bg_scanline(2U, 2U, y, bgs[2]);
        render_bg_scanline(3U, 2U, y, bgs[3]);
    } else if (mode == 1U) {
        render_bg_scanline(0U, 4U, y, bgs[0]);
        render_bg_scanline(1U, 4U, y, bgs[1]);
        render_bg_scanline(2U, 2U, y, bgs[2]);
    } else if (mode == 2U) {
        render_bg_scanline(0U, 4U, y, bgs[0]);
        render_bg_scanline(1U, 4U, y, bgs[1]);
    } else if (mode == 3U) {
        render_bg_scanline(0U, 8U, y, bgs[0]);
        render_bg_scanline(1U, 4U, y, bgs[1]);
    } else if (mode == 4U) {
        render_bg_scanline(0U, 8U, y, bgs[0]);
        render_bg_scanline(1U, 2U, y, bgs[1]);
    } else if (mode == 5U) {
        render_bg_scanline(0U, 4U, y, bgs[0]);
        render_bg_scanline(1U, 2U, y, bgs[1]);
    } else if (mode == 7U) {
        render_mode7_scanline(y, bgs[0]);
    }

    evaluate_sprites_on_scanline(y, objs);

    const uint8_t brightness = inidisp_ & 0x0FU;
    const uint16_t backdrop_bgr = cgram_[0];
    const uint8_t bg3_prio = (mode == 1U && (bgmode_ & 0x08U) != 0U) ? 1U : 0U;

    for (uint16_t x = 0; x < kScreenWidth; ++x) {
        // Evaluate window clipping on main and sub screens
        const bool w_bg1 = ((tmw_ & 0x01U) != 0U) && eval_layer_window(0U, x);
        const bool w_bg2 = ((tmw_ & 0x02U) != 0U) && eval_layer_window(1U, x);
        const bool w_bg3 = ((tmw_ & 0x04U) != 0U) && eval_layer_window(2U, x);
        const bool w_bg4 = ((tmw_ & 0x08U) != 0U) && eval_layer_window(3U, x);
        const bool w_obj = ((tmw_ & 0x10U) != 0U) && eval_layer_window(4U, x);

        const bool w_sub_bg1 = ((tsw_ & 0x01U) != 0U) && eval_layer_window(0U, x);
        const bool w_sub_bg2 = ((tsw_ & 0x02U) != 0U) && eval_layer_window(1U, x);
        const bool w_sub_bg3 = ((tsw_ & 0x04U) != 0U) && eval_layer_window(2U, x);
        const bool w_sub_bg4 = ((tsw_ & 0x08U) != 0U) && eval_layer_window(3U, x);
        const bool w_sub_obj = ((tsw_ & 0x10U) != 0U) && eval_layer_window(4U, x);

        // Helper lambda to pick highest priority pixel on a screen
        auto pick_screen_pixel = [&](uint8_t screen_mask, bool w1, bool w2, bool w3, bool w4,
                                     bool wo) -> ScreenPixel {
            const bool bg1_on = ((screen_mask & 0x01U) != 0U) && !w1 && bgs[0][x].valid;
            const bool bg2_on = ((screen_mask & 0x02U) != 0U) && !w2 && bgs[1][x].valid;
            const bool bg3_on = ((screen_mask & 0x04U) != 0U) && !w3 && bgs[2][x].valid;
            const bool bg4_on = ((screen_mask & 0x08U) != 0U) && !w4 && bgs[3][x].valid;
            const bool obj_on = ((screen_mask & 0x10U) != 0U) && !wo && objs[x].valid;

            auto make_px = [&](uint16_t col, uint8_t l, uint8_t pal = 0U) {
                ScreenPixel p;
                p.r = static_cast<uint8_t>(col & 0x1FU);
                p.g = static_cast<uint8_t>((col >> 5U) & 0x1FU);
                p.b = static_cast<uint8_t>((col >> 10U) & 0x1FU);
                p.layer = l;
                p.obj_palette = pal;
                return p;
            };

            if (mode == 1U) {
                if (bg3_prio != 0U && bg3_on && bgs[2][x].priority == 1U) {
                    return make_px(bgs[2][x].color, 2U);
                }
                if (obj_on && objs[x].priority == 3U) {
                    return make_px(objs[x].color, 4U, objs[x].palette);
                }
                if (bg1_on && bgs[0][x].priority == 1U) {
                    return make_px(bgs[0][x].color, 0U);
                }
                if (bg2_on && bgs[1][x].priority == 1U) {
                    return make_px(bgs[1][x].color, 1U);
                }
                if (obj_on && objs[x].priority == 2U) {
                    return make_px(objs[x].color, 4U, objs[x].palette);
                }
                if (bg1_on && bgs[0][x].priority == 0U) {
                    return make_px(bgs[0][x].color, 0U);
                }
                if (bg2_on && bgs[1][x].priority == 0U) {
                    return make_px(bgs[1][x].color, 1U);
                }
                if (obj_on && objs[x].priority == 1U) {
                    return make_px(objs[x].color, 4U, objs[x].palette);
                }
                if (bg3_prio == 0U && bg3_on && bgs[2][x].priority == 1U) {
                    return make_px(bgs[2][x].color, 2U);
                }
                if (obj_on && objs[x].priority == 0U) {
                    return make_px(objs[x].color, 4U, objs[x].palette);
                }
                if (bg3_on && bgs[2][x].priority == 0U) {
                    return make_px(bgs[2][x].color, 2U);
                }
            } else if (mode == 7U) {
                if (obj_on && objs[x].priority == 3U) {
                    return make_px(objs[x].color, 4U, objs[x].palette);
                }
                if (obj_on && objs[x].priority == 2U) {
                    return make_px(objs[x].color, 4U, objs[x].palette);
                }
                if (obj_on && objs[x].priority == 1U) {
                    return make_px(objs[x].color, 4U, objs[x].palette);
                }
                if (bg1_on) {
                    return make_px(bgs[0][x].color, 0U);
                }
                if (obj_on && objs[x].priority == 0U) {
                    return make_px(objs[x].color, 4U, objs[x].palette);
                }
            } else {
                // Generic order for modes 0, 2..6
                if (obj_on && objs[x].priority == 3U) {
                    return make_px(objs[x].color, 4U, objs[x].palette);
                }
                if (bg1_on && bgs[0][x].priority == 1U) {
                    return make_px(bgs[0][x].color, 0U);
                }
                if (bg2_on && bgs[1][x].priority == 1U) {
                    return make_px(bgs[1][x].color, 1U);
                }
                if (obj_on && objs[x].priority == 2U) {
                    return make_px(objs[x].color, 4U, objs[x].palette);
                }
                if (bg1_on && bgs[0][x].priority == 0U) {
                    return make_px(bgs[0][x].color, 0U);
                }
                if (bg2_on && bgs[1][x].priority == 0U) {
                    return make_px(bgs[1][x].color, 1U);
                }
                if (obj_on && objs[x].priority == 1U) {
                    return make_px(objs[x].color, 4U, objs[x].palette);
                }
                if (bg3_on && bgs[2][x].priority == 1U) {
                    return make_px(bgs[2][x].color, 2U);
                }
                if (bg4_on && bgs[3][x].priority == 1U) {
                    return make_px(bgs[3][x].color, 3U);
                }
                if (obj_on && objs[x].priority == 0U) {
                    return make_px(objs[x].color, 4U, objs[x].palette);
                }
                if (bg3_on && bgs[2][x].priority == 0U) {
                    return make_px(bgs[2][x].color, 2U);
                }
                if (bg4_on && bgs[3][x].priority == 0U) {
                    return make_px(bgs[3][x].color, 3U);
                }
            }

            return make_px(backdrop_bgr, 5U);
        };

        const ScreenPixel main_px = pick_screen_pixel(tm_, w_bg1, w_bg2, w_bg3, w_bg4, w_obj);
        const ScreenPixel sub_px =
            pick_screen_pixel(ts_, w_sub_bg1, w_sub_bg2, w_sub_bg3, w_sub_bg4, w_sub_obj);

        // Check if color math applies to main screen pixel
        bool color_math_enable = false;
        if (main_px.layer < 4U) {
            color_math_enable = (cgadsub_ & (1U << main_px.layer)) != 0U;
        } else if (main_px.layer == 4U) {
            // Sprites only participate in color math when using palettes 4..7
            color_math_enable = ((cgadsub_ & 0x10U) != 0U) && (main_px.obj_palette >= 4U);
        } else {
            color_math_enable = (cgadsub_ & 0x20U) != 0U;  // Backdrop
        }

        // Color window clipping check ($2130 CGWSEL bits 6-7)
        const uint8_t math_clip = (cgwsel_ >> 6U) & 0x03U;
        const bool in_color_window = eval_color_window(x);
        if (math_clip == 1U && !in_color_window) {
            color_math_enable = false;
        } else if (math_clip == 2U && in_color_window) {
            color_math_enable = false;
        } else if (math_clip == 3U) {
            color_math_enable = false;
        }

        uint8_t out_r = main_px.r;
        uint8_t out_g = main_px.g;
        uint8_t out_b = main_px.b;

        if (color_math_enable) {
            uint8_t sec_r = fixed_r_;
            uint8_t sec_g = fixed_g_;
            uint8_t sec_b = fixed_b_;

            // If sub screen is enabled for color math and has a non-backdrop pixel
            if (ts_ != 0U && sub_px.layer != 5U) {
                sec_r = sub_px.r;
                sec_g = sub_px.g;
                sec_b = sub_px.b;
            }

            const bool is_subtract = (cgadsub_ & 0x80U) != 0U;
            const bool is_half = (cgadsub_ & 0x40U) != 0U;

            int32_t mr = static_cast<int32_t>(main_px.r);
            int32_t mg = static_cast<int32_t>(main_px.g);
            int32_t mb = static_cast<int32_t>(main_px.b);

            const int32_t sr = static_cast<int32_t>(sec_r);
            const int32_t sg = static_cast<int32_t>(sec_g);
            const int32_t sb = static_cast<int32_t>(sec_b);

            if (!is_subtract) {
                mr += sr;
                mg += sg;
                mb += sb;
            } else {
                mr -= sr;
                mg -= sg;
                mb -= sb;
            }

            if (is_half) {
                mr >>= 1;
                mg >>= 1;
                mb >>= 1;
            }

            out_r = clamp5(mr);
            out_g = clamp5(mg);
            out_b = clamp5(mb);
        }

        // Apply Master Brightness (0..15)
        const uint8_t scaled_r = static_cast<uint8_t>((out_r * brightness) / 15U);
        const uint8_t scaled_g = static_cast<uint8_t>((out_g * brightness) / 15U);
        const uint8_t scaled_b = static_cast<uint8_t>((out_b * brightness) / 15U);

        const uint8_t r8 = static_cast<uint8_t>((scaled_r << 3U) | (scaled_r >> 2U));
        const uint8_t g8 = static_cast<uint8_t>((scaled_g << 3U) | (scaled_g >> 2U));
        const uint8_t b8 = static_cast<uint8_t>((scaled_b << 3U) | (scaled_b >> 2U));

        framebuffer_[static_cast<size_t>(y) * kScreenWidth + x] =
            (static_cast<uint32_t>(r8) << 24U) | (static_cast<uint32_t>(g8) << 16U) |
            (static_cast<uint32_t>(b8) << 8U) | 0xFFU;
    }
}

void Ppu::save_snapshot(Snapshot& snap) const noexcept {
    snap.vram = vram_;
    snap.cgram = cgram_;
    snap.oam = oam_;
    snap.vmain = vmain_;
    snap.vram_addr = vram_addr_;
    snap.vram_prefetch = vram_prefetch_;
    snap.oam_addr = oam_addr_;
    snap.oam_priority = oam_priority_rotation_;
    snap.cgram_addr = cgram_addr_;
    snap.cgram_second_write = cgram_second_write_;
    snap.cgram_read_second = cgram_read_second_;
    snap.cgram_latch = cgram_latch_;
    snap.inidisp = inidisp_;
    snap.obsel = obsel_;
    snap.bgmode = bgmode_;
    snap.mosaic = mosaic_;
    snap.bg_sc = bg_sc_;
    snap.bg12nba = bg12nba_;
    snap.bg34nba = bg34nba_;
    snap.bg_hofs = bg_hofs_;
    snap.bg_vofs = bg_vofs_;
    snap.bg_ofs_latch = bg_ofs_latch_;
    snap.bg_hofs_latch = bg_hofs_latch_;
    snap.m7sel = m7sel_;
    snap.m7_latch = m7_latch_;
    snap.m7a = m7a_;
    snap.m7b = m7b_;
    snap.m7c = m7c_;
    snap.m7d = m7d_;
    snap.m7x = m7x_;
    snap.m7y = m7y_;
    snap.m7_hofs = m7_hofs_;
    snap.m7_vofs = m7_vofs_;
    snap.mpy_result = mpy_result_;
    snap.w12sel = w12sel_;
    snap.w34sel = w34sel_;
    snap.wobjsel = wobjsel_;
    snap.wh0 = wh0_;
    snap.wh1 = wh1_;
    snap.wh2 = wh2_;
    snap.wh3 = wh3_;
    snap.wbglog = wbglog_;
    snap.wobjlog = wobjlog_;
    snap.tm = tm_;
    snap.ts = ts_;
    snap.tmw = tmw_;
    snap.tsw = tsw_;
    snap.cgwsel = cgwsel_;
    snap.cgadsub = cgadsub_;
    snap.setini = setini_;
    snap.fixed_r = fixed_r_;
    snap.fixed_g = fixed_g_;
    snap.fixed_b = fixed_b_;
    snap.latched_h = latched_h_;
    snap.latched_v = latched_v_;
    snap.ophct_second = ophct_second_;
    snap.opvct_second = opvct_second_;
    snap.stat77 = stat77_;
    snap.stat78 = stat78_;
}

void Ppu::load_snapshot(const Snapshot& snap) noexcept {
    vram_ = snap.vram;
    cgram_ = snap.cgram;
    oam_ = snap.oam;
    vmain_ = snap.vmain;
    vram_addr_ = snap.vram_addr;
    vram_prefetch_ = snap.vram_prefetch;
    oam_addr_ = snap.oam_addr;
    oam_priority_rotation_ = snap.oam_priority;
    cgram_addr_ = snap.cgram_addr;
    cgram_second_write_ = snap.cgram_second_write;
    cgram_read_second_ = snap.cgram_read_second;
    cgram_latch_ = snap.cgram_latch;
    inidisp_ = snap.inidisp;
    obsel_ = snap.obsel;
    bgmode_ = snap.bgmode;
    mosaic_ = snap.mosaic;
    bg_sc_ = snap.bg_sc;
    bg12nba_ = snap.bg12nba;
    bg34nba_ = snap.bg34nba;
    bg_hofs_ = snap.bg_hofs;
    bg_vofs_ = snap.bg_vofs;
    bg_ofs_latch_ = snap.bg_ofs_latch;
    bg_hofs_latch_ = snap.bg_hofs_latch;
    m7sel_ = snap.m7sel;
    m7_latch_ = snap.m7_latch;
    m7a_ = snap.m7a;
    m7b_ = snap.m7b;
    m7c_ = snap.m7c;
    m7d_ = snap.m7d;
    m7x_ = snap.m7x;
    m7y_ = snap.m7y;
    m7_hofs_ = snap.m7_hofs;
    m7_vofs_ = snap.m7_vofs;
    mpy_result_ = snap.mpy_result;
    w12sel_ = snap.w12sel;
    w34sel_ = snap.w34sel;
    wobjsel_ = snap.wobjsel;
    wh0_ = snap.wh0;
    wh1_ = snap.wh1;
    wh2_ = snap.wh2;
    wh3_ = snap.wh3;
    wbglog_ = snap.wbglog;
    wobjlog_ = snap.wobjlog;
    tm_ = snap.tm;
    ts_ = snap.ts;
    tmw_ = snap.tmw;
    tsw_ = snap.tsw;
    cgwsel_ = snap.cgwsel;
    cgadsub_ = snap.cgadsub;
    setini_ = snap.setini;
    fixed_r_ = snap.fixed_r;
    fixed_g_ = snap.fixed_g;
    fixed_b_ = snap.fixed_b;
    latched_h_ = snap.latched_h;
    latched_v_ = snap.latched_v;
    ophct_second_ = snap.ophct_second;
    opvct_second_ = snap.opvct_second;
    stat77_ = snap.stat77;
    stat78_ = snap.stat78;
}

}  // namespace dragon::snes
