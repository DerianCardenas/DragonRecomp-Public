#include "gb/apu.h"

#include <algorithm>
#include <cstring>

namespace dragon::gb {

namespace {

constexpr uint8_t kDutyWaveforms[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1},  // 12.5%
    {1, 0, 0, 0, 0, 0, 0, 1},  // 25%
    {1, 0, 0, 0, 0, 1, 1, 1},  // 50%
    {0, 1, 1, 1, 1, 1, 1, 0}   // 75%
};

constexpr uint32_t kNoiseDivisors[8] = {8, 16, 32, 48, 64, 80, 96, 112};

}  // namespace

Apu::Apu() noexcept {
    reset();
}

void Apu::reset() noexcept {
    enabled_ = true;
    nr50_ = 0x77U;
    nr51_ = 0xF3U;
    nr52_ = 0xF1U;

    frame_seq_timer_ = 0;
    frame_seq_step_ = 0;
    sample_timer_ = 0;
    sample_buffer_.clear();
    sample_buffer_.reserve(kMaxSamples * 2U);

    ch1_ = {};
    ch2_ = {};
    ch3_ = {};
    ch4_ = {};

    ch4_.lfsr = 0x7FFF;
    ch3_.wave_ram.fill(0);
}

bool Apu::channel_active(uint8_t ch) const noexcept {
    switch (ch) {
        case 1:
            return ch1_.active;
        case 2:
            return ch2_.active;
        case 3:
            return ch3_.active;
        case 4:
            return ch4_.active;
        default:
            return false;
    }
}

uint8_t Apu::read_register(uint16_t addr) const noexcept {
    switch (addr) {
        // Channel 1
        case 0xFF10:
            return ch1_.nr10 | 0x80U;
        case 0xFF11:
            return ch1_.nr11 | 0x3FU;
        case 0xFF12:
            return ch1_.nr12;
        case 0xFF14:
            return ch1_.nr14 | 0xBFU;

        // Channel 2
        case 0xFF16:
            return ch2_.nr21 | 0x3FU;
        case 0xFF17:
            return ch2_.nr22;
        case 0xFF19:
            return ch2_.nr24 | 0xBFU;

        // Channel 3
        case 0xFF1A:
            return ch3_.nr30 | 0x7FU;
        case 0xFF1C:
            return ch3_.nr32 | 0x9FU;
        case 0xFF1E:
            return ch3_.nr34 | 0xBFU;

        // Channel 4
        case 0xFF21:
            return ch4_.nr42;
        case 0xFF22:
            return ch4_.nr43;
        case 0xFF23:
            return ch4_.nr44 | 0xBFU;

        // Control
        case 0xFF24:
            return nr50_;
        case 0xFF25:
            return nr51_;
        case 0xFF26: {
            uint8_t val = (enabled_ ? 0x80U : 0x00U) | 0x70U;
            if (ch1_.active)
                val |= 0x01U;
            if (ch2_.active)
                val |= 0x02U;
            if (ch3_.active)
                val |= 0x04U;
            if (ch4_.active)
                val |= 0x08U;
            return val;
        }

        default:
            if (addr >= 0xFF30 && addr <= 0xFF3F) {
                return ch3_.wave_ram[addr - 0xFF30U];
            }
            return 0xFF;
    }
}

void Apu::write_register(uint16_t addr, uint8_t value) noexcept {
    if (!enabled_ && addr != 0xFF26) {
        // When APU is disabled, ignore writes except to Wave RAM
        if (addr >= 0xFF30 && addr <= 0xFF3F) {
            ch3_.wave_ram[addr - 0xFF30U] = value;
        }
        return;
    }

    switch (addr) {
        // Channel 1
        case 0xFF10:
            ch1_.nr10 = value;
            break;
        case 0xFF11:
            ch1_.nr11 = value;
            ch1_.length = 64U - (value & 0x3FU);
            break;
        case 0xFF12:
            ch1_.nr12 = value;
            break;
        case 0xFF13:
            ch1_.nr13 = value;
            ch1_.frequency = (ch1_.frequency & 0x0700U) | value;
            break;
        case 0xFF14:
            ch1_.nr14 = value;
            ch1_.frequency =
                (ch1_.frequency & 0x00FFU) | (static_cast<uint16_t>(value & 0x07U) << 8U);
            ch1_.length_enabled = (value & 0x40U) != 0;
            if (value & 0x80U)
                trigger_ch1();
            break;

        // Channel 2
        case 0xFF16:
            ch2_.nr21 = value;
            ch2_.length = 64U - (value & 0x3FU);
            break;
        case 0xFF17:
            ch2_.nr22 = value;
            break;
        case 0xFF18:
            ch2_.nr23 = value;
            ch2_.frequency = (ch2_.frequency & 0x0700U) | value;
            break;
        case 0xFF19:
            ch2_.nr24 = value;
            ch2_.frequency =
                (ch2_.frequency & 0x00FFU) | (static_cast<uint16_t>(value & 0x07U) << 8U);
            ch2_.length_enabled = (value & 0x40U) != 0;
            if (value & 0x80U)
                trigger_ch2();
            break;

        // Channel 3
        case 0xFF1A:
            ch3_.nr30 = value;
            if (!(value & 0x80U))
                ch3_.active = false;
            break;
        case 0xFF1B:
            ch3_.nr31 = value;
            ch3_.length = 256U - value;
            break;
        case 0xFF1C:
            ch3_.nr32 = value;
            break;
        case 0xFF1D:
            ch3_.nr33 = value;
            ch3_.frequency = (ch3_.frequency & 0x0700U) | value;
            break;
        case 0xFF1E:
            ch3_.nr34 = value;
            ch3_.frequency =
                (ch3_.frequency & 0x00FFU) | (static_cast<uint16_t>(value & 0x07U) << 8U);
            ch3_.length_enabled = (value & 0x40U) != 0;
            if (value & 0x80U)
                trigger_ch3();
            break;

        // Channel 4
        case 0xFF20:
            ch4_.nr41 = value;
            ch4_.length = 64U - (value & 0x3FU);
            break;
        case 0xFF21:
            ch4_.nr42 = value;
            break;
        case 0xFF22:
            ch4_.nr43 = value;
            break;
        case 0xFF23:
            ch4_.nr44 = value;
            ch4_.length_enabled = (value & 0x40U) != 0;
            if (value & 0x80U)
                trigger_ch4();
            break;

        // Master Control
        case 0xFF24:
            nr50_ = value;
            break;
        case 0xFF25:
            nr51_ = value;
            break;
        case 0xFF26:
            enabled_ = (value & 0x80U) != 0;
            if (!enabled_) {
                ch1_.active = false;
                ch2_.active = false;
                ch3_.active = false;
                ch4_.active = false;
            }
            break;

        default:
            if (addr >= 0xFF30 && addr <= 0xFF3F) {
                ch3_.wave_ram[addr - 0xFF30U] = value;
            }
            break;
    }
}

void Apu::trigger_ch1() noexcept {
    ch1_.active = true;
    if (ch1_.length == 0)
        ch1_.length = 64;
    ch1_.volume = (ch1_.nr12 >> 4U) & 0x0FU;
    ch1_.env_timer = ch1_.nr12 & 0x07U;
    ch1_.timer = (2048U - ch1_.frequency) * 4U;

    // Sweep setup
    ch1_.shadow_freq = ch1_.frequency;
    uint8_t sweep_period = (ch1_.nr10 >> 4U) & 0x07U;
    uint8_t sweep_shift = ch1_.nr10 & 0x07U;
    ch1_.sweep_timer = (sweep_period > 0) ? sweep_period : 8;
    ch1_.sweep_enabled = (sweep_period > 0 || sweep_shift > 0);
}

void Apu::trigger_ch2() noexcept {
    ch2_.active = true;
    if (ch2_.length == 0)
        ch2_.length = 64;
    ch2_.volume = (ch2_.nr22 >> 4U) & 0x0FU;
    ch2_.env_timer = ch2_.nr22 & 0x07U;
    ch2_.timer = (2048U - ch2_.frequency) * 4U;
}

void Apu::trigger_ch3() noexcept {
    ch3_.active = (ch3_.nr30 & 0x80U) != 0;
    if (ch3_.length == 0)
        ch3_.length = 256;
    ch3_.timer = (2048U - ch3_.frequency) * 2U;
    ch3_.sample_idx = 0;
}

void Apu::trigger_ch4() noexcept {
    ch4_.active = true;
    if (ch4_.length == 0)
        ch4_.length = 64;
    ch4_.volume = (ch4_.nr42 >> 4U) & 0x0FU;
    ch4_.env_timer = ch4_.nr42 & 0x07U;
    ch4_.lfsr = 0x7FFF;

    uint32_t divisor = kNoiseDivisors[ch4_.nr43 & 0x07U];
    uint32_t shift = (ch4_.nr43 >> 4U) & 0x0FU;
    ch4_.timer = divisor << shift;
}

void Apu::advance(uint32_t t_cycles) noexcept {
    if (!enabled_)
        return;

    // 1. Frame sequencer (512 Hz, every 8192 T-cycles)
    frame_seq_timer_ += t_cycles;
    while (frame_seq_timer_ >= 8192U) {
        frame_seq_timer_ -= 8192U;
        step_frame_sequencer();
    }

    // 2. Channel 1 timer
    if (ch1_.active) {
        uint32_t period = (2048U - ch1_.frequency) * 4U;
        if (period == 0)
            period = 4;
        uint32_t remaining = t_cycles;
        while (remaining >= ch1_.timer) {
            remaining -= ch1_.timer;
            ch1_.duty_step = (ch1_.duty_step + 1U) & 0x07U;
            ch1_.timer = period;
        }
        ch1_.timer -= remaining;
    }

    // 3. Channel 2 timer
    if (ch2_.active) {
        uint32_t period = (2048U - ch2_.frequency) * 4U;
        if (period == 0)
            period = 4;
        uint32_t remaining = t_cycles;
        while (remaining >= ch2_.timer) {
            remaining -= ch2_.timer;
            ch2_.duty_step = (ch2_.duty_step + 1U) & 0x07U;
            ch2_.timer = period;
        }
        ch2_.timer -= remaining;
    }

    // 4. Channel 3 timer
    if (ch3_.active) {
        uint32_t period = (2048U - ch3_.frequency) * 2U;
        if (period == 0)
            period = 2;
        uint32_t remaining = t_cycles;
        while (remaining >= ch3_.timer) {
            remaining -= ch3_.timer;
            ch3_.sample_idx = (ch3_.sample_idx + 1U) & 0x1FU;
            ch3_.timer = period;
        }
        ch3_.timer -= remaining;
    }

    // 5. Channel 4 timer (Noise)
    if (ch4_.active) {
        uint32_t divisor = kNoiseDivisors[ch4_.nr43 & 0x07U];
        uint32_t shift = (ch4_.nr43 >> 4U) & 0x0FU;
        uint32_t period = divisor << shift;
        if (period == 0)
            period = 8;
        uint32_t remaining = t_cycles;
        while (remaining >= ch4_.timer) {
            remaining -= ch4_.timer;
            ch4_.timer = period;

            uint16_t b0 = ch4_.lfsr & 1U;
            uint16_t b1 = (ch4_.lfsr >> 1U) & 1U;
            uint16_t xored = b0 ^ b1;
            ch4_.lfsr = (ch4_.lfsr >> 1U) | (xored << 14U);
            if (ch4_.nr43 & 0x08U) {  // 7-bit mode
                ch4_.lfsr = (ch4_.lfsr & ~0x40U) | (xored << 6U);
            }
        }
        ch4_.timer -= remaining;
    }

    // 6. Sample generation at 48000 Hz
    sample_timer_ += t_cycles * kSampleRate;
    while (sample_timer_ >= kBaseClockHz) {
        sample_timer_ -= kBaseClockHz;
        mix_audio_sample();
    }
}

void Apu::step_frame_sequencer() noexcept {
    switch (frame_seq_step_) {
        case 0:
            step_length_counters();
            break;
        case 2:
            step_length_counters();
            step_sweep();
            break;
        case 4:
            step_length_counters();
            break;
        case 6:
            step_length_counters();
            step_sweep();
            break;
        case 7:
            step_envelopes();
            break;
        default:
            break;
    }
    frame_seq_step_ = (frame_seq_step_ + 1U) & 0x07U;
}

void Apu::step_length_counters() noexcept {
    if (ch1_.active && ch1_.length_enabled && ch1_.length > 0) {
        if (--ch1_.length == 0)
            ch1_.active = false;
    }
    if (ch2_.active && ch2_.length_enabled && ch2_.length > 0) {
        if (--ch2_.length == 0)
            ch2_.active = false;
    }
    if (ch3_.active && ch3_.length_enabled && ch3_.length > 0) {
        if (--ch3_.length == 0)
            ch3_.active = false;
    }
    if (ch4_.active && ch4_.length_enabled && ch4_.length > 0) {
        if (--ch4_.length == 0)
            ch4_.active = false;
    }
}

void Apu::step_envelopes() noexcept {
    auto step_env = [](auto& ch, uint8_t nr) {
        if (!ch.active)
            return;
        uint8_t period = nr & 0x07U;
        if (period == 0)
            return;

        if (ch.env_timer > 0) {
            --ch.env_timer;
        }
        if (ch.env_timer == 0) {
            ch.env_timer = period;
            bool direction = (nr & 0x08U) != 0;  // 1 = increase
            if (direction && ch.volume < 15) {
                ++ch.volume;
            } else if (!direction && ch.volume > 0) {
                --ch.volume;
            }
        }
    };

    step_env(ch1_, ch1_.nr12);
    step_env(ch2_, ch2_.nr22);
    step_env(ch4_, ch4_.nr42);
}

void Apu::step_sweep() noexcept {
    if (!ch1_.sweep_enabled)
        return;

    if (ch1_.sweep_timer > 0) {
        --ch1_.sweep_timer;
    }
    if (ch1_.sweep_timer == 0) {
        uint8_t period = (ch1_.nr10 >> 4U) & 0x07U;
        uint8_t shift = ch1_.nr10 & 0x07U;
        ch1_.sweep_timer = (period > 0) ? period : 8;

        if (period > 0 && shift > 0) {
            uint16_t delta = ch1_.shadow_freq >> shift;
            uint16_t new_freq;
            if (ch1_.nr10 & 0x08U) {  // negate
                new_freq = (ch1_.shadow_freq >= delta) ? (ch1_.shadow_freq - delta) : 0;
            } else {
                new_freq = ch1_.shadow_freq + delta;
            }

            if (new_freq <= 2047) {
                ch1_.shadow_freq = new_freq;
                ch1_.frequency = new_freq;
            } else {
                ch1_.active = false;
            }
        }
    }
}

void Apu::mix_audio_sample() noexcept {
    if (sample_buffer_.size() >= kMaxSamples * 2) {
        return;
    }

    int16_t ch1_out = 0;
    if (ch1_.active && ((ch1_.nr12 >> 4U) > 0 || (ch1_.nr12 & 0x08U))) {
        uint8_t duty = (ch1_.nr11 >> 6U) & 0x03U;
        uint8_t wave = kDutyWaveforms[duty][ch1_.duty_step];
        ch1_out = wave ? static_cast<int16_t>(ch1_.volume * 400)
                       : static_cast<int16_t>(-ch1_.volume * 400);
    }

    int16_t ch2_out = 0;
    if (ch2_.active && ((ch2_.nr22 >> 4U) > 0 || (ch2_.nr22 & 0x08U))) {
        uint8_t duty = (ch2_.nr21 >> 6U) & 0x03U;
        uint8_t wave = kDutyWaveforms[duty][ch2_.duty_step];
        ch2_out = wave ? static_cast<int16_t>(ch2_.volume * 400)
                       : static_cast<int16_t>(-ch2_.volume * 400);
    }

    int16_t ch3_out = 0;
    if (ch3_.active) {
        uint8_t byte = ch3_.wave_ram[ch3_.sample_idx / 2U];
        uint8_t sample = (ch3_.sample_idx % 2U == 0) ? (byte >> 4U) : (byte & 0x0FU);
        uint8_t vol_shift = (ch3_.nr32 >> 5U) & 0x03U;
        if (vol_shift == 1) {
            // 100%
        } else if (vol_shift == 2) {
            sample >>= 1;  // 50%
        } else if (vol_shift == 3) {
            sample >>= 2;  // 25%
        } else {
            sample = 0;  // Mute
        }
        ch3_out = static_cast<int16_t>((static_cast<int>(sample) - 8) * 400);
    }

    int16_t ch4_out = 0;
    if (ch4_.active) {
        uint8_t bit = (~ch4_.lfsr) & 1U;
        ch4_out = bit ? static_cast<int16_t>(ch4_.volume * 400)
                      : static_cast<int16_t>(-ch4_.volume * 400);
    }

    int32_t left = 0;
    int32_t right = 0;

    // Pan channels into left/right according to NR51
    if (nr51_ & 0x01U)
        right += ch1_out;
    if (nr51_ & 0x02U)
        right += ch2_out;
    if (nr51_ & 0x04U)
        right += ch3_out;
    if (nr51_ & 0x08U)
        right += ch4_out;

    if (nr51_ & 0x10U)
        left += ch1_out;
    if (nr51_ & 0x20U)
        left += ch2_out;
    if (nr51_ & 0x40U)
        left += ch3_out;
    if (nr51_ & 0x80U)
        left += ch4_out;

    // Master volume NR50
    uint8_t r_vol = (nr50_ & 0x07U) + 1U;
    uint8_t l_vol = ((nr50_ >> 4U) & 0x07U) + 1U;

    right = (right * r_vol) / 8;
    left = (left * l_vol) / 8;

    right = std::clamp<int32_t>(right, -32768, 32767);
    left = std::clamp<int32_t>(left, -32768, 32767);

    sample_buffer_.push_back(static_cast<int16_t>(left));
    sample_buffer_.push_back(static_cast<int16_t>(right));
}

void Apu::save_snapshot(Snapshot& snap) const noexcept {
    snap.enabled = enabled_;
    snap.nr50 = nr50_;
    snap.nr51 = nr51_;
    snap.nr52 = nr52_;
    snap.frame_seq_timer = frame_seq_timer_;
    snap.frame_seq_step = frame_seq_step_;
    snap.sample_timer = sample_timer_;
    snap.ch1 = ch1_;
    snap.ch2 = ch2_;
    snap.ch3 = ch3_;
    snap.ch4 = ch4_;
}

void Apu::load_snapshot(const Snapshot& snap) noexcept {
    enabled_ = snap.enabled;
    nr50_ = snap.nr50;
    nr51_ = snap.nr51;
    nr52_ = snap.nr52;
    frame_seq_timer_ = snap.frame_seq_timer;
    frame_seq_step_ = snap.frame_seq_step;
    sample_timer_ = snap.sample_timer;
    ch1_ = snap.ch1;
    ch2_ = snap.ch2;
    ch3_ = snap.ch3;
    ch4_ = snap.ch4;
}

}  // namespace dragon::gb
