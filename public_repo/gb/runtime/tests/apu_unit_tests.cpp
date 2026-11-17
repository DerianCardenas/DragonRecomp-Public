#include "gb/apu.h"

#include <cassert>
#include <cstdio>
#include <vector>

#include "gb/bus.h"
#include "gb/cartridge.h"

using namespace dragon::gb;

static void test_apu_pulse_channel() {
    Apu apu;
    apu.reset();

    // Channel 1 initial state: inactive
    assert(!apu.channel_active(1));

    // Configure Channel 1:
    // NR10: sweep period 1, negate 0, shift 1 (0x11)
    // NR11: duty 50% (2), length 0 (0x80)
    // NR12: initial volume 10, envelope decrease, period 3 (0xA3)
    // NR13: frequency low 0x00
    // NR14: trigger, length enabled, freq high 0x04 (0xC4)
    apu.write_register(0xFF10, 0x11U);
    apu.write_register(0xFF11, 0x80U);
    apu.write_register(0xFF12, 0xA3U);
    apu.write_register(0xFF13, 0x00U);
    apu.write_register(0xFF14, 0xC4U);

    assert(apu.channel_active(1));

    // Read registers back and verify open bus / unused bit masks
    assert((apu.read_register(0xFF10) & 0x80U) == 0x80U);
    assert((apu.read_register(0xFF11) & 0x3FU) == 0x3FU);
    assert(apu.read_register(0xFF12) == 0xA3U);
    assert((apu.read_register(0xFF14) & 0xBFU) == 0xBFU);

    // Advance APU by 1 frame sequencer step (8192 cycles = 512 Hz)
    apu.advance(8192);

    // Channel 2 test
    apu.write_register(0xFF16, 0x80U);  // Duty 50%
    apu.write_register(0xFF17, 0xF0U);  // Vol 15
    apu.write_register(0xFF18, 0x50U);  // Freq low
    apu.write_register(0xFF19, 0x86U);  // Trigger, freq high 6
    assert(apu.channel_active(2));
}

static void test_apu_wave_channel() {
    Apu apu;
    apu.reset();

    // Wave RAM write & read
    for (uint16_t i = 0; i < 16; ++i) {
        apu.write_register(static_cast<uint16_t>(0xFF30U + i), static_cast<uint8_t>(i * 0x11U));
    }
    for (uint16_t i = 0; i < 16; ++i) {
        assert(apu.read_register(static_cast<uint16_t>(0xFF30U + i)) ==
               static_cast<uint8_t>(i * 0x11U));
    }

    // Enable Ch3 DAC: NR30 bit 7
    apu.write_register(0xFF1A, 0x80U);
    apu.write_register(0xFF1B, 0x00U);  // Length 256
    apu.write_register(0xFF1C, 0x20U);  // 100% volume
    apu.write_register(0xFF1D, 0x00U);  // Freq low
    apu.write_register(0xFF1E, 0x87U);  // Trigger
    assert(apu.channel_active(3));
}

static void test_apu_noise_and_mixer() {
    Apu apu;
    apu.reset();

    // Channel 4 Noise
    apu.write_register(0xFF20, 0x00U);
    apu.write_register(0xFF21, 0xF0U);  // Vol 15
    apu.write_register(0xFF22, 0x10U);  // Clock shift 1, 15-bit mode
    apu.write_register(0xFF23, 0x80U);  // Trigger
    assert(apu.channel_active(4));

    // Master volume & panning
    apu.write_register(0xFF24, 0x77U);  // Max L & R volume
    apu.write_register(0xFF25, 0xFFU);  // Pan all channels L & R

    // Advance 41943 cycles (~10 ms), check that audio samples are generated at 48000 Hz
    apu.advance(41943);
    assert(apu.available_samples() > 0);
    assert(apu.samples().size() % 2 == 0);  // Pairs of [L, R]

    apu.clear_samples();
    assert(apu.available_samples() == 0);

    // Disable APU via NR52 bit 7
    apu.write_register(0xFF26, 0x00U);
    assert(!apu.channel_active(1));
    assert(!apu.channel_active(2));
    assert(!apu.channel_active(3));
    assert(!apu.channel_active(4));
    assert((apu.read_register(0xFF26) & 0x80U) == 0);
}

static void test_cgb_hdma() {
    Bus bus;
    bus.set_cgb(true);

    // Set up source in WRAM Bank 0 ($C000)
    for (uint16_t i = 0; i < 64; ++i) {
        bus.write(static_cast<uint16_t>(0xC000U + i), static_cast<uint8_t>(0xA0U + i));
    }

    // Configure GDMA: src = $C000, dst = $8000, 2 blocks (32 bytes)
    bus.write(0xFF51, 0xC0U);  // Src high
    bus.write(0xFF52, 0x00U);  // Src low
    bus.write(0xFF53, 0x80U);  // Dst high
    bus.write(0xFF54, 0x00U);  // Dst low
    bus.write(0xFF55, 0x01U);  // Bit 7 = 0 (GDMA), length = 1 (2 blocks = 32 bytes)

    // Check that destination VRAM got the bytes immediately
    for (uint16_t i = 0; i < 32; ++i) {
        assert(bus.read(static_cast<uint16_t>(0x8000U + i)) == static_cast<uint8_t>(0xA0U + i));
    }
    // $FF55 read returns 0xFF when complete
    assert(bus.read(0xFF55) == 0xFFU);

    // Test HBlank DMA
    bus.write(0xFF51, 0xC0U);
    bus.write(0xFF52, 0x20U);  // Src $C020
    bus.write(0xFF53, 0x80U);
    bus.write(0xFF54, 0x20U);  // Dst $8020
    bus.write(0xFF55, 0x81U);  // Bit 7 = 1 (HDMA), length = 1 (2 blocks = 32 bytes)

    // Active HDMA returns remaining blocks in bits 0-6 and bit 7 is 0
    assert((bus.read(0xFF55) & 0x80U) == 0);

    // Step 1 HBlank
    bus.step_hdma();
    for (uint16_t i = 0; i < 16; ++i) {
        assert(bus.read(static_cast<uint16_t>(0x8020U + i)) ==
               static_cast<uint8_t>(0xA0U + 32U + i));
    }
    assert((bus.read(0xFF55) & 0x80U) == 0);

    // Step 2nd HBlank (transfer completes)
    bus.step_hdma();
    for (uint16_t i = 16; i < 32; ++i) {
        assert(bus.read(static_cast<uint16_t>(0x8020U + i)) ==
               static_cast<uint8_t>(0xA0U + 32U + i));
    }
    assert(bus.read(0xFF55) == 0xFFU);
}

static void test_mbc3_rtc() {
    Bus bus;
    Cartridge cart;
    cart.mbc = MbcType::Mbc3;
    cart.ram_size = 32768;
    cart.rom.assign(0x8000, 0);
    bus.load_cartridge(cart);

    // Enable RAM / RTC access
    bus.write(0x0000, 0x0AU);

    // Select RTC Seconds register ($08)
    bus.write(0x4000, 0x08U);
    bus.write(0xA000, 45U);  // set seconds to 45

    // Select RTC Minutes register ($09)
    bus.write(0x4000, 0x09U);
    bus.write(0xA000, 59U);  // set minutes to 59

    // Select RTC Hours register ($0A)
    bus.write(0x4000, 0x0AU);
    bus.write(0xA000, 23U);  // set hours to 23

    // Select RTC Day Low register ($0B)
    bus.write(0x4000, 0x0BU);
    bus.write(0xA000, 10U);  // Day 10

    // Latch RTC: write 0x00 then 0x01 to $6000
    bus.write(0x6000, 0x00U);
    bus.write(0x6000, 0x01U);

    bus.write(0x4000, 0x08U);
    assert(bus.read(0xA000) == 45U);

    // Step 15 seconds (15 * 4,194,304 cycles)
    // Seconds should roll over: 45 + 15 = 60 -> 0s, 59m + 1 = 0m, 23h + 1 = 0h, day 10 + 1 = 11!
    bus.step_timers(15U * 4194304U);

    // Before latching, reads still return old latched values
    assert(bus.read(0xA000) == 45U);

    // Latch again
    bus.write(0x6000, 0x00U);
    bus.write(0x6000, 0x01U);

    // Read back latched values
    bus.write(0x4000, 0x08U);
    assert(bus.read(0xA000) == 0U);  // Seconds = 0

    bus.write(0x4000, 0x09U);
    assert(bus.read(0xA000) == 0U);  // Minutes = 0

    bus.write(0x4000, 0x0AU);
    assert(bus.read(0xA000) == 0U);  // Hours = 0

    bus.write(0x4000, 0x0BU);
    assert(bus.read(0xA000) == 11U);  // Days = 11
}

int main() {
    test_apu_pulse_channel();
    test_apu_wave_channel();
    test_apu_noise_and_mixer();
    test_cgb_hdma();
    test_mbc3_rtc();
    std::printf("gb_apu_unit_tests: all APU, HDMA, and RTC tests passed successfully.\n");
    return 0;
}
