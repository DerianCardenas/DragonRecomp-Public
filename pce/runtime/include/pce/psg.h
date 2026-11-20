#pragma once

// Hudson Soft HuC6280 6-channel Wavetable PSG (+ Noise ch 4-5, LFO ch 1->0, DDA).
// Reference: docs/PCE_SPEC.md §4, Charles MacDonald pcetech.txt.

#include <array>
#include <cstddef>
#include <cstdint>

namespace dragon::pce {

struct PsgChannel {
    uint16_t freq = 0;         // 12-bit frequency divider
    uint8_t control = 0;       // Bit 7: enable, Bit 6: DDA, Bits 4-0: volume
    uint8_t balance = 0xFFU;   // Bits 7-4: left, Bits 3-0: right
    uint8_t noise_ctrl = 0;    // Ch 4-5: Bit 7: noise enable, Bits 4-0: noise freq
    std::array<uint8_t, 32> wave{};
    uint8_t wave_pos = 0;
    uint8_t dda_sample = 0;
    uint32_t counter = 0;
    uint32_t noise_counter = 0;
    uint32_t lfsr = 1U;
};

struct PsgState {
    uint8_t ch_select = 0;
    uint8_t main_balance = 0xFFU;
    uint8_t lfo_freq = 0;
    uint8_t lfo_ctrl = 0;
    uint32_t lfo_counter = 0;
    std::array<PsgChannel, 6> ch{};
    uint64_t sample_accum = 0;
    uint32_t psg_clock_rem = 0;
    int32_t lp_l = 0;
    int32_t lp_r = 0;
};

class Psg {
public:
    static constexpr uint32_t kCpuClockHz = 7159090U;
    static constexpr uint32_t kSampleRate = 48000U;
    static constexpr size_t kRingFrames = 8192U;

    Psg();
    void reset() noexcept;

    void write(uint8_t reg, uint8_t val) noexcept;
    void step(uint32_t cpu_cycles) noexcept;

    size_t drain_samples(int16_t* dst_interleaved, size_t max_frames) noexcept;
    size_t available_frames() const noexcept;

    PsgState state() const noexcept { return st_; }
    void set_state(const PsgState& s) noexcept { st_ = s; }

private:
    void tick_psg(uint32_t psg_clocks) noexcept;
    void emit_sample() noexcept;

    PsgState st_{};
    std::array<int16_t, kRingFrames * 2> ring_{};
    size_t ring_read_ = 0;
    size_t ring_write_ = 0;
};

}  // namespace dragon::pce
