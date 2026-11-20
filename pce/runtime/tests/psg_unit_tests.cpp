#include <array>
#include <cassert>
#include <iostream>

#include "pce/psg.h"

using namespace dragon::pce;

int main() {
    std::cout << "[Test] HuC6280 6-channel Wavetable PSG...\n";
    Psg psg;

    // Global balance max L/R
    psg.write(1U, 0xFFU);
    // Select channel 0, reset wave pos
    psg.write(0U, 0x00U);
    psg.write(4U, 0x40U);  // reset wave index
    psg.write(4U, 0x00U);  // disable while writing waveform
    // Load 50% duty square wave (16 x 31, 16 x 0)
    for (int i = 0; i < 16; ++i) psg.write(6U, 31U);
    for (int i = 0; i < 16; ++i) psg.write(6U, 0U);

    // Frequency = 128, balance max, enable channel 0 at max volume
    psg.write(2U, 128U);
    psg.write(3U, 0U);
    psg.write(5U, 0xFFU);
    psg.write(4U, 0x9FU);

    // Step ~10 ms (~71,590 CPU cycles)
    psg.step(71590U);
    assert(psg.available_frames() >= 400U);

    std::array<int16_t, 512 * 2> buf{};
    const size_t n = psg.drain_samples(buf.data(), 512U);
    assert(n >= 400U);

    bool saw_pos = false;
    bool saw_neg = false;
    for (size_t i = 0; i < n; ++i) {
        if (buf[i * 2U] > 100) saw_pos = true;
        if (buf[i * 2U] < -100) saw_neg = true;
    }
    assert(saw_pos && saw_neg);

    // Test noise on channel 4
    psg.write(0U, 4U);
    psg.write(5U, 0xFFU);
    psg.write(7U, 0x90U);  // noise enable + freq 16
    psg.write(4U, 0x9FU);
    psg.step(35000U);
    assert(psg.available_frames() >= 200U);

    std::cout << "All PCE PSG unit tests passed!\n";
    return 0;
}
