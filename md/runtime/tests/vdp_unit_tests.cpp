#include <cassert>
#include <cstdio>
#include <vector>

#include "md/bus.h"
#include "md/vdp.h"

using namespace dragon::md;

namespace {

void test_vdp_cram_and_plane_render() {
    std::printf("[Test] VDP Mode 5 CRAM / VRAM / Plane A rendering...\n");
    Vdp vdp;
    vdp.reset(false);

    // Enable display (Reg $01 = $44), H40 mode (Reg $0C = $81), Plane A at $C000 (Reg $02 = $30),
    // Auto-increment = 2 (Reg $0F = $02)
    vdp.write_control(0x8144U, nullptr);
    vdp.write_control(0x8C81U, nullptr);
    vdp.write_control(0x8230U, nullptr);
    vdp.write_control(0x8F02U, nullptr);
    assert(vdp.active_width() == 320U);
    assert(vdp.active_height() == 224U);

    // Write CRAM[0] = black, CRAM[1] = bright red (0x000E)
    vdp.write_control(0xC000U, nullptr);
    vdp.write_control(0x0000U, nullptr);
    vdp.write_data(0x0000U, nullptr);
    vdp.write_data(0x000EU, nullptr);

    // Write Tile #1 (at VRAM $0020): fill with color index 1 (0x1111 per word)
    vdp.write_control(0x4020U, nullptr);
    vdp.write_control(0x0000U, nullptr);
    for (int i = 0; i < 16; ++i) {
        vdp.write_data(0x1111U, nullptr);
    }

    // Put Tile #1 at Plane A (0,0) = VRAM $C000
    vdp.write_control(0x4000U, nullptr);
    vdp.write_control(0x0003U, nullptr);
    vdp.write_data(0x0001U, nullptr);

    // Advance 1 full frame (262 scanlines * 488 cycles)
    vdp.step(Vdp::kScanlinesNtsc * Vdp::kCyclesPerScanline, nullptr);
    assert(vdp.consume_frame_complete());

    // Pixel (0,0) should be bright red (R=255, G=0, B=0 -> 0xFF0000FF in ABGR8888)
    const uint32_t px = vdp.framebuffer()[0];
    assert((px & 0xFFU) == 255U);
    assert(((px >> 8U) & 0xFFU) == 0U);
    assert(((px >> 16U) & 0xFFU) == 0U);
    std::printf("  passed!\n");
}

void test_vdp_dma_68k_to_vram() {
    std::printf("[Test] VDP 68K -> VRAM DMA...\n");
    Bus bus;
    std::vector<uint8_t> rom(65536, 0);
    rom[0x1000] = 0xAAU;
    rom[0x1001] = 0x55U;
    rom[0x1002] = 0x12U;
    rom[0x1003] = 0x34U;
    assert(bus.load_rom_bytes(rom));
    MdCpuState cpu{};
    bus.reset(cpu);

    auto& vdp = bus.vdp();
    vdp.write_control(0x8114U, &bus);  // DMA enable (bit 4)
    vdp.write_control(0x8F02U, &bus);  // Inc = 2
    vdp.write_control(0x9302U, &bus);  // Length = 2 words
    vdp.write_control(0x9400U, &bus);
    vdp.write_control(0x9500U, &bus);  // Source = $001000 >> 1 = $000800
    vdp.write_control(0x9608U, &bus);
    vdp.write_control(0x9700U, &bus);
    // Destination VRAM $0100 with DMA bit (CD5 = 1 -> 0x0080 in 2nd word)
    vdp.write_control(0x4100U, &bus);
    vdp.write_control(0x0080U, &bus);

    assert(vdp.vram()[0x0100] == 0xAAU);
    assert(vdp.vram()[0x0101] == 0x55U);
    assert(vdp.vram()[0x0102] == 0x12U);
    assert(vdp.vram()[0x0103] == 0x34U);
    std::printf("  passed!\n");
}

}  // namespace

int main() {
    test_vdp_cram_and_plane_render();
    test_vdp_dma_68k_to_vram();
    std::printf("All MD VDP unit tests passed!\n");
    return 0;
}
