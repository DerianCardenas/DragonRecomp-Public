#include <cassert>
#include <iostream>

#include "pce/vdc.h"

using namespace dragon::pce;

namespace {

void write_vdc_reg(Vdc& vdc, uint8_t reg, uint16_t val) {
    vdc.write_vdc(0U, reg);
    vdc.write_vdc(2U, static_cast<uint8_t>(val & 0xFFU));
    vdc.write_vdc(3U, static_cast<uint8_t>((val >> 8U) & 0xFFU));
}

void write_vce_color(Vdc& vdc, uint16_t idx, uint16_t grb9) {
    vdc.write_vce(2U, static_cast<uint8_t>(idx & 0xFFU));
    vdc.write_vce(3U, static_cast<uint8_t>((idx >> 8U) & 0x01U));
    vdc.write_vce(4U, static_cast<uint8_t>(grb9 & 0xFFU));
    vdc.write_vce(5U, static_cast<uint8_t>((grb9 >> 8U) & 0x01U));
}

void test_vce_palette_grb9() {
    std::cout << "[Test] HuC6260 VCE 9-bit GGGRRRBBB palette conversion...\n";
    // Pure red: G=0, R=7, B=0 -> 0b000_111_000 = 0x038
    assert(Vdc::vce_to_rgba32(0x038U) == 0xFF0000FFU);
    // Pure green: G=7, R=0, B=0 -> 0b111_000_000 = 0x1C0
    assert(Vdc::vce_to_rgba32(0x1C0U) == 0xFF00FF00U);
    // Pure blue: G=0, R=0, B=7 -> 0b000_000_111 = 0x007
    assert(Vdc::vce_to_rgba32(0x007U) == 0xFFFF0000U);
    // Pure white: 0x1FF
    assert(Vdc::vce_to_rgba32(0x1FFU) == 0xFFFFFFFFU);
}

void test_bg_and_sprite_render_and_dma() {
    std::cout << "[Test] HuC6270 VDC BG tile, Sprite SATB DMA, VRAM DMA, and RCR IRQ...\n";
    Vdc vdc;

    // Palette 0: color 0 = black, color 1 = pure red (0x038)
    write_vce_color(vdc, 0U, 0x000U);
    write_vce_color(vdc, 1U, 0x038U);
    // Sprite palette 0 (index 256+1): pure green (0x1C0)
    write_vce_color(vdc, 257U, 0x1C0U);

    // Write BG tile #16 at VRAM word 16*16 = 0x0100 (solid color 1: plane 0 = 0xFF)
    write_vdc_reg(vdc, 0x00U, 0x0100U);  // MAWR = 0x0100
    vdc.write_vdc(0U, 0x02U);
    for (int row = 0; row < 8; ++row) {
        vdc.write_vdc(2U, 0xFFU);  // plane 0 = all 1s
        vdc.write_vdc(3U, 0x00U);  // plane 1 = 0
    }
    for (int row = 0; row < 8; ++row) {
        vdc.write_vdc(2U, 0x00U);  // plane 2 = 0
        vdc.write_vdc(3U, 0x00U);  // plane 3 = 0
    }

    // Place tile #16 at BAT(0, 0) = VRAM word 0
    write_vdc_reg(vdc, 0x00U, 0x0000U);
    write_vdc_reg(vdc, 0x02U, 0x0010U);  // tile 16, palette 0

    // Write sprite pattern #16 at VRAM word 16*64 = 0x0400 (plane 0 = 0xFFFF -> color 1)
    write_vdc_reg(vdc, 0x00U, 0x0400U);
    vdc.write_vdc(0U, 0x02U);
    for (int row = 0; row < 16; ++row) {
        vdc.write_vdc(2U, 0xFFU);
        vdc.write_vdc(3U, 0xFFU);
    }

    // Put SATB in VRAM at 0x1000: sprite 0 at (x=32+16, y=64+0), pattern=16<<1=32, front of BG
    write_vdc_reg(vdc, 0x00U, 0x1000U);
    write_vdc_reg(vdc, 0x02U, 64U);       // Y = 0
    write_vdc_reg(vdc, 0x02U, 32U + 16U); // X = 16
    write_vdc_reg(vdc, 0x02U, 32U);       // pattern index 16 (bits 1..10)
    write_vdc_reg(vdc, 0x02U, 0x0080U);   // SPBG=1 (in front), 16x16, pal 0

    // Trigger SATB DMA from 0x1000 + enable BG, Sprites, RCR IRQ at line 10 (RCR = 74), VBlank IRQ
    write_vdc_reg(vdc, 0x05U, 0x00CCU);  // BG + SPR + VBlank IRQ + RCR IRQ
    write_vdc_reg(vdc, 0x06U, 64U + 10U);
    write_vdc_reg(vdc, 0x13U, 0x1000U);  // DVSSR

    // Step 1 full frame so SATB DMA runs at VBlank, then step 1 more frame to render both BG + Sprite
    for (int frame = 0; frame < 2; ++frame) {
        bool saw_rcr = false;
        while (!vdc.consume_frame_ready()) {
            vdc.step(Vdc::kCyclesPerScanline);
            if (vdc.irq_pending()) {
                const uint8_t st = vdc.read_vdc(0U);
                if (st & Vdc::kStatRcr) {
                    saw_rcr = true;
                }
            }
        }
        assert(saw_rcr);
    }

    const auto& fb = vdc.framebuffer();
    assert(fb[0] == 0xFF0000FFU);   // (0,0) = BG tile red
    assert(fb[16] == 0xFF00FF00U);  // (16,0) = Sprite green

    // Verify VRAM-to-VRAM DMA (copy 4 words from 0x0100 -> 0x2000)
    write_vdc_reg(vdc, 0x10U, 0x0100U);
    write_vdc_reg(vdc, 0x11U, 0x2000U);
    write_vdc_reg(vdc, 0x12U, 0x0003U);  // 4 words
    assert(vdc.vram()[0x2000U] == 0x00FFU);
    assert(vdc.vram()[0x2003U] == 0x00FFU);
}

}  // namespace

int main() {
    test_vce_palette_grb9();
    test_bg_and_sprite_render_and_dma();
    std::cout << "All PCE VDC/VCE unit tests passed!\n";
    return 0;
}
