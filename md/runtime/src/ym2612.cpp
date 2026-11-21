#include "md/ym2612.h"

#include <algorithm>
#include <cmath>

namespace dragon::md {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Operator slot mapping from register offset (0..3 -> OP1, OP3, OP2, OP4)
constexpr uint8_t kSlotMap[4] = {0, 2, 1, 3};

// Detune table (YM2612 hardware values, indexed by [dt & 3][keycode])
constexpr uint8_t kDetuneTable[4][32] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2,
     2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8},
    {1, 1, 1, 1, 2, 2, 2, 2,  2,  3,  3,  3,  4,  4,  4,  5,
     5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16},
    {2, 2, 2, 2,  2,  3,  3,  3,  4,  4,  4,  5,  5,  6,  6,  7,
     8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22},
};

// Precomputed 1024-entry sine table (-8191..+8191) and 1024-entry attenuation table (0..1023 ->
// 0..1)
struct FmTables {
    std::array<int16_t, 1024> sine{};
    std::array<uint16_t, 1024> atten{};  // Fixed-point 0..4096

    FmTables() noexcept {
        for (size_t i = 0; i < 1024; ++i) {
            const double angle = (static_cast<double>(i) * 2.0 * kPi) / 1024.0;
            sine[i] = static_cast<int16_t>(std::sin(angle) * 8191.0);
        }
        for (size_t i = 0; i < 1024; ++i) {
            // YM2612 envelope attenuation: 0.09375 dB per step (96 dB across 1024 steps)
            const double db = static_cast<double>(i) * (96.0 / 1024.0);
            const double gain = std::pow(10.0, -db / 20.0);
            atten[i] = static_cast<uint16_t>(std::clamp(gain * 4096.0, 0.0, 4096.0));
        }
    }
};

const FmTables& fm_tables() noexcept {
    static const FmTables kTables;
    return kTables;
}

}  // namespace

Ym2612::Ym2612() noexcept {
    reset();
}

void Ym2612::reset() noexcept {
    ch_ = {};
    for (auto& c : ch_) {
        c.pan_l = 1;
        c.pan_r = 1;
        for (auto& o : c.op) {
            o.tl = 127;
            o.env_level = 0x3FFU;
            o.env_phase = EnvPhase::Off;
        }
    }
    ch3_fnum_[0] = ch3_fnum_[1] = ch3_fnum_[2] = 0;
    ch3_block_[0] = ch3_block_[1] = ch3_block_[2] = 0;
    ch3_latch_[0] = ch3_latch_[1] = ch3_latch_[2] = 0;
    ch3_mode_ = 0;
    addr_latch_[0] = addr_latch_[1] = 0;
    lfo_enable_ = false;
    lfo_freq_ = 0;
    lfo_cnt_ = 0;
    lfo_step_ = 0;
    timer_a_latch_ = 0;
    timer_a_cnt_ = 0;
    timer_b_latch_ = 0;
    timer_b_cnt_ = 0;
    timer_b_sub_ = 0;
    timer_ctrl_ = 0;
    status_ = 0;
    dac_enable_ = false;
    dac_data_ = 0x80U;
    fm_cycle_accum_ = 0;
    eg_counter_ = 0;
    eg_div_ = 0;
    sample_accum_ = 0;
    ring_.fill(0);
    ring_read_ = ring_write_ = ring_count_ = 0;
}

uint8_t Ym2612::read_status() const noexcept {
    // Bit 7 = Busy (0 = ready), Bit 1 = Timer B overflow, Bit 0 = Timer A overflow
    return status_ & 0x03U;
}

void Ym2612::write(uint8_t port, uint8_t value) noexcept {
    switch (port & 0x03U) {
        case 0:
            addr_latch_[0] = value;
            break;
        case 1:
            write_register(0, addr_latch_[0], value);
            break;
        case 2:
            addr_latch_[1] = value;
            break;
        case 3:
            write_register(1, addr_latch_[1], value);
            break;
    }
}

void Ym2612::write_register(uint8_t bank, uint8_t reg, uint8_t value) noexcept {
    if (bank == 0 && reg < 0x30U) {
        switch (reg) {
            case 0x22U:
                lfo_enable_ = (value & 0x08U) != 0U;
                lfo_freq_ = value & 0x07U;
                break;
            case 0x24U:
                timer_a_latch_ = static_cast<uint16_t>((timer_a_latch_ & 0x0003U) |
                                                       (static_cast<uint16_t>(value) << 2U));
                break;
            case 0x25U:
                timer_a_latch_ =
                    static_cast<uint16_t>((timer_a_latch_ & 0x03FCU) | (value & 0x03U));
                break;
            case 0x26U:
                timer_b_latch_ = value;
                break;
            case 0x27U: {
                ch3_mode_ = (value >> 6U) & 0x03U;
                if ((value & 0x01U) && !(timer_ctrl_ & 0x01U)) {
                    timer_a_cnt_ = timer_a_latch_;
                }
                if ((value & 0x02U) && !(timer_ctrl_ & 0x02U)) {
                    timer_b_cnt_ = timer_b_latch_;
                    timer_b_sub_ = 0;
                }
                if (value & 0x10U)
                    status_ &= ~0x01U;
                if (value & 0x20U)
                    status_ &= ~0x02U;
                timer_ctrl_ = value;
                break;
            }
            case 0x28U: {
                const uint8_t ch_bits = value & 0x07U;
                if ((ch_bits & 0x03U) == 3U)
                    return;
                const uint8_t ch_idx =
                    (ch_bits & 0x04U) ? (3U + (ch_bits & 0x03U)) : (ch_bits & 0x03U);
                if (ch_idx >= 6U)
                    return;
                for (uint8_t s = 0; s < 4U; ++s) {
                    auto& op = ch_[ch_idx].op[s];
                    const bool on = (value & (0x10U << s)) != 0U;
                    if (on && !op.key_on) {
                        op.key_on = true;
                        op.phase_counter = 0;
                        op.env_phase = EnvPhase::Attack;
                        if (op.ar >= 31U) {
                            op.env_level = 0;
                            op.env_phase = EnvPhase::Decay;
                        }
                    } else if (!on && op.key_on) {
                        op.key_on = false;
                        op.env_phase = EnvPhase::Release;
                    }
                }
                break;
            }
            case 0x2AU:
                dac_data_ = value;
                break;
            case 0x2BU:
                dac_enable_ = (value & 0x80U) != 0U;
                break;
            default:
                break;
        }
        return;
    }

    const uint8_t ch_off = reg & 0x03U;
    if (ch_off == 3U)
        return;
    const uint8_t ch_idx = static_cast<uint8_t>(bank * 3U + ch_off);

    if (reg >= 0x30U && reg < 0xA0U) {
        const uint8_t slot = kSlotMap[(reg >> 2U) & 0x03U];
        auto& op = ch_[ch_idx].op[slot];
        switch (reg & 0xF0U) {
            case 0x30U:
                op.dt = (value >> 4U) & 0x07U;
                op.mul = value & 0x0FU;
                break;
            case 0x40U:
                op.tl = value & 0x7FU;
                break;
            case 0x50U:
                op.rs = (value >> 6U) & 0x03U;
                op.ar = value & 0x1FU;
                break;
            case 0x60U:
                op.am = (value >> 7U) & 0x01U;
                op.d1r = value & 0x1FU;
                break;
            case 0x70U:
                op.d2r = value & 0x1FU;
                break;
            case 0x80U:
                op.sl = (value >> 4U) & 0x0FU;
                op.rr = value & 0x0FU;
                break;
            case 0x90U:
                op.ssg_eg = value & 0x0FU;
                break;
        }
        return;
    }

    if (reg >= 0xA0U && reg <= 0xA2U) {
        auto& c = ch_[ch_idx];
        c.block = (c.fnum_latch >> 3U) & 0x07U;
        c.fnum = static_cast<uint16_t>((static_cast<uint16_t>(c.fnum_latch & 0x07U) << 8U) | value);
    } else if (reg >= 0xA4U && reg <= 0xA6U) {
        ch_[ch_idx].fnum_latch = value;
    } else if (bank == 0 && reg >= 0xA8U && reg <= 0xAAU) {
        const uint8_t idx = reg - 0xA8U;
        ch3_block_[idx] = (ch3_latch_[idx] >> 3U) & 0x07U;
        ch3_fnum_[idx] =
            static_cast<uint16_t>((static_cast<uint16_t>(ch3_latch_[idx] & 0x07U) << 8U) | value);
    } else if (bank == 0 && reg >= 0xACU && reg <= 0xAEU) {
        ch3_latch_[reg - 0xACU] = value;
    } else if (reg >= 0xB0U && reg <= 0xB2U) {
        ch_[ch_idx].alg = value & 0x07U;
        ch_[ch_idx].fb = (value >> 3U) & 0x07U;
    } else if (reg >= 0xB4U && reg <= 0xB6U) {
        ch_[ch_idx].pan_l = (value & 0x80U) ? 1U : 0U;
        ch_[ch_idx].pan_r = (value & 0x40U) ? 1U : 0U;
        ch_[ch_idx].ams = (value >> 4U) & 0x03U;
        ch_[ch_idx].pms = value & 0x07U;
    }
}

void Ym2612::tick_timers() noexcept {
    if (timer_ctrl_ & 0x01U) {
        ++timer_a_cnt_;
        if (timer_a_cnt_ >= 1024U) {
            timer_a_cnt_ = timer_a_latch_;
            if (timer_ctrl_ & 0x04U) {
                status_ |= 0x01U;
            }
        }
    }
    if (timer_ctrl_ & 0x02U) {
        ++timer_b_sub_;
        if (timer_b_sub_ >= 16U) {
            timer_b_sub_ = 0;
            ++timer_b_cnt_;
            if (timer_b_cnt_ == 0U) {
                timer_b_cnt_ = timer_b_latch_;
                if (timer_ctrl_ & 0x08U) {
                    status_ |= 0x02U;
                }
            }
        }
    }
}

void Ym2612::step_envelope(Operator& op, uint8_t keycode) noexcept {
    uint8_t rate_in = 0;
    switch (op.env_phase) {
        case EnvPhase::Attack:
            rate_in = op.ar;
            break;
        case EnvPhase::Decay:
            rate_in = op.d1r;
            break;
        case EnvPhase::Sustain:
            rate_in = op.d2r;
            break;
        case EnvPhase::Release:
            rate_in = static_cast<uint8_t>(op.rr * 2U + 1U);
            break;
        case EnvPhase::Off:
            op.env_level = 0x3FFU;
            return;
    }

    if (rate_in == 0U) {
        return;
    }
    const uint8_t ks = keycode >> (3U - op.rs);
    const uint8_t rate = static_cast<uint8_t>(std::min<uint32_t>(63U, rate_in * 2U + ks));
    const uint32_t shift = (rate < 48U) ? (11U - (rate >> 2U)) : 0U;
    if (shift > 0U && (eg_counter_ & ((1U << shift) - 1U)) != 0U) {
        return;
    }
    const uint32_t inc = (rate >= 60U) ? 8U : (rate >= 52U ? 4U : (rate >= 48U ? 2U : 1U));

    if (op.env_phase == EnvPhase::Attack) {
        if (rate >= 62U) {
            op.env_level = 0;
        } else {
            const uint32_t step_val = ((~op.env_level & 0x3FFU) * inc) >> 4U;
            op.env_level = static_cast<uint16_t>(
                (op.env_level > step_val + 1U) ? (op.env_level - step_val - 1U) : 0U);
        }
        if (op.env_level == 0U) {
            op.env_phase = EnvPhase::Decay;
        }
    } else {
        const uint32_t next = static_cast<uint32_t>(op.env_level) + inc;
        if (next >= 0x3FFU) {
            op.env_level = 0x3FFU;
            if (op.env_phase == EnvPhase::Release) {
                op.env_phase = EnvPhase::Off;
            }
        } else {
            op.env_level = static_cast<uint16_t>(next);
            if (op.env_phase == EnvPhase::Decay) {
                const uint16_t sl_level =
                    (op.sl == 15U) ? 0x3E0U : static_cast<uint16_t>(op.sl << 5U);
                if (op.env_level >= sl_level) {
                    op.env_phase = EnvPhase::Sustain;
                }
            }
        }
    }
}

int32_t Ym2612::compute_operator(Operator& op, uint16_t fnum, uint8_t block, int32_t mod,
                                 bool use_fb, uint8_t fb_level) noexcept {
    const uint8_t keycode = static_cast<uint8_t>((block << 2U) | ((fnum >> 9U) & 0x03U));
    // Phase increment (20-bit phase accumulator, top 10 bits index 1024-entry sine LUT)
    int32_t base_inc = static_cast<int32_t>((static_cast<uint32_t>(fnum) << block) >> 1U);
    const int32_t dt_mag = kDetuneTable[op.dt & 0x03U][keycode & 0x1FU];
    base_inc += (op.dt & 0x04U) ? -dt_mag : dt_mag;
    if (base_inc < 0)
        base_inc = 0;

    uint32_t phase_inc = static_cast<uint32_t>(base_inc);
    if (op.mul == 0U) {
        phase_inc >>= 1U;
    } else {
        phase_inc *= op.mul;
    }
    op.phase_counter = (op.phase_counter + phase_inc) & 0xFFFFFU;

    if (op.env_level >= 0x3E0U) {
        op.prev_out[1] = op.prev_out[0];
        op.prev_out[0] = 0;
        return 0;
    }

    int32_t phase_mod = mod;
    if (use_fb && fb_level > 0U) {
        phase_mod = (op.prev_out[0] + op.prev_out[1]) >> (10U - fb_level);
    }

    const uint32_t phase_idx =
        ((op.phase_counter >> 10U) + static_cast<uint32_t>(phase_mod)) & 0x3FFU;
    const uint32_t total_atten = std::min<uint32_t>(
        1023U, static_cast<uint32_t>(op.env_level) + (static_cast<uint32_t>(op.tl) << 3U));

    const auto& tbl = fm_tables();
    const int32_t raw = (static_cast<int32_t>(tbl.sine[phase_idx]) *
                         static_cast<int32_t>(tbl.atten[total_atten])) >>
                        12;

    op.prev_out[1] = op.prev_out[0];
    op.prev_out[0] = raw;
    return raw;
}

void Ym2612::tick_fm() noexcept {
    tick_timers();

    ++eg_div_;
    if (eg_div_ >= 3U) {
        eg_div_ = 0;
        ++eg_counter_;
        for (size_t c = 0; c < 6U; ++c) {
            for (size_t s = 0; s < 4U; ++s) {
                uint16_t fn = ch_[c].fnum;
                uint8_t bl = ch_[c].block;
                if (c == 2U && ch3_mode_ != 0U && s < 3U) {
                    fn = ch3_fnum_[s];
                    bl = ch3_block_[s];
                }
                const uint8_t kc = static_cast<uint8_t>((bl << 2U) | ((fn >> 9U) & 0x03U));
                step_envelope(ch_[c].op[s], kc);
            }
        }
    }
}

void Ym2612::generate_sample(int32_t& out_left, int32_t& out_right) noexcept {
    int32_t mix_l = 0;
    int32_t mix_r = 0;

    for (size_t c = 0; c < 6U; ++c) {
        auto& ch = ch_[c];
        int32_t ch_out = 0;

        if (c == 5U && dac_enable_) {
            ch_out = (static_cast<int32_t>(dac_data_) - 128) * 64;
        } else {
            uint16_t f0 = ch.fnum, f1 = ch.fnum, f2 = ch.fnum, f3 = ch.fnum;
            uint8_t b0 = ch.block, b1 = ch.block, b2 = ch.block, b3 = ch.block;
            if (c == 2U && ch3_mode_ != 0U) {
                f0 = ch3_fnum_[0];
                b0 = ch3_block_[0];
                f1 = ch3_fnum_[1];
                b1 = ch3_block_[1];
                f2 = ch3_fnum_[2];
                b2 = ch3_block_[2];
            }

            const int32_t m1 = compute_operator(ch.op[0], f0, b0, 0, true, ch.fb);
            int32_t m2 = 0;
            int32_t c1 = 0;
            int32_t c2 = 0;

            switch (ch.alg & 0x07U) {
                case 0:
                    m2 = compute_operator(ch.op[1], f1, b1, m1 >> 4, false, 0);
                    c1 = compute_operator(ch.op[2], f2, b2, m2 >> 4, false, 0);
                    c2 = compute_operator(ch.op[3], f3, b3, c1 >> 4, false, 0);
                    ch_out = c2;
                    break;
                case 1:
                    m2 = compute_operator(ch.op[1], f1, b1, 0, false, 0);
                    c1 = compute_operator(ch.op[2], f2, b2, (m1 + m2) >> 4, false, 0);
                    c2 = compute_operator(ch.op[3], f3, b3, c1 >> 4, false, 0);
                    ch_out = c2;
                    break;
                case 2:
                    m2 = compute_operator(ch.op[1], f1, b1, 0, false, 0);
                    c1 = compute_operator(ch.op[2], f2, b2, m2 >> 4, false, 0);
                    c2 = compute_operator(ch.op[3], f3, b3, (m1 + c1) >> 4, false, 0);
                    ch_out = c2;
                    break;
                case 3:
                    m2 = compute_operator(ch.op[1], f1, b1, m1 >> 4, false, 0);
                    c1 = compute_operator(ch.op[2], f2, b2, 0, false, 0);
                    c2 = compute_operator(ch.op[3], f3, b3, (m2 + c1) >> 4, false, 0);
                    ch_out = c2;
                    break;
                case 4:
                    m2 = compute_operator(ch.op[1], f1, b1, m1 >> 4, false, 0);
                    c1 = compute_operator(ch.op[2], f2, b2, 0, false, 0);
                    c2 = compute_operator(ch.op[3], f3, b3, c1 >> 4, false, 0);
                    ch_out = (m2 + c2) >> 1;
                    break;
                case 5:
                    m2 = compute_operator(ch.op[1], f1, b1, m1 >> 4, false, 0);
                    c1 = compute_operator(ch.op[2], f2, b2, m1 >> 4, false, 0);
                    c2 = compute_operator(ch.op[3], f3, b3, m1 >> 4, false, 0);
                    ch_out = (m2 + c1 + c2) / 3;
                    break;
                case 6:
                    m2 = compute_operator(ch.op[1], f1, b1, m1 >> 4, false, 0);
                    c1 = compute_operator(ch.op[2], f2, b2, 0, false, 0);
                    c2 = compute_operator(ch.op[3], f3, b3, 0, false, 0);
                    ch_out = (m2 + c1 + c2) / 3;
                    break;
                case 7:
                default:
                    m2 = compute_operator(ch.op[1], f1, b1, 0, false, 0);
                    c1 = compute_operator(ch.op[2], f2, b2, 0, false, 0);
                    c2 = compute_operator(ch.op[3], f3, b3, 0, false, 0);
                    ch_out = (m1 + m2 + c1 + c2) >> 2;
                    break;
            }
        }

        if (ch.pan_l)
            mix_l += ch_out;
        if (ch.pan_r)
            mix_r += ch_out;
    }

    out_left = std::clamp(mix_l >> 1, -32768, 32767);
    out_right = std::clamp(mix_r >> 1, -32768, 32767);
}

void Ym2612::step(uint32_t m68k_cycles) noexcept {
    // YM2612 internal sample tick = M68K_CLOCK / 144 (~53.267 kHz)
    fm_cycle_accum_ += m68k_cycles;
    while (fm_cycle_accum_ >= 144U) {
        fm_cycle_accum_ -= 144U;
        tick_fm();
    }

    sample_accum_ += static_cast<uint64_t>(m68k_cycles) * kSampleRate;
    while (sample_accum_ >= kClockHz) {
        sample_accum_ -= kClockHz;
        int32_t l = 0, r = 0;
        generate_sample(l, r);
        if (ring_count_ < kRingSamples) {
            ring_[ring_write_ * 2U + 0U] = static_cast<int16_t>(l);
            ring_[ring_write_ * 2U + 1U] = static_cast<int16_t>(r);
            ring_write_ = (ring_write_ + 1U) % kRingSamples;
            ++ring_count_;
        }
    }
}

size_t Ym2612::drain_samples(int16_t* dst, size_t max_frames) noexcept {
    const size_t n = std::min(max_frames, ring_count_);
    for (size_t i = 0; i < n; ++i) {
        dst[i * 2U + 0U] = ring_[ring_read_ * 2U + 0U];
        dst[i * 2U + 1U] = ring_[ring_read_ * 2U + 1U];
        ring_read_ = (ring_read_ + 1U) % kRingSamples;
    }
    ring_count_ -= n;
    return n;
}

}  // namespace dragon::md
