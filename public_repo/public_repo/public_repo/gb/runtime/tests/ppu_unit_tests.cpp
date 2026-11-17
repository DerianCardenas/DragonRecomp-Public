#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "gb/runtime.h"

namespace {

using dragon::gb::Bus;
using dragon::gb::Ppu;

void test_ppu_modes_and_vblank() {
    Bus bus;
    Ppu& ppu = bus.ppu();

    // Reset PPU: LCD is ON (LCDC = 0x91), LY = 0, Mode = 2 (OAM scan)
    assert((ppu.lcdc() & 0x80) != 0);

    // Advance 80 T-cycles -> transitions to Mode 3 (drawing)
    ppu.advance(80, &bus);
    assert((ppu.stat() & 0x03) == 3);

    // Advance 172 T-cycles -> transitions to Mode 0 (HBlank)
    ppu.advance(172, &bus);
    assert((ppu.stat() & 0x03) == 0);

    // Advance 204 T-cycles (456 total for scanline 0) -> LY becomes 1, Mode 2
    ppu.advance(204, &bus);
    assert(ppu.ly() == 1);
    assert((ppu.stat() & 0x03) == 2);

    // Advance 143 more scanlines (143 * 456 T-cycles) -> reaches line 144 (VBlank)
    for (int i = 1; i < 144; ++i) {
        ppu.advance(456, &bus);
    }
    assert(ppu.ly() == 144);
    assert((ppu.stat() & 0x03) == 1);  // Mode 1: VBlank
    assert(ppu.consume_frame_complete() == true);
    assert((bus.interrupt_flags() & 0x01) != 0);  // VBlank interrupt requested

    std::printf("test_ppu_modes_and_vblank: PASS\n");
}

void test_ppu_lyc_coincidence() {
    Bus bus;
    Ppu& ppu = bus.ppu();

    // Set LYC = 5, and enable LYC interrupt (STAT bit 6)
    bus.write(0xFF45, 5);     // LYC = 5
    bus.write(0xFF41, 0x40);  // STAT bit 6 = 1

    // Step 5 scanlines
    for (int i = 0; i < 5; ++i) {
        ppu.advance(456, &bus);
    }
    assert(ppu.ly() == 5);
    assert((ppu.stat() & 0x04) != 0);             // LYC coincidence flag set!
    assert((bus.interrupt_flags() & 0x02) != 0);  // STAT interrupt requested!

    std::printf("test_ppu_lyc_coincidence: PASS\n");
}

void test_tile_background_rendering() {
    Bus bus;
    Ppu& ppu = bus.ppu();

    // Write a solid 8x8 tile of Color 3 (both bitplanes = 0xFF) at tile index 0 ($8000)
    for (uint16_t i = 0; i < 16; ++i) {
        bus.write(static_cast<uint16_t>(0x8000U + i), 0xFFU);
    }

    // Set BGP to default: 11_10_01_00 (Color 3 -> Black 0xFF000000)
    bus.write(0xFF47, 0xE4U);

    // Render scanline 0
    ppu.advance(80, &bus);  // Enters Mode 3 and renders scanline 0

    const auto& fb = ppu.framebuffer();
    // Pixel 0 of scanline 0 must be Black (0xFF000000)
    assert(fb[0] == 0xFF000000U);
    assert(fb[159] == 0xFF000000U);

    std::printf("test_tile_background_rendering: PASS\n");
}

void test_sprite_rendering_and_transparency() {
    Bus bus;
    Ppu& ppu = bus.ppu();

    // Background tile 0 is white (0x00 bytes)
    for (uint16_t i = 0; i < 16; ++i) {
        bus.write(static_cast<uint16_t>(0x8000U + i), 0x00U);
    }

    // Sprite tile 1 ($8010) has Color 2 for the first row:
    // byte 0 = 0x00, byte 1 = 0xFF -> (0 | (1 << 1)) = 2
    bus.write(0x8010, 0x00U);
    bus.write(0x8011, 0xFFU);

    // Place sprite 0 in OAM: Y=16 (screen Y=0), X=8 (screen X=0), Tile=1, Attr=0
    bus.write(0xFE00, 16);
    bus.write(0xFE01, 8);
    bus.write(0xFE02, 1);
    bus.write(0xFE03, 0);

    // Set OBP0 to: Color 2 -> Dark Gray (0xFF555555)
    bus.write(0xFF48, 0xE4U);

    // Enable LCD, BG, and Sprites (LCDC bits 7, 0, 1 -> 0x93)
    bus.write(0xFF40, 0x93U);

    // Render scanline 0
    ppu.advance(80, &bus);

    const auto& fb = ppu.framebuffer();
    // Pixel 0 should have the sprite Color 2 (Dark Gray)
    assert(fb[0] == 0xFF555555U);
    // Pixel 8 is outside the 8-pixel sprite, so it remains background white
    assert(fb[8] == 0xFFFFFFFFU);

    std::printf("test_sprite_rendering_and_transparency: PASS\n");
}

}  // namespace

int main() {
    std::printf("=== Running Game Boy PPU Unit Tests ===\n");
    test_ppu_modes_and_vblank();
    test_ppu_lyc_coincidence();
    test_tile_background_rendering();
    test_sprite_rendering_and_transparency();
    std::printf("=== All Game Boy PPU Unit Tests Passed (4/4) ===\n");
    return 0;
}
