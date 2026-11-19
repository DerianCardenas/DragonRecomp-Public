#include <cassert>
#include <iostream>

#include "sms/vdp.h"

using namespace dragon::sms;

namespace {

void write_vdp_reg(Vdp& vdp, uint8_t reg, uint8_t val) {
    vdp.write_control(val);
    vdp.write_control(static_cast<uint8_t>(0x80U | (reg & 0x0FU)));
}

void set_vram_write(Vdp& vdp, uint16_t addr) {
    vdp.write_control(static_cast<uint8_t>(addr & 0xFFU));
    vdp.write_control(static_cast<uint8_t>(0x40U | ((addr >> 8U) & 0x3FU)));
}

void set_cram_write(Vdp& vdp, uint8_t addr) {
    vdp.write_control(addr);
    vdp.write_control(0xC0U);
}

void test_sms_cram_and_bg_tile() {
    std::cout << "[Test] SMS Mode 4 6-bit CRAM, tile rendering, and column-0 mask...\n";
    Vdp vdp;
    vdp.reset(false);
    // Display ON, Mode 4, Name table at $3800 (reg2=0xFF), SAT at $3F00 (reg5=0xFF)
    write_vdp_reg(vdp, 0, 0x04U);  // M4=1, mask_col0=0
    write_vdp_reg(vdp, 1, 0x40U);  // Display ON
    write_vdp_reg(vdp, 2, 0xFFU);
    write_vdp_reg(vdp, 5, 0xFFU);
    write_vdp_reg(vdp, 7, 0x02U);  // Backdrop = CRAM[18]

    // Set CRAM[1] = pure red (0b000011 = 0x03 -> R=255,G=0,B=0)
    // Set CRAM[18] = pure blue (0b110000 = 0x30 -> R=0,G=0,B=255)
    set_cram_write(vdp, 1U);
    vdp.write_data(0x03U);
    set_cram_write(vdp, 18U);
    vdp.write_data(0x30U);

    // Tile 1: solid color index 1 on row 0 (plane 0 = 0xFF, planes 1..3 = 0x00)
    set_vram_write(vdp, 32U);
    vdp.write_data(0xFFU);
    vdp.write_data(0x00U);
    vdp.write_data(0x00U);
    vdp.write_data(0x00U);

    // Put Tile 1 at nametable (0,0) = $3800 and terminate SAT at $3F00 with 0xD0
    set_vram_write(vdp, 0x3800U);
    vdp.write_data(0x01U);
    vdp.write_data(0x00U);
    set_vram_write(vdp, 0x3F00U);
    vdp.write_data(0xD0U);

    vdp.step(Vdp::kCyclesPerScanline * 2U);
    const uint32_t px0 = vdp.framebuffer()[0];
    assert((px0 & 0xFFU) == 255U);         // R = 255
    assert(((px0 >> 8U) & 0xFFU) == 0U);   // G = 0
    assert(((px0 >> 16U) & 0xFFU) == 0U);  // B = 0

    // Now enable Column 0 mask (Reg 0 bit 5) and render next frame line 0
    write_vdp_reg(vdp, 0, 0x24U);
    vdp.step(Vdp::kCyclesPerScanline * Vdp::kTotalScanlines);
    const uint32_t masked_px0 = vdp.framebuffer()[0];
    assert((masked_px0 & 0xFFU) == 0U);           // R = 0
    assert(((masked_px0 >> 16U) & 0xFFU) == 255U);  // B = 255 (backdrop CRAM[18])
}

void test_gg_12bit_cram_latch() {
    std::cout << "[Test] Game Gear 12-bit CRAM even/odd byte latch...\n";
    Vdp vdp;
    vdp.reset(true);
    assert(vdp.visible_width() == 160);
    assert(vdp.visible_height() == 144);

    // Write even byte (GGGGRRRR = 0x5A -> G=5*17=85, R=10*17=170) -> must NOT commit yet
    set_cram_write(vdp, 0U);
    vdp.write_data(0x5AU);
    assert(vdp.cram_rgba32(0U) == 0xFF000000U);

    // Write odd byte (----BBBB = 0x0F -> B=15*17=255) -> commits both bytes!
    vdp.write_data(0x0FU);
    const uint32_t col = vdp.cram_rgba32(0U);
    assert((col & 0xFFU) == 170U);          // R
    assert(((col >> 8U) & 0xFFU) == 85U);   // G
    assert(((col >> 16U) & 0xFFU) == 255U); // B
}

void test_line_interrupt_and_sprites() {
    std::cout << "[Test] VDP line interrupt underflow, sprite overflow & collision...\n";
    Vdp vdp;
    vdp.reset(false);
    write_vdp_reg(vdp, 0, 0x14U);  // M4=1, IE1 (Line IRQ) = 1
    write_vdp_reg(vdp, 1, 0x40U);  // Display ON, IE0 = 0
    write_vdp_reg(vdp, 5, 0xFFU);  // SAT = $3F00
    write_vdp_reg(vdp, 6, 0x00U);  // Sprite patterns at $0000
    write_vdp_reg(vdp, 10, 0x05U); // Reload line counter = 5

    // Advance out of active display so counter reloads to 5, then wrap to scanline 0
    vdp.step(Vdp::kCyclesPerScanline * 262U);
    (void)vdp.read_status();
    assert(!vdp.irq_asserted());

    // Place 9 overlapping sprites at Y=10 (stored as Y-1=9), X=20, tile 1 (solid non-zero)
    set_vram_write(vdp, 32U);
    for (int r = 0; r < 8; ++r) {
        vdp.write_data(0xFFU); vdp.write_data(0x00U); vdp.write_data(0x00U); vdp.write_data(0x00U);
    }
    set_vram_write(vdp, 0x3F00U);
    for (int i = 0; i < 9; ++i) vdp.write_data(9U);
    vdp.write_data(0xD0U);
    set_vram_write(vdp, 0x3F80U);
    for (int i = 0; i < 9; ++i) {
        vdp.write_data(20U);
        vdp.write_data(1U);
    }

    // Step 6 scanlines -> line counter 5 -> 4 -> 3 -> 2 -> 1 -> 0 -> underflow!
    vdp.step(Vdp::kCyclesPerScanline * 6U);
    assert(vdp.irq_asserted());
    (void)vdp.read_status();
    assert(!vdp.irq_asserted());

    // Step past scanline 10 where the 9 sprites overlap -> Overflow (0x40) & Collision (0x20)
    vdp.step(Vdp::kCyclesPerScanline * 8U);
    const uint8_t st = vdp.read_status();
    assert((st & 0x40U) != 0U);
    assert((st & 0x20U) != 0U);
}

}  // namespace

int main() {
    test_sms_cram_and_bg_tile();
    test_gg_12bit_cram_latch();
    test_line_interrupt_and_sprites();
    std::cout << "All SMS/GG VDP unit tests passed!\n";
    return 0;
}
