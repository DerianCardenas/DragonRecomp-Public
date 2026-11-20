#include "pce/psg.h"

#include <algorithm>
#include <cmath>

namespace dragon::pce {

namespace {

// 32-step logarithmic attenuation table (-1.5 dB per step, 0..31 -> 0..1024)
constexpr std::array<int32_t, 32> make_vol_table() {
    std::array<int32_t, 32> t{};
    // Precomputed approximate 1.5 dB/step curve (max = 1024 at vol 31, 0 at vol 0)
    constexpr int32_t kVals[32] = {
        0,   5,   6,   7,   8,   10,  12,  14,
        16,  19,  23,  27,  32,  38,  45,  54,
        64,  76,  91,  108, 128, 152, 181, 215,
        256, 304, 362, 431, 512, 609, 724, 861,
    };
    for (size_t i = 0; i < 32; ++i) {
        t[i] = kVals[i];
    }
    return t;
}

constexpr auto kVolTable = make_vol_table();

}  // namespace

Psg::Psg() {
    reset();
}

void Psg::reset() noexcept {
    st_ = {};
    st_.main_balance = 0xFFU;
    for (auto& c : st_.ch) {
        c.balance = 0xFFU;
        c.lfsr = 1U;
    }
    ring_.fill(0);
    ring_read_ = 0;
    ring_write_ = 0;
}

void Psg::write(uint8_t reg, uint8_t val) noexcept {
    const uint8_t r = reg & 0x0FU;
    if (r == 0U) {
        st_.ch_select = val & 0x07U;
        return;
    }
    if (r == 1U) {
        st_.main_balance = val;
        return;
    }
    if (r == 8U) {
        st_.lfo_freq = val;
        return;
    }
    if (r == 9U) {
        st_.lfo_ctrl = val;
        if (val & 0x80U) {
            st_.ch[1].wave_pos = 0;
        }
        return;
    }

    if (st_.ch_select >= 6U) {
        return;
    }
    PsgChannel& c = st_.ch[st_.ch_select];
    switch (r) {
        case 2U:
            c.freq = static_cast<uint16_t>((c.freq & 0x0F00U) | val);
            break;
        case 3U:
            c.freq = static_cast<uint16_t>((c.freq & 0x00FFU) | (static_cast<uint16_t>(val & 0x0FU) << 8U));
            break;
        case 4U:
            c.control = val;
            if ((val & 0xC0U) == 0x40U) {
                // Bit 7=0, Bit 6=1 resets waveform write/play index
                c.wave_pos = 0;
            }
            break;
        case 5U:
            c.balance = val;
            break;
        case 6U: {
            const uint8_t s = val & 0x1FU;
            c.dda_sample = s;
            if ((c.control & 0x80U) == 0U) {
                // Write to 32-byte waveform buffer when channel disabled
                c.wave[c.wave_pos & 0x1FU] = s;
                c.wave_pos = static_cast<uint8_t>((c.wave_pos + 1U) & 0x1FU);
            }
            break;
        }
        case 7U:
            if (st_.ch_select >= 4U) {
                c.noise_ctrl = val;
            }
            break;
        default:
            break;
    }
}

void Psg::tick_psg(uint32_t psg_clocks) noexcept {
    // PSG runs at CPU / 2 = 3.579545 MHz
    for (int i = 0; i < 6; ++i) {
        PsgChannel& c = st_.ch[static_cast<size_t>(i)];
        if ((c.control & 0x80U) == 0U) {
            continue;
        }

        // Noise generator on channels 4 and 5
        if (i >= 4 && (c.noise_ctrl & 0x80U) != 0U) {
            const uint32_t nfreq = static_cast<uint32_t>(c.noise_ctrl & 0x1FU);
            const uint32_t period = (nfreq == 31U) ? 32U : ((31U - nfreq) * 64U);
            c.noise_counter += psg_clocks;
            while (c.noise_counter >= period) {
                c.noise_counter -= period;
                const uint32_t bit = ((c.lfsr & 1U) ^ ((c.lfsr >> 1U) & 1U) ^
                                      ((c.lfsr >> 11U) & 1U) ^ ((c.lfsr >> 12U) & 1U) ^
                                      ((c.lfsr >> 17U) & 1U)) &
                                     1U;
                c.lfsr = (c.lfsr >> 1U) | (bit << 17U);
            }
            continue;
        }

        // Skip waveform clock in DDA mode
        if ((c.control & 0x40U) != 0U) {
            continue;
        }

        // LFO modulation: channel 1 modulates channel 0 when enabled
        int32_t freq_val = (c.freq == 0U) ? 4096 : static_cast<int32_t>(c.freq);
        if (i == 0 && (st_.lfo_ctrl & 0x83U) != 0U && (st_.lfo_ctrl & 0x80U) == 0U) {
            const int32_t lfo_s = static_cast<int32_t>(st_.ch[1].wave[st_.ch[1].wave_pos & 0x1FU]) - 16;
            const uint32_t shift = static_cast<uint32_t>((st_.lfo_ctrl & 0x03U) - 1U) * 2U;
            freq_val = std::clamp(freq_val + (lfo_s << shift), 1, 4096);
        }

        uint32_t period = static_cast<uint32_t>(freq_val);
        if (i == 1 && (st_.lfo_ctrl & 0x03U) != 0U && (st_.lfo_ctrl & 0x80U) == 0U) {
            const uint32_t lfo_mul = (st_.lfo_freq == 0U) ? 256U : static_cast<uint32_t>(st_.lfo_freq);
            period *= lfo_mul;
        }

        c.counter += psg_clocks;
        while (c.counter >= period) {
            c.counter -= period;
            c.wave_pos = static_cast<uint8_t>((c.wave_pos + 1U) & 0x1FU);
        }
    }
}

void Psg::emit_sample() noexcept {
    int32_t mix_l = 0;
    int32_t mix_r = 0;
    const uint8_t main_l = static_cast<uint8_t>((st_.main_balance >> 4U) & 0x0FU);
    const uint8_t main_r = static_cast<uint8_t>(st_.main_balance & 0x0FU);

    for (int i = 0; i < 6; ++i) {
        const PsgChannel& c = st_.ch[static_cast<size_t>(i)];
        if ((c.control & 0x80U) == 0U) {
            continue;
        }
        // Channel 1 is muted when used as LFO modulator
        if (i == 1 && (st_.lfo_ctrl & 0x03U) != 0U && (st_.lfo_ctrl & 0x80U) == 0U) {
            continue;
        }

        int32_t raw = 0;
        if (i >= 4 && (c.noise_ctrl & 0x80U) != 0U) {
            raw = (c.lfsr & 1U) ? 15 : -16;
        } else if ((c.control & 0x40U) != 0U) {
            raw = static_cast<int32_t>(c.dda_sample) - 16;
        } else {
            raw = static_cast<int32_t>(c.wave[c.wave_pos & 0x1FU]) - 16;
        }

        const uint8_t ch_vol = c.control & 0x1FU;
        const uint8_t ch_l = static_cast<uint8_t>((c.balance >> 4U) & 0x0FU);
        const uint8_t ch_r = static_cast<uint8_t>(c.balance & 0x0FU);

        // Combine channel vol (0..31) + channel balance (0..15) + main balance (0..15)
        const int idx_l = std::max(0, static_cast<int>(ch_vol) - (15 - static_cast<int>(ch_l)) * 2 -
                                          (15 - static_cast<int>(main_l)) * 2);
        const int idx_r = std::max(0, static_cast<int>(ch_vol) - (15 - static_cast<int>(ch_r)) * 2 -
                                          (15 - static_cast<int>(main_r)) * 2);

        mix_l += (raw * kVolTable[static_cast<size_t>(std::min(idx_l, 31))]) / 8;
        mix_r += (raw * kVolTable[static_cast<size_t>(std::min(idx_r, 31))]) / 8;
    }

    // Gentle 1-pole low-pass filter to remove harsh 48 kHz aliasing edges
    st_.lp_l += (mix_l - st_.lp_l) >> 1;
    st_.lp_r += (mix_r - st_.lp_r) >> 1;

    const int16_t sl = static_cast<int16_t>(std::clamp(st_.lp_l, -32768, 32767));
    const int16_t sr = static_cast<int16_t>(std::clamp(st_.lp_r, -32768, 32767));

    const size_t next_w = (ring_write_ + 1U) % kRingFrames;
    if (next_w != ring_read_) {
        ring_[ring_write_ * 2U + 0U] = sl;
        ring_[ring_write_ * 2U + 1U] = sr;
        ring_write_ = next_w;
    }
}

void Psg::step(uint32_t cpu_cycles) noexcept {
    // Interleave PSG ticks & 48 kHz sampling in chronological chunks, preserving odd CPU cycles
    uint32_t rem = cpu_cycles;
    while (rem > 0U) {
        const uint32_t chunk = (rem > 32U) ? 32U : rem;
        rem -= chunk;

        const uint32_t total_cpu = chunk + st_.psg_clock_rem;
        st_.psg_clock_rem = total_cpu & 1U;
        if (total_cpu >= 2U) {
            tick_psg(total_cpu >> 1U);  // PSG clock = CPU / 2 (exact, no odd-cycle drift)
        }

        st_.sample_accum += static_cast<uint64_t>(chunk) * kSampleRate;
        while (st_.sample_accum >= kCpuClockHz) {
            st_.sample_accum -= kCpuClockHz;
            emit_sample();
        }
    }
}

size_t Psg::available_frames() const noexcept {
    if (ring_write_ >= ring_read_) {
        return ring_write_ - ring_read_;
    }
    return kRingFrames - (ring_read_ - ring_write_);
}

size_t Psg::drain_samples(int16_t* dst_interleaved, size_t max_frames) noexcept {
    size_t count = 0;
    while (count < max_frames && ring_read_ != ring_write_) {
        dst_interleaved[count * 2U + 0U] = ring_[ring_read_ * 2U + 0U];
        dst_interleaved[count * 2U + 1U] = ring_[ring_read_ * 2U + 1U];
        ring_read_ = (ring_read_ + 1U) % kRingFrames;
        ++count;
    }
    return count;
}

}  // namespace dragon::pce
