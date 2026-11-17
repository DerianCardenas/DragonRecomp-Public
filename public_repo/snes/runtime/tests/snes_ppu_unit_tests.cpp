#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "snes/bus.h"
#include "snes/frontend.h"
#include "snes/ppu.h"

namespace {

using dragon::snes::Bus;
using dragon::snes::Ppu;

void test_vram_and_cgram_access() {
    Ppu ppu;
    ppu.reset();

    // 1. VMAIN step and write
    // Step by 1 word ($2115 = 0x80 -> increment on high byte $2119 write)
    ppu.write_register(0x15U, 0x80U);  // VMAIN: inc on $2119, step 1
    ppu.write_register(0x16U, 0x00U);  // VMADDL = 0
    ppu.write_register(0x17U, 0x10U);  // VMADDH = $10 -> word addr $1000

    ppu.write_register(0x18U, 0x34U);  // VMDATAL
    ppu.write_register(0x19U, 0x12U);  // VMDATAH -> writes $1234 at $1000 and increments to $1001
    assert(ppu.read_vram_word(0x1000U) == 0x1234U);

    ppu.write_register(0x18U, 0x78U);
    ppu.write_register(0x19U, 0x56U);  // writes $5678 at $1001
    assert(ppu.read_vram_word(0x1001U) == 0x5678U);

    // Test read prefetch latch
    ppu.write_register(0x16U, 0x00U);
    ppu.write_register(0x17U, 0x10U);  // sets addr to $1000 and pre-fetches $1234
    const uint8_t lo = ppu.read_register(0x39U);
    const uint8_t hi = ppu.read_register(0x3AU);
    assert(lo == 0x34U);
    assert(hi == 0x12U);

    // 2. CGRAM writes (15-bit BGR555)
    ppu.write_register(0x21U, 0x05U);  // CGADD = 5
    ppu.write_register(0x22U, 0x1FU);  // low byte (red=31)
    ppu.write_register(0x22U, 0x00U);  // high byte -> color $001F (Pure Red)
    assert(ppu.read_cgram_color(5U) == 0x001FU);

    ppu.write_register(0x21U, 0x05U);  // CGADD = 5 for read
    const uint8_t cg_lo = ppu.read_register(0x3BU);
    const uint8_t cg_hi = ppu.read_register(0x3BU);
    assert(cg_lo == 0x1FU);
    assert(cg_hi == 0x00U);

    std::printf("test_vram_and_cgram_access passed\n");
}

void test_oam_and_hardware_multiply() {
    Ppu ppu;
    ppu.reset();

    // 1. OAM write and read
    ppu.write_register(0x02U, 0x00U);  // OAMADDL = 0
    ppu.write_register(0x03U, 0x00U);  // OAMADDH = 0
    ppu.write_register(0x04U, 0x40U);  // X = 64 (latched)
    ppu.write_register(0x04U, 0x50U);  // Y = 80 (written with X)
    ppu.write_register(0x04U, 0x02U);  // Tile = 2
    ppu.write_register(0x04U, 0x30U);  // Attr = priority 3, palette 0

    assert(ppu.oam()[0] == 0x40U);
    assert(ppu.oam()[1] == 0x50U);
    assert(ppu.oam()[2] == 0x02U);
    assert(ppu.oam()[3] == 0x30U);

    // 2. Hardware signed 16x8 multiplication via M7A ($211B) and M7B ($211C)
    // Product = M7A (signed 16-bit) * M7B_high (signed 8-bit written to $211C)
    // Example: 250 * -4 = -1000 (0xFFFFFC18 in 24-bit)
    ppu.write_register(0x1BU, 250U & 0xFFU);
    ppu.write_register(0x1BU, 0U);                        // M7A = 250
    ppu.write_register(0x1CU, static_cast<uint8_t>(-4));  // M7B write
    const int32_t res = ppu.mpy_result();
    assert(res == -1000);

    const uint8_t mpyl = ppu.read_register(0x34U);
    const uint8_t mpym = ppu.read_register(0x35U);
    const uint8_t mpyh = ppu.read_register(0x36U);
    const int32_t read_res =
        static_cast<int32_t>((static_cast<uint32_t>(mpyh) << 16U) |
                             (static_cast<uint32_t>(mpym) << 8U) | static_cast<uint32_t>(mpyl));
    // Sign-extend 24-bit to 32-bit:
    const int32_t signed_read_res = (read_res & 0x800000) ? (read_res | ~0xFFFFFF) : read_res;
    assert(signed_read_res == -1000);

    std::printf("test_oam_and_hardware_multiply passed\n");
}

void test_mode1_and_bg3_priority() {
    Ppu ppu;
    ppu.reset();

    // Configure Mode 1: BG1 (4bpp), BG2 (4bpp), BG3 (2bpp)
    ppu.write_register(0x00U, 0x0FU);  // INIDISP = full brightness (15)
    ppu.write_register(0x05U, 0x09U);  // BGMODE = Mode 1 with BG3 priority bit set (bit 3)
    ppu.write_register(0x2CU, 0x07U);  // TM = enable BG1, BG2, BG3 on main screen

    // Set palettes:
    // BG1 palette 0 color 1: Green ($03E0)
    ppu.write_cgram_color(1U, 0x03E0U);
    // BG3 palette 0 color 1: Blue ($7C00)
    ppu.write_cgram_color(1U, 0x7C00U);  // Wait, Mode 1 BG3 palette 0 color 1 is CGRAM index 1

    // Setup a 4bpp tile in BG1 with color 1 (Green)
    // char base 0: tile 0
    ppu.write_vram_word(0U, 0x00FFU);  // bitplane 0 = 0xFF across line 0
    ppu.write_vram_word(8U, 0x0000U);  // bitplane 2/3 = 0

    // Setup a 2bpp tile in BG3 with color 1 (Blue) and High Priority (bit 13 set in map)
    // Let BG3 tilemap be at $0400 (1K words):
    ppu.write_register(0x09U, 0x04U);  // BG3SC base = $0400
    // Tile entry at $0400: tile 0, priority 1 -> 0x2000
    ppu.write_vram_word(0x0400U, 0x2000U);

    // BG3 char base at $2000:
    ppu.write_register(0x0CU, 0x02U);       // BG34NBA: BG3 char base = $2000
    ppu.write_vram_word(0x2000U, 0x00FFU);  // bitplane 0 = 0xFF across line 0

    ppu.render_scanline(0U);

    // With BG3 priority bit set in Mode 1, BG3 Priority 1 should take precedence over BG1!
    // Pixel 0 should be BG3's Blue color ($7C00 = 0x0000F8FF in RGBA)
    const uint32_t pixel = ppu.framebuffer()[0];
    const uint8_t b8 = static_cast<uint8_t>((pixel >> 8U) & 0xFFU);
    assert(b8 > 200U);

    std::printf("test_mode1_and_bg3_priority passed\n");
}

void test_color_math_and_master_brightness() {
    Ppu ppu;
    ppu.reset();

    // Backdrop color = pure blue (0x7C00: R=0, G=0, B=31)
    ppu.write_cgram_color(0U, 0x7C00U);

    // Master brightness 15, forced blank off
    ppu.write_register(0x00U, 0x0FU);

    // Enable color math on backdrop ($2131 CGADSUB bit 5 = 1)
    // Sub-screen math add fixed color (R=31, G=0, B=0)
    ppu.write_register(0x31U, 0x20U);  // CGADSUB: add backdrop
    ppu.write_register(0x32U, 0x3FU);  // COLDATA: write Red = 31 ($20 | 31)

    ppu.render_scanline(0U);

    // Main screen has Backdrop (Blue=31) + Fixed Color (Red=31) -> Magenta!
    const uint32_t pixel = ppu.framebuffer()[0];
    const uint8_t r8 = static_cast<uint8_t>((pixel >> 24U) & 0xFFU);
    const uint8_t g8 = static_cast<uint8_t>((pixel >> 16U) & 0xFFU);
    const uint8_t b8 = static_cast<uint8_t>((pixel >> 8U) & 0xFFU);

    assert(r8 > 240U);
    assert(g8 == 0U);
    assert(b8 > 240U);

    // Now test master brightness scale (e.g. brightness = 7 -> half)
    ppu.write_register(0x00U, 0x07U);
    ppu.render_scanline(0U);
    const uint32_t half_pixel = ppu.framebuffer()[0];
    const uint8_t half_r = static_cast<uint8_t>((half_pixel >> 24U) & 0xFFU);
    assert(half_r < 130U && half_r > 100U);

    // Test forced blank ($2100 bit 7)
    ppu.write_register(0x00U, 0x8FU);
    ppu.render_scanline(0U);
    const uint32_t black_pixel = ppu.framebuffer()[0];
    assert(black_pixel == 0xFF000000U);

    std::printf("test_color_math_and_master_brightness passed\n");
}

void test_mode7_affine_rendering() {
    Ppu ppu;
    ppu.reset();

    ppu.write_register(0x00U, 0x0FU);  // INIDISP = 15
    ppu.write_register(0x05U, 0x07U);  // BGMODE = Mode 7
    ppu.write_register(0x2CU, 0x01U);  // TM = BG1 enabled

    // In Mode 7:
    // Tilemap low byte at word $0000..$3FFF.
    // Tile 0 at map (0,0): word $0000 has low byte = tile index 0.
    // Tile graphics high byte: word $0000 has high byte = pixel color index.
    // Set tile index 0 at map (0,0) to color 10
    ppu.write_vram_word(0U, 0x0A00U);  // High byte = 10 (color), low byte = 0 (tile)
    // CGRAM color 10: Yellow ($03FF: R=31, G=31, B=0)
    ppu.write_cgram_color(10U, 0x03FFU);

    // Mode 7 matrix: Identity (M7A = 0x0100 = 1.0, M7D = 0x0100 = 1.0)
    ppu.write_register(0x1BU, 0x00U);
    ppu.write_register(0x1BU, 0x01U);  // M7A = 0x0100
    ppu.write_register(0x1EU, 0x00U);
    ppu.write_register(0x1EU, 0x01U);  // M7D = 0x0100

    ppu.render_scanline(0U);

    const uint32_t pixel = ppu.framebuffer()[0];
    const uint8_t r8 = static_cast<uint8_t>((pixel >> 24U) & 0xFFU);
    const uint8_t g8 = static_cast<uint8_t>((pixel >> 16U) & 0xFFU);
    const uint8_t b8 = static_cast<uint8_t>((pixel >> 8U) & 0xFFU);

    assert(r8 > 240U);
    assert(g8 > 240U);
    assert(b8 == 0U);

    std::printf("test_mode7_affine_rendering passed\n");
}

void test_hdma_transfers() {
    Bus bus;
    bus.reset();

    // Configure HDMA Channel 0:
    // Direct table, Mode 0 (1 register write once), target B-bus $2100 (INIDISP)
    // Table at $7E:2000 (in WRAM):
    // Line 0..4 (5 lines): value 0x0F (brightness 15)
    // Line 5..9 (5 lines): value 0x07 (brightness 7)
    // Line 10 (terminate): 0x00
    bus.write(0x7E2000U, 0x85U);  // Repeat mode ON, 5 lines
    bus.write(0x7E2001U, 0x0FU);  // brightness 15
    bus.write(0x7E2002U, 0x0FU);
    bus.write(0x7E2003U, 0x0FU);
    bus.write(0x7E2004U, 0x0FU);
    bus.write(0x7E2005U, 0x0FU);
    bus.write(0x7E2006U, 0x85U);  // Repeat mode ON, 5 lines
    bus.write(0x7E2007U, 0x07U);  // brightness 7
    bus.write(0x7E2008U, 0x07U);
    bus.write(0x7E2009U, 0x07U);
    bus.write(0x7E200AU, 0x07U);
    bus.write(0x7E200BU, 0x07U);
    bus.write(0x7E200CU, 0x00U);  // End of table

    // Setup channel 0 registers ($4300..$4304)
    bus.write(0x4300U, 0x00U);  // DMAP0: direct, mode 0
    bus.write(0x4301U, 0x00U);  // BBAD0: $2100 (INIDISP)
    bus.write(0x4302U, 0x00U);  // A1T0L = $2000
    bus.write(0x4303U, 0x20U);  // A1T0H = $2000
    bus.write(0x4304U, 0x7EU);  // A1B0  = $7E

    // Enable HDMA Channel 0 in $420C
    bus.write(0x420CU, 0x01U);

    // Run scanlines 0..4 sequentially
    for (uint16_t line = 0; line < 5; ++line) {
        bus.on_scanline(line);
        assert(bus.ppu().inidisp() == 0x0FU);
    }

    // Scanline 5: entry 2 begins -> brightness 7
    bus.on_scanline(5U);
    assert(bus.ppu().inidisp() == 0x07U);

    std::printf("test_hdma_transfers passed\n");
}

}  // namespace

int main() {
    test_vram_and_cgram_access();
    test_oam_and_hardware_multiply();
    test_mode1_and_bg3_priority();
    test_color_math_and_master_brightness();
    test_mode7_affine_rendering();
    test_hdma_transfers();

    dragon::snes::RunnerOptions opts;
    char prog[] = "snes_runner";
    char rom[] = "dummy.sfc";
    char fs[] = "--fullscreen";
    char crt[] = "--crt";
    char* argv[] = {prog, rom, fs, crt};
    int exit_code = 0;
    bool ok = dragon::snes::parse_runner_args(4, argv, opts, exit_code);
    assert(ok);
    assert(exit_code == 0);
    assert(opts.fullscreen);
    assert(opts.crt_filter);
    assert(opts.rom_path == "dummy.sfc");

    std::printf("All snes_ppu_unit_tests passed!\n");
    return 0;
}
