#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

#include "sms/psg.h"

using namespace dragon::sms;

namespace {

void test_tone_and_stereo_panning() {
    std::cout << "[Test] SN76489 tone wave & Game Gear stereo panning...\n";
    Psg psg;
    psg.reset();

    // Channel 0: period = 0x100 (low 4 bits = 0, high 6 bits = 0x10), volume = 0 (max)
    psg.write(0x80U);  // Latch ch0 tone, low = 0
    psg.write(0x10U);  // High 6 bits = 0x10 -> period = 256
    psg.write(0x90U);  // Latch ch0 volume = 0 (max)

    // Pan ch0 ONLY to Left speaker (GG port $06 = 0x10)
    psg.write_stereo(0x10U);
    psg.step(60000U);

    std::vector<int16_t> buf(2048 * 2, 0);
    const size_t n = psg.drain_samples(buf.data(), 2048U);
    assert(n > 400U);

    bool saw_pos = false;
    bool saw_neg = false;
    for (size_t i = 0; i < n; ++i) {
        const int16_t l = buf[i * 2U];
        const int16_t r = buf[i * 2U + 1U];
        assert(r == 0);  // Right muted by GG panning!
        if (l > 0) saw_pos = true;
        if (l < 0) saw_neg = true;
    }
    assert(saw_pos && saw_neg);
}

void test_noise_lfsr() {
    std::cout << "[Test] SN76489 16-bit 0x0009 white noise LFSR...\n";
    Psg psg;
    psg.reset();

    psg.write(0xE4U);  // Noise: white noise (bit 2 = 1), rate 0 (16)
    psg.write(0xF0U);  // Noise volume = 0 (max)
    psg.step(60000U);

    assert(psg.state().lfsr != 0x8000U);  // LFSR advanced
    std::vector<int16_t> buf(2048 * 2, 0);
    const size_t n = psg.drain_samples(buf.data(), 2048U);
    assert(n > 400U);
    int transitions = 0;
    for (size_t i = 1; i < n; ++i) {
        if ((buf[i * 2U] > 0) != (buf[(i - 1U) * 2U] > 0)) {
            ++transitions;
        }
    }
    assert(transitions > 10);
}

}  // namespace

int main() {
    test_tone_and_stereo_panning();
    test_noise_lfsr();
    std::cout << "All SMS/GG SN76489 PSG unit tests passed!\n";
    return 0;
}
