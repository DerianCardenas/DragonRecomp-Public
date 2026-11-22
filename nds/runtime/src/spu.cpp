#include "nds/spu.h"

#include <algorithm>

#include "nds/bus.h"

namespace dragon::nds {

namespace {

constexpr int8_t kImaIndexTable[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

constexpr int16_t kImaStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

constexpr uint64_t kArm7ClockHz = 33513982ULL;

}  // namespace

void Spu::reset() noexcept {
    ch_.fill(Channel{});
    soundcnt_ = 0;
    soundbias_ = 0x0200U;
    sample_acc_ = 0;
    ring_.fill(0);
    ring_read_ = 0;
    ring_write_ = 0;
    ring_count_ = 0;
}

uint32_t Spu::read32(uint32_t offset) const noexcept {
    if (offset >= 0x100U) {
        return 0U;
    }
    const int idx = static_cast<int>(offset >> 4U);
    const uint32_t reg = offset & 0x0CU;
    if (reg == 0x00U) {
        uint32_t v = ch_[idx].cnt & ~(1U << 31);
        if (ch_[idx].active) {
            v |= (1U << 31);
        }
        return v;
    }
    return 0U;
}

void Spu::write32(uint32_t offset, uint32_t val, uint32_t mask, const Bus& bus) noexcept {
    if (offset >= 0x100U) {
        return;
    }
    const int idx = static_cast<int>(offset >> 4U);
    const uint32_t reg = offset & 0x0CU;
    auto& c = ch_[idx];
    switch (reg) {
        case 0x00U: {
            const bool was_active = c.active;
            c.cnt = (c.cnt & ~mask) | (val & mask);
            if ((mask & (1U << 31)) != 0U) {
                const bool want_active = (c.cnt & (1U << 31)) != 0U;
                if (!was_active && want_active) {
                    start_channel(idx, bus);
                } else if (!want_active) {
                    c.active = false;
                }
            }
            break;
        }
        case 0x04U:
            c.sad = ((c.sad & ~mask) | (val & mask)) & 0x07FFFFFFU;
            break;
        case 0x08U: {
            const uint32_t cur =
                static_cast<uint32_t>(c.tmr) | (static_cast<uint32_t>(c.pnt) << 16U);
            const uint32_t next = (cur & ~mask) | (val & mask);
            c.tmr = static_cast<uint16_t>(next & 0xFFFFU);
            c.pnt = static_cast<uint16_t>(next >> 16U);
            break;
        }
        case 0x0CU:
            c.len = ((c.len & ~mask) | (val & mask)) & 0x003FFFFFU;
            break;
        default:
            break;
    }
}

void Spu::start_channel(int idx, const Bus& bus) noexcept {
    auto& c = ch_[idx];
    c.active = true;
    c.cur_addr = c.sad;
    c.byte_pos = 0;
    c.timer_acc = 0;
    c.cur_sample = 0;
    c.psg_phase = 0;
    c.lfsr = 0x7FFFU;
    c.adpcm_high_nibble = false;
    c.adpcm_header_read = false;

    const uint32_t fmt = (c.cnt >> 29U) & 0x03U;
    if (fmt == 2U) {
        // Read 4-byte IMA-ADPCM header
        const uint16_t init_val =
            static_cast<uint16_t>(bus.spu_read8(c.cur_addr) |
                                  (static_cast<uint16_t>(bus.spu_read8(c.cur_addr + 1U)) << 8U));
        c.adpcm_val = static_cast<int16_t>(init_val);
        c.adpcm_idx = std::clamp<int32_t>(bus.spu_read8(c.cur_addr + 2U) & 0x7FU, 0, 88);
        c.adpcm_loop_val = c.adpcm_val;
        c.adpcm_loop_idx = c.adpcm_idx;
        c.cur_addr += 4U;
        c.byte_pos = 4U;
        c.adpcm_header_read = true;
        c.cur_sample = static_cast<int16_t>(c.adpcm_val);
    }
}

void Spu::step_channel(int idx, uint32_t cycles, const Bus& bus) noexcept {
    auto& c = ch_[idx];
    if (!c.active) {
        return;
    }
    // Timer ticks at 16.757 MHz = arm7_cycles / 2
    const uint32_t period = static_cast<uint32_t>(0x10000U - c.tmr) * 2U;
    if (period == 0U) {
        return;
    }
    c.timer_acc += cycles;
    const uint32_t fmt = (c.cnt >> 29U) & 0x03U;
    const uint32_t repeat_mode = (c.cnt >> 27U) & 0x03U;
    const uint32_t total_bytes = (static_cast<uint32_t>(c.pnt) + c.len) * 4U;
    const uint32_t loop_start_bytes = static_cast<uint32_t>(c.pnt) * 4U;

    while (c.timer_acc >= period) {
        c.timer_acc -= period;

        if (fmt == 3U) {
            if (idx >= 8 && idx <= 13) {
                const uint8_t duty = static_cast<uint8_t>((c.cnt >> 24U) & 0x07U);
                c.psg_phase = (c.psg_phase + 1U) & 7U;
                c.cur_sample = (duty == 7U) ? 0 : ((c.psg_phase <= duty) ? 0x4000 : -0x4000);
            } else if (idx >= 14 && idx <= 15) {
                const uint16_t carry = c.lfsr & 1U;
                c.lfsr >>= 1U;
                if (carry) {
                    c.lfsr ^= 0x6000U;
                    c.cur_sample = -0x4000;
                } else {
                    c.cur_sample = 0x4000;
                }
            }
            continue;
        }

        if (total_bytes == 0U) {
            c.active = false;
            c.cnt &= ~(1U << 31);
            return;
        }

        if (fmt == 0U) {
            // PCM8
            const int8_t s = static_cast<int8_t>(bus.spu_read8(c.sad + c.byte_pos));
            c.cur_sample = static_cast<int16_t>(s << 8);
            ++c.byte_pos;
        } else if (fmt == 1U) {
            // PCM16
            const uint8_t lo = bus.spu_read8(c.sad + c.byte_pos);
            const uint8_t hi = bus.spu_read8(c.sad + c.byte_pos + 1U);
            c.cur_sample = static_cast<int16_t>(lo | (static_cast<uint16_t>(hi) << 8U));
            c.byte_pos += 2U;
        } else {
            // IMA-ADPCM
            if (c.byte_pos == loop_start_bytes && !c.adpcm_high_nibble) {
                c.adpcm_loop_val = c.adpcm_val;
                c.adpcm_loop_idx = c.adpcm_idx;
            }
            const uint8_t raw = bus.spu_read8(c.sad + c.byte_pos);
            const uint8_t nib = c.adpcm_high_nibble ? (raw >> 4U) : (raw & 0x0FU);
            c.adpcm_high_nibble = !c.adpcm_high_nibble;
            if (!c.adpcm_high_nibble) {
                ++c.byte_pos;
            }
            const int32_t step = kImaStepTable[c.adpcm_idx];
            int32_t diff = step >> 3;
            if (nib & 1U)
                diff += step >> 2;
            if (nib & 2U)
                diff += step >> 1;
            if (nib & 4U)
                diff += step;
            if (nib & 8U)
                c.adpcm_val = std::max(-0x7FFF, c.adpcm_val - diff);
            else
                c.adpcm_val = std::min(0x7FFF, c.adpcm_val + diff);
            c.adpcm_idx = std::clamp(c.adpcm_idx + kImaIndexTable[nib & 7U], 0, 88);
            c.cur_sample = static_cast<int16_t>(c.adpcm_val);
        }

        if (c.byte_pos >= total_bytes) {
            if (repeat_mode == 1U || repeat_mode == 0U) {
                if (fmt == 2U) {
                    c.byte_pos = std::max<uint32_t>(4U, loop_start_bytes);
                    c.adpcm_val = c.adpcm_loop_val;
                    c.adpcm_idx = c.adpcm_loop_idx;
                    c.adpcm_high_nibble = false;
                } else {
                    c.byte_pos = loop_start_bytes;
                }
            } else {
                c.active = false;
                c.cnt &= ~(1U << 31);
                c.cur_sample = 0;
                return;
            }
        }
    }
}

void Spu::mix_sample() noexcept {
    if ((soundcnt_ & (1U << 15)) == 0U) {
        if (ring_count_ < kRingFrames) {
            ring_[ring_write_ * 2U + 0U] = 0;
            ring_[ring_write_ * 2U + 1U] = 0;
            ring_write_ = (ring_write_ + 1U) % kRingFrames;
            ++ring_count_;
        }
        return;
    }

    int64_t left = 0;
    int64_t right = 0;
    static constexpr int kShift[4] = {0, 1, 2, 4};

    for (int i = 0; i < 16; ++i) {
        const auto& c = ch_[i];
        if (!c.active)
            continue;
        int32_t vol = static_cast<int32_t>(c.cnt & 0x7FU);
        if (vol == 127)
            vol = 128;
        const int shift = kShift[(c.cnt >> 8U) & 0x03U];
        int32_t pan = static_cast<int32_t>((c.cnt >> 16U) & 0x7FU);
        if (pan == 127)
            pan = 128;

        const int64_t s = (static_cast<int64_t>(c.cur_sample) * vol) >> (7 + shift);
        left += (s * (128 - pan)) >> 7;
        right += (s * pan) >> 7;
    }

    int32_t master = static_cast<int32_t>(soundcnt_ & 0x7FU);
    if (master == 127)
        master = 128;
    left = (left * master) >> 9;
    right = (right * master) >> 9;

    const int16_t out_l = static_cast<int16_t>(std::clamp<int64_t>(left, -32768, 32767));
    const int16_t out_r = static_cast<int16_t>(std::clamp<int64_t>(right, -32768, 32767));

    if (ring_count_ < kRingFrames) {
        ring_[ring_write_ * 2U + 0U] = out_l;
        ring_[ring_write_ * 2U + 1U] = out_r;
        ring_write_ = (ring_write_ + 1U) % kRingFrames;
        ++ring_count_;
    }
}

void Spu::step(uint32_t arm7_cycles, const Bus& bus) noexcept {
    // Advance channels per 48 kHz output sample (~698 ARM7 cycles/sample) to avoid 15.7 kHz
    // aliasing
    constexpr uint32_t kCyclesPerSample = static_cast<uint32_t>(kArm7ClockHz / kSampleRate);
    uint32_t rem = arm7_cycles;
    while (rem > 0U) {
        const uint32_t chunk = std::min(rem, kCyclesPerSample);
        for (int i = 0; i < 16; ++i) {
            if (ch_[i].active) {
                step_channel(i, chunk, bus);
            }
        }
        rem -= chunk;
        sample_acc_ += static_cast<uint64_t>(chunk) * kSampleRate;
        while (sample_acc_ >= kArm7ClockHz) {
            sample_acc_ -= kArm7ClockHz;
            mix_sample();
        }
    }
}

size_t Spu::drain_samples(int16_t* dst, size_t max_frames) noexcept {
    const size_t n = std::min(max_frames, ring_count_);
    for (size_t i = 0; i < n; ++i) {
        dst[i * 2U + 0U] = ring_[ring_read_ * 2U + 0U];
        dst[i * 2U + 1U] = ring_[ring_read_ * 2U + 1U];
        ring_read_ = (ring_read_ + 1U) % kRingFrames;
    }
    ring_count_ -= n;
    return n;
}

}  // namespace dragon::nds
