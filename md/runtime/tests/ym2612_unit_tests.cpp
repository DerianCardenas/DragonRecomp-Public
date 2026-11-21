#include <cassert>
#include <cstdio>
#include <vector>

#include "md/ym2612.h"

using namespace dragon::md;

namespace {

void test_ym2612_timers_and_dac() {
    std::printf("[Test] YM2612 Timer A/B & Channel 6 DAC...\n");
    Ym2612 ym;
    ym.reset();

    // Enable DAC (Reg $2B = $80) and write DAC sample $FF (full positive)
    ym.write(0, 0x2BU);
    ym.write(1, 0x80U);
    ym.write(0, 0x2AU);
    ym.write(1, 0xFFU);
    assert(ym.dac_enabled());
    assert(ym.dac_sample() == 0xFFU);

    // Set Timer A to 1023 (overflows on 1st tick) and enable load + flag
    ym.write(0, 0x24U);
    ym.write(1, 0xFFU);
    ym.write(0, 0x25U);
    ym.write(1, 0x03U);
    ym.write(0, 0x27U);
    ym.write(1, 0x05U);

    ym.step(4000U);
    assert((ym.read_status() & 0x01U) != 0U);
    assert(ym.available_frames() > 0U);

    std::vector<int16_t> samples(ym.available_frames() * 2U);
    const size_t n = ym.drain_samples(samples.data(), ym.available_frames());
    assert(n > 0U);
    assert(samples[0] > 0);  // DAC $FF produces positive amplitude
    std::printf("  passed!\n");
}

void test_ym2612_fm_voice() {
    std::printf("[Test] YM2612 FM Channel 1 tone generation...\n");
    Ym2612 ym;
    ym.reset();

    // Algorithm 7 (all 4 carriers), Pan L+R
    ym.write(0, 0xB0U);
    ym.write(1, 0x07U);
    ym.write(0, 0xB4U);
    ym.write(1, 0xC0U);
    // Operator 1: MUL=1, TL=0, AR=31 (instant attack)
    ym.write(0, 0x30U);
    ym.write(1, 0x01U);
    ym.write(0, 0x40U);
    ym.write(1, 0x00U);
    ym.write(0, 0x50U);
    ym.write(1, 0x1FU);
    // Frequency: Block 4, FNum = 0x284 (~440 Hz)
    ym.write(0, 0xA4U);
    ym.write(1, 0x22U);
    ym.write(0, 0xA0U);
    ym.write(1, 0x84U);
    // Key-On Ch 0, Op 1
    ym.write(0, 0x28U);
    ym.write(1, 0x10U);

    ym.step(127856U);  // 1 NTSC frame
    assert(ym.available_frames() > 500U);

    std::vector<int16_t> pcm(ym.available_frames() * 2U);
    const size_t got = ym.drain_samples(pcm.data(), ym.available_frames());
    bool nonzero = false;
    for (size_t i = 0; i < got * 2U; ++i) {
        if (pcm[i] != 0) {
            nonzero = true;
            break;
        }
    }
    assert(nonzero);
    std::printf("  passed!\n");
}

}  // namespace

int main() {
    test_ym2612_timers_and_dac();
    test_ym2612_fm_voice();
    std::printf("All MD YM2612 unit tests passed!\n");
    return 0;
}
