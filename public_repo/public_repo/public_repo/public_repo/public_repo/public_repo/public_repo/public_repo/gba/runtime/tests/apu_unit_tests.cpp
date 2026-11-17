#include <cassert>
#include <cstdio>

#include "gba/apu.h"

namespace {

using dragon::gba::Apu;

void test_directsound_fifo_and_dma_request() {
    Apu apu;
    apu.reset();

    // Enable master sound (SOUNDCNT_X bit 7 at offset 0x84)
    apu.write_io16(0x84U, 0x0080U);
    // SOUNDCNT_H (0x82): FIFO A 100% volume (bit 2), Right + Left enable (bits 8,9), Timer 0 (bit
    // 10=0)
    apu.write_io16(0x82U, 0x0304U);

    // Push 5 words (20 bytes) into FIFO A
    for (int i = 0; i < 5; ++i) {
        apu.write_fifo32(0, 0x40404040U);
    }

    // First 3 timer overflows leave 17 bytes -> no DMA refill needed yet
    assert((apu.on_timer_overflow(0) & 1U) == 0U);
    assert((apu.on_timer_overflow(0) & 1U) == 0U);
    assert((apu.on_timer_overflow(0) & 1U) == 0U);
    // 4th timer overflow drops FIFO A to 16 bytes -> triggers DMA refill request (bit 0)!
    assert((apu.on_timer_overflow(0) & 1U) != 0U);
}

void test_audio_sample_generation() {
    Apu apu;
    apu.reset();

    // Enable master sound
    apu.write_io16(0x84U, 0x0080U);
    // SOUNDCNT_H: FIFO A 100% volume, L+R enabled
    apu.write_io16(0x82U, 0x0304U);
    apu.write_fifo32(0, 0x7F7F7F7FU);
    (void)apu.on_timer_overflow(0);

    // Step 1/60th of a second (~280896 cycles -> ~800 stereo frames at 48 kHz)
    apu.step(280896U);

    const auto out = apu.samples();
    assert(out.size() > 1400U);
    bool non_zero = false;
    for (int16_t s : out) {
        if (s > 100) {
            non_zero = true;
            break;
        }
    }
    assert(non_zero);
}

void test_psg_channels_and_wave_ram() {
    Apu apu;
    apu.reset();

    // Enable master sound
    apu.write_io16(0x84U, 0x0080U);
    // SOUNDCNT_L: Master volume 7 on L & R, enable all 4 PSG channels on L & R
    apu.write_io16(0x80U, 0xFF77U);
    // SOUNDCNT_H: 100% PSG mixing ratio (bits 0-1 = 2)
    apu.write_io16(0x82U, 0x0002U);

    // Write Wave RAM pattern into bank 0 (0x090 - 0x09F)
    for (uint32_t i = 0; i < 16; ++i) {
        apu.write_io8(0x090U + i, static_cast<uint8_t>(0xF0U | (i & 0x0FU)));
        assert(apu.read_io8(0x090U + i) == static_cast<uint8_t>(0xF0U | (i & 0x0FU)));
    }

    // Enable Channel 3 Wave (0x70 bit 7 = master enable, bank 0 = bit 6 is 0)
    apu.write_io16(0x70U, 0x0080U);
    // Wave volume 100% (0x72 bits 13-14 = 1)
    apu.write_io16(0x72U, 0x2000U);
    // Trigger Channel 3 with frequency = 1024
    apu.write_io16(0x74U, 0x8400U);

    // Channel 4 Noise: volume 15, envelope decrease, trigger
    apu.write_io16(0x78U, 0xF000U);
    apu.write_io16(0x7CU, 0x8000U);

    // Step APU and check non-zero audio generation from PSG
    apu.step(280896U);
    const auto out = apu.samples();
    assert(!out.empty());
    bool has_sound = false;
    for (int16_t s : out) {
        if (s != 0) {
            has_sound = true;
            break;
        }
    }
    assert(has_sound);
}

void test_psg_length_counter() {
    Apu apu;
    apu.reset();

    apu.write_io16(0x84U, 0x0080U);
    apu.write_io16(0x80U, 0xFF77U);

    // Square 1: length = 64 - 63 = 1 tick (shortest duration: ~1/256th sec = 65536 cycles)
    apu.write_io16(0x62U, 0xF83FU);
    // Trigger with length enable (bit 14 = 0x4000)
    apu.write_io16(0x64U, 0xC400U);

    // SOUNDCNT_X bit 0 should be 1 (Square 1 active)
    assert((apu.read_io16(0x84U) & 1U) != 0U);

    // Advance frame sequencer past 1 length tick (~70000 cycles)
    apu.step(70000U);

    // Square 1 should now be disabled by length counter
    assert((apu.read_io16(0x84U) & 1U) == 0U);
}

void test_mixer_saturates_at_10_bits_with_headroom() {
    Apu apu;
    apu.reset();
    apu.write_io16(0x84U, 0x0080U);
    // FIFO A + B at 100%, both routed to L and R, driven by timer 0.
    apu.write_io16(0x82U, 0x330CU);
    apu.write_fifo32(0, 0x7F7F7F7FU);
    apu.write_fifo32(1, 0x7F7F7F7FU);
    (void)apu.on_timer_overflow(0);
    apu.step(280896U);

    const auto out = apu.samples();
    assert(out.size() > 1400U);
    constexpr int kExpectedPeak = 511 * 48;  // 10-bit DAC saturation scaled to 75% of int16
    int peak = 0;
    for (int16_t s : out) {
        const int a = s < 0 ? -static_cast<int>(s) : static_cast<int>(s);
        if (a > peak) {
            peak = a;
        }
    }
    assert(peak == kExpectedPeak);
}

}  // namespace

int main() {
    test_mixer_saturates_at_10_bits_with_headroom();
    test_directsound_fifo_and_dma_request();
    test_audio_sample_generation();
    test_psg_channels_and_wave_ram();
    test_psg_length_counter();
    std::printf("All GBA APU unit tests passed.\n");
    return 0;
}
