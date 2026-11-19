#include "sms/psg.h"

#include <algorithm>

namespace dragon::sms {

namespace {

// 4-bit logarithmic attenuation table (-2 dB per step, index 15 = silent)
// Scaled so 4 simultaneous max-volume channels stay comfortably within int16 range.
constexpr std::array<int16_t, 16> kVolumeTable = {{
    4096, 3254, 2584, 2053, 1631, 1295, 1029, 817,
    649,  516,  410,  325,  258,  205,  163,  0,
}};

inline uint16_t parity16(uint16_t v) noexcept {
    v ^= static_cast<uint16_t>(v >> 8U);
    v ^= static_cast<uint16_t>(v >> 4U);
    v ^= static_cast<uint16_t>(v >> 2U);
    v ^= static_cast<uint16_t>(v >> 1U);
    return static_cast<uint16_t>(v & 1U);
}

}  // namespace

Psg::Psg() {
    reset();
}

void Psg::reset() noexcept {
    st_ = {};
    div16_accum_ = 0;
    sample_accum_ = 0;
    ring_read_ = 0;
    ring_write_ = 0;
    ring_count_ = 0;
}

void Psg::write(uint8_t value) noexcept {
    if ((value & 0x80U) != 0U) {
        // Latch/Data byte: 1 CC T DDDD
        st_.latched_channel = static_cast<uint8_t>((value >> 5U) & 0x03U);
        st_.latched_is_volume = static_cast<uint8_t>((value >> 4U) & 0x01U);
        const uint8_t data = static_cast<uint8_t>(value & 0x0FU);

        if (st_.latched_is_volume != 0U) {
            st_.volume[st_.latched_channel] = data;
        } else if (st_.latched_channel < 3U) {
            st_.tone_period[st_.latched_channel] = static_cast<uint16_t>(
                (st_.tone_period[st_.latched_channel] & 0x3F0U) | data);
        } else {
            st_.noise_control = static_cast<uint8_t>(data & 0x07U);
            st_.lfsr = 0x8000U;  // Writing noise register resets 16-bit LFSR
        }
    } else {
        // Data-only byte: 0 - DDDDDD
        const uint8_t data = static_cast<uint8_t>(value & 0x3FU);
        if (st_.latched_is_volume != 0U) {
            st_.volume[st_.latched_channel] = static_cast<uint8_t>(data & 0x0FU);
        } else if (st_.latched_channel < 3U) {
            st_.tone_period[st_.latched_channel] = static_cast<uint16_t>(
                (st_.tone_period[st_.latched_channel] & 0x00FU) |
                (static_cast<uint16_t>(data) << 4U));
        } else {
            st_.noise_control = static_cast<uint8_t>(data & 0x07U);
            st_.lfsr = 0x8000U;
        }
    }
}

void Psg::tick_psg() noexcept {
    // Tick 3 square-wave tone channels
    for (int ch = 0; ch < 3; ++ch) {
        if (st_.tone_counter[ch] > 0U) {
            --st_.tone_counter[ch];
        }
        if (st_.tone_counter[ch] == 0U) {
            st_.tone_counter[ch] = st_.tone_period[ch];
            if (st_.tone_period[ch] <= 1U) {
                st_.tone_polarity[ch] = 1;  // Period 0/1 outputs flat high DC (used for PCM)
            } else {
                st_.tone_polarity[ch] = static_cast<int8_t>(-st_.tone_polarity[ch]);
            }
        }
    }

    // Tick noise channel (16-bit LFSR, Sega tap mask 0x0009)
    if (st_.noise_counter > 0U) {
        --st_.noise_counter;
    }
    if (st_.noise_counter == 0U) {
        const uint8_t rate = static_cast<uint8_t>(st_.noise_control & 0x03U);
        if (rate == 0U) {
            st_.noise_counter = 0x10U;
        } else if (rate == 1U) {
            st_.noise_counter = 0x20U;
        } else if (rate == 2U) {
            st_.noise_counter = 0x40U;
        } else {
            st_.noise_counter = st_.tone_period[2];
        }

        st_.noise_polarity = static_cast<int8_t>(-st_.noise_polarity);
        if (st_.noise_polarity > 0) {
            const bool white_noise = (st_.noise_control & 0x04U) != 0U;
            const uint16_t feedback =
                white_noise ? parity16(static_cast<uint16_t>(st_.lfsr & 0x0009U))
                            : static_cast<uint16_t>(st_.lfsr & 0x0001U);
            st_.lfsr = static_cast<uint16_t>((st_.lfsr >> 1U) | (feedback << 15U));
        }
    }
}

void Psg::emit_sample() noexcept {
    int32_t left = 0;
    int32_t right = 0;

    for (int ch = 0; ch < 3; ++ch) {
        const int32_t s = static_cast<int32_t>(st_.tone_polarity[ch]) *
                          static_cast<int32_t>(kVolumeTable[st_.volume[ch] & 0x0FU]);
        if ((st_.gg_stereo & (0x10U << ch)) != 0U) left += s;
        if ((st_.gg_stereo & (0x01U << ch)) != 0U) right += s;
    }

    const int32_t noise_out = ((st_.lfsr & 1U) != 0U ? 1 : -1) *
                              static_cast<int32_t>(kVolumeTable[st_.volume[3] & 0x0FU]);
    if ((st_.gg_stereo & 0x80U) != 0U) left += noise_out;
    if ((st_.gg_stereo & 0x08U) != 0U) right += noise_out;

    const auto clamp16 = [](int32_t v) noexcept -> int16_t {
        return static_cast<int16_t>(std::clamp(v, -32768, 32767));
    };

    if (ring_count_ >= kRingSamples) {
        // Drop oldest sample on overflow (lock-free, zero allocations)
        ring_read_ = (ring_read_ + 1U) % kRingSamples;
        --ring_count_;
    }

    ring_[ring_write_ * 2U] = clamp16(left);
    ring_[ring_write_ * 2U + 1U] = clamp16(right);
    ring_write_ = (ring_write_ + 1U) % kRingSamples;
    ++ring_count_;
}

void Psg::step(uint32_t cpu_cycles) noexcept {
    // Interleave PSG clock ticks (every 16 T-cycles) with 48 kHz sample emission
    // so multi-cycle step() batches preserve accurate waveform transitions.
    uint32_t remaining = cpu_cycles;
    while (remaining > 0U) {
        const uint32_t to_tick = 16U - div16_accum_;
        const uint32_t chunk = (remaining < to_tick) ? remaining : to_tick;
        remaining -= chunk;
        div16_accum_ += chunk;
        if (div16_accum_ >= 16U) {
            div16_accum_ -= 16U;
            tick_psg();
        }
        sample_accum_ += static_cast<uint64_t>(chunk) * kSampleRate;
        while (sample_accum_ >= kCpuClockHz) {
            sample_accum_ -= kCpuClockHz;
            emit_sample();
        }
    }
}

size_t Psg::drain_samples(int16_t* dst, size_t max_frames) noexcept {
    const size_t n = std::min(max_frames, ring_count_);
    for (size_t i = 0; i < n; ++i) {
        dst[i * 2U] = ring_[ring_read_ * 2U];
        dst[i * 2U + 1U] = ring_[ring_read_ * 2U + 1U];
        ring_read_ = (ring_read_ + 1U) % kRingSamples;
    }
    ring_count_ -= n;
    return n;
}

size_t Psg::available_frames() const noexcept {
    return ring_count_;
}

}  // namespace dragon::sms
