#include <cassert>
#include <cstdio>

#include "nds/bus.h"

using namespace dragon::nds;

namespace {

void test_2d_engine_and_3d_gx_compositing() {
    std::printf("Running test_2d_engine_and_3d_gx_compositing...\n");
    Bus bus;
    NdsCpuState arm9{}, arm7{};
    bus.reset(arm9, arm7);

    // Enable Engine A normal display + BG0 3D (DISPCNT_A = 0x00010108)
    bus.arm9_write32(0x04000000U, 0x00010108U, &arm9);
    // Backdrop color = pure blue (BGR555 = 0x7C00)
    bus.arm9_write16(0x05000000U, 0x7C00U, &arm9);

    // Submit a centered bright green 3D triangle via GX direct command ports
    bus.arm9_write32(0x04000350U, 0x00000000U, &arm9);  // CLEAR_COLOR (alpha=0 -> transparent)
    bus.arm9_write32(0x04000440U, 0U, &arm9);           // MTX_MODE = Projection
    bus.arm9_write32(0x04000454U, 0U, &arm9);           // MTX_IDENTITY
    bus.arm9_write32(0x04000440U, 1U, &arm9);           // MTX_MODE = Position
    bus.arm9_write32(0x04000454U, 0U, &arm9);           // MTX_IDENTITY
    bus.arm9_write32(0x040004A4U, 0x001F00C0U, &arm9);  // POLYGON_ATTR (opaque, both sides)
    bus.arm9_write32(0x04000480U, 0x03E0U, &arm9);      // COLOR = pure green
    bus.arm9_write32(0x04000500U, 0U, &arm9);           // BEGIN_VTXS (Triangles)
    // V0 (-0.5, -0.5, 0) in 1.3.12 fixed point (-2048 = 0xF800)
    bus.arm9_write32(0x0400048CU, 0xF800F800U, &arm9);
    bus.arm9_write32(0x0400048CU, 0x00000000U, &arm9);
    // V1 (+0.5, -0.5, 0)
    bus.arm9_write32(0x0400048CU, 0xF8000800U, &arm9);
    bus.arm9_write32(0x0400048CU, 0x00000000U, &arm9);
    // V2 (0, +0.5, 0)
    bus.arm9_write32(0x0400048CU, 0x08000000U, &arm9);
    bus.arm9_write32(0x0400048CU, 0x00000000U, &arm9);
    bus.arm9_write32(0x04000504U, 0U, &arm9);  // END_VTXS
    bus.arm9_write32(0x04000540U, 0U, &arm9);  // SWAP_BUFFERS

    // Render one full frame (VBlank triggers GX3D render -> next frame Engine A composites it)
    for (uint32_t i = 0; i < Bus::kTotalScanlines * 2U; ++i) {
        bus.step_scanline(arm9, arm7);
    }

    const auto& fb = bus.framebuffer();
    // Corner pixel should be backdrop blue (R=0, G=0, B=255)
    const uint32_t corner = fb[2 * 256 + 2];
    assert((corner & 0xFFU) == 0U && ((corner >> 8U) & 0xFFU) == 0U &&
           ((corner >> 16U) & 0xFFU) == 255U);
    // Center pixel of top screen (Engine A, y=96, x=128) should be green from 3D GX!
    const uint32_t center = fb[96 * 256 + 128];
    assert((center & 0xFFU) == 0U && ((center >> 8U) & 0xFFU) == 255U &&
           ((center >> 16U) & 0xFFU) == 0U);
    std::printf("  passed!\n");
}

void test_spu_psg_audio_output() {
    std::printf("Running test_spu_psg_audio_output...\n");
    Bus bus;
    NdsCpuState arm9{}, arm7{};
    bus.reset(arm9, arm7);

    // Enable master sound (vol 127, bit 15 enable)
    bus.arm7_write16(0x04000500U, 0x807FU);
    // Configure channel 8 as PSG square wave (fmt=3, duty=3, pan=64, vol=127, start=1)
    bus.arm7_write16(0x04000488U, 0xFE00U);  // Timer
    bus.arm7_write32(0x04000480U, (1U << 31) | (3U << 29) | (3U << 24) | (64U << 16) | 127U);

    bus.spu().step(100000U, bus);
    int16_t buf[512]{};
    const size_t got = bus.drain_audio_samples(buf, 256U);
    assert(got > 50U);
    bool non_zero = false;
    for (size_t i = 0; i < got * 2U; ++i) {
        if (buf[i] != 0) {
            non_zero = true;
            break;
        }
    }
    assert(non_zero);
    std::printf("  passed!\n");
}

void test_obj_extended_palette() {
    std::printf("Running test_obj_extended_palette...\n");
    Bus bus;
    NdsCpuState arm9{}, arm7{};
    bus.reset(arm9, arm7);

    // Map VRAM Bank A to OBJ A (MST=2, enabled=0x82) and VRAM Bank F to LCDC first
    bus.arm9_write8(0x04000240U, 0x82U, &arm9);  // VRAM A -> Engine A OBJ
    bus.arm9_write8(0x04000245U, 0x80U, &arm9);  // VRAM F -> LCDC (0x06890000)

    // Write bright red (BGR555 = 0x001F) to OBJ Extended Palette 3, color index 5 in VRAM F
    const uint32_t pal_off = (3U * 256U + 5U) * 2U;
    bus.arm9_write16(0x06890000U + pal_off, 0x001FU, &arm9);

    // Remap VRAM Bank F to Engine A OBJ Extended Palette (MST=5, enabled=0x85)
    bus.arm9_write8(0x04000245U, 0x85U, &arm9);

    // Write an 8bpp 8x8 tile (tile 0) filled with color index 5 in Engine A OBJ VRAM (0x06400000)
    for (uint32_t i = 0; i < 64U; ++i) {
        bus.arm9_write8(0x06400000U + i, 5U, &arm9);
    }

    // Configure OAM entry 0: 8x8, 8bpp (attr0 bit 13), y=16, x=16, tile=0, ext pal=3 (attr2[15:12])
    bus.arm9_write16(0x07000000U, static_cast<uint16_t>(16U | (1U << 13)), &arm9);
    bus.arm9_write16(0x07000002U, 16U, &arm9);
    bus.arm9_write16(0x07000004U, static_cast<uint16_t>(3U << 12), &arm9);

    // Enable Engine A normal display + OBJ + 1D OBJ + OBJ Extended Palette (bit 31)
    bus.arm9_write32(0x04000000U, 0x80011010U, &arm9);

    for (uint32_t i = 0; i < Bus::kTotalScanlines; ++i) {
        bus.step_scanline(arm9, arm7);
    }

    const uint32_t px = bus.framebuffer()[20 * 256 + 20];
    assert((px & 0xFFU) == 255U && ((px >> 8U) & 0xFFU) == 0U && ((px >> 16U) & 0xFFU) == 0U);
    std::printf("  passed!\n");
}

}  // namespace

int main() {
    test_2d_engine_and_3d_gx_compositing();
    test_spu_psg_audio_output();
    test_obj_extended_palette();
    std::printf("All NDS 2D/3D/SPU unit tests passed!\n");
    return 0;
}
