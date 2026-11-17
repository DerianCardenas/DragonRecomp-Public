#include <cassert>
#include <cstdint>
#include <cstdio>

#include "nes/frontend.h"
#include "nes/runtime.h"

namespace {

using dragon::nes::Bus;
using dragon::nes::cpu_init;
using dragon::nes::Mirroring;
using dragon::nes::Ppu;

void test_vblank_and_nmi_timing() {
    Ppu ppu;
    ppu.reset();

    // Step to scanline 241, dot 0 (just before VBlank)
    ppu.step_dots(241ULL * Ppu::kDotsPerScanline);
    assert(ppu.scanline() == 241U && ppu.dot() == 0U);
    assert((ppu.status() & 0x80U) == 0U);
    assert(!ppu.nmi_pending());

    // Step 1 dot to (241, 1) -> VBlank flag set, no NMI yet because PPUCTRL.7 == 0
    ppu.step_dot();
    assert(ppu.scanline() == 241U && ppu.dot() == 1U);
    assert((ppu.status() & 0x80U) != 0U);
    assert(ppu.consume_frame_complete());
    assert(!ppu.nmi_pending());

    // Enabling PPUCTRL.7 while VBlank flag is set immediately triggers an NMI edge
    ppu.write_register(0U, 0x80U);
    assert(ppu.consume_nmi());
    assert(!ppu.nmi_pending());

    // Reading $2002 returns bit 7 = 1 and clears bit 7 and write latch w
    ppu.write_register(5U, 0x10U);  // sets w = 1
    assert(ppu.w() == 1U);
    const uint8_t status_read = ppu.read_register(2U);
    assert((status_read & 0x80U) != 0U);
    assert((ppu.status() & 0x80U) == 0U);
    assert(ppu.w() == 0U);

    // Set VBlank and Sprite-0 hit via internal progression to next frame, verify cleared at (261,
    // 1)
    ppu.reset();
    ppu.write_register(0U, 0x80U);  // Enable NMI
    ppu.step_dots(241ULL * Ppu::kDotsPerScanline + 1ULL);
    assert((ppu.status() & 0x80U) != 0U);
    assert(ppu.consume_nmi());

    // Step from (241, 1) to (261, 1) -> 20 * 341 dots
    ppu.step_dots(20ULL * Ppu::kDotsPerScanline);
    assert(ppu.scanline() == 261U && ppu.dot() == 1U);
    assert((ppu.status() & 0xE0U) == 0U);

    // Verify odd-frame skipped dot at (261, 339 -> 0, 0) when rendering is enabled
    ppu.reset();
    ppu.write_register(1U, 0x18U);  // Enable BG + sprites
    // Frame 0 (even): full 262 * 341 = 89342 dots to return to (0, 0)
    ppu.step_dots(262ULL * Ppu::kDotsPerScanline);
    assert(ppu.frame_count() == 1ULL && ppu.scanline() == 0U && ppu.dot() == 0U);
    // Frame 1 (odd): 89341 dots (1 dot shorter) to return to (0, 0)
    ppu.step_dots(262ULL * Ppu::kDotsPerScanline - 1ULL);
    assert(ppu.frame_count() == 2ULL && ppu.scanline() == 0U && ppu.dot() == 0U);
}

void test_loopy_scroll_and_addr_updates() {
    Ppu ppu;
    ppu.reset();

    // Follow canonical nesdev wiki PPU scrolling register sequence:
    // $2000 write: %00000010 -> t = 000010000000000
    ppu.write_register(0U, 0x02U);
    assert(ppu.t() == 0x0800U);

    // $2002 read: w = 0
    (void)ppu.read_register(2U);
    assert(ppu.w() == 0U);

    // $2005 first write: %01111101 (0x7D) -> coarse X = 15 (0x0F), fine X = 5, w = 1
    ppu.write_register(5U, 0x7DU);
    assert((ppu.t() & 0x001FU) == 0x0FU);
    assert(ppu.fine_x() == 5U);
    assert(ppu.w() == 1U);

    // $2005 second write: %01011110 (0x5E) -> coarse Y = 11 (0x0B), fine Y = 6, w = 0
    ppu.write_register(5U, 0x5EU);
    assert(ppu.t() == 0x696FU);
    assert(ppu.w() == 0U);

    // $2006 first write: %00111101 (0x3D) -> high byte of t (bit 14 cleared), w = 1
    ppu.write_register(6U, 0x3DU);
    assert(ppu.t() == 0x3D6FU);
    assert(ppu.w() == 1U);

    // $2006 second write: %11110000 (0xF0) -> low byte of t, v = t, w = 0
    ppu.write_register(6U, 0xF0U);
    assert(ppu.t() == 0x3DF0U);
    assert(ppu.v() == 0x3DF0U);
    assert(ppu.w() == 0U);

    // Verify $2007 increment by 1 and by 32
    ppu.write_register(0U, 0x00U);  // +1 increment
    ppu.write_register(6U, 0x20U);
    ppu.write_register(6U, 0x00U);
    assert(ppu.v() == 0x2000U);
    ppu.write_register(7U, 0xAAU);
    assert(ppu.v() == 0x2001U);

    ppu.write_register(0U, 0x04U);  // +32 increment
    ppu.write_register(7U, 0xBBU);
    assert(ppu.v() == 0x2021U);
}

void test_ppudata_read_buffer_and_palette_mirroring() {
    Ppu ppu;
    ppu.reset();
    ppu.set_mirroring(Mirroring::Vertical);

    // Write distinct values to $2000 and $2001
    ppu.write_register(6U, 0x20U);
    ppu.write_register(6U, 0x00U);
    ppu.write_register(7U, 0x42U);
    ppu.write_register(7U, 0x99U);

    // Read back from $2000 via $2007: first read returns stale buffer (0x00), second returns 0x42,
    // third returns 0x99.
    ppu.write_register(6U, 0x20U);
    ppu.write_register(6U, 0x00U);
    assert(ppu.read_register(7U) == 0x00U);
    assert(ppu.read_register(7U) == 0x42U);
    assert(ppu.read_register(7U) == 0x99U);

    // Verify vertical mirroring ($2000 mirrors $2800; $2400 mirrors $2C00)
    ppu.write_register(6U, 0x28U);
    ppu.write_register(6U, 0x00U);
    (void)ppu.read_register(7U);  // prime buffer
    assert(ppu.read_register(7U) == 0x42U);

    // Verify palette RAM immediate read (no buffer delay) and $3F10/$3F14/$3F18/$3F1C mirroring
    ppu.write_register(6U, 0x3FU);
    ppu.write_register(6U, 0x00U);
    ppu.write_register(7U, 0x21U);  // $3F00 = $21

    ppu.write_register(6U, 0x3FU);
    ppu.write_register(6U, 0x10U);
    assert((ppu.read_register(7U) & 0x3FU) == 0x21U);  // $3F10 mirrors $3F00 immediately!

    // Writing to $3F14 updates $3F04
    ppu.write_register(6U, 0x3FU);
    ppu.write_register(6U, 0x14U);
    ppu.write_register(7U, 0x16U);

    ppu.write_register(6U, 0x3FU);
    ppu.write_register(6U, 0x04U);
    assert((ppu.read_register(7U) & 0x3FU) == 0x16U);
}

void test_sprite_zero_hit_dot_accuracy_and_clipping() {
    Ppu ppu;
    ppu.reset();

    // Fill pattern table tile #1 ($0010-$001F) with solid opaque pixels (0xFF)
    for (uint16_t i = 0U; i < 16U; ++i) {
        ppu.write_vram(static_cast<uint16_t>(0x0010U + i), 0xFFU);
    }

    // Place tile #1 across the entire top of nametable 0 ($2000-$207F)
    for (uint16_t i = 0U; i < 128U; ++i) {
        ppu.write_vram(static_cast<uint16_t>(0x2000U + i), 0x01U);
    }

    // Place Sprite 0 at X = 24, Y = 8 (drawn on scanlines 9..16, X = 24..31 -> dot 25..32)
    ppu.write_register(3U, 0x00U);
    ppu.write_register(4U, 8U);     // Y = 8 (scanline 9)
    ppu.write_register(4U, 0x01U);  // Tile #1 (opaque)
    ppu.write_register(4U, 0x00U);  // Attr = 0
    ppu.write_register(4U, 24U);    // X = 24

    // Enable BG + sprites + left-8px BG/sprites
    ppu.write_register(1U, 0x1EU);

    // Step to scanline 9, dot 24 (X = 23, 1 dot before Sprite-0 hit at X = 24 / dot = 25)
    ppu.step_dots(9ULL * Ppu::kDotsPerScanline + 24ULL);
    assert(ppu.scanline() == 9U && ppu.dot() == 24U);
    assert((ppu.status() & 0x40U) == 0U);

    // Step 1 dot to (scanline 9, dot 25 -> X = 24): Sprite-0 hit must set on this exact dot!
    ppu.step_dot();
    assert(ppu.scanline() == 9U && ppu.dot() == 25U);
    assert((ppu.status() & 0x40U) != 0U);

    // Test left-8px clipping: place Sprite 0 at X = 0, Y = 4, and disable left-8px sprite mask
    ppu.reset();
    for (uint16_t i = 0U; i < 16U; ++i) {
        ppu.write_vram(static_cast<uint16_t>(0x0010U + i), 0xFFU);
    }
    for (uint16_t i = 0U; i < 128U; ++i) {
        ppu.write_vram(static_cast<uint16_t>(0x2000U + i), 0x01U);
    }
    ppu.write_register(3U, 0x00U);
    ppu.write_register(4U, 4U);     // Y = 4 (drawn on scanline 5)
    ppu.write_register(4U, 0x01U);  // Tile #1
    ppu.write_register(4U, 0x00U);
    ppu.write_register(4U, 0U);     // X = 0..7
    ppu.write_register(1U, 0x1AU);  // BG + sprites enabled, left-8px BG on, left-8px sprites OFF

    ppu.step_dots(10ULL * Ppu::kDotsPerScanline);
    assert((ppu.status() & 0x40U) == 0U);

    // Test X = 255 inhibition: Sprite 0 at X = 255 must never trigger Sprite-0 hit
    ppu.reset();
    for (uint16_t i = 0U; i < 16U; ++i) {
        ppu.write_vram(static_cast<uint16_t>(0x0010U + i), 0xFFU);
    }
    for (uint16_t i = 0U; i < 128U; ++i) {
        ppu.write_vram(static_cast<uint16_t>(0x2000U + i), 0x01U);
    }
    ppu.write_register(3U, 0x00U);
    ppu.write_register(4U, 4U);
    ppu.write_register(4U, 0x01U);
    ppu.write_register(4U, 0x00U);
    ppu.write_register(4U, 255U);  // X = 255
    ppu.write_register(1U, 0x1EU);

    ppu.step_dots(10ULL * Ppu::kDotsPerScanline);
    assert((ppu.status() & 0x40U) == 0U);
}

void test_oam_dma_and_controllers() {
    Bus bus;
    CpuState cpu{};
    cpu_init(cpu, &bus);

    // Populate CPU RAM page $0200-$02FF with test pattern
    for (uint16_t i = 0U; i < 256U; ++i) {
        bus.write(static_cast<uint16_t>(0x0200U + i), static_cast<uint8_t>(i ^ 0x5AU));
    }

    // Trigger OAM DMA on an even CPU cycle (8) -> should stall 513 cycles
    cpu.cycles = 8ULL;
    bus_write(&cpu, 0x2003U, 0x00U);
    bus_write(&cpu, 0x4014U, 0x02U);
    assert(cpu.cycles == 8ULL + 513ULL);

    for (uint16_t i = 0U; i < 256U; ++i) {
        uint8_t expected = static_cast<uint8_t>(i ^ 0x5AU);
        if ((i & 3U) == 2U) {
            expected &= 0xE3U;  // Sprite attribute bits 2..4 are unimplemented in OAM
        }
        assert(bus.ppu().oam()[i] == expected);
    }

    // Trigger OAM DMA on an odd CPU cycle -> should stall 514 cycles
    cpu.cycles = 9ULL;
    bus_write(&cpu, 0x4014U, 0x02U);
    assert(cpu.cycles == 9ULL + 514ULL);

    // Test controller 1 strobe and serial readout (A, Start, Left, Right pressed = 0xC9)
    const uint8_t pattern = dragon::nes::kButtonA | dragon::nes::kButtonStart |
                            dragon::nes::kButtonLeft | dragon::nes::kButtonRight;
    bus.set_controller(0U, pattern);
    bus.write(0x4016U, 0x01U);  // Strobe high
    bus.write(0x4016U, 0x00U);  // Strobe low -> latches shift register

    for (uint8_t bit = 0U; bit < 8U; ++bit) {
        const uint8_t val = bus.read(0x4016U);
        const uint8_t expected_bit = static_cast<uint8_t>((pattern >> bit) & 0x01U);
        assert((val & 0x01U) == expected_bit);
    }
    // 9th read and beyond return 1 on standard NES controller
    assert((bus.read(0x4016U) & 0x01U) == 1U);
}

}  // namespace

int main() {
    test_vblank_and_nmi_timing();
    test_loopy_scroll_and_addr_updates();
    test_ppudata_read_buffer_and_palette_mirroring();
    test_sprite_zero_hit_dot_accuracy_and_clipping();
    test_oam_dma_and_controllers();

    dragon::nes::RunnerOptions opts;
    char prog[] = "nes_runner";
    char rom[] = "dummy.nes";
    char fs[] = "--fullscreen";
    char crt[] = "--crt";
    char* argv[] = {prog, rom, fs, crt};
    int exit_code = 0;
    bool ok = dragon::nes::parse_runner_args(4, argv, opts, exit_code);
    assert(ok);
    assert(exit_code == 0);
    assert(opts.fullscreen);
    assert(opts.crt_filter);
    assert(opts.rom_path == "dummy.nes");

    std::printf("nes_ppu_unit_tests: all PPU & controller unit tests passed\n");
    return 0;
}
