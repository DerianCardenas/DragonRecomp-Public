#pragma once

// Texas Instruments / Sega SN76489 Programmable Sound Generator (PSG).
// Reference: https://www.smspower.org/Development/SN76489, docs/SMS_SPEC.md §5.

#include <array>
#include <cstddef>
#include <cstdint>

namespace dragon::sms {

struct PsgState {
    uint16_t tone_period[3] = {0, 0, 0};
    uint16_t tone_counter[3] = {0, 0, 0};
    int8_t tone_polarity[3] = {1, 1, 1};

    uint8_t noise_control = 0;
    uint16_t noise_counter = 0;
    uint16_t lfsr = 0x8000U;
    int8_t noise_polarity = 1;

    uint8_t volume[4] = {0x0FU, 0x0FU, 0x0FU, 0x0FU};  // 0x0 = max, 0xF = silent
    uint8_t latched_channel = 0;
    uint8_t latched_is_volume = 0;
    uint8_t gg_stereo = 0xFFU;  // Game Gear port $06 stereo panning (all L+R on SMS)
};

class Psg {
public:
    static constexpr uint32_t kCpuClockHz = 3579545U;
    static constexpr uint32_t kSampleRate = 48000U;
    static constexpr size_t kRingSamples = 8192U;  // Stereo frames (x2 int16)

    Psg();

    void reset() noexcept;
    void write(uint8_t value) noexcept;
    void write_stereo(uint8_t value) noexcept { st_.gg_stereo = value; }

    // Advance PSG by `cpu_cycles` Z80 T-cycles and enqueue 48 kHz stereo PCM samples
    void step(uint32_t cpu_cycles) noexcept;

    // Drain interleaved stereo int16 PCM samples into `dst` (max_frames stereo pairs).
    size_t drain_samples(int16_t* dst, size_t max_frames) noexcept;
    size_t available_frames() const noexcept;

    const PsgState& state() const noexcept { return st_; }
    void set_state(const PsgState& s) noexcept { st_ = s; }

private:
    void tick_psg() noexcept;
    void emit_sample() noexcept;

    PsgState st_{};
    uint32_t div16_accum_ = 0;
    uint64_t sample_accum_ = 0;

    std::array<int16_t, kRingSamples * 2U> ring_{};
    size_t ring_read_ = 0;
    size_t ring_write_ = 0;
    size_t ring_count_ = 0;
};

}  // namespace dragon::sms
