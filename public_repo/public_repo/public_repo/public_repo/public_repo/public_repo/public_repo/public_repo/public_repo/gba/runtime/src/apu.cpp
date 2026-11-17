#include "gba/apu.h"

#include <algorithm>

namespace dragon::gba {

namespace {

constexpr uint8_t kDutyTable[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1},  // 12.5%
    {1, 0, 0, 0, 0, 0, 0, 1},  // 25%
    {1, 0, 0, 0, 0, 1, 1, 1},  // 50%
    {0, 1, 1, 1, 1, 1, 1, 0}   // 75%
};

constexpr uint32_t kFrameSequencerPeriod = Apu::kCpuClockHz / 512U;  // 32768 cycles

}  // namespace

Apu::Apu() {
    sample_buffer_.reserve(2048);
    reset();
}

void Apu::reset() {
    fifos_[0].clear();
    fifos_[1].clear();
    sq1_ = SquareChannel{};
    sq2_ = SquareChannel{};
    wave_ = WaveChannel{};
    noise_ = NoiseChannel{};
    for (auto& bank : wave_ram_) {
        bank.fill(0U);
    }
    soundcnt_l_ = 0U;
    soundcnt_h_ = 0U;
    soundcnt_x_ = 0U;
    soundbias_ = 0x0200U;
    io_regs_.fill(0U);
    frame_seq_cycles_ = 0U;
    frame_seq_step_ = 0U;
    sample_accum_ = 0ULL;
    sample_buffer_.clear();
}

uint8_t Apu::read_io8(uint32_t offset) const noexcept {
    if (offset >= 0x090U && offset < 0x0A0U) {
        const uint8_t cpu_bank = ((wave_.cnt_l >> 6U) & 1U) ^ 1U;
        return wave_ram_[cpu_bank][offset - 0x090U];
    }
    const uint16_t hw = read_io16(offset & ~1U);
    return (offset & 1U) ? static_cast<uint8_t>((hw >> 8U) & 0xFFU)
                         : static_cast<uint8_t>(hw & 0xFFU);
}

uint16_t Apu::read_io16(uint32_t offset) const noexcept {
    if (offset >= 0x090U && offset < 0x0A0U) {
        const uint8_t cpu_bank = ((wave_.cnt_l >> 6U) & 1U) ^ 1U;
        const uint32_t idx = (offset & ~1U) - 0x090U;
        return static_cast<uint16_t>(wave_ram_[cpu_bank][idx] |
                                     (static_cast<uint16_t>(wave_ram_[cpu_bank][idx + 1U]) << 8U));
    }
    switch (offset & 0xFFEU) {
        case 0x060:
            return sq1_.cnt_l;
        case 0x062:
            return sq1_.cnt_h;
        case 0x064:
            return sq1_.cnt_x;
        case 0x068:
            return sq2_.cnt_h;
        case 0x06C:
            return sq2_.cnt_x;
        case 0x070:
            return wave_.cnt_l;
        case 0x072:
            return wave_.cnt_h;
        case 0x074:
            return wave_.cnt_x;
        case 0x078:
            return noise_.cnt_l;
        case 0x07C:
            return noise_.cnt_h;
        case 0x080:
            return soundcnt_l_;
        case 0x082:
            return soundcnt_h_ & 0x770FU;
        case 0x084: {
            uint16_t st = soundcnt_x_ & 0x0080U;
            if (sq1_.enabled)
                st |= 0x0001U;
            if (sq2_.enabled)
                st |= 0x0002U;
            if (wave_.enabled && (wave_.cnt_l & 0x0080U) != 0U)
                st |= 0x0004U;
            if (noise_.enabled)
                st |= 0x0008U;
            return st;
        }
        case 0x088:
            return soundbias_;
        default:
            if (offset >= 0x060U && offset + 1U < 0x0B0U) {
                const uint32_t idx = offset - 0x060U;
                return static_cast<uint16_t>(io_regs_[idx] |
                                             (static_cast<uint16_t>(io_regs_[idx + 1U]) << 8U));
            }
            return 0U;
    }
}

void Apu::write_io8(uint32_t offset, uint8_t value) noexcept {
    if (offset >= 0x090U && offset < 0x0A0U) {
        const uint8_t cpu_bank = ((wave_.cnt_l >> 6U) & 1U) ^ 1U;
        wave_ram_[cpu_bank][offset - 0x090U] = value;
        return;
    }
    if (offset >= 0x0A0U && offset < 0x0A4U) {
        fifos_[0].push8(static_cast<int8_t>(value));
        return;
    }
    if (offset >= 0x0A4U && offset < 0x0A8U) {
        fifos_[1].push8(static_cast<int8_t>(value));
        return;
    }
    const uint32_t aligned = offset & ~1U;
    uint16_t cur = read_io16(aligned);
    if (offset & 1U) {
        cur = static_cast<uint16_t>((cur & 0x00FFU) | (static_cast<uint16_t>(value) << 8U));
    } else {
        cur = static_cast<uint16_t>((cur & 0xFF00U) | value);
    }
    write_io16(aligned, cur);
}

void Apu::write_io16(uint32_t offset, uint16_t value) noexcept {
    if (offset >= 0x090U && offset < 0x0A0U) {
        const uint8_t cpu_bank = ((wave_.cnt_l >> 6U) & 1U) ^ 1U;
        const uint32_t idx = (offset & ~1U) - 0x090U;
        wave_ram_[cpu_bank][idx] = static_cast<uint8_t>(value & 0xFFU);
        wave_ram_[cpu_bank][idx + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
        return;
    }
    if (offset >= 0x0A0U && offset < 0x0A4U) {
        fifos_[0].push8(static_cast<int8_t>(value & 0xFFU));
        fifos_[0].push8(static_cast<int8_t>((value >> 8U) & 0xFFU));
        return;
    }
    if (offset >= 0x0A4U && offset < 0x0A8U) {
        fifos_[1].push8(static_cast<int8_t>(value & 0xFFU));
        fifos_[1].push8(static_cast<int8_t>((value >> 8U) & 0xFFU));
        return;
    }

    if (offset >= 0x060U && offset + 1U < 0x0B0U) {
        const uint32_t idx = offset - 0x060U;
        io_regs_[idx] = static_cast<uint8_t>(value & 0xFFU);
        io_regs_[idx + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    }

    switch (offset & 0xFFEU) {
        case 0x060:
            sq1_.cnt_l = value;
            break;
        case 0x062:
            sq1_.cnt_h = value;
            sq1_.length_counter = static_cast<uint16_t>(64U - (value & 0x3FU));
            sq1_.volume = static_cast<uint8_t>((value >> 12U) & 0x0FU);
            if ((value & 0xF800U) == 0U) {
                sq1_.enabled = false;
            }
            break;
        case 0x064:
            sq1_.cnt_x = value;
            if (value & 0x8000U) {
                sq1_.enabled = (sq1_.cnt_h & 0xF800U) != 0U;
                if (sq1_.length_counter == 0U) {
                    sq1_.length_counter = 64U;
                }
                sq1_.volume = static_cast<uint8_t>((sq1_.cnt_h >> 12U) & 0x0FU);
                sq1_.env_timer = static_cast<uint8_t>((sq1_.cnt_h >> 8U) & 7U);
                const uint16_t freq = value & 0x07FFU;
                sq1_.phase_timer = (2048U - freq) * 16U;
                sq1_.shadow_freq = freq;
                const uint8_t sweep_period = static_cast<uint8_t>((sq1_.cnt_l >> 4U) & 7U);
                const uint8_t sweep_shift = static_cast<uint8_t>(sq1_.cnt_l & 7U);
                sq1_.sweep_timer = (sweep_period == 0U) ? 8U : sweep_period;
                sq1_.sweep_enabled = (sweep_period != 0U || sweep_shift != 0U);
            }
            break;
        case 0x068:
            sq2_.cnt_h = value;
            sq2_.length_counter = static_cast<uint16_t>(64U - (value & 0x3FU));
            sq2_.volume = static_cast<uint8_t>((value >> 12U) & 0x0FU);
            if ((value & 0xF800U) == 0U) {
                sq2_.enabled = false;
            }
            break;
        case 0x06C:
            sq2_.cnt_x = value;
            if (value & 0x8000U) {
                sq2_.enabled = (sq2_.cnt_h & 0xF800U) != 0U;
                if (sq2_.length_counter == 0U) {
                    sq2_.length_counter = 64U;
                }
                sq2_.volume = static_cast<uint8_t>((sq2_.cnt_h >> 12U) & 0x0FU);
                sq2_.env_timer = static_cast<uint8_t>((sq2_.cnt_h >> 8U) & 7U);
                const uint32_t freq = value & 0x07FFU;
                sq2_.phase_timer = (2048U - freq) * 16U;
            }
            break;
        case 0x070:
            wave_.cnt_l = value;
            if ((value & 0x0080U) == 0U) {
                wave_.enabled = false;
            }
            break;
        case 0x072:
            wave_.cnt_h = value;
            wave_.length_counter = static_cast<uint16_t>(256U - (value & 0xFFU));
            break;
        case 0x074:
            wave_.cnt_x = value;
            if (value & 0x8000U) {
                wave_.enabled = (wave_.cnt_l & 0x0080U) != 0U;
                if (wave_.length_counter == 0U) {
                    wave_.length_counter = 256U;
                }
                wave_.wave_pos = 0U;
                const uint32_t freq = value & 0x07FFU;
                wave_.phase_timer = (2048U - freq) * 8U;
            }
            break;
        case 0x078:
            noise_.cnt_l = value;
            noise_.length_counter = static_cast<uint16_t>(64U - (value & 0x3FU));
            noise_.volume = static_cast<uint8_t>((value >> 12U) & 0x0FU);
            if ((value & 0xF800U) == 0U) {
                noise_.enabled = false;
            }
            break;
        case 0x07C:
            noise_.cnt_h = value;
            if (value & 0x8000U) {
                noise_.enabled = (noise_.cnt_l & 0xF800U) != 0U;
                if (noise_.length_counter == 0U) {
                    noise_.length_counter = 64U;
                }
                noise_.volume = static_cast<uint8_t>((noise_.cnt_l >> 12U) & 0x0FU);
                noise_.env_timer = static_cast<uint8_t>((noise_.cnt_l >> 8U) & 7U);
                noise_.lfsr = 0x7FFFU;
                const uint32_t r = value & 7U;
                const uint32_t s = (value >> 4U) & 0x0FU;
                const uint32_t div = (r == 0U) ? 32U : (r * 64U);
                noise_.phase_timer = div << s;
            }
            break;
        case 0x080:
            soundcnt_l_ = value;
            break;
        case 0x082:
            if (value & 0x0800U) {
                fifos_[0].clear();
            }
            if (value & 0x8000U) {
                fifos_[1].clear();
            }
            soundcnt_h_ = value & 0x770FU;
            break;
        case 0x084:
            soundcnt_x_ = value & 0x0080U;
            if ((soundcnt_x_ & 0x0080U) == 0U) {
                sq1_ = SquareChannel{};
                sq2_ = SquareChannel{};
                wave_ = WaveChannel{};
                noise_ = NoiseChannel{};
            }
            break;
        case 0x088:
            soundbias_ = value;
            break;
        default:
            break;
    }
}

void Apu::write_fifo32(int fifo_idx, uint32_t value) noexcept {
    auto& f = fifos_[fifo_idx & 1];
    f.push8(static_cast<int8_t>(value & 0xFFU));
    f.push8(static_cast<int8_t>((value >> 8U) & 0xFFU));
    f.push8(static_cast<int8_t>((value >> 16U) & 0xFFU));
    f.push8(static_cast<int8_t>((value >> 24U) & 0xFFU));
}

uint8_t Apu::on_timer_overflow(int timer_idx) noexcept {
    if ((soundcnt_x_ & 0x0080U) == 0U) {
        return 0U;
    }

    uint8_t dma_req = 0U;
    const int timer_a = (soundcnt_h_ & 0x0400U) ? 1 : 0;
    const int timer_b = (soundcnt_h_ & 0x4000U) ? 1 : 0;

    if (timer_idx == timer_a) {
        fifos_[0].pop8();
        if (fifos_[0].count <= 16U) {
            dma_req |= 1U;
        }
    }
    if (timer_idx == timer_b) {
        fifos_[1].pop8();
        if (fifos_[1].count <= 16U) {
            dma_req |= 2U;
        }
    }
    return dma_req;
}

void Apu::step_frame_sequencer() noexcept {
    // 256 Hz: Length counter (steps 0, 2, 4, 6)
    if ((frame_seq_step_ & 1U) == 0U) {
        if ((sq1_.cnt_x & 0x4000U) && sq1_.length_counter > 0U) {
            if (--sq1_.length_counter == 0U) {
                sq1_.enabled = false;
            }
        }
        if ((sq2_.cnt_x & 0x4000U) && sq2_.length_counter > 0U) {
            if (--sq2_.length_counter == 0U) {
                sq2_.enabled = false;
            }
        }
        if ((wave_.cnt_x & 0x4000U) && wave_.length_counter > 0U) {
            if (--wave_.length_counter == 0U) {
                wave_.enabled = false;
            }
        }
        if ((noise_.cnt_h & 0x4000U) && noise_.length_counter > 0U) {
            if (--noise_.length_counter == 0U) {
                noise_.enabled = false;
            }
        }
    }

    // 128 Hz: Channel 1 Frequency Sweep (steps 2, 6)
    if (frame_seq_step_ == 2U || frame_seq_step_ == 6U) {
        if (sq1_.enabled && sq1_.sweep_enabled) {
            if (sq1_.sweep_timer > 0U) {
                --sq1_.sweep_timer;
            }
            if (sq1_.sweep_timer == 0U) {
                const uint8_t sweep_period = static_cast<uint8_t>((sq1_.cnt_l >> 4U) & 7U);
                sq1_.sweep_timer = (sweep_period == 0U) ? 8U : sweep_period;
                if (sweep_period > 0U) {
                    const uint8_t shift = static_cast<uint8_t>(sq1_.cnt_l & 7U);
                    const uint16_t delta = sq1_.shadow_freq >> shift;
                    const int32_t next_freq =
                        (sq1_.cnt_l & 0x0008U)
                            ? (static_cast<int32_t>(sq1_.shadow_freq) - static_cast<int32_t>(delta))
                            : (static_cast<int32_t>(sq1_.shadow_freq) +
                               static_cast<int32_t>(delta));
                    if (next_freq > 2047) {
                        sq1_.enabled = false;
                    } else if (shift > 0U && next_freq >= 0) {
                        sq1_.shadow_freq = static_cast<uint16_t>(next_freq);
                        sq1_.cnt_x = static_cast<uint16_t>((sq1_.cnt_x & ~0x07FFU) |
                                                           (sq1_.shadow_freq & 0x07FFU));
                    }
                }
            }
        }
    }

    // 64 Hz: Volume Envelope (step 7)
    if (frame_seq_step_ == 7U) {
        auto step_env = [](uint8_t& volume, uint8_t& timer, uint16_t reg) {
            const uint8_t period = static_cast<uint8_t>((reg >> 8U) & 7U);
            if (period == 0U) {
                return;
            }
            if (timer > 0U) {
                --timer;
            }
            if (timer == 0U) {
                timer = period;
                const bool increase = (reg & 0x0800U) != 0U;
                if (increase && volume < 15U) {
                    ++volume;
                } else if (!increase && volume > 0U) {
                    --volume;
                }
            }
        };
        if (sq1_.enabled) {
            step_env(sq1_.volume, sq1_.env_timer, sq1_.cnt_h);
        }
        if (sq2_.enabled) {
            step_env(sq2_.volume, sq2_.env_timer, sq2_.cnt_h);
        }
        if (noise_.enabled) {
            step_env(noise_.volume, noise_.env_timer, noise_.cnt_l);
        }
    }

    frame_seq_step_ = (frame_seq_step_ + 1U) & 7U;
}

void Apu::step(uint32_t cycles) {
    if ((soundcnt_x_ & 0x0080U) != 0U) {
        frame_seq_cycles_ += cycles;
        while (frame_seq_cycles_ >= kFrameSequencerPeriod) {
            frame_seq_cycles_ -= kFrameSequencerPeriod;
            step_frame_sequencer();
        }

        // Advance PSG square channels
        auto step_sq = [&](SquareChannel& ch) {
            if (!ch.enabled)
                return;
            const uint32_t freq = ch.cnt_x & 0x07FFU;
            const uint32_t period = std::max<uint32_t>(16U, (2048U - freq) * 16U);
            if (cycles >= ch.phase_timer) {
                const uint32_t rem = cycles - ch.phase_timer;
                ch.duty_pos = (ch.duty_pos + 1U + static_cast<uint8_t>(rem / period)) & 7U;
                ch.phase_timer = period - (rem % period);
            } else {
                ch.phase_timer -= cycles;
            }
        };
        step_sq(sq1_);
        step_sq(sq2_);

        // Advance PSG Wave channel
        if (wave_.enabled && (wave_.cnt_l & 0x0080U) != 0U) {
            const uint32_t freq = wave_.cnt_x & 0x07FFU;
            const uint32_t period = std::max<uint32_t>(8U, (2048U - freq) * 8U);
            if (cycles >= wave_.phase_timer) {
                const uint32_t rem = cycles - wave_.phase_timer;
                const uint32_t steps = 1U + (rem / period);
                const uint8_t mask = (wave_.cnt_l & 0x0020U) ? 63U : 31U;
                wave_.wave_pos = static_cast<uint8_t>((wave_.wave_pos + steps) & mask);
                wave_.phase_timer = period - (rem % period);
            } else {
                wave_.phase_timer -= cycles;
            }
        }

        // Advance PSG Noise channel
        if (noise_.enabled) {
            const uint32_t r = noise_.cnt_h & 7U;
            const uint32_t s = (noise_.cnt_h >> 4U) & 0x0FU;
            const uint32_t div = (r == 0U) ? 32U : (r * 64U);
            const uint32_t period = std::max<uint32_t>(32U, div << s);
            uint32_t rem_cycles = cycles;
            while (rem_cycles >= noise_.phase_timer) {
                rem_cycles -= noise_.phase_timer;
                noise_.phase_timer = period;
                const uint16_t xor_bit = (noise_.lfsr ^ (noise_.lfsr >> 1U)) & 1U;
                noise_.lfsr = static_cast<uint16_t>((noise_.lfsr >> 1U) | (xor_bit << 14U));
                if (noise_.cnt_h & 0x0008U) {
                    noise_.lfsr =
                        static_cast<uint16_t>((noise_.lfsr & ~(1U << 6U)) | (xor_bit << 6U));
                }
            }
            noise_.phase_timer -= rem_cycles;
        }
    }

    sample_accum_ += static_cast<uint64_t>(cycles) * kSampleRate;
    while (sample_accum_ >= kCpuClockHz) {
        sample_accum_ -= kCpuClockHz;
        sample_frame();
    }
}

void Apu::sample_frame() noexcept {
    if ((soundcnt_x_ & 0x0080U) == 0U) {
        sample_buffer_.push_back(0);
        sample_buffer_.push_back(0);
        return;
    }

    int32_t left = 0;
    int32_t right = 0;

    // DirectSound FIFO A
    const int32_t shift_a = (soundcnt_h_ & 0x0004U) ? 2 : 1;
    const int32_t samp_a = static_cast<int32_t>(fifos_[0].latch) << shift_a;
    if (soundcnt_h_ & 0x0100U) {
        right += samp_a;
    }
    if (soundcnt_h_ & 0x0200U) {
        left += samp_a;
    }

    // DirectSound FIFO B
    const int32_t shift_b = (soundcnt_h_ & 0x0008U) ? 2 : 1;
    const int32_t samp_b = static_cast<int32_t>(fifos_[1].latch) << shift_b;
    if (soundcnt_h_ & 0x1000U) {
        right += samp_b;
    }
    if (soundcnt_h_ & 0x2000U) {
        left += samp_b;
    }

    // PSG channels 1..4
    int32_t psg_l = 0;
    int32_t psg_r = 0;
    if (sq1_.enabled && sq1_.volume > 0U) {
        const uint8_t duty = (sq1_.cnt_h >> 6U) & 3U;
        const int32_t v = kDutyTable[duty][sq1_.duty_pos] ? static_cast<int32_t>(sq1_.volume)
                                                          : -static_cast<int32_t>(sq1_.volume);
        if (soundcnt_l_ & 0x0100U)
            psg_r += v;
        if (soundcnt_l_ & 0x1000U)
            psg_l += v;
    }
    if (sq2_.enabled && sq2_.volume > 0U) {
        const uint8_t duty = (sq2_.cnt_h >> 6U) & 3U;
        const int32_t v = kDutyTable[duty][sq2_.duty_pos] ? static_cast<int32_t>(sq2_.volume)
                                                          : -static_cast<int32_t>(sq2_.volume);
        if (soundcnt_l_ & 0x0200U)
            psg_r += v;
        if (soundcnt_l_ & 0x2000U)
            psg_l += v;
    }
    if (wave_.enabled && (wave_.cnt_l & 0x0080U) != 0U) {
        uint8_t bank = (wave_.cnt_l >> 6U) & 1U;
        uint8_t step_idx = wave_.wave_pos & 31U;
        if (wave_.cnt_l & 0x0020U) {
            bank = (wave_.wave_pos >> 5U) & 1U;
        }
        const uint8_t byte_val = wave_ram_[bank][step_idx >> 1U];
        const uint8_t nibble =
            (step_idx & 1U) ? (byte_val & 0x0FU) : static_cast<uint8_t>((byte_val >> 4U) & 0x0FU);
        int32_t centered = static_cast<int32_t>(nibble) - 8;
        if (wave_.cnt_h & 0x8000U) {
            centered = (centered * 3) >> 2;  // 75%
        } else {
            switch ((wave_.cnt_h >> 13U) & 3U) {
                case 0U:
                    centered = 0;
                    break;
                case 1U:
                    break;  // 100%
                case 2U:
                    centered >>= 1;  // 50%
                    break;
                case 3U:
                    centered >>= 2;  // 25%
                    break;
            }
        }
        if (soundcnt_l_ & 0x0400U)
            psg_r += centered;
        if (soundcnt_l_ & 0x4000U)
            psg_l += centered;
    }
    if (noise_.enabled && noise_.volume > 0U) {
        const int32_t v = ((noise_.lfsr & 1U) == 0U) ? static_cast<int32_t>(noise_.volume)
                                                     : -static_cast<int32_t>(noise_.volume);
        if (soundcnt_l_ & 0x0800U)
            psg_r += v;
        if (soundcnt_l_ & 0x8000U)
            psg_l += v;
    }

    const int32_t vol_r = (soundcnt_l_ & 7U) + 1;
    const int32_t vol_l = ((soundcnt_l_ >> 4U) & 7U) + 1;
    // SOUNDCNT_H bits 0-1: PSG mixing ratio (0 = 25%, 1 = 50%, 2 = 100%)
    const uint32_t psg_ratio = soundcnt_h_ & 3U;
    const int32_t psg_shift = (psg_ratio == 0U) ? 3 : ((psg_ratio == 1U) ? 2 : 1);
    right += (psg_r * vol_r) >> psg_shift;
    left += (psg_l * vol_l) >> psg_shift;

    // The hardware mixer output is a 10-bit signed value (-512..+511): saturate there, exactly like
    // the real DAC, then scale to 16-bit. A gain of 48 (75% of full scale) leaves headroom so that
    // two 100% DirectSound channels plus PSG do not slam into the int16 rails (which sounded like
    // harsh clipping/distortion at the previous gain of 64).
    constexpr int32_t kMixGain = 48;
    const int32_t out_l = std::clamp<int32_t>(left, -512, 511) * kMixGain;
    const int32_t out_r = std::clamp<int32_t>(right, -512, 511) * kMixGain;

    sample_buffer_.push_back(static_cast<int16_t>(out_l));
    sample_buffer_.push_back(static_cast<int16_t>(out_r));
}

}  // namespace dragon::gba
