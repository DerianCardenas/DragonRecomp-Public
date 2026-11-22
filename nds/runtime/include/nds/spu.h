#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dragon::nds {

class Bus;

/*
 * Nintendo DS 16-Channel Sound Processing Unit (SPU) — ARM7 @ 33.51 MHz.
 * Channels 0..15: PCM8, PCM16, IMA-ADPCM.
 * Channels 8..13: PSG Square Wave; Channels 14..15: PSG White Noise.
 */
class Spu {
public:
    static constexpr uint32_t kSampleRate = 48000U;
    static constexpr size_t kRingFrames = 8192U;

    void reset() noexcept;
    void step(uint32_t arm7_cycles, const Bus& bus) noexcept;

    uint32_t read32(uint32_t offset) const noexcept;
    void write32(uint32_t offset, uint32_t val, uint32_t mask, const Bus& bus) noexcept;

    uint16_t read_soundcnt() const noexcept { return soundcnt_; }
    void write_soundcnt(uint16_t val) noexcept { soundcnt_ = val; }
    uint16_t read_soundbias() const noexcept { return soundbias_; }
    void write_soundbias(uint16_t val) noexcept { soundbias_ = val & 0x03FFU; }

    size_t drain_samples(int16_t* dst, size_t max_frames) noexcept;

private:
    struct Channel {
        uint32_t cnt = 0;
        uint32_t sad = 0;
        uint16_t tmr = 0;
        uint16_t pnt = 0;
        uint32_t len = 0;

        bool active = false;
        uint32_t cur_addr = 0;
        uint32_t byte_pos = 0;
        uint32_t timer_acc = 0;
        int16_t cur_sample = 0;

        // IMA-ADPCM state
        int32_t adpcm_val = 0;
        int32_t adpcm_idx = 0;
        int32_t adpcm_loop_val = 0;
        int32_t adpcm_loop_idx = 0;
        bool adpcm_high_nibble = false;
        bool adpcm_header_read = false;

        // PSG state
        uint8_t psg_phase = 0;
        uint16_t lfsr = 0x7FFFU;
    };

    std::array<Channel, 16> ch_{};
    uint16_t soundcnt_ = 0;
    uint16_t soundbias_ = 0x0200U;
    uint64_t sample_acc_ = 0;

    std::array<int16_t, kRingFrames * 2U> ring_{};
    size_t ring_read_ = 0;
    size_t ring_write_ = 0;
    size_t ring_count_ = 0;

    void start_channel(int idx, const Bus& bus) noexcept;
    void step_channel(int idx, uint32_t cycles, const Bus& bus) noexcept;
    void mix_sample() noexcept;
};

}  // namespace dragon::nds
