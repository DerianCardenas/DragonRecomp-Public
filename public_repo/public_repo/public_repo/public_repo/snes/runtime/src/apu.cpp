#include "snes/apu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace dragon::snes {

namespace {

// Standard Nintendo SPC700 IPL Boot ROM (64 bytes mapped at $FFC0-$FFFF).
// Responds with $AA,$BB on ports $F4,$F5, handshakes with $CC on $F4, transfers
// code/data blocks, and jumps to uploaded entry point!
constexpr std::array<uint8_t, 64> kSpcIplRom = {
    {0xCD, 0xEF, 0xBD, 0xE8, 0x00, 0xC6, 0x1D, 0xD0, 0xFC, 0x8F, 0xAA, 0xF4, 0x8F,
     0xBB, 0xF5, 0x78, 0xCC, 0xF4, 0xD0, 0xFB, 0x2F, 0x19, 0xEB, 0xF4, 0xD0, 0xFC,
     0x7E, 0xF4, 0xD0, 0x0B, 0xE4, 0xF5, 0xCB, 0xF4, 0xD7, 0x00, 0xFC, 0xD0, 0xF3,
     0xAB, 0x01, 0x10, 0xEF, 0x7E, 0xF4, 0x10, 0xEB, 0xBA, 0xF6, 0xDA, 0x00, 0xBA,
     0xF4, 0xC4, 0xF4, 0xDD, 0x5D, 0xD0, 0xDB, 0x1F, 0x00, 0x00, 0xC0, 0xFF}};

inline int16_t clamp16(int32_t val) noexcept {
    if (val > 32767) {
        return 32767;
    }
    if (val < -32768) {
        return -32768;
    }
    return static_cast<int16_t>(val);
}

}  // namespace

Apu::Apu() noexcept {
    reset();
}

void Apu::reset() noexcept {
    aram_.fill(0U);

    pc_ = 0xFFC0U;
    a_ = 0U;
    x_ = 0U;
    y_ = 0U;
    sp_ = 0xEFU;
    psw_ = 0U;
    halted_ = false;

    ipl_rom_enabled_ = true;
    dsp_addr_ = 0U;
    cpu_to_spc_.fill(0U);
    spc_to_cpu_.fill(0U);

    timers_[0] = Timer{.target = 0U,
                       .counter = 0U,
                       .divider = 0U,
                       .divider_max = 128U,
                       .stage1 = 0U,
                       .enabled = false};
    timers_[1] = Timer{.target = 0U,
                       .counter = 0U,
                       .divider = 0U,
                       .divider_max = 128U,
                       .stage1 = 0U,
                       .enabled = false};
    timers_[2] = Timer{.target = 0U,
                       .counter = 0U,
                       .divider = 0U,
                       .divider_max = 16U,
                       .stage1 = 0U,
                       .enabled = false};

    dsp_regs_.fill(0U);
    for (auto& v : voices_) {
        v = DspVoice{};
    }
    mvol_l_ = 127;
    mvol_r_ = 127;
    dir_page_ = 0U;
    endx_ = 0U;

    spc_cycles_executed_ = 0ULL;
    last_cpu_master_cycles_ = 0ULL;

    resample_phase_ = 0U;
    current_dsp_l_ = 0;
    current_dsp_r_ = 0;
}

uint8_t Apu::spc_read(uint16_t addr) noexcept {
    if (addr >= 0xFFC0U && ipl_rom_enabled_) {
        return kSpcIplRom[addr - 0xFFC0U];
    }
    if (addr == 0x00F0U || addr == 0x00F1U) {
        return 0U;
    }
    if (addr == 0x00F2U) {
        return dsp_addr_;
    }
    if (addr == 0x00F3U) {
        return read_dsp_register(static_cast<uint8_t>(dsp_addr_ & 0x7FU));
    }
    if (addr >= 0x00F4U && addr <= 0x00F7U) {
        return cpu_to_spc_[addr - 0x00F4U];
    }
    if (addr == 0x00FDU) {
        const uint8_t val = timers_[0].counter;
        timers_[0].counter = 0U;
        return val;
    }
    if (addr == 0x00FEU) {
        const uint8_t val = timers_[1].counter;
        timers_[1].counter = 0U;
        return val;
    }
    if (addr == 0x00FFU) {
        const uint8_t val = timers_[2].counter;
        timers_[2].counter = 0U;
        return val;
    }
    return aram_[addr];
}

void Apu::spc_write(uint16_t addr, uint8_t val) noexcept {
    aram_[addr] = val;

    if (addr == 0x00F1U) {
        timers_[0].enabled = (val & 0x01U) != 0U;
        timers_[1].enabled = (val & 0x02U) != 0U;
        timers_[2].enabled = (val & 0x04U) != 0U;
        if ((val & 0x10U) != 0U) {
            cpu_to_spc_[0] = 0U;
            cpu_to_spc_[1] = 0U;
        }
        if ((val & 0x20U) != 0U) {
            cpu_to_spc_[2] = 0U;
            cpu_to_spc_[3] = 0U;
        }
        ipl_rom_enabled_ = (val & 0x80U) != 0U;
        return;
    }
    if (addr == 0x00F2U) {
        dsp_addr_ = val;
        return;
    }
    if (addr == 0x00F3U) {
        write_dsp_register(static_cast<uint8_t>(dsp_addr_ & 0x7FU), val);
        return;
    }
    if (addr >= 0x00F4U && addr <= 0x00F7U) {
        spc_to_cpu_[addr - 0x00F4U] = val;
        return;
    }
    if (addr == 0x00FAU) {
        timers_[0].target = val;
        return;
    }
    if (addr == 0x00FBU) {
        timers_[1].target = val;
        return;
    }
    if (addr == 0x00FCU) {
        timers_[2].target = val;
        return;
    }
}

uint16_t Apu::spc_read16(uint16_t addr) noexcept {
    const uint8_t lo = spc_read(addr);
    const uint8_t hi = spc_read(static_cast<uint16_t>(addr + 1U));
    return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
}

void Apu::spc_write16(uint16_t addr, uint16_t val) noexcept {
    spc_write(addr, static_cast<uint8_t>(val & 0xFFU));
    spc_write(static_cast<uint16_t>(addr + 1U), static_cast<uint8_t>(val >> 8U));
}

uint8_t Apu::spc_pop() noexcept {
    sp_++;
    return aram_[0x0100U | sp_];
}

void Apu::spc_push(uint8_t val) noexcept {
    aram_[0x0100U | sp_] = val;
    sp_--;
}

uint8_t Apu::read_port(uint8_t port) noexcept {
    return spc_to_cpu_[port & 0x03U];
}

void Apu::write_port(uint8_t port, uint8_t value) noexcept {
    cpu_to_spc_[port & 0x03U] = value;
}

void Apu::catchup(uint64_t cpu_master_cycles) noexcept {
    if (cpu_master_cycles <= last_cpu_master_cycles_) {
        return;
    }
    const uint64_t delta_master = cpu_master_cycles - last_cpu_master_cycles_;
    last_cpu_master_cycles_ = cpu_master_cycles;

    // 1 SPC cycle ~= 20.9739 CPU master cycles.
    // Using integer rational: (delta_master * 1024) / 21477
    const uint64_t spc_cycles = (delta_master * 1024ULL) / 21477ULL;
    if (spc_cycles > 0ULL) {
        step_cycles(spc_cycles);
    }
}

void Apu::step_timers(uint8_t cycles) noexcept {
    for (auto& t : timers_) {
        if (!t.enabled) {
            continue;
        }
        t.divider = static_cast<uint16_t>(t.divider + cycles);
        while (t.divider >= t.divider_max) {
            t.divider = static_cast<uint16_t>(t.divider - t.divider_max);
            t.stage1++;
            const uint16_t target_eff = (t.target == 0U) ? 256U : t.target;
            if (t.stage1 >= target_eff) {
                t.stage1 = 0U;
                t.counter = static_cast<uint8_t>((t.counter + 1U) & 0x0FU);
            }
        }
    }
}

void Apu::write_dsp_register(uint8_t reg, uint8_t val) noexcept {
    dsp_regs_[reg & 0x7FU] = val;

    if (reg < 0x80U) {
        const size_t v = (reg >> 4U) & 0x07U;
        switch (reg & 0x0FU) {
            case 0x00U:
                voices_[v].vol_l = static_cast<int8_t>(val);
                break;
            case 0x01U:
                voices_[v].vol_r = static_cast<int8_t>(val);
                break;
            case 0x02U:
                voices_[v].pitch = static_cast<uint16_t>((voices_[v].pitch & 0x3F00U) | val);
                break;
            case 0x03U:
                voices_[v].pitch = static_cast<uint16_t>(
                    (voices_[v].pitch & 0x00FFU) | ((static_cast<uint16_t>(val) & 0x3FU) << 8U));
                break;
            case 0x04U:
                voices_[v].srcn = val;
                break;
            case 0x05U:
                voices_[v].adsr1 = val;
                break;
            case 0x06U:
                voices_[v].adsr2 = val;
                break;
            case 0x07U:
                voices_[v].gain = val;
                break;
            default:
                break;
        }
    }

    switch (reg) {
        case 0x0CU:
            mvol_l_ = static_cast<int8_t>(val);
            break;
        case 0x1CU:
            mvol_r_ = static_cast<int8_t>(val);
            break;
        case 0x4CU:
            key_on(val);
            break;
        case 0x5CU:
            for (size_t i = 0; i < kVoiceCount; ++i) {
                if ((val & (1U << i)) != 0U) {
                    voices_[i].env_state = DspVoice::EnvState::Release;
                }
            }
            break;
        case 0x5DU:
            dir_page_ = val;
            break;
        default:
            break;
    }
}

uint8_t Apu::read_dsp_register(uint8_t reg) noexcept {
    reg &= 0x7FU;
    if ((reg & 0x0FU) == 0x08U) {
        const size_t v = (reg >> 4U) & 0x07U;
        return voices_[v].envx;
    }
    if ((reg & 0x0FU) == 0x09U) {
        const size_t v = (reg >> 4U) & 0x07U;
        return static_cast<uint8_t>(voices_[v].outx);
    }
    if (reg == 0x7CU) {
        return endx_;
    }
    return dsp_regs_[reg];
}

void Apu::key_on(uint8_t voice_mask) noexcept {
    for (size_t i = 0; i < kVoiceCount; ++i) {
        if ((voice_mask & (1U << i)) == 0U) {
            continue;
        }
        DspVoice& v = voices_[i];
        v.active = true;
        v.env_state = DspVoice::EnvState::Attack;
        v.env_level = 0;
        v.sample_pos = 0U;
        v.frac_pos = 0U;
        v.prev1 = 0;
        v.prev2 = 0;
        endx_ &= static_cast<uint8_t>(~(1U << i));

        const uint16_t dir_addr =
            static_cast<uint16_t>((static_cast<uint16_t>(dir_page_) << 8U) + v.srcn * 4U);
        v.brr_block_addr = static_cast<uint16_t>(aram_[dir_addr] | (aram_[dir_addr + 1U] << 8U));
        v.loop_addr = static_cast<uint16_t>(aram_[dir_addr + 2U] | (aram_[dir_addr + 3U] << 8U));

        decode_next_brr_block(v);
    }
}

void Apu::decode_next_brr_block(DspVoice& v) noexcept {
    const uint8_t header = aram_[v.brr_block_addr];
    const uint8_t shift = header >> 4U;
    const uint8_t filter = (header >> 2U) & 0x03U;
    const bool loop_flag = (header & 0x02U) != 0U;
    const bool end_flag = (header & 0x01U) != 0U;

    for (size_t b = 0; b < 8U; ++b) {
        const uint8_t data_byte = aram_[static_cast<uint16_t>(v.brr_block_addr + 1U + b)];
        const int8_t n0 = static_cast<int8_t>(((data_byte >> 4U) & 0x0FU) |
                                              (((data_byte & 0x80U) != 0U) ? 0xF0 : 0x00));
        const int8_t n1 =
            static_cast<int8_t>((data_byte & 0x0FU) | (((data_byte & 0x08U) != 0U) ? 0xF0 : 0x00));

        const std::array<int8_t, 2> nibbles = {n0, n1};
        for (size_t n = 0; n < 2U; ++n) {
            int32_t s = 0;
            if (shift <= 12U) {
                s = (static_cast<int32_t>(nibbles[n]) << shift) >> 1;
            } else {
                s = (nibbles[n] < 0) ? -2048 : 0;
            }

            int32_t sample = s;
            const int32_t p1 = static_cast<int32_t>(v.prev1);
            const int32_t p2 = static_cast<int32_t>(v.prev2);
            switch (filter) {
                case 0:
                    sample = s;
                    break;
                case 1:
                    sample = s + p1 + (-p1 >> 4);
                    break;
                case 2:
                    sample = s + (p1 * 2) + (-(p1 * 3) >> 5) - p2 + (p2 >> 4);
                    break;
                case 3:
                    sample = s + (p1 * 2) + (-(p1 * 13) >> 6) - p2 + ((p2 * 3) >> 4);
                    break;
                default:
                    break;
            }

            v.prev2 = v.prev1;
            v.prev1 = clamp16(sample);
            v.decoded_samples[b * 2U + n] = v.prev1;
        }
    }

    if (end_flag) {
        if (loop_flag) {
            v.brr_block_addr = v.loop_addr;
        } else {
            v.active = false;
            endx_ |= static_cast<uint8_t>(1U << (&v - voices_.data()));
        }
    } else {
        v.brr_block_addr = static_cast<uint16_t>(v.brr_block_addr + 9U);
    }
}

void Apu::step_dsp_32k() noexcept {
    int32_t mix_l = 0;
    int32_t mix_r = 0;

    for (auto& v : voices_) {
        if (!v.active) {
            continue;
        }

        // Envelope update (simplified ADSR model)
        switch (v.env_state) {
            case DspVoice::EnvState::Attack:
                v.env_level += 64;
                if (v.env_level >= 0x7FF) {
                    v.env_level = 0x7FF;
                    v.env_state = DspVoice::EnvState::Decay;
                }
                break;
            case DspVoice::EnvState::Decay:
                v.env_level -= 16;
                if (v.env_level <= 0x400) {
                    v.env_level = 0x400;
                    v.env_state = DspVoice::EnvState::Sustain;
                }
                break;
            case DspVoice::EnvState::Sustain:
                // Keep steady sustain
                break;
            case DspVoice::EnvState::Release:
                v.env_level -= 32;
                if (v.env_level <= 0) {
                    v.env_level = 0;
                    v.active = false;
                }
                break;
        }
        v.envx = static_cast<uint8_t>((v.env_level >> 4) & 0x7FU);

        // Fetch sample
        const int16_t raw_sample = v.decoded_samples[v.sample_pos & 0x0FU];
        const int32_t env_sample = (static_cast<int32_t>(raw_sample) * (v.env_level >> 4)) >> 7;
        v.outx = static_cast<int8_t>(env_sample >> 8);

        mix_l += (env_sample * static_cast<int32_t>(v.vol_l)) >> 7;
        mix_r += (env_sample * static_cast<int32_t>(v.vol_r)) >> 7;

        // Advance sample pointer by pitch (0x1000 = 1.0 sample / 32 kHz step)
        v.frac_pos += (v.pitch == 0U) ? 0x1000U : v.pitch;
        while (v.frac_pos >= 0x1000U) {
            v.frac_pos -= 0x1000U;
            v.sample_pos++;
            if (v.sample_pos >= 16U) {
                v.sample_pos = 0U;
                if (v.active) {
                    decode_next_brr_block(v);
                }
            }
        }
    }

    current_dsp_l_ = clamp16((mix_l * static_cast<int32_t>(mvol_l_)) >> 7);
    current_dsp_r_ = clamp16((mix_r * static_cast<int32_t>(mvol_r_)) >> 7);
}

void Apu::render_samples(std::span<int16_t> out_stereo) noexcept {
    const size_t sample_pairs = out_stereo.size() / 2U;
    for (size_t i = 0; i < sample_pairs; ++i) {
        // Resample from 32 kHz to 48 kHz
        resample_phase_ += kDspSampleRate;
        while (resample_phase_ >= kOutputSampleRate) {
            resample_phase_ -= kOutputSampleRate;
            step_dsp_32k();
        }
        out_stereo[i * 2U + 0U] = current_dsp_l_;
        out_stereo[i * 2U + 1U] = current_dsp_r_;
    }
}

void Apu::step_spc_instruction() noexcept {
    const uint8_t op = spc_read(pc_++);
    uint8_t cycles = 2U;

    auto dp_addr = [this](uint8_t offset) noexcept -> uint16_t {
        return static_cast<uint16_t>(((psw_ & 0x20U) ? 0x0100U : 0x0000U) | offset);
    };

    auto dp_ind_x = [this, dp_addr](uint8_t d) noexcept -> uint16_t {
        const uint8_t lo = static_cast<uint8_t>(d + x_);
        const uint8_t hi = static_cast<uint8_t>(lo + 1U);
        return static_cast<uint16_t>(spc_read(dp_addr(lo)) | (spc_read(dp_addr(hi)) << 8U));
    };

    auto dp_ind_y = [this, dp_addr](uint8_t d) noexcept -> uint16_t {
        const uint16_t ptr = static_cast<uint16_t>(
            spc_read(dp_addr(d)) | (spc_read(dp_addr(static_cast<uint8_t>(d + 1U))) << 8U));
        return static_cast<uint16_t>(ptr + y_);
    };

    auto set_nz = [this](uint8_t val) noexcept {
        if (val == 0U) {
            psw_ |= 0x02U;
        } else {
            psw_ &= ~0x02U;
        }
        if ((val & 0x80U) != 0U) {
            psw_ |= 0x80U;
        } else {
            psw_ &= ~0x80U;
        }
    };

    auto set_nz16 = [this](uint16_t val) noexcept {
        if (val == 0U) {
            psw_ |= 0x02U;
        } else {
            psw_ &= ~0x02U;
        }
        if ((val & 0x8000U) != 0U) {
            psw_ |= 0x80U;
        } else {
            psw_ &= ~0x80U;
        }
    };

    auto ya = [this]() noexcept -> uint16_t {
        return static_cast<uint16_t>((static_cast<uint16_t>(y_) << 8U) | a_);
    };

    auto set_ya = [this, set_nz16](uint16_t val) noexcept {
        a_ = static_cast<uint8_t>(val & 0xFFU);
        y_ = static_cast<uint8_t>(val >> 8U);
        set_nz16(val);
    };

    auto do_adc_val = [this, set_nz](uint8_t dest, uint8_t val) noexcept -> uint8_t {
        const uint16_t sum = static_cast<uint16_t>(dest) + val + ((psw_ & 1U) ? 1U : 0U);
        const bool v = (~(dest ^ val) & (dest ^ sum) & 0x80U) != 0U;
        const bool h = ((dest & 0x0FU) + (val & 0x0FU) + ((psw_ & 1U) ? 1U : 0U)) > 0x0FU;
        if (v)
            psw_ |= 0x40U;
        else
            psw_ &= ~0x40U;
        if (h)
            psw_ |= 0x08U;
        else
            psw_ &= ~0x08U;
        if (sum > 0xFFU)
            psw_ |= 0x01U;
        else
            psw_ &= ~0x01U;
        const uint8_t res = static_cast<uint8_t>(sum & 0xFFU);
        set_nz(res);
        return res;
    };

    auto do_adc = [this, do_adc_val](uint8_t val) noexcept { a_ = do_adc_val(a_, val); };

    auto do_sbc_val = [this, set_nz](uint8_t dest, uint8_t val) noexcept -> uint8_t {
        const uint16_t diff = static_cast<uint16_t>(dest) - val - ((psw_ & 1U) ? 0U : 1U);
        const bool v = ((dest ^ val) & (dest ^ diff) & 0x80U) != 0U;
        const bool h = (dest & 0x0FU) < (val & 0x0FU) + ((psw_ & 1U) ? 0U : 1U);
        if (v)
            psw_ |= 0x40U;
        else
            psw_ &= ~0x40U;
        if (!h)
            psw_ |= 0x08U;
        else
            psw_ &= ~0x08U;
        if (diff <= 0xFFU)
            psw_ |= 0x01U;
        else
            psw_ &= ~0x01U;
        const uint8_t res = static_cast<uint8_t>(diff & 0xFFU);
        set_nz(res);
        return res;
    };

    auto do_sbc = [this, do_sbc_val](uint8_t val) noexcept { a_ = do_sbc_val(a_, val); };

    auto do_cmp = [this, set_nz](uint8_t reg, uint8_t val) noexcept {
        const uint16_t diff = static_cast<uint16_t>(reg) - val;
        if (diff <= 0xFFU)
            psw_ |= 0x01U;
        else
            psw_ &= ~0x01U;
        set_nz(static_cast<uint8_t>(diff & 0xFFU));
    };

    auto do_asl = [this, set_nz](uint8_t val) noexcept -> uint8_t {
        if ((val & 0x80U) != 0U)
            psw_ |= 0x01U;
        else
            psw_ &= ~0x01U;
        const uint8_t res = static_cast<uint8_t>(val << 1U);
        set_nz(res);
        return res;
    };

    auto do_lsr = [this, set_nz](uint8_t val) noexcept -> uint8_t {
        if ((val & 0x01U) != 0U)
            psw_ |= 0x01U;
        else
            psw_ &= ~0x01U;
        const uint8_t res = static_cast<uint8_t>(val >> 1U);
        set_nz(res);
        return res;
    };

    auto do_rol = [this, set_nz](uint8_t val) noexcept -> uint8_t {
        const uint8_t old_c = (psw_ & 1U) ? 1U : 0U;
        if ((val & 0x80U) != 0U)
            psw_ |= 0x01U;
        else
            psw_ &= ~0x01U;
        const uint8_t res = static_cast<uint8_t>((val << 1U) | old_c);
        set_nz(res);
        return res;
    };

    auto do_ror = [this, set_nz](uint8_t val) noexcept -> uint8_t {
        const uint8_t old_c = (psw_ & 1U) ? 0x80U : 0x00U;
        if ((val & 0x01U) != 0U)
            psw_ |= 0x01U;
        else
            psw_ &= ~0x01U;
        const uint8_t res = static_cast<uint8_t>((val >> 1U) | old_c);
        set_nz(res);
        return res;
    };

    auto read_membit = [this](uint16_t& out_addr, uint8_t& out_bit) noexcept -> bool {
        const uint16_t operand = spc_read16(pc_);
        pc_ += 2U;
        out_bit = static_cast<uint8_t>(operand >> 13U);
        out_addr = static_cast<uint16_t>(operand & 0x1FFFU);
        return (spc_read(out_addr) & (1U << out_bit)) != 0U;
    };

    switch (op) {
        // --- 0x00 - 0x0F ---
        case 0x00U:  // NOP
            cycles = 2U;
            break;
        case 0x01U:    // TCALL 0
        case 0x11U:    // TCALL 1
        case 0x21U:    // TCALL 2
        case 0x31U:    // TCALL 3
        case 0x41U:    // TCALL 4
        case 0x51U:    // TCALL 5
        case 0x61U:    // TCALL 6
        case 0x71U:    // TCALL 7
        case 0x81U:    // TCALL 8
        case 0x91U:    // TCALL 9
        case 0xA1U:    // TCALL 10
        case 0xB1U:    // TCALL 11
        case 0xC1U:    // TCALL 12
        case 0xD1U:    // TCALL 13
        case 0xE1U:    // TCALL 14
        case 0xF1U: {  // TCALL 15
            const uint8_t idx = static_cast<uint8_t>(op >> 4U);
            spc_push(static_cast<uint8_t>(pc_ >> 8U));
            spc_push(static_cast<uint8_t>(pc_ & 0xFFU));
            pc_ = spc_read16(static_cast<uint16_t>(0xFFDEU - (idx * 2U)));
            cycles = 8U;
            break;
        }
        case 0x02U:    // SET1 dp.0
        case 0x22U:    // SET1 dp.1
        case 0x42U:    // SET1 dp.2
        case 0x62U:    // SET1 dp.3
        case 0x82U:    // SET1 dp.4
        case 0xA2U:    // SET1 dp.5
        case 0xC2U:    // SET1 dp.6
        case 0xE2U: {  // SET1 dp.7
            const uint8_t bit = static_cast<uint8_t>((op >> 5U) & 7U);
            const uint16_t addr = dp_addr(spc_read(pc_++));
            spc_write(addr, static_cast<uint8_t>(spc_read(addr) | (1U << bit)));
            cycles = 4U;
            break;
        }
        case 0x03U:    // BBS dp.0, rel
        case 0x23U:    // BBS dp.1, rel
        case 0x43U:    // BBS dp.2, rel
        case 0x63U:    // BBS dp.3, rel
        case 0x83U:    // BBS dp.4, rel
        case 0xA3U:    // BBS dp.5, rel
        case 0xC3U:    // BBS dp.6, rel
        case 0xE3U: {  // BBS dp.7, rel
            const uint8_t bit = static_cast<uint8_t>((op >> 5U) & 7U);
            const uint8_t d = spc_read(pc_++);
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            if ((spc_read(dp_addr(d)) & (1U << bit)) != 0U) {
                pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
                cycles = 7U;
            } else {
                cycles = 5U;
            }
            break;
        }
        case 0x04U: {  // OR A, dp
            const uint8_t d = spc_read(pc_++);
            a_ |= spc_read(dp_addr(d));
            set_nz(a_);
            cycles = 3U;
            break;
        }
        case 0x05U: {  // OR A, abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            a_ |= spc_read(addr);
            set_nz(a_);
            cycles = 4U;
            break;
        }
        case 0x06U: {  // OR A, (X)
            a_ |= spc_read(dp_addr(x_));
            set_nz(a_);
            cycles = 3U;
            break;
        }
        case 0x07U: {  // OR A, [dp+X]
            const uint8_t d = spc_read(pc_++);
            a_ |= spc_read(dp_ind_x(d));
            set_nz(a_);
            cycles = 6U;
            break;
        }
        case 0x08U:  // OR A, #imm
            a_ |= spc_read(pc_++);
            set_nz(a_);
            cycles = 2U;
            break;
        case 0x09U: {  // OR dp, dp
            const uint8_t src_d = spc_read(pc_++);
            const uint16_t dst_addr = dp_addr(spc_read(pc_++));
            const uint8_t res = static_cast<uint8_t>(spc_read(dst_addr) | spc_read(dp_addr(src_d)));
            spc_write(dst_addr, res);
            set_nz(res);
            cycles = 6U;
            break;
        }
        case 0x0AU: {  // OR1 C, membit
            uint16_t addr = 0;
            uint8_t bit = 0;
            if (read_membit(addr, bit)) {
                psw_ |= 0x01U;
            }
            cycles = 5U;
            break;
        }
        case 0x0BU: {  // ASL dp
            const uint16_t addr = dp_addr(spc_read(pc_++));
            spc_write(addr, do_asl(spc_read(addr)));
            cycles = 4U;
            break;
        }
        case 0x0CU: {  // ASL abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            spc_write(addr, do_asl(spc_read(addr)));
            cycles = 5U;
            break;
        }
        case 0x0DU:  // PUSH PSW
            spc_push(psw_);
            cycles = 4U;
            break;
        case 0x0EU: {  // TSET1 abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            const uint8_t val = spc_read(addr);
            set_nz(static_cast<uint8_t>(a_ - val));
            spc_write(addr, static_cast<uint8_t>(val | a_));
            cycles = 6U;
            break;
        }
        case 0x0FU: {  // BRK
            spc_push(static_cast<uint8_t>(pc_ >> 8U));
            spc_push(static_cast<uint8_t>(pc_ & 0xFFU));
            spc_push(psw_);
            psw_ |= 0x04U;
            psw_ &= ~0x20U;
            pc_ = spc_read16(0xFFDEU);
            cycles = 8U;
            break;
        }

        // --- 0x10 - 0x1F ---
        case 0x10U: {  // BPL rel
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            if ((psw_ & 0x80U) == 0U) {
                pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
                cycles = 4U;
            } else {
                cycles = 2U;
            }
            break;
        }
        case 0x12U:    // CLR1 dp.0
        case 0x32U:    // CLR1 dp.1
        case 0x52U:    // CLR1 dp.2
        case 0x72U:    // CLR1 dp.3
        case 0x92U:    // CLR1 dp.4
        case 0xB2U:    // CLR1 dp.5
        case 0xD2U:    // CLR1 dp.6
        case 0xF2U: {  // CLR1 dp.7
            const uint8_t bit = static_cast<uint8_t>((op >> 5U) & 7U);
            const uint16_t addr = dp_addr(spc_read(pc_++));
            spc_write(addr, static_cast<uint8_t>(spc_read(addr) & ~(1U << bit)));
            cycles = 4U;
            break;
        }
        case 0x13U:    // BBC dp.0, rel
        case 0x33U:    // BBC dp.1, rel
        case 0x53U:    // BBC dp.2, rel
        case 0x73U:    // BBC dp.3, rel
        case 0x93U:    // BBC dp.4, rel
        case 0xB3U:    // BBC dp.5, rel
        case 0xD3U:    // BBC dp.6, rel
        case 0xF3U: {  // BBC dp.7, rel
            const uint8_t bit = static_cast<uint8_t>((op >> 5U) & 7U);
            const uint8_t d = spc_read(pc_++);
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            if ((spc_read(dp_addr(d)) & (1U << bit)) == 0U) {
                pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
                cycles = 7U;
            } else {
                cycles = 5U;
            }
            break;
        }
        case 0x14U: {  // OR A, dp+X
            const uint8_t d = spc_read(pc_++);
            a_ |= spc_read(dp_addr(static_cast<uint8_t>(d + x_)));
            set_nz(a_);
            cycles = 4U;
            break;
        }
        case 0x15U: {  // OR A, abs+X
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + x_);
            pc_ += 2U;
            a_ |= spc_read(addr);
            set_nz(a_);
            cycles = 5U;
            break;
        }
        case 0x16U: {  // OR A, abs+Y
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + y_);
            pc_ += 2U;
            a_ |= spc_read(addr);
            set_nz(a_);
            cycles = 5U;
            break;
        }
        case 0x17U: {  // OR A, [dp]+Y
            const uint8_t d = spc_read(pc_++);
            a_ |= spc_read(dp_ind_y(d));
            set_nz(a_);
            cycles = 6U;
            break;
        }
        case 0x18U: {  // OR dp, #imm
            const uint8_t imm = spc_read(pc_++);
            const uint16_t dst = dp_addr(spc_read(pc_++));
            const uint8_t res = static_cast<uint8_t>(spc_read(dst) | imm);
            spc_write(dst, res);
            set_nz(res);
            cycles = 5U;
            break;
        }
        case 0x19U: {  // OR (X), (Y)
            const uint16_t dst = dp_addr(x_);
            const uint8_t res = static_cast<uint8_t>(spc_read(dst) | spc_read(dp_addr(y_)));
            spc_write(dst, res);
            set_nz(res);
            cycles = 5U;
            break;
        }
        case 0x1AU: {  // DECW dp
            const uint8_t d = spc_read(pc_++);
            const uint16_t a_lo = dp_addr(d);
            const uint16_t a_hi = dp_addr(static_cast<uint8_t>(d + 1U));
            uint16_t v = static_cast<uint16_t>(spc_read(a_lo) | (spc_read(a_hi) << 8U));
            v--;
            spc_write(a_lo, static_cast<uint8_t>(v & 0xFFU));
            spc_write(a_hi, static_cast<uint8_t>(v >> 8U));
            set_nz16(v);
            cycles = 6U;
            break;
        }
        case 0x1BU: {  // ASL dp+X
            const uint16_t addr = dp_addr(static_cast<uint8_t>(spc_read(pc_++) + x_));
            spc_write(addr, do_asl(spc_read(addr)));
            cycles = 5U;
            break;
        }
        case 0x1CU:  // ASL A
            a_ = do_asl(a_);
            cycles = 2U;
            break;
        case 0x1DU:  // DEC X
            x_--;
            set_nz(x_);
            cycles = 2U;
            break;
        case 0x1EU: {  // CMP X, abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            do_cmp(x_, spc_read(addr));
            cycles = 4U;
            break;
        }
        case 0x1FU: {  // JMP [abs+X]
            const uint16_t ptr = static_cast<uint16_t>(spc_read16(pc_) + x_);
            pc_ = spc_read16(ptr);
            cycles = 6U;
            break;
        }

        // --- 0x20 - 0x2F ---
        case 0x20U:  // CLRP
            psw_ &= ~0x20U;
            cycles = 2U;
            break;
        case 0x24U: {  // AND A, dp
            const uint8_t d = spc_read(pc_++);
            a_ &= spc_read(dp_addr(d));
            set_nz(a_);
            cycles = 3U;
            break;
        }
        case 0x25U: {  // AND A, abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            a_ &= spc_read(addr);
            set_nz(a_);
            cycles = 4U;
            break;
        }
        case 0x26U: {  // AND A, (X)
            a_ &= spc_read(dp_addr(x_));
            set_nz(a_);
            cycles = 3U;
            break;
        }
        case 0x27U: {  // AND A, [dp+X]
            const uint8_t d = spc_read(pc_++);
            a_ &= spc_read(dp_ind_x(d));
            set_nz(a_);
            cycles = 6U;
            break;
        }
        case 0x28U:  // AND A, #imm
            a_ &= spc_read(pc_++);
            set_nz(a_);
            cycles = 2U;
            break;
        case 0x29U: {  // AND dp, dp
            const uint8_t src_d = spc_read(pc_++);
            const uint16_t dst_addr = dp_addr(spc_read(pc_++));
            const uint8_t res = static_cast<uint8_t>(spc_read(dst_addr) & spc_read(dp_addr(src_d)));
            spc_write(dst_addr, res);
            set_nz(res);
            cycles = 6U;
            break;
        }
        case 0x2AU: {  // OR1 C, /membit
            uint16_t addr = 0;
            uint8_t bit = 0;
            if (!read_membit(addr, bit)) {
                psw_ |= 0x01U;
            }
            cycles = 5U;
            break;
        }
        case 0x2BU: {  // ROL dp
            const uint16_t addr = dp_addr(spc_read(pc_++));
            spc_write(addr, do_rol(spc_read(addr)));
            cycles = 4U;
            break;
        }
        case 0x2CU: {  // ROL abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            spc_write(addr, do_rol(spc_read(addr)));
            cycles = 5U;
            break;
        }
        case 0x2DU:  // PUSH A
            spc_push(a_);
            cycles = 4U;
            break;
        case 0x2EU: {  // CBNE dp, rel
            const uint8_t d = spc_read(pc_++);
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            if (a_ != spc_read(dp_addr(d))) {
                pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
                cycles = 5U;
            } else {
                cycles = 3U;
            }
            break;
        }
        case 0x2FU: {  // BRA rel
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
            cycles = 4U;
            break;
        }

        // --- 0x30 - 0x3F ---
        case 0x30U: {  // BMI rel
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            if ((psw_ & 0x80U) != 0U) {
                pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
                cycles = 4U;
            } else {
                cycles = 2U;
            }
            break;
        }
        case 0x34U: {  // AND A, dp+X
            const uint8_t d = spc_read(pc_++);
            a_ &= spc_read(dp_addr(static_cast<uint8_t>(d + x_)));
            set_nz(a_);
            cycles = 4U;
            break;
        }
        case 0x35U: {  // AND A, abs+X
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + x_);
            pc_ += 2U;
            a_ &= spc_read(addr);
            set_nz(a_);
            cycles = 5U;
            break;
        }
        case 0x36U: {  // AND A, abs+Y
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + y_);
            pc_ += 2U;
            a_ &= spc_read(addr);
            set_nz(a_);
            cycles = 5U;
            break;
        }
        case 0x37U: {  // AND A, [dp]+Y
            const uint8_t d = spc_read(pc_++);
            a_ &= spc_read(dp_ind_y(d));
            set_nz(a_);
            cycles = 6U;
            break;
        }
        case 0x38U: {  // AND dp, #imm
            const uint8_t imm = spc_read(pc_++);
            const uint16_t dst = dp_addr(spc_read(pc_++));
            const uint8_t res = static_cast<uint8_t>(spc_read(dst) & imm);
            spc_write(dst, res);
            set_nz(res);
            cycles = 5U;
            break;
        }
        case 0x39U: {  // AND (X), (Y)
            const uint16_t dst = dp_addr(x_);
            const uint8_t res = static_cast<uint8_t>(spc_read(dst) & spc_read(dp_addr(y_)));
            spc_write(dst, res);
            set_nz(res);
            cycles = 5U;
            break;
        }
        case 0x3AU: {  // INCW dp
            const uint8_t d = spc_read(pc_++);
            const uint16_t a_lo = dp_addr(d);
            const uint16_t a_hi = dp_addr(static_cast<uint8_t>(d + 1U));
            uint16_t v = static_cast<uint16_t>(spc_read(a_lo) | (spc_read(a_hi) << 8U));
            v++;
            spc_write(a_lo, static_cast<uint8_t>(v & 0xFFU));
            spc_write(a_hi, static_cast<uint8_t>(v >> 8U));
            set_nz16(v);
            cycles = 6U;
            break;
        }
        case 0x3BU: {  // ROL dp+X
            const uint16_t addr = dp_addr(static_cast<uint8_t>(spc_read(pc_++) + x_));
            spc_write(addr, do_rol(spc_read(addr)));
            cycles = 5U;
            break;
        }
        case 0x3CU:  // ROL A
            a_ = do_rol(a_);
            cycles = 2U;
            break;
        case 0x3DU:  // INC X
            x_++;
            set_nz(x_);
            cycles = 2U;
            break;
        case 0x3EU: {  // CMP X, dp
            const uint8_t d = spc_read(pc_++);
            do_cmp(x_, spc_read(dp_addr(d)));
            cycles = 3U;
            break;
        }
        case 0x3FU: {  // CALL abs
            const uint16_t target = spc_read16(pc_);
            pc_ += 2U;
            spc_push(static_cast<uint8_t>(pc_ >> 8U));
            spc_push(static_cast<uint8_t>(pc_ & 0xFFU));
            pc_ = target;
            cycles = 8U;
            break;
        }

        // --- 0x40 - 0x4F ---
        case 0x40U:  // SETP
            psw_ |= 0x20U;
            cycles = 2U;
            break;
        case 0x44U: {  // EOR A, dp
            const uint8_t d = spc_read(pc_++);
            a_ ^= spc_read(dp_addr(d));
            set_nz(a_);
            cycles = 3U;
            break;
        }
        case 0x45U: {  // EOR A, abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            a_ ^= spc_read(addr);
            set_nz(a_);
            cycles = 4U;
            break;
        }
        case 0x46U: {  // EOR A, (X)
            a_ ^= spc_read(dp_addr(x_));
            set_nz(a_);
            cycles = 3U;
            break;
        }
        case 0x47U: {  // EOR A, [dp+X]
            const uint8_t d = spc_read(pc_++);
            a_ ^= spc_read(dp_ind_x(d));
            set_nz(a_);
            cycles = 6U;
            break;
        }
        case 0x48U:  // EOR A, #imm
            a_ ^= spc_read(pc_++);
            set_nz(a_);
            cycles = 2U;
            break;
        case 0x49U: {  // EOR dp, dp
            const uint8_t src_d = spc_read(pc_++);
            const uint16_t dst_addr = dp_addr(spc_read(pc_++));
            const uint8_t res = static_cast<uint8_t>(spc_read(dst_addr) ^ spc_read(dp_addr(src_d)));
            spc_write(dst_addr, res);
            set_nz(res);
            cycles = 6U;
            break;
        }
        case 0x4AU: {  // AND1 C, membit
            uint16_t addr = 0;
            uint8_t bit = 0;
            if (!read_membit(addr, bit)) {
                psw_ &= ~0x01U;
            }
            cycles = 4U;
            break;
        }
        case 0x4BU: {  // LSR dp
            const uint16_t addr = dp_addr(spc_read(pc_++));
            spc_write(addr, do_lsr(spc_read(addr)));
            cycles = 4U;
            break;
        }
        case 0x4CU: {  // LSR abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            spc_write(addr, do_lsr(spc_read(addr)));
            cycles = 5U;
            break;
        }
        case 0x4DU:  // PUSH X
            spc_push(x_);
            cycles = 4U;
            break;
        case 0x4EU: {  // TCLR1 abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            const uint8_t val = spc_read(addr);
            set_nz(static_cast<uint8_t>(a_ - val));
            spc_write(addr, static_cast<uint8_t>(val & ~a_));
            cycles = 6U;
            break;
        }
        case 0x4FU: {  // PCALL upage
            const uint8_t u = spc_read(pc_++);
            spc_push(static_cast<uint8_t>(pc_ >> 8U));
            spc_push(static_cast<uint8_t>(pc_ & 0xFFU));
            pc_ = static_cast<uint16_t>(0xFF00U | u);
            cycles = 6U;
            break;
        }

        // --- 0x50 - 0x5F ---
        case 0x50U: {  // BVC rel
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            if ((psw_ & 0x40U) == 0U) {
                pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
                cycles = 4U;
            } else {
                cycles = 2U;
            }
            break;
        }
        case 0x54U: {  // EOR A, dp+X
            const uint8_t d = spc_read(pc_++);
            a_ ^= spc_read(dp_addr(static_cast<uint8_t>(d + x_)));
            set_nz(a_);
            cycles = 4U;
            break;
        }
        case 0x55U: {  // EOR A, abs+X
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + x_);
            pc_ += 2U;
            a_ ^= spc_read(addr);
            set_nz(a_);
            cycles = 5U;
            break;
        }
        case 0x56U: {  // EOR A, abs+Y
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + y_);
            pc_ += 2U;
            a_ ^= spc_read(addr);
            set_nz(a_);
            cycles = 5U;
            break;
        }
        case 0x57U: {  // EOR A, [dp]+Y
            const uint8_t d = spc_read(pc_++);
            a_ ^= spc_read(dp_ind_y(d));
            set_nz(a_);
            cycles = 6U;
            break;
        }
        case 0x58U: {  // EOR dp, #imm
            const uint8_t imm = spc_read(pc_++);
            const uint16_t dst = dp_addr(spc_read(pc_++));
            const uint8_t res = static_cast<uint8_t>(spc_read(dst) ^ imm);
            spc_write(dst, res);
            set_nz(res);
            cycles = 5U;
            break;
        }
        case 0x59U: {  // EOR (X), (Y)
            const uint16_t dst = dp_addr(x_);
            const uint8_t res = static_cast<uint8_t>(spc_read(dst) ^ spc_read(dp_addr(y_)));
            spc_write(dst, res);
            set_nz(res);
            cycles = 5U;
            break;
        }
        case 0x5AU: {  // CMPW YA, dp
            const uint8_t d = spc_read(pc_++);
            const uint16_t src = static_cast<uint16_t>(
                spc_read(dp_addr(d)) | (spc_read(dp_addr(static_cast<uint8_t>(d + 1U))) << 8U));
            const int32_t diff = static_cast<int32_t>(ya()) - static_cast<int32_t>(src);
            if (diff >= 0)
                psw_ |= 0x01U;
            else
                psw_ &= ~0x01U;
            set_nz16(static_cast<uint16_t>(diff));
            cycles = 4U;
            break;
        }
        case 0x5BU: {  // LSR dp+X
            const uint16_t addr = dp_addr(static_cast<uint8_t>(spc_read(pc_++) + x_));
            spc_write(addr, do_lsr(spc_read(addr)));
            cycles = 5U;
            break;
        }
        case 0x5CU:  // LSR A
            a_ = do_lsr(a_);
            cycles = 2U;
            break;
        case 0x5DU:  // MOV X, A
            x_ = a_;
            set_nz(x_);
            cycles = 2U;
            break;
        case 0x5EU: {  // CMP Y, abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            do_cmp(y_, spc_read(addr));
            cycles = 4U;
            break;
        }
        case 0x5FU: {  // JMP abs
            pc_ = spc_read16(pc_);
            cycles = 3U;
            break;
        }

        // --- 0x60 - 0x6F ---
        case 0x60U:  // CLRC
            psw_ &= ~0x01U;
            cycles = 2U;
            break;
        case 0x64U: {  // CMP A, dp
            const uint8_t d = spc_read(pc_++);
            do_cmp(a_, spc_read(dp_addr(d)));
            cycles = 3U;
            break;
        }
        case 0x65U: {  // CMP A, abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            do_cmp(a_, spc_read(addr));
            cycles = 4U;
            break;
        }
        case 0x66U: {  // CMP A, (X)
            do_cmp(a_, spc_read(dp_addr(x_)));
            cycles = 3U;
            break;
        }
        case 0x67U: {  // CMP A, [dp+X]
            const uint8_t d = spc_read(pc_++);
            do_cmp(a_, spc_read(dp_ind_x(d)));
            cycles = 6U;
            break;
        }
        case 0x68U: {  // CMP A, #imm
            const uint8_t imm = spc_read(pc_++);
            do_cmp(a_, imm);
            cycles = 2U;
            break;
        }
        case 0x69U: {  // CMP dp, dp
            const uint8_t src_d = spc_read(pc_++);
            const uint8_t dst_d = spc_read(pc_++);
            do_cmp(spc_read(dp_addr(dst_d)), spc_read(dp_addr(src_d)));
            cycles = 6U;
            break;
        }
        case 0x6AU: {  // AND1 C, /membit
            uint16_t addr = 0;
            uint8_t bit = 0;
            if (read_membit(addr, bit)) {
                psw_ &= ~0x01U;
            }
            cycles = 4U;
            break;
        }
        case 0x6BU: {  // ROR dp
            const uint16_t addr = dp_addr(spc_read(pc_++));
            spc_write(addr, do_ror(spc_read(addr)));
            cycles = 4U;
            break;
        }
        case 0x6CU: {  // ROR abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            spc_write(addr, do_ror(spc_read(addr)));
            cycles = 5U;
            break;
        }
        case 0x6DU:  // PUSH Y
            spc_push(y_);
            cycles = 4U;
            break;
        case 0x6EU: {  // DBNZ dp, rel
            const uint8_t d = spc_read(pc_++);
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            const uint16_t a_eff = dp_addr(d);
            const uint8_t val = static_cast<uint8_t>(spc_read(a_eff) - 1U);
            spc_write(a_eff, val);
            if (val != 0U) {
                pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
                cycles = 6U;
            } else {
                cycles = 4U;
            }
            break;
        }
        case 0x6FU: {  // RET
            const uint8_t lo = spc_pop();
            const uint8_t hi = spc_pop();
            pc_ = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
            cycles = 5U;
            break;
        }

        // --- 0x70 - 0x7F ---
        case 0x70U: {  // BVS rel
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            if ((psw_ & 0x40U) != 0U) {
                pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
                cycles = 4U;
            } else {
                cycles = 2U;
            }
            break;
        }
        case 0x74U: {  // CMP A, dp+X
            const uint8_t d = spc_read(pc_++);
            do_cmp(a_, spc_read(dp_addr(static_cast<uint8_t>(d + x_))));
            cycles = 4U;
            break;
        }
        case 0x75U: {  // CMP A, abs+X
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + x_);
            pc_ += 2U;
            do_cmp(a_, spc_read(addr));
            cycles = 5U;
            break;
        }
        case 0x76U: {  // CMP A, abs+Y
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + y_);
            pc_ += 2U;
            do_cmp(a_, spc_read(addr));
            cycles = 5U;
            break;
        }
        case 0x77U: {  // CMP A, [dp]+Y
            const uint8_t d = spc_read(pc_++);
            do_cmp(a_, spc_read(dp_ind_y(d)));
            cycles = 6U;
            break;
        }
        case 0x78U: {  // CMP dp, #imm
            const uint8_t imm = spc_read(pc_++);
            const uint8_t d = spc_read(pc_++);
            do_cmp(spc_read(dp_addr(d)), imm);
            cycles = 5U;
            break;
        }
        case 0x79U: {  // CMP (X), (Y)
            do_cmp(spc_read(dp_addr(x_)), spc_read(dp_addr(y_)));
            cycles = 5U;
            break;
        }
        case 0x7AU: {  // ADDW YA, dp
            const uint8_t d = spc_read(pc_++);
            const uint16_t src = static_cast<uint16_t>(
                spc_read(dp_addr(d)) | (spc_read(dp_addr(static_cast<uint8_t>(d + 1U))) << 8U));
            const uint16_t dst = ya();
            const uint32_t sum = static_cast<uint32_t>(dst) + src;
            const bool v = (~(dst ^ src) & (dst ^ sum) & 0x8000U) != 0U;
            const bool h = ((dst & 0x0FFFU) + (src & 0x0FFFU)) > 0x0FFFU;
            if (v)
                psw_ |= 0x40U;
            else
                psw_ &= ~0x40U;
            if (h)
                psw_ |= 0x08U;
            else
                psw_ &= ~0x08U;
            if (sum > 0xFFFFU)
                psw_ |= 0x01U;
            else
                psw_ &= ~0x01U;
            set_ya(static_cast<uint16_t>(sum & 0xFFFFU));
            cycles = 5U;
            break;
        }
        case 0x7BU: {  // ROR dp+X
            const uint16_t addr = dp_addr(static_cast<uint8_t>(spc_read(pc_++) + x_));
            spc_write(addr, do_ror(spc_read(addr)));
            cycles = 5U;
            break;
        }
        case 0x7CU:  // ROR A
            a_ = do_ror(a_);
            cycles = 2U;
            break;
        case 0x7DU:  // MOV A, X
            a_ = x_;
            set_nz(a_);
            cycles = 2U;
            break;
        case 0x7EU: {  // CMP Y, dp
            const uint8_t d = spc_read(pc_++);
            do_cmp(y_, spc_read(dp_addr(d)));
            cycles = 3U;
            break;
        }
        case 0x7FU: {  // RETI
            psw_ = spc_pop();
            const uint8_t lo = spc_pop();
            const uint8_t hi = spc_pop();
            pc_ = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
            cycles = 6U;
            break;
        }

        // --- 0x80 - 0x8F ---
        case 0x80U:  // SETC
            psw_ |= 0x01U;
            cycles = 2U;
            break;
        case 0x84U: {  // ADC A, dp
            const uint8_t d = spc_read(pc_++);
            do_adc(spc_read(dp_addr(d)));
            cycles = 3U;
            break;
        }
        case 0x85U: {  // ADC A, abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            do_adc(spc_read(addr));
            cycles = 4U;
            break;
        }
        case 0x86U: {  // ADC A, (X)
            do_adc(spc_read(dp_addr(x_)));
            cycles = 3U;
            break;
        }
        case 0x87U: {  // ADC A, [dp+X]
            const uint8_t d = spc_read(pc_++);
            do_adc(spc_read(dp_ind_x(d)));
            cycles = 6U;
            break;
        }
        case 0x88U:  // ADC A, #imm
            do_adc(spc_read(pc_++));
            cycles = 2U;
            break;
        case 0x89U: {  // ADC dp, dp
            const uint8_t src_d = spc_read(pc_++);
            const uint16_t dst_addr = dp_addr(spc_read(pc_++));
            const uint8_t res = do_adc_val(spc_read(dst_addr), spc_read(dp_addr(src_d)));
            spc_write(dst_addr, res);
            cycles = 6U;
            break;
        }
        case 0x8AU: {  // EOR1 C, membit
            uint16_t addr = 0;
            uint8_t bit = 0;
            if (read_membit(addr, bit)) {
                psw_ ^= 0x01U;
            }
            cycles = 5U;
            break;
        }
        case 0x8BU: {  // DEC dp
            const uint16_t addr = dp_addr(spc_read(pc_++));
            const uint8_t val = static_cast<uint8_t>(spc_read(addr) - 1U);
            spc_write(addr, val);
            set_nz(val);
            cycles = 4U;
            break;
        }
        case 0x8CU: {  // DEC abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            const uint8_t val = static_cast<uint8_t>(spc_read(addr) - 1U);
            spc_write(addr, val);
            set_nz(val);
            cycles = 5U;
            break;
        }
        case 0x8DU:  // MOV Y, #imm
            y_ = spc_read(pc_++);
            set_nz(y_);
            cycles = 2U;
            break;
        case 0x8EU:  // POP PSW
            psw_ = spc_pop();
            cycles = 4U;
            break;
        case 0x8FU: {  // MOV dp, #imm
            const uint8_t imm = spc_read(pc_++);
            const uint8_t d = spc_read(pc_++);
            spc_write(dp_addr(d), imm);
            cycles = 5U;
            break;
        }

        // --- 0x90 - 0x9F ---
        case 0x90U: {  // BCC rel
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            if ((psw_ & 0x01U) == 0U) {
                pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
                cycles = 4U;
            } else {
                cycles = 2U;
            }
            break;
        }
        case 0x94U: {  // ADC A, dp+X
            const uint8_t d = spc_read(pc_++);
            do_adc(spc_read(dp_addr(static_cast<uint8_t>(d + x_))));
            cycles = 4U;
            break;
        }
        case 0x95U: {  // ADC A, abs+X
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + x_);
            pc_ += 2U;
            do_adc(spc_read(addr));
            cycles = 5U;
            break;
        }
        case 0x96U: {  // ADC A, abs+Y
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + y_);
            pc_ += 2U;
            do_adc(spc_read(addr));
            cycles = 5U;
            break;
        }
        case 0x97U: {  // ADC A, [dp]+Y
            const uint8_t d = spc_read(pc_++);
            do_adc(spc_read(dp_ind_y(d)));
            cycles = 6U;
            break;
        }
        case 0x98U: {  // ADC dp, #imm
            const uint8_t imm = spc_read(pc_++);
            const uint16_t dst = dp_addr(spc_read(pc_++));
            const uint8_t res = do_adc_val(spc_read(dst), imm);
            spc_write(dst, res);
            cycles = 5U;
            break;
        }
        case 0x99U: {  // ADC (X), (Y)
            const uint16_t dst = dp_addr(x_);
            const uint8_t res = do_adc_val(spc_read(dst), spc_read(dp_addr(y_)));
            spc_write(dst, res);
            cycles = 5U;
            break;
        }
        case 0x9AU: {  // SUBW YA, dp
            const uint8_t d = spc_read(pc_++);
            const uint16_t src = static_cast<uint16_t>(
                spc_read(dp_addr(d)) | (spc_read(dp_addr(static_cast<uint8_t>(d + 1U))) << 8U));
            const uint16_t dst = ya();
            const uint32_t diff = static_cast<uint32_t>(dst) - src;
            const bool v = ((dst ^ src) & (dst ^ diff) & 0x8000U) != 0U;
            const bool h = (dst & 0x0FFFU) < (src & 0x0FFFU);
            if (v)
                psw_ |= 0x40U;
            else
                psw_ &= ~0x40U;
            if (!h)
                psw_ |= 0x08U;
            else
                psw_ &= ~0x08U;
            if (diff <= 0xFFFFU)
                psw_ |= 0x01U;
            else
                psw_ &= ~0x01U;
            set_ya(static_cast<uint16_t>(diff & 0xFFFFU));
            cycles = 5U;
            break;
        }
        case 0x9BU: {  // DEC dp+X
            const uint16_t addr = dp_addr(static_cast<uint8_t>(spc_read(pc_++) + x_));
            const uint8_t val = static_cast<uint8_t>(spc_read(addr) - 1U);
            spc_write(addr, val);
            set_nz(val);
            cycles = 5U;
            break;
        }
        case 0x9CU:  // DEC A
            a_--;
            set_nz(a_);
            cycles = 2U;
            break;
        case 0x9DU:  // MOV X, SP
            x_ = sp_;
            set_nz(x_);
            cycles = 2U;
            break;
        case 0x9EU: {  // DIV YA, X
            const uint16_t dividend = ya();
            if (x_ == 0U) {
                psw_ |= 0x48U;
                a_ = 0xFFU;
                y_ = static_cast<uint8_t>(dividend & 0xFFU);
            } else {
                a_ = static_cast<uint8_t>(dividend / x_);
                y_ = static_cast<uint8_t>(dividend % x_);
            }
            set_nz(a_);
            cycles = 12U;
            break;
        }
        case 0x9FU:  // XCN A (exchange nibbles)
            a_ = static_cast<uint8_t>((a_ >> 4U) | (a_ << 4U));
            set_nz(a_);
            cycles = 5U;
            break;

        // --- 0xA0 - 0xAF ---
        case 0xA0U:  // EI
            psw_ |= 0x04U;
            cycles = 3U;
            break;
        case 0xA4U: {  // SBC A, dp
            const uint8_t d = spc_read(pc_++);
            do_sbc(spc_read(dp_addr(d)));
            cycles = 3U;
            break;
        }
        case 0xA5U: {  // SBC A, abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            do_sbc(spc_read(addr));
            cycles = 4U;
            break;
        }
        case 0xA6U: {  // SBC A, (X)
            do_sbc(spc_read(dp_addr(x_)));
            cycles = 3U;
            break;
        }
        case 0xA7U: {  // SBC A, [dp+X]
            const uint8_t d = spc_read(pc_++);
            do_sbc(spc_read(dp_ind_x(d)));
            cycles = 6U;
            break;
        }
        case 0xA8U:  // SBC A, #imm
            do_sbc(spc_read(pc_++));
            cycles = 2U;
            break;
        case 0xA9U: {  // SBC dp, dp
            const uint8_t src_d = spc_read(pc_++);
            const uint16_t dst_addr = dp_addr(spc_read(pc_++));
            const uint8_t res = do_sbc_val(spc_read(dst_addr), spc_read(dp_addr(src_d)));
            spc_write(dst_addr, res);
            cycles = 6U;
            break;
        }
        case 0xAAU: {  // MOV1 C, membit
            uint16_t addr = 0;
            uint8_t bit = 0;
            if (read_membit(addr, bit)) {
                psw_ |= 0x01U;
            } else {
                psw_ &= ~0x01U;
            }
            cycles = 4U;
            break;
        }
        case 0xABU: {  // INC dp
            const uint8_t d = spc_read(pc_++);
            const uint16_t a_eff = dp_addr(d);
            const uint8_t v = static_cast<uint8_t>(spc_read(a_eff) + 1U);
            spc_write(a_eff, v);
            set_nz(v);
            cycles = 4U;
            break;
        }
        case 0xACU: {  // INC abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            const uint8_t v = static_cast<uint8_t>(spc_read(addr) + 1U);
            spc_write(addr, v);
            set_nz(v);
            cycles = 5U;
            break;
        }
        case 0xADU: {  // CMP Y, #imm
            const uint8_t imm = spc_read(pc_++);
            do_cmp(y_, imm);
            cycles = 2U;
            break;
        }
        case 0xAEU:  // POP A
            a_ = spc_pop();
            cycles = 4U;
            break;
        case 0xAFU:  // MOV (X)+, A
            spc_write(dp_addr(x_++), a_);
            cycles = 4U;
            break;

        // --- 0xB0 - 0xBF ---
        case 0xB0U: {  // BCS rel
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            if ((psw_ & 0x01U) != 0U) {
                pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
                cycles = 4U;
            } else {
                cycles = 2U;
            }
            break;
        }
        case 0xB4U: {  // SBC A, dp+X
            const uint8_t d = spc_read(pc_++);
            do_sbc(spc_read(dp_addr(static_cast<uint8_t>(d + x_))));
            cycles = 4U;
            break;
        }
        case 0xB5U: {  // SBC A, abs+X
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + x_);
            pc_ += 2U;
            do_sbc(spc_read(addr));
            cycles = 5U;
            break;
        }
        case 0xB6U: {  // SBC A, abs+Y
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + y_);
            pc_ += 2U;
            do_sbc(spc_read(addr));
            cycles = 5U;
            break;
        }
        case 0xB7U: {  // SBC A, [dp]+Y
            const uint8_t d = spc_read(pc_++);
            do_sbc(spc_read(dp_ind_y(d)));
            cycles = 6U;
            break;
        }
        case 0xB8U: {  // SBC dp, #imm
            const uint8_t imm = spc_read(pc_++);
            const uint16_t dst = dp_addr(spc_read(pc_++));
            const uint8_t res = do_sbc_val(spc_read(dst), imm);
            spc_write(dst, res);
            cycles = 5U;
            break;
        }
        case 0xB9U: {  // SBC (X), (Y)
            const uint16_t dst = dp_addr(x_);
            const uint8_t res = do_sbc_val(spc_read(dst), spc_read(dp_addr(y_)));
            spc_write(dst, res);
            cycles = 5U;
            break;
        }
        case 0xBAU: {  // MOVW YA, dp
            const uint8_t d = spc_read(pc_++);
            const uint16_t lo = spc_read(dp_addr(d));
            const uint16_t hi = spc_read(dp_addr(static_cast<uint8_t>(d + 1U)));
            set_ya(static_cast<uint16_t>((hi << 8U) | lo));
            cycles = 5U;
            break;
        }
        case 0xBBU: {  // INC dp+X
            const uint16_t addr = dp_addr(static_cast<uint8_t>(spc_read(pc_++) + x_));
            const uint8_t v = static_cast<uint8_t>(spc_read(addr) + 1U);
            spc_write(addr, v);
            set_nz(v);
            cycles = 5U;
            break;
        }
        case 0xBCU:  // INC A
            a_++;
            set_nz(a_);
            cycles = 2U;
            break;
        case 0xBDU:  // MOV SP, X
            sp_ = x_;
            cycles = 2U;
            break;
        case 0xBEU: {  // DAS A
            uint8_t a = a_;
            if (!(psw_ & 0x08U) || ((a & 0x0FU) > 9U)) {
                a = static_cast<uint8_t>(a - 6U);
            }
            if (!(psw_ & 0x01U) || (a > 0x99U)) {
                a = static_cast<uint8_t>(a - 0x60U);
                psw_ &= ~0x01U;
            }
            a_ = a;
            set_nz(a_);
            cycles = 3U;
            break;
        }
        case 0xBFU:  // MOV A, (X)+
            a_ = spc_read(dp_addr(x_++));
            set_nz(a_);
            cycles = 4U;
            break;

        // --- 0xC0 - 0xCF ---
        case 0xC0U:  // DI
            psw_ &= ~0x04U;
            cycles = 3U;
            break;
        case 0xC4U: {  // MOV dp, A
            const uint8_t d = spc_read(pc_++);
            spc_write(dp_addr(d), a_);
            cycles = 4U;
            break;
        }
        case 0xC5U: {  // MOV abs, A
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            spc_write(addr, a_);
            cycles = 5U;
            break;
        }
        case 0xC6U:  // MOV (X), A
            spc_write(dp_addr(x_), a_);
            cycles = 4U;
            break;
        case 0xC7U: {  // MOV [dp+X], A
            const uint8_t d = spc_read(pc_++);
            spc_write(dp_ind_x(d), a_);
            cycles = 7U;
            break;
        }
        case 0xC8U: {  // CMP X, #imm
            const uint8_t imm = spc_read(pc_++);
            do_cmp(x_, imm);
            cycles = 2U;
            break;
        }
        case 0xC9U: {  // MOV abs, X
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            spc_write(addr, x_);
            cycles = 5U;
            break;
        }
        case 0xCAU: {  // MOV1 membit, C
            uint16_t addr = 0;
            uint8_t bit = 0;
            read_membit(addr, bit);
            uint8_t val = spc_read(addr);
            if ((psw_ & 0x01U) != 0U) {
                val |= static_cast<uint8_t>(1U << bit);
            } else {
                val &= static_cast<uint8_t>(~(1U << bit));
            }
            spc_write(addr, val);
            cycles = 6U;
            break;
        }
        case 0xCBU: {  // MOV dp, Y
            const uint8_t d = spc_read(pc_++);
            spc_write(dp_addr(d), y_);
            cycles = 4U;
            break;
        }
        case 0xCCU: {  // MOV abs, Y
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            spc_write(addr, y_);
            cycles = 5U;
            break;
        }
        case 0xCDU:  // MOV X, #imm
            x_ = spc_read(pc_++);
            set_nz(x_);
            cycles = 2U;
            break;
        case 0xCEU:  // POP X
            x_ = spc_pop();
            cycles = 4U;
            break;
        case 0xCFU: {  // MUL YA
            const uint32_t prod = static_cast<uint32_t>(y_) * static_cast<uint32_t>(a_);
            a_ = static_cast<uint8_t>(prod & 0xFFU);
            y_ = static_cast<uint8_t>(prod >> 8U);
            set_nz(y_);
            cycles = 9U;
            break;
        }

        // --- 0xD0 - 0xDF ---
        case 0xD0U: {  // BNE rel
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            if ((psw_ & 0x02U) == 0U) {
                pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
                cycles = 4U;
            } else {
                cycles = 2U;
            }
            break;
        }
        case 0xD4U: {  // MOV dp+X, A
            const uint8_t d = spc_read(pc_++);
            spc_write(dp_addr(static_cast<uint8_t>(d + x_)), a_);
            cycles = 5U;
            break;
        }
        case 0xD5U: {  // MOV abs+X, A
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + x_);
            pc_ += 2U;
            spc_write(addr, a_);
            cycles = 6U;
            break;
        }
        case 0xD6U: {  // MOV abs+Y, A
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + y_);
            pc_ += 2U;
            spc_write(addr, a_);
            cycles = 6U;
            break;
        }
        case 0xD7U: {  // MOV [dp]+Y, A
            const uint8_t d = spc_read(pc_++);
            spc_write(dp_ind_y(d), a_);
            cycles = 6U;
            break;
        }
        case 0xD8U: {  // MOV dp, X
            const uint8_t d = spc_read(pc_++);
            spc_write(dp_addr(d), x_);
            cycles = 4U;
            break;
        }
        case 0xD9U: {  // MOV dp+Y, X
            const uint8_t d = spc_read(pc_++);
            spc_write(dp_addr(static_cast<uint8_t>(d + y_)), x_);
            cycles = 5U;
            break;
        }
        case 0xDAU: {  // MOVW dp, YA
            const uint8_t d = spc_read(pc_++);
            spc_write(dp_addr(d), a_);
            spc_write(dp_addr(static_cast<uint8_t>(d + 1U)), y_);
            cycles = 5U;
            break;
        }
        case 0xDBU: {  // MOV dp+X, Y
            const uint8_t d = spc_read(pc_++);
            spc_write(dp_addr(static_cast<uint8_t>(d + x_)), y_);
            cycles = 5U;
            break;
        }
        case 0xDCU:  // DEC Y
            y_--;
            set_nz(y_);
            cycles = 2U;
            break;
        case 0xDDU:  // MOV A, Y
            a_ = y_;
            set_nz(a_);
            cycles = 2U;
            break;
        case 0xDEU: {  // CBNE dp+X, rel
            const uint8_t d = spc_read(pc_++);
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            if (a_ != spc_read(dp_addr(static_cast<uint8_t>(d + x_)))) {
                pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
                cycles = 6U;
            } else {
                cycles = 4U;
            }
            break;
        }
        case 0xDFU: {  // DAA A
            uint8_t a = a_;
            if (((a & 0x0FU) > 9U) || ((psw_ & 0x08U) != 0U)) {
                a = static_cast<uint8_t>(a + 6U);
                if (a < 6U) {
                    psw_ |= 0x01U;
                }
            }
            if ((a > 0x99U) || ((psw_ & 0x01U) != 0U)) {
                a = static_cast<uint8_t>(a + 0x60U);
                psw_ |= 0x01U;
            }
            a_ = a;
            set_nz(a_);
            cycles = 3U;
            break;
        }

        // --- 0xE0 - 0xEF ---
        case 0xE0U:  // CLRV
            psw_ &= ~0x48U;
            cycles = 2U;
            break;
        case 0xE4U: {  // MOV A, dp
            const uint8_t d = spc_read(pc_++);
            a_ = spc_read(dp_addr(d));
            set_nz(a_);
            cycles = 3U;
            break;
        }
        case 0xE5U: {  // MOV A, abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            a_ = spc_read(addr);
            set_nz(a_);
            cycles = 4U;
            break;
        }
        case 0xE6U: {  // MOV A, (X)
            a_ = spc_read(dp_addr(x_));
            set_nz(a_);
            cycles = 3U;
            break;
        }
        case 0xE7U: {  // MOV A, [dp+X]
            const uint8_t d = spc_read(pc_++);
            a_ = spc_read(dp_ind_x(d));
            set_nz(a_);
            cycles = 6U;
            break;
        }
        case 0xE8U:  // MOV A, #imm
            a_ = spc_read(pc_++);
            set_nz(a_);
            cycles = 2U;
            break;
        case 0xE9U: {  // MOV X, abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            x_ = spc_read(addr);
            set_nz(x_);
            cycles = 4U;
            break;
        }
        case 0xEAU: {  // NOT1 membit
            uint16_t addr = 0;
            uint8_t bit = 0;
            read_membit(addr, bit);
            spc_write(addr, static_cast<uint8_t>(spc_read(addr) ^ (1U << bit)));
            cycles = 5U;
            break;
        }
        case 0xEBU: {  // MOV Y, dp
            const uint8_t d = spc_read(pc_++);
            y_ = spc_read(dp_addr(d));
            set_nz(y_);
            cycles = 3U;
            break;
        }
        case 0xECU: {  // MOV Y, abs
            const uint16_t addr = spc_read16(pc_);
            pc_ += 2U;
            y_ = spc_read(addr);
            set_nz(y_);
            cycles = 4U;
            break;
        }
        case 0xEDU:  // NOTC
            psw_ ^= 0x01U;
            cycles = 3U;
            break;
        case 0xEEU:  // POP Y
            y_ = spc_pop();
            cycles = 4U;
            break;
        case 0xEFU:  // SLEEP
            halted_ = true;
            cycles = 3U;
            break;

        // --- 0xF0 - 0xFF ---
        case 0xF0U: {  // BEQ rel
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            if ((psw_ & 0x02U) != 0U) {
                pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
                cycles = 4U;
            } else {
                cycles = 2U;
            }
            break;
        }
        case 0xF4U: {  // MOV A, dp+X
            const uint8_t d = spc_read(pc_++);
            a_ = spc_read(dp_addr(static_cast<uint8_t>(d + x_)));
            set_nz(a_);
            cycles = 4U;
            break;
        }
        case 0xF5U: {  // MOV A, abs+X
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + x_);
            pc_ += 2U;
            a_ = spc_read(addr);
            set_nz(a_);
            cycles = 5U;
            break;
        }
        case 0xF6U: {  // MOV A, abs+Y
            const uint16_t addr = static_cast<uint16_t>(spc_read16(pc_) + y_);
            pc_ += 2U;
            a_ = spc_read(addr);
            set_nz(a_);
            cycles = 5U;
            break;
        }
        case 0xF7U: {  // MOV A, [dp]+Y
            const uint8_t d = spc_read(pc_++);
            a_ = spc_read(dp_ind_y(d));
            set_nz(a_);
            cycles = 6U;
            break;
        }
        case 0xF8U: {  // MOV X, dp
            const uint8_t d = spc_read(pc_++);
            x_ = spc_read(dp_addr(d));
            set_nz(x_);
            cycles = 3U;
            break;
        }
        case 0xF9U: {  // MOV X, dp+Y
            const uint8_t d = spc_read(pc_++);
            x_ = spc_read(dp_addr(static_cast<uint8_t>(d + y_)));
            set_nz(x_);
            cycles = 4U;
            break;
        }
        case 0xFAU: {  // MOV dp, dp
            const uint8_t src_d = spc_read(pc_++);
            const uint8_t dst_d = spc_read(pc_++);
            spc_write(dp_addr(dst_d), spc_read(dp_addr(src_d)));
            cycles = 5U;
            break;
        }
        case 0xFBU: {  // MOV Y, dp+X
            const uint8_t d = spc_read(pc_++);
            y_ = spc_read(dp_addr(static_cast<uint8_t>(d + x_)));
            set_nz(y_);
            cycles = 4U;
            break;
        }
        case 0xFCU:  // INC Y
            y_++;
            set_nz(y_);
            cycles = 2U;
            break;
        case 0xFDU:  // MOV Y, A
            y_ = a_;
            set_nz(y_);
            cycles = 2U;
            break;
        case 0xFEU: {  // DBNZ Y, rel
            const auto rel = static_cast<int8_t>(spc_read(pc_++));
            y_--;
            if (y_ != 0U) {
                pc_ = static_cast<uint16_t>(static_cast<int32_t>(pc_) + rel);
                cycles = 4U;
            } else {
                cycles = 2U;
            }
            break;
        }
        case 0xFFU:  // STOP
            halted_ = true;
            cycles = 3U;
            break;
        default:
            cycles = 2U;
            break;
    }

    spc_cycles_executed_ += cycles;
    step_timers(cycles);
}

void Apu::step_cycles(uint64_t cycles) noexcept {
    const uint64_t target = spc_cycles_executed_ + cycles;
    while (spc_cycles_executed_ < target && !halted_) {
        step_spc_instruction();
    }
}

void Apu::save_snapshot(Snapshot& snap) const noexcept {
    snap.aram = aram_;
    snap.pc = pc_;
    snap.a = a_;
    snap.x = x_;
    snap.y = y_;
    snap.sp = sp_;
    snap.psw = psw_;
    snap.halted = halted_;
    snap.ipl_rom_enabled = ipl_rom_enabled_;
    snap.dsp_addr = dsp_addr_;
    snap.cpu_to_spc = cpu_to_spc_;
    snap.spc_to_cpu = spc_to_cpu_;
    snap.timers = timers_;
    snap.dsp_regs = dsp_regs_;
    snap.voices = voices_;
    snap.mvol_l = mvol_l_;
    snap.mvol_r = mvol_r_;
    snap.dir_page = dir_page_;
    snap.endx = endx_;
    snap.spc_cycles_executed = spc_cycles_executed_;
    snap.last_cpu_master_cycles = last_cpu_master_cycles_;
}

void Apu::load_snapshot(const Snapshot& snap) noexcept {
    aram_ = snap.aram;
    pc_ = snap.pc;
    a_ = snap.a;
    x_ = snap.x;
    y_ = snap.y;
    sp_ = snap.sp;
    psw_ = snap.psw;
    halted_ = snap.halted;
    ipl_rom_enabled_ = snap.ipl_rom_enabled;
    dsp_addr_ = snap.dsp_addr;
    cpu_to_spc_ = snap.cpu_to_spc;
    spc_to_cpu_ = snap.spc_to_cpu;
    timers_ = snap.timers;
    dsp_regs_ = snap.dsp_regs;
    voices_ = snap.voices;
    mvol_l_ = snap.mvol_l;
    mvol_r_ = snap.mvol_r;
    dir_page_ = snap.dir_page;
    endx_ = snap.endx;
    spc_cycles_executed_ = snap.spc_cycles_executed;
    last_cpu_master_cycles_ = snap.last_cpu_master_cycles;
}

}  // namespace dragon::snes
