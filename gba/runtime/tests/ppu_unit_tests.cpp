#include <cassert>
#include <cstdio>

#include "gba/ppu.h"

namespace {

using dragon::gba::Ppu;

void step_full_frame(Ppu& ppu) {
    while (!ppu.consume_frame_complete()) {
        (void)ppu.step(308U);
    }
}

void test_mode0_tile_bg_and_sprite() {
    Ppu ppu;
    ppu.reset();

    // DISPCNT: Mode 0, BG0 enabled (bit 8), OBJ enabled (bit 12), 1D OBJ mapping (bit 6)
    ppu.write_io16(0x00U, 0x1140U);
    // BG0CNT: char_base = 0, screen_base = 8 (0x4000), 4bpp
    ppu.write_io16(0x08U, 0x0800U);

    // BG Palette index 1 = pure red (R=31 -> 0x001F)
    ppu.write_palette16(0x002U, 0x001FU);
    // OBJ Palette index 1 = pure green (G=31 -> 0x03E0)
    ppu.write_palette16(0x202U, 0x03E0U);

    // Tile 1 in charblock 0 (offset 0x20): fill row 0 with color index 1 (0x11 per byte)
    for (uint32_t i = 0; i < 4U; ++i) {
        ppu.write_vram16(0x0020U + i * 2U, 0x1111U);
    }
    // Screenblock 8 (0x4000): tile (0,0) uses tile 1
    ppu.write_vram16(0x4000U, 0x0001U);

    // OAM Sprite 0 at (x=16, y=0), 8x8, tile 2 in OBJ VRAM (0x10040)
    ppu.write_oam16(0x00U, 0x0000U);  // attr0: Y=0, normal, 4bpp, square
    ppu.write_oam16(0x02U, 0x0010U);  // attr1: X=16, size 0 (8x8)
    ppu.write_oam16(0x04U, 0x0002U);  // attr2: tile 2, prio 0, pal 0
    for (uint32_t i = 0; i < 4U; ++i) {
        ppu.write_vram16(0x10040U + i * 2U, 0x1111U);
    }

    step_full_frame(ppu);

    const auto& fb = ppu.framebuffer();
    // Pixel (0, 0) should be BG0 red (0xFF0000FF in RGBA32 little-endian: R=255, G=0, B=0, A=255)
    assert(fb[0] == 0xFF0000FFU);
    // Pixel (16, 0) should be Sprite 0 green (0xFF00FF00: R=0, G=255, B=0, A=255)
    assert(fb[16] == 0xFF00FF00U);
}

void test_mode3_bitmap() {
    Ppu ppu;
    ppu.reset();

    // DISPCNT: Mode 3, BG2 enabled (bit 10)
    ppu.write_io16(0x00U, 0x0403U);
    // Write pure blue (B=31 -> 0x7C00) to pixel (5, 2)
    const uint32_t offset = (2U * 240U + 5U) * 2U;
    ppu.write_vram16(offset, 0x7C00U);

    step_full_frame(ppu);

    const auto& fb = ppu.framebuffer();
    assert(fb[2U * 240U + 5U] == 0xFFFF0000U);
}

void test_mode4_paletted_page_flip() {
    Ppu ppu;
    ppu.reset();

    // Palette index 5 = pure green (0x03E0)
    ppu.write_palette16(5U * 2U, 0x03E0U);
    // Write palette index 5 to Page 1 (0xA000) pixel (0, 0)
    ppu.write_vram16(0xA000U, 0x0005U);

    // DISPCNT: Mode 4, Page 1 (bit 4 = 0x10), BG2 enabled (bit 10)
    ppu.write_io16(0x00U, 0x0414U);

    step_full_frame(ppu);

    const auto& fb = ppu.framebuffer();
    assert(fb[0] == 0xFF00FF00U);
}

void test_brightness_increase_effect() {
    Ppu ppu;
    ppu.reset();

    // DISPCNT: Mode 0, BG0 enabled
    ppu.write_io16(0x00U, 0x0100U);
    // Backdrop color = black (0x0000)
    ppu.write_palette16(0x000U, 0x0000U);
    // BLDCNT: Backdrop (bit 5 = 0x20) + Brightness increase (mode 2 = 0x80) -> 0x00A0
    ppu.write_io16(0x50U, 0x00A0U);
    // BLDY: EVY = 16 (full white)
    ppu.write_io16(0x54U, 16U);

    step_full_frame(ppu);

    const auto& fb = ppu.framebuffer();
    assert(fb[0] == 0xFFFFFFFFU);

    // Test WIN0 horizontal/vertical clipping mask (WIN0 enables BG0 only for x in [0, 8), y in [0,
    // 8); WINOUT disables BG0 outside WIN0)
    ppu.reset();
    ppu.write_io16(0x00U, 0x2100U);        // Mode 0, BG0 + WIN0 enabled
    ppu.write_io16(0x08U, 0x0800U);        // BG0CNT: screen_base = 8
    ppu.write_palette16(0x000U, 0x0000U);  // Backdrop = black
    ppu.write_palette16(0x002U, 0x001FU);  // BG color 1 = red
    for (uint32_t i = 0; i < 4U; ++i) {
        ppu.write_vram16(0x0020U + i * 2U, 0x1111U);
    }
    ppu.write_vram16(0x4000U, 0x0001U);  // Tile (0,0) = red
    ppu.write_vram16(0x4002U, 0x0001U);  // Tile (1,0) = red
    ppu.write_io16(0x40U, 0x0008U);      // WIN0H: X1=0, X2=8
    ppu.write_io16(0x44U, 0x0008U);      // WIN0V: Y1=0, Y2=8
    ppu.write_io16(0x48U, 0x0001U);      // WININ: BG0 enabled inside WIN0
    ppu.write_io16(0x4AU, 0x0000U);      // WINOUT: all layers disabled outside WIN0
    step_full_frame(ppu);
    assert(ppu.framebuffer()[0] == 0xFF0000FFU);   // Inside WIN0 (x=0, y=0) -> red
    assert(ppu.framebuffer()[10] == 0xFF000000U);  // Outside WIN0 (x=10, y=0) -> backdrop black
}

}  // namespace

int main() {
    test_mode0_tile_bg_and_sprite();
    test_mode3_bitmap();
    test_mode4_paletted_page_flip();
    test_brightness_increase_effect();
    std::printf("All GBA PPU unit tests passed.\n");
    return 0;
}
