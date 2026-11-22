#include "ps1/spu.h"

#include <algorithm>
#include <cstring>

namespace dragon::ps1 {

namespace {

constexpr size_t kRingCapacitySamples = 44100U * 2U;

// PSX ADPCM filter tables (scaled by 64, psx-spx)
constexpr int32_t kFilterPos[5] = {0, 60, 115, 98, 122};
constexpr int32_t kFilterNeg[5] = {0, 0, -52, -55, -60};

}  // namespace

Ps1Spu::Ps1Spu() : ram_(kSpuRamSize, 0U), ring_(kRingCapacitySamples, 0) {
    reset();
}

void Ps1Spu::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fill(ram_.begin(), ram_.end(), uint8_t{0});
    for (auto& v : voices_)
        v = Voice{};
    main_vol_l_ = 0x3FFF;
    main_vol_r_ = 0x3FFF;
    spucnt_ = 0;
    spustat_ = 0;
    transfer_addr_reg_ = 0;
    transfer_ptr_ = 0;
    transfer_ctrl_ = 4;
    key_on_reg_ = 0;
    key_off_reg_ = 0;
    read_pos_ = 0;
    write_pos_ = 0;
    count_ = 0;
    std::fill(ring_.begin(), ring_.end(), int16_t{0});
}

void Ps1Spu::decode_adpcm_block(Voice& v) noexcept {
    const uint32_t base = v.cur_addr & (kSpuRamSize - 1U);
    const uint8_t hdr = ram_[base];
    const uint8_t flags = ram_[(base + 1U) & (kSpuRamSize - 1U)];
    const int shift = std::min(12, static_cast<int>(hdr & 0x0FU));
    const int filter = std::min(4, static_cast<int>((hdr >> 4U) & 0x07U));

    if ((flags & 0x04U) != 0U) {
        v.repeat_addr = static_cast<uint16_t>((base >> 3U) & 0xFFFFU);
    }

    const int32_t f0 = kFilterPos[filter];
    const int32_t f1 = kFilterNeg[filter];
    for (int i = 0; i < 28; ++i) {
        const uint8_t byte = ram_[(base + 2U + static_cast<uint32_t>(i >> 1)) & (kSpuRamSize - 1U)];
        int32_t nibble = (i & 1) ? ((byte >> 4U) & 0x0FU) : (byte & 0x0FU);
        if (nibble >= 8)
            nibble -= 16;
        int32_t sample = (nibble << 12) >> shift;
        sample += (v.hist1 * f0 + v.hist2 * f1 + 32) >> 6;
        sample = std::clamp(sample, -32768, 32767);
        v.hist2 = v.hist1;
        v.hist1 = sample;
        v.decoded[static_cast<size_t>(i)] = static_cast<int16_t>(sample);
    }

    v.sample_idx = 0;
    v.cur_addr = (v.cur_addr + 16U) & (kSpuRamSize - 1U);
    if ((flags & 0x01U) != 0U) {  // Loop end
        v.endx = true;
        if ((flags & 0x02U) != 0U) {
            v.cur_addr = (static_cast<uint32_t>(v.repeat_addr) << 3U) & (kSpuRamSize - 1U);
        } else {
            v.active = false;
            v.adsr_vol = 0;
        }
    }
}

void Ps1Spu::key_on(uint32_t mask) noexcept {
    for (int i = 0; i < kNumVoices; ++i) {
        if ((mask & (1U << i)) != 0U) {
            auto& v = voices_[static_cast<size_t>(i)];
            v.active = true;
            v.endx = false;
            v.cur_addr = (static_cast<uint32_t>(v.start_addr) << 3U) & (kSpuRamSize - 1U);
            v.pitch_counter = 0;
            v.sample_idx = 28;
            v.hist1 = 0;
            v.hist2 = 0;
            v.adsr_vol = 0x7FFF;
        }
    }
}

void Ps1Spu::key_off(uint32_t mask) noexcept {
    for (int i = 0; i < kNumVoices; ++i) {
        if ((mask & (1U << i)) != 0U) {
            voices_[static_cast<size_t>(i)].active = false;
            voices_[static_cast<size_t>(i)].adsr_vol = 0;
        }
    }
}

uint16_t Ps1Spu::read16(uint32_t addr) noexcept {
    const uint32_t off = addr & 0x3FFU;
    if (off < 0x180U) {
        const size_t vi = off >> 4U;
        const uint32_t reg = off & 0x0FU;
        const auto& v = voices_[vi];
        switch (reg) {
            case 0x0:
                return static_cast<uint16_t>(v.vol_l);
            case 0x2:
                return static_cast<uint16_t>(v.vol_r);
            case 0x4:
                return v.pitch;
            case 0x6:
                return v.start_addr;
            case 0x8:
                return v.adsr_lo;
            case 0xA:
                return v.adsr_hi;
            case 0xC:
                return static_cast<uint16_t>(v.adsr_vol);
            case 0xE:
                return v.repeat_addr;
            default:
                return 0U;
        }
    }
    switch (off) {
        case 0x180:
            return static_cast<uint16_t>(main_vol_l_);
        case 0x182:
            return static_cast<uint16_t>(main_vol_r_);
        case 0x188:
            return static_cast<uint16_t>(key_on_reg_ & 0xFFFFU);
        case 0x18A:
            return static_cast<uint16_t>(key_on_reg_ >> 16U);
        case 0x18C:
            return static_cast<uint16_t>(key_off_reg_ & 0xFFFFU);
        case 0x18E:
            return static_cast<uint16_t>(key_off_reg_ >> 16U);
        case 0x19C: {
            uint16_t endx = 0;
            for (int i = 0; i < 16; ++i)
                if (voices_[static_cast<size_t>(i)].endx)
                    endx |= (1U << i);
            return endx;
        }
        case 0x19E: {
            uint16_t endx = 0;
            for (int i = 16; i < 24; ++i)
                if (voices_[static_cast<size_t>(i)].endx)
                    endx |= (1U << (i - 16));
            return endx;
        }
        case 0x1A6:
            return transfer_addr_reg_;
        case 0x1AA:
            return spucnt_;
        case 0x1AC:
            return transfer_ctrl_;
        case 0x1AE:
            return static_cast<uint16_t>(spucnt_ & 0x3FU);  // SPUSTAT
        default:
            return 0U;
    }
}

void Ps1Spu::write16(uint32_t addr, uint16_t val) noexcept {
    const uint32_t off = addr & 0x3FFU;
    if (off < 0x180U) {
        const size_t vi = off >> 4U;
        const uint32_t reg = off & 0x0FU;
        auto& v = voices_[vi];
        switch (reg) {
            case 0x0:
                v.vol_l = static_cast<int16_t>(val << 1U);
                break;
            case 0x2:
                v.vol_r = static_cast<int16_t>(val << 1U);
                break;
            case 0x4:
                v.pitch = val;
                break;
            case 0x6:
                v.start_addr = val;
                break;
            case 0x8:
                v.adsr_lo = val;
                break;
            case 0xA:
                v.adsr_hi = val;
                break;
            case 0xC:
                v.adsr_vol = static_cast<int16_t>(val);
                break;
            case 0xE:
                v.repeat_addr = val;
                break;
            default:
                break;
        }
        return;
    }
    switch (off) {
        case 0x180:
            main_vol_l_ = static_cast<int16_t>(val << 1U);
            break;
        case 0x182:
            main_vol_r_ = static_cast<int16_t>(val << 1U);
            break;
        case 0x188:
            key_on_reg_ = (key_on_reg_ & 0xFFFF0000U) | val;
            key_on(val);
            break;
        case 0x18A:
            key_on_reg_ = (key_on_reg_ & 0x0000FFFFU) | (static_cast<uint32_t>(val) << 16U);
            key_on(static_cast<uint32_t>(val) << 16U);
            break;
        case 0x18C:
            key_off_reg_ = (key_off_reg_ & 0xFFFF0000U) | val;
            key_off(val);
            break;
        case 0x18E:
            key_off_reg_ = (key_off_reg_ & 0x0000FFFFU) | (static_cast<uint32_t>(val) << 16U);
            key_off(static_cast<uint32_t>(val) << 16U);
            break;
        case 0x1A6:
            transfer_addr_reg_ = val;
            transfer_ptr_ = (static_cast<uint32_t>(val) << 3U) & (kSpuRamSize - 1U);
            break;
        case 0x1A8:
            dma_write16(val);
            break;
        case 0x1AA:
            spucnt_ = val;
            break;
        case 0x1AC:
            transfer_ctrl_ = val;
            break;
        default:
            break;
    }
}

void Ps1Spu::dma_write16(uint16_t val) noexcept {
    ram_[transfer_ptr_ & (kSpuRamSize - 1U)] = static_cast<uint8_t>(val & 0xFFU);
    ram_[(transfer_ptr_ + 1U) & (kSpuRamSize - 1U)] = static_cast<uint8_t>(val >> 8U);
    transfer_ptr_ = (transfer_ptr_ + 2U) & (kSpuRamSize - 1U);
}

uint16_t Ps1Spu::dma_read16() noexcept {
    const uint16_t lo = ram_[transfer_ptr_ & (kSpuRamSize - 1U)];
    const uint16_t hi = ram_[(transfer_ptr_ + 1U) & (kSpuRamSize - 1U)];
    transfer_ptr_ = (transfer_ptr_ + 2U) & (kSpuRamSize - 1U);
    return static_cast<uint16_t>(lo | (hi << 8U));
}

void Ps1Spu::step_samples(size_t stereo_frames) {
    const bool enabled = (spucnt_ & 0x8000U) != 0U;
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t f = 0; f < stereo_frames; ++f) {
        int32_t mix_l = 0;
        int32_t mix_r = 0;
        if (enabled) {
            for (auto& v : voices_) {
                if (!v.active)
                    continue;
                if (v.sample_idx >= 28) {
                    decode_adpcm_block(v);
                    if (!v.active)
                        continue;
                }
                const int32_t s = v.decoded[static_cast<size_t>(v.sample_idx)];
                mix_l += (s * v.vol_l) >> 15;
                mix_r += (s * v.vol_r) >> 15;

                const uint32_t step = std::min<uint32_t>(v.pitch, 0x4000U);
                v.pitch_counter += step;
                while (v.pitch_counter >= 0x1000U) {
                    v.pitch_counter -= 0x1000U;
                    ++v.sample_idx;
                }
            }
            mix_l = (mix_l * main_vol_l_) >> 15;
            mix_r = (mix_r * main_vol_r_) >> 15;
        }
        const int16_t out_l = static_cast<int16_t>(std::clamp(mix_l, -32768, 32767));
        const int16_t out_r = static_cast<int16_t>(std::clamp(mix_r, -32768, 32767));
        if (count_ + 2U > kRingCapacitySamples) {
            read_pos_ = (read_pos_ + 2U) % kRingCapacitySamples;
            count_ -= 2U;
        }
        ring_[write_pos_] = out_l;
        write_pos_ = (write_pos_ + 1U) % kRingCapacitySamples;
        ring_[write_pos_] = out_r;
        write_pos_ = (write_pos_ + 1U) % kRingCapacitySamples;
        count_ += 2U;
    }
}

void Ps1Spu::pop_stereo(int16_t* dst, size_t stereo_frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t wanted = stereo_frames * 2U;
    const size_t avail = std::min(count_, wanted);
    for (size_t i = 0; i < avail; ++i) {
        dst[i] = ring_[read_pos_];
        read_pos_ = (read_pos_ + 1U) % kRingCapacitySamples;
    }
    count_ -= avail;
    if (avail < wanted) {
        std::memset(dst + avail, 0, (wanted - avail) * sizeof(int16_t));
    }
}

size_t Ps1Spu::queued_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ / 2U;
}

}  // namespace dragon::ps1
