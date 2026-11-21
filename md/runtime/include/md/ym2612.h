#pragma once

// Yamaha YM2612 (OPN2) 6-channel 4-operator FM synthesizer + Channel 6 8-bit DAC.
// Reference: https://www.smspower.org/maxim/Documents/YM2612, docs/GENESIS_SPEC.md §4.
// Hot-path rules (AGENTS.md): no exceptions, no heap allocation, no virtual dispatch.

#include <array>
#include <cstddef>
#include <cstdint>

namespace dragon::md {

class Ym2612 {
public:
    static constexpr uint32_t kClockHz = 7670454U;
    static constexpr uint32_t kSampleRate = 48000U;
    static constexpr size_t kRingSamples = 8192U;

    Ym2612() noexcept;

    void reset() noexcept;

    // Port writes: port 0 = Bank0 addr, 1 = Bank0 data, 2 = Bank1 addr, 3 = Bank1 data
    void write(uint8_t port, uint8_t value) noexcept;
    uint8_t read_status() const noexcept;

    // Advance timers & FM synthesis by `m68k_cycles` and push 48 kHz stereo PCM into ring buffer
    void step(uint32_t m68k_cycles) noexcept;

    size_t drain_samples(int16_t* dst, size_t max_frames) noexcept;
    size_t available_frames() const noexcept { return ring_count_; }

    // Direct sample generation for mixed audio output (returns one stereo pair)
    void generate_sample(int32_t& out_left, int32_t& out_right) noexcept;

    bool dac_enabled() const noexcept { return dac_enable_; }
    uint8_t dac_sample() const noexcept { return dac_data_; }

private:
    enum class EnvPhase : uint8_t { Attack, Decay, Sustain, Release, Off };

    struct Operator {
        uint8_t dt = 0;      // Detune (0..7)
        uint8_t mul = 0;     // Multiple (0..15)
        uint8_t tl = 127;    // Total Level (0..127)
        uint8_t rs = 0;      // Rate Scaling (0..3)
        uint8_t ar = 0;      // Attack Rate (0..31)
        uint8_t am = 0;      // AM Enable (0..1)
        uint8_t d1r = 0;     // Decay Rate (0..31)
        uint8_t d2r = 0;     // Sustain Rate (0..31)
        uint8_t sl = 0;      // Sustain Level (0..15)
        uint8_t rr = 0;      // Release Rate (0..15)
        uint8_t ssg_eg = 0;  // SSG-EG (0..15)

        uint32_t phase_counter = 0;
        uint16_t env_level = 0x3FFU;  // 10-bit attenuation (0 = max, 0x3FF = silence)
        EnvPhase env_phase = EnvPhase::Off;
        bool key_on = false;
        int32_t prev_out[2] = {0, 0};  // Feedback history (op 0)
    };

    struct Channel {
        Operator op[4]{};
        uint16_t fnum = 0;
        uint8_t block = 0;
        uint8_t fnum_latch = 0;
        uint8_t alg = 0;
        uint8_t fb = 0;
        uint8_t pan_l = 1;
        uint8_t pan_r = 1;
        uint8_t ams = 0;
        uint8_t pms = 0;
    };

    void write_register(uint8_t bank, uint8_t reg, uint8_t value) noexcept;
    void tick_fm() noexcept;
    void tick_timers() noexcept;
    int32_t compute_operator(Operator& op, uint16_t fnum, uint8_t block, int32_t mod, bool use_fb,
                             uint8_t fb_level) noexcept;
    void step_envelope(Operator& op, uint8_t keycode) noexcept;

    std::array<Channel, 6> ch_{};
    // Channel 3 special per-operator frequency latches
    uint16_t ch3_fnum_[3] = {0, 0, 0};
    uint8_t ch3_block_[3] = {0, 0, 0};
    uint8_t ch3_latch_[3] = {0, 0, 0};
    uint8_t ch3_mode_ = 0;

    uint8_t addr_latch_[2] = {0, 0};
    bool lfo_enable_ = false;
    uint8_t lfo_freq_ = 0;
    uint32_t lfo_cnt_ = 0;
    uint8_t lfo_step_ = 0;

    // Timers A & B
    uint16_t timer_a_latch_ = 0;
    uint16_t timer_a_cnt_ = 0;
    uint8_t timer_b_latch_ = 0;
    uint16_t timer_b_cnt_ = 0;
    uint8_t timer_b_sub_ = 0;
    uint8_t timer_ctrl_ = 0;
    uint8_t status_ = 0;

    // Channel 6 DAC
    bool dac_enable_ = false;
    uint8_t dac_data_ = 0x80U;

    uint32_t fm_cycle_accum_ = 0;
    uint32_t eg_counter_ = 0;
    uint8_t eg_div_ = 0;
    uint64_t sample_accum_ = 0;

    std::array<int16_t, kRingSamples * 2U> ring_{};
    size_t ring_read_ = 0;
    size_t ring_write_ = 0;
    size_t ring_count_ = 0;
};

}  // namespace dragon::md
