#include "nes/apu.h"

#include <algorithm>

namespace dragon::nes {

namespace {

// Reference: https://www.nesdev.org/wiki/APU_Length_Counter
constexpr std::array<uint8_t, 32> kLengthTable = {
    10U, 254U, 20U, 2U,  40U, 4U,  80U, 6U,  160U, 8U,  60U, 10U, 14U, 12U, 26U, 14U,
    12U, 16U,  24U, 18U, 48U, 20U, 96U, 22U, 192U, 24U, 72U, 26U, 16U, 28U, 32U, 30U,
};

// Reference: https://www.nesdev.org/wiki/APU_Pulse
constexpr std::array<std::array<uint8_t, 8>, 4> kPulseDutyTable = {{
    {{0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U}},  // 12.5%
    {{0U, 1U, 1U, 0U, 0U, 0U, 0U, 0U}},  // 25%
    {{0U, 1U, 1U, 1U, 1U, 0U, 0U, 0U}},  // 50%
    {{1U, 0U, 0U, 1U, 1U, 1U, 1U, 1U}},  // 25% negated (75%)
}};

// Reference: https://www.nesdev.org/wiki/APU_Triangle
constexpr std::array<uint8_t, 32> kTriangleSequence = {
    15U, 14U, 13U, 12U, 11U, 10U, 9U, 8U, 7U, 6U, 5U,  4U,  3U,  2U,  1U,  0U,
    0U,  1U,  2U,  3U,  4U,  5U,  6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U,
};

// NTSC noise timer periods in CPU cycles (https://www.nesdev.org/wiki/APU_Noise)
constexpr std::array<uint16_t, 16> kNoisePeriodTable = {
    4U, 8U, 16U, 32U, 64U, 96U, 128U, 160U, 202U, 254U, 380U, 508U, 762U, 1016U, 2034U, 4068U,
};

// NTSC DMC rate periods in CPU cycles (https://www.nesdev.org/wiki/APU_DMC)
constexpr std::array<uint16_t, 16> kDmcRateTable = {
    428U, 380U, 340U, 320U, 286U, 254U, 226U, 214U, 190U, 160U, 142U, 128U, 106U, 84U, 72U, 54U,
};

}  // namespace

void Apu::Envelope::clock() noexcept {
    if (!start_flag) {
        if (divider == 0U) {
            divider = volume_or_period;
            if (decay_level > 0U) {
                --decay_level;
            } else if (loop_flag) {
                decay_level = 15U;
            }
        } else {
            --divider;
        }
    } else {
        start_flag = false;
        decay_level = 15U;
        divider = volume_or_period;
    }
}

uint16_t Apu::PulseChannel::sweep_target_period() const noexcept {
    const uint16_t change = static_cast<uint16_t>(timer_period >> sweep_shift);
    if (sweep_negate) {
        // Pulse 1 uses ones' complement (subtracts change + 1); Pulse 2 uses two's complement.
        const uint16_t sub = static_cast<uint16_t>(change + (is_pulse1 ? 1U : 0U));
        return (timer_period >= sub) ? static_cast<uint16_t>(timer_period - sub) : 0U;
    }
    return static_cast<uint16_t>(timer_period + change);
}

bool Apu::PulseChannel::is_sweep_muted() const noexcept {
    return (timer_period < 8U) || (sweep_target_period() > 0x07FFU);
}

void Apu::PulseChannel::clock_timer() noexcept {
    if (timer_value == 0U) {
        timer_value = timer_period;
        duty_pos = static_cast<uint8_t>((duty_pos + 1U) & 0x07U);
    } else {
        --timer_value;
    }
}

void Apu::PulseChannel::clock_length() noexcept {
    if (!env.loop_flag && length_counter > 0U) {
        --length_counter;
    }
}

void Apu::PulseChannel::clock_sweep() noexcept {
    if (sweep_divider == 0U && sweep_enabled && sweep_shift > 0U && !is_sweep_muted()) {
        timer_period = sweep_target_period();
    }
    if (sweep_divider == 0U || sweep_reload) {
        sweep_divider = sweep_period;
        sweep_reload = false;
    } else {
        --sweep_divider;
    }
}

uint8_t Apu::PulseChannel::output() const noexcept {
    if (!enabled || length_counter == 0U || is_sweep_muted()) {
        return 0U;
    }
    if (kPulseDutyTable[duty & 0x03U][duty_pos & 0x07U] == 0U) {
        return 0U;
    }
    return env.output();
}

void Apu::TriangleChannel::clock_timer() noexcept {
    if (timer_value == 0U) {
        timer_value = timer_period;
        if (length_counter > 0U && linear_counter > 0U && timer_period >= 2U) {
            seq_pos = static_cast<uint8_t>((seq_pos + 1U) & 0x1FU);
        }
    } else {
        --timer_value;
    }
}

void Apu::TriangleChannel::clock_linear() noexcept {
    if (linear_reload_flag) {
        linear_counter = linear_reload_value;
    } else if (linear_counter > 0U) {
        --linear_counter;
    }
    if (!control_flag) {
        linear_reload_flag = false;
    }
}

void Apu::TriangleChannel::clock_length() noexcept {
    if (!control_flag && length_counter > 0U) {
        --length_counter;
    }
}

uint8_t Apu::TriangleChannel::output() const noexcept {
    // Ultrasonic silencing when period < 2 to avoid pops/aliasing; also 0 when counters are 0.
    if (!enabled || timer_period < 2U || length_counter == 0U || linear_counter == 0U) {
        return 0U;
    }
    return kTriangleSequence[seq_pos & 0x1FU];
}

void Apu::NoiseChannel::clock_timer() noexcept {
    if (timer_value == 0U) {
        timer_value = (timer_period > 0U) ? static_cast<uint16_t>(timer_period - 1U) : 0U;
        const uint8_t tap_bit = mode ? 6U : 1U;
        const uint16_t feedback =
            static_cast<uint16_t>((shift_reg & 0x01U) ^ ((shift_reg >> tap_bit) & 0x01U));
        shift_reg = static_cast<uint16_t>((shift_reg >> 1U) | (feedback << 14U));
    } else {
        --timer_value;
    }
}

void Apu::NoiseChannel::clock_length() noexcept {
    if (!env.loop_flag && length_counter > 0U) {
        --length_counter;
    }
}

uint8_t Apu::NoiseChannel::output() const noexcept {
    if (!enabled || length_counter == 0U || (shift_reg & 0x01U) != 0U) {
        return 0U;
    }
    return env.output();
}

Apu::Apu() noexcept {
    // Precompute non-linear mixer lookup tables per https://www.nesdev.org/wiki/APU_Mixer
    pulse_mixer_table_[0] = 0.0f;
    for (size_t n = 1; n < pulse_mixer_table_.size(); ++n) {
        pulse_mixer_table_[n] = 95.52f / (8128.0f / static_cast<float>(n) + 100.0f);
    }
    tnd_mixer_table_[0] = 0.0f;
    for (size_t n = 1; n < tnd_mixer_table_.size(); ++n) {
        tnd_mixer_table_[n] = 163.67f / (24329.0f / static_cast<float>(n) + 100.0f);
    }
    reset();
}

void Apu::reset() noexcept {
    pulse1_ = PulseChannel{};
    pulse1_.is_pulse1 = true;
    pulse2_ = PulseChannel{};
    pulse2_.is_pulse1 = false;
    triangle_ = TriangleChannel{};
    noise_ = NoiseChannel{};
    noise_.shift_reg = 1U;
    noise_.timer_period = kNoisePeriodTable[0];
    dmc_ = DmcChannel{};
    dmc_.timer_period = kDmcRateTable[0];
    dmc_.timer_value = kDmcRateTable[0];

    total_cpu_cycles_ = 0ULL;
    frame_counter_cycles_ = 0U;
    five_step_mode_ = false;
    irq_inhibit_ = true;
    frame_irq_ = false;
    dmc_irq_ = false;

    resample_phase_ = 0U;
    mix_accum_ = 0.0f;
    mix_accum_count_ = 0U;
    hp_prev_in_ = 0.0f;
    hp_prev_out_ = 0.0f;
    clear_samples();
}

void Apu::clear_samples() noexcept {
    sample_read_idx_ = 0U;
    sample_write_idx_ = 0U;
    sample_count_ = 0U;
    total_samples_generated_ = 0ULL;
}

void Apu::write_register(uint16_t addr, uint8_t value) noexcept {
    switch (addr) {
        // Pulse 1 ($4000-$4003)
        case 0x4000U:
            pulse1_.duty = static_cast<uint8_t>((value >> 6U) & 0x03U);
            pulse1_.env.loop_flag = (value & 0x20U) != 0U;
            pulse1_.env.constant_volume = (value & 0x10U) != 0U;
            pulse1_.env.volume_or_period = static_cast<uint8_t>(value & 0x0FU);
            break;
        case 0x4001U:
            pulse1_.sweep_enabled = (value & 0x80U) != 0U;
            pulse1_.sweep_period = static_cast<uint8_t>((value >> 4U) & 0x07U);
            pulse1_.sweep_negate = (value & 0x08U) != 0U;
            pulse1_.sweep_shift = static_cast<uint8_t>(value & 0x07U);
            pulse1_.sweep_reload = true;
            break;
        case 0x4002U:
            pulse1_.timer_period = static_cast<uint16_t>((pulse1_.timer_period & 0x0700U) | value);
            break;
        case 0x4003U:
            pulse1_.timer_period = static_cast<uint16_t>(
                (pulse1_.timer_period & 0x00FFU) | (static_cast<uint16_t>(value & 0x07U) << 8U));
            if (pulse1_.enabled) {
                pulse1_.length_counter = kLengthTable[(value >> 3U) & 0x1FU];
            }
            pulse1_.duty_pos = 0U;
            pulse1_.env.start_flag = true;
            break;

        // Pulse 2 ($4004-$4007)
        case 0x4004U:
            pulse2_.duty = static_cast<uint8_t>((value >> 6U) & 0x03U);
            pulse2_.env.loop_flag = (value & 0x20U) != 0U;
            pulse2_.env.constant_volume = (value & 0x10U) != 0U;
            pulse2_.env.volume_or_period = static_cast<uint8_t>(value & 0x0FU);
            break;
        case 0x4005U:
            pulse2_.sweep_enabled = (value & 0x80U) != 0U;
            pulse2_.sweep_period = static_cast<uint8_t>((value >> 4U) & 0x07U);
            pulse2_.sweep_negate = (value & 0x08U) != 0U;
            pulse2_.sweep_shift = static_cast<uint8_t>(value & 0x07U);
            pulse2_.sweep_reload = true;
            break;
        case 0x4006U:
            pulse2_.timer_period = static_cast<uint16_t>((pulse2_.timer_period & 0x0700U) | value);
            break;
        case 0x4007U:
            pulse2_.timer_period = static_cast<uint16_t>(
                (pulse2_.timer_period & 0x00FFU) | (static_cast<uint16_t>(value & 0x07U) << 8U));
            if (pulse2_.enabled) {
                pulse2_.length_counter = kLengthTable[(value >> 3U) & 0x1FU];
            }
            pulse2_.duty_pos = 0U;
            pulse2_.env.start_flag = true;
            break;

        // Triangle ($4008-$400B)
        case 0x4008U:
            triangle_.control_flag = (value & 0x80U) != 0U;
            triangle_.linear_reload_value = static_cast<uint8_t>(value & 0x7FU);
            break;
        case 0x4009U:
            break;
        case 0x400AU:
            triangle_.timer_period =
                static_cast<uint16_t>((triangle_.timer_period & 0x0700U) | value);
            break;
        case 0x400BU:
            triangle_.timer_period = static_cast<uint16_t>(
                (triangle_.timer_period & 0x00FFU) | (static_cast<uint16_t>(value & 0x07U) << 8U));
            if (triangle_.enabled) {
                triangle_.length_counter = kLengthTable[(value >> 3U) & 0x1FU];
            }
            triangle_.linear_reload_flag = true;
            break;

        // Noise ($400C-$400F)
        case 0x400CU:
            noise_.env.loop_flag = (value & 0x20U) != 0U;
            noise_.env.constant_volume = (value & 0x10U) != 0U;
            noise_.env.volume_or_period = static_cast<uint8_t>(value & 0x0FU);
            break;
        case 0x400DU:
            break;
        case 0x400EU:
            noise_.mode = (value & 0x80U) != 0U;
            noise_.timer_period = kNoisePeriodTable[value & 0x0FU];
            break;
        case 0x400FU:
            if (noise_.enabled) {
                noise_.length_counter = kLengthTable[(value >> 3U) & 0x1FU];
            }
            noise_.env.start_flag = true;
            break;

        // DMC ($4010-$4013)
        case 0x4010U:
            dmc_.irq_enabled = (value & 0x80U) != 0U;
            if (!dmc_.irq_enabled) {
                dmc_irq_ = false;
            }
            dmc_.loop = (value & 0x40U) != 0U;
            dmc_.timer_period = kDmcRateTable[value & 0x0FU];
            if (dmc_.timer_value > dmc_.timer_period) {
                dmc_.timer_value = dmc_.timer_period;
            }
            break;
        case 0x4011U:
            dmc_.output_level = static_cast<uint8_t>(value & 0x7FU);
            break;
        case 0x4012U:
            dmc_.sample_address =
                static_cast<uint16_t>(0xC000U + (static_cast<uint16_t>(value) << 6U));
            break;
        case 0x4013U:
            dmc_.sample_length = static_cast<uint16_t>((static_cast<uint16_t>(value) << 4U) + 1U);
            break;

        // Status ($4015)
        case 0x4015U:
            dmc_irq_ = false;
            pulse1_.enabled = (value & 0x01U) != 0U;
            if (!pulse1_.enabled) {
                pulse1_.length_counter = 0U;
            }
            pulse2_.enabled = (value & 0x02U) != 0U;
            if (!pulse2_.enabled) {
                pulse2_.length_counter = 0U;
            }
            triangle_.enabled = (value & 0x04U) != 0U;
            if (!triangle_.enabled) {
                triangle_.length_counter = 0U;
            }
            noise_.enabled = (value & 0x08U) != 0U;
            if (!noise_.enabled) {
                noise_.length_counter = 0U;
            }
            dmc_.enabled = (value & 0x10U) != 0U;
            if (!dmc_.enabled) {
                dmc_.bytes_remaining = 0U;
            } else {
                if (dmc_.bytes_remaining == 0U) {
                    dmc_.current_address = dmc_.sample_address;
                    dmc_.bytes_remaining = dmc_.sample_length;
                }
                if (dmc_.sample_buffer_empty && dmc_.bytes_remaining > 0U) {
                    fetch_dmc_sample_byte();
                }
            }
            break;

        // Frame Counter ($4017)
        case 0x4017U:
            five_step_mode_ = (value & 0x80U) != 0U;
            irq_inhibit_ = (value & 0x40U) != 0U;
            if (irq_inhibit_) {
                frame_irq_ = false;
            }
            frame_counter_cycles_ = 0U;
            // Writing $4017 with bit 7 set immediately clocks quarter and half frame units.
            if (five_step_mode_) {
                clock_quarter_frame();
                clock_half_frame();
            }
            break;

        default:
            break;
    }
}

uint8_t Apu::peek_status() const noexcept {
    uint8_t res = 0U;
    if (pulse1_.length_counter > 0U) {
        res |= 0x01U;
    }
    if (pulse2_.length_counter > 0U) {
        res |= 0x02U;
    }
    if (triangle_.length_counter > 0U) {
        res |= 0x04U;
    }
    if (noise_.length_counter > 0U) {
        res |= 0x08U;
    }
    if (dmc_.bytes_remaining > 0U) {
        res |= 0x10U;
    }
    if (frame_irq_) {
        res |= 0x40U;
    }
    if (dmc_irq_) {
        res |= 0x80U;
    }
    return res;
}

uint8_t Apu::read_status() noexcept {
    const uint8_t res = peek_status();
    frame_irq_ = false;
    return res;
}

void Apu::clock_quarter_frame() noexcept {
    pulse1_.env.clock();
    pulse2_.env.clock();
    noise_.env.clock();
    triangle_.clock_linear();
}

void Apu::clock_half_frame() noexcept {
    pulse1_.clock_length();
    pulse1_.clock_sweep();
    pulse2_.clock_length();
    pulse2_.clock_sweep();
    triangle_.clock_length();
    noise_.clock_length();
}

void Apu::fetch_dmc_sample_byte() noexcept {
    if (!dmc_.sample_buffer_empty || dmc_.bytes_remaining == 0U) {
        return;
    }
    uint8_t byte = 0U;
    if (mem_read_cb_) {
        byte = mem_read_cb_(mem_read_user_, dmc_.current_address);
    }
    dmc_.sample_buffer = byte;
    dmc_.sample_buffer_empty = false;

    if (dmc_.current_address == 0xFFFFU) {
        dmc_.current_address = 0x8000U;
    } else {
        ++dmc_.current_address;
    }

    --dmc_.bytes_remaining;
    if (dmc_.bytes_remaining == 0U) {
        if (dmc_.loop) {
            dmc_.current_address = dmc_.sample_address;
            dmc_.bytes_remaining = dmc_.sample_length;
        } else if (dmc_.irq_enabled) {
            dmc_irq_ = true;
        }
    }
}

void Apu::clock_dmc_timer() noexcept {
    if (dmc_.sample_buffer_empty && dmc_.bytes_remaining > 0U) {
        fetch_dmc_sample_byte();
    }

    if (dmc_.timer_value > 1U) {
        --dmc_.timer_value;
        return;
    }
    dmc_.timer_value = dmc_.timer_period;

    if (!dmc_.silence_flag) {
        if ((dmc_.shift_reg & 0x01U) != 0U) {
            if (dmc_.output_level <= 125U) {
                dmc_.output_level = static_cast<uint8_t>(dmc_.output_level + 2U);
            }
        } else {
            if (dmc_.output_level >= 2U) {
                dmc_.output_level = static_cast<uint8_t>(dmc_.output_level - 2U);
            }
        }
    }

    dmc_.shift_reg >>= 1U;
    if (dmc_.bits_remaining > 0U) {
        --dmc_.bits_remaining;
    }
    if (dmc_.bits_remaining == 0U) {
        dmc_.bits_remaining = 8U;
        if (dmc_.sample_buffer_empty) {
            dmc_.silence_flag = true;
        } else {
            dmc_.silence_flag = false;
            dmc_.shift_reg = dmc_.sample_buffer;
            dmc_.sample_buffer_empty = true;
            if (dmc_.bytes_remaining > 0U) {
                fetch_dmc_sample_byte();
            }
        }
    }
}

float Apu::current_mixer_output() const noexcept {
    const uint8_t p1 = pulse1_.output();
    const uint8_t p2 = pulse2_.output();
    const uint8_t tri = triangle_.output();
    const uint8_t noi = noise_.output();
    const uint8_t dmc = dmc_.output_level;

    const size_t pulse_idx = static_cast<size_t>(p1) + static_cast<size_t>(p2);
    const size_t tnd_idx =
        3U * static_cast<size_t>(tri) + 2U * static_cast<size_t>(noi) + static_cast<size_t>(dmc);

    return pulse_mixer_table_[pulse_idx] + tnd_mixer_table_[tnd_idx];
}

void Apu::push_output_sample(float mixed) noexcept {
    // First-order DC-blocking high-pass filter (~90 Hz at 48 kHz: alpha = 0.9883f)
    const float filtered = mixed - hp_prev_in_ + 0.9883f * hp_prev_out_;
    hp_prev_in_ = mixed;
    hp_prev_out_ = filtered;

    // Scale to [-1.0f, 1.0f] and int16_t PCM
    const float clamped = std::clamp(filtered * 1.8f, -1.0f, 1.0f);
    const auto pcm16 = static_cast<int16_t>(clamped * 32767.0f);

    sample_buf_f32_[sample_write_idx_] = clamped;
    sample_buf_i16_[sample_write_idx_] = pcm16;
    sample_write_idx_ = (sample_write_idx_ + 1U) % kSampleBufferCapacity;
    if (sample_count_ < kSampleBufferCapacity) {
        ++sample_count_;
    } else {
        sample_read_idx_ = (sample_read_idx_ + 1U) % kSampleBufferCapacity;
    }
    ++total_samples_generated_;
}

void Apu::step_cycle() noexcept {
    ++total_cpu_cycles_;
    ++frame_counter_cycles_;

    // Frame counter sequencer (https://www.nesdev.org/wiki/APU_Frame_Counter)
    if (!five_step_mode_) {
        // Mode 0: 4-step sequence (29830 CPU cycles)
        switch (frame_counter_cycles_) {
            case 7457U:
                clock_quarter_frame();
                break;
            case 14913U:
                clock_quarter_frame();
                clock_half_frame();
                break;
            case 22371U:
                clock_quarter_frame();
                break;
            case 29828U:
                if (!irq_inhibit_) {
                    frame_irq_ = true;
                }
                break;
            case 29829U:
                clock_quarter_frame();
                clock_half_frame();
                if (!irq_inhibit_) {
                    frame_irq_ = true;
                }
                break;
            case 29830U:
                if (!irq_inhibit_) {
                    frame_irq_ = true;
                }
                frame_counter_cycles_ = 0U;
                break;
            default:
                break;
        }
    } else {
        // Mode 1: 5-step sequence (37282 CPU cycles, never triggers frame IRQ)
        switch (frame_counter_cycles_) {
            case 7457U:
                clock_quarter_frame();
                break;
            case 14913U:
                clock_quarter_frame();
                clock_half_frame();
                break;
            case 22371U:
                clock_quarter_frame();
                break;
            case 37281U:
                clock_quarter_frame();
                clock_half_frame();
                break;
            case 37282U:
                frame_counter_cycles_ = 0U;
                break;
            default:
                break;
        }
    }

    // Triangle, Noise, and DMC timers are clocked every CPU cycle;
    // Pulse 1 and Pulse 2 timers are clocked every 2 CPU cycles (1 APU cycle).
    triangle_.clock_timer();
    noise_.clock_timer();
    clock_dmc_timer();
    if ((total_cpu_cycles_ & 1ULL) == 0ULL) {
        pulse1_.clock_timer();
        pulse2_.clock_timer();
    }

    // Boxcar accumulate non-linear mixer output and decimate to 48,000 Hz
    mix_accum_ += current_mixer_output();
    ++mix_accum_count_;
    resample_phase_ += kSampleRate;
    if (resample_phase_ >= kCpuClockHz) {
        resample_phase_ -= kCpuClockHz;
        const float avg = mix_accum_ / static_cast<float>(mix_accum_count_);
        mix_accum_ = 0.0f;
        mix_accum_count_ = 0U;
        push_output_sample(avg);
    }
}

void Apu::step_cycles(uint64_t cpu_cycles) noexcept {
    for (uint64_t i = 0ULL; i < cpu_cycles; ++i) {
        step_cycle();
    }
}

uint32_t Apu::cycles_until_next_irq() const noexcept {
    if (irq_pending()) {
        return 0U;
    }
    uint32_t min_cyc = 0xFFFFFFFFU;
    if (!five_step_mode_ && !irq_inhibit_) {
        if (frame_counter_cycles_ < 29828U) {
            min_cyc = 29828U - frame_counter_cycles_;
        } else {
            min_cyc = (29830U - frame_counter_cycles_) + 29828U;
        }
    }
    if (dmc_.irq_enabled && !dmc_.loop && dmc_.bytes_remaining > 0U) {
        const uint32_t dmc_cyc = static_cast<uint32_t>(dmc_.timer_value) +
                                 static_cast<uint32_t>(dmc_.bits_remaining) * dmc_.timer_period;
        if (dmc_cyc < min_cyc) {
            min_cyc = dmc_cyc;
        }
    }
    return min_cyc;
}

uint8_t Apu::length_counter(uint8_t channel) const noexcept {
    switch (channel) {
        case 0U:
            return pulse1_.length_counter;
        case 1U:
            return pulse2_.length_counter;
        case 2U:
            return triangle_.length_counter;
        case 3U:
            return noise_.length_counter;
        default:
            return 0U;
    }
}

size_t Apu::pop_samples(int16_t* dst, size_t max_count) noexcept {
    const size_t n = std::min(sample_count_, max_count);
    for (size_t i = 0U; i < n; ++i) {
        if (dst) {
            dst[i] = sample_buf_i16_[sample_read_idx_];
        }
        sample_read_idx_ = (sample_read_idx_ + 1U) % kSampleBufferCapacity;
    }
    sample_count_ -= n;
    return n;
}

size_t Apu::pop_float_samples(float* dst, size_t max_count) noexcept {
    const size_t n = std::min(sample_count_, max_count);
    for (size_t i = 0U; i < n; ++i) {
        if (dst) {
            dst[i] = sample_buf_f32_[sample_read_idx_];
        }
        sample_read_idx_ = (sample_read_idx_ + 1U) % kSampleBufferCapacity;
    }
    sample_count_ -= n;
    return n;
}

void Apu::save_snapshot(Snapshot& snap) const noexcept {
    snap.total_cpu_cycles = total_cpu_cycles_;
    snap.frame_counter_cycles = frame_counter_cycles_;
    snap.five_step_mode = five_step_mode_;
    snap.irq_inhibit = irq_inhibit_;
    snap.frame_irq = frame_irq_;
    snap.dmc_irq = dmc_irq_;
    snap.pulse1 = pulse1_;
    snap.pulse2 = pulse2_;
    snap.triangle = triangle_;
    snap.noise = noise_;
    snap.dmc = dmc_;
}

void Apu::load_snapshot(const Snapshot& snap) noexcept {
    total_cpu_cycles_ = snap.total_cpu_cycles;
    frame_counter_cycles_ = snap.frame_counter_cycles;
    five_step_mode_ = snap.five_step_mode;
    irq_inhibit_ = snap.irq_inhibit;
    frame_irq_ = snap.frame_irq;
    dmc_irq_ = snap.dmc_irq;
    pulse1_ = snap.pulse1;
    pulse2_ = snap.pulse2;
    triangle_ = snap.triangle;
    noise_ = snap.noise;
    dmc_ = snap.dmc;
}

}  // namespace dragon::nes
