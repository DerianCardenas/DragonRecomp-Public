#pragma once

// Game Boy / Game Boy Color APU (Audio Processing Unit) emulation.
// References:
//   https://gbdev.io/pandocs/Audio_Registers.html
//   https://gbdev.io/pandocs/Audio_details.html
// Hot-path rules (AGENTS.md): no exceptions, no heap allocation, no virtual dispatch.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dragon::gb {

class Apu {
public:
    static constexpr uint32_t kBaseClockHz = 4194304U;
    static constexpr uint32_t kSampleRate = 48000U;
    static constexpr size_t kMaxSamples = 16384U;

    Apu() noexcept;

    void reset() noexcept;

    // CPU-facing register read/write ($FF10 - $FF3F)
    uint8_t read_register(uint16_t addr) const noexcept;
    void write_register(uint16_t addr, uint8_t value) noexcept;

    // Advance APU by T-cycles
    void advance(uint32_t t_cycles) noexcept;

    // Read available stereo audio samples (pairs of int16_t: Left, Right)
    size_t available_samples() const noexcept { return sample_buffer_.size(); }
    std::span<const int16_t> samples() const noexcept { return sample_buffer_; }
    void clear_samples() noexcept { sample_buffer_.clear(); }

    bool channel_active(uint8_t ch) const noexcept;

    struct Channel1State {
        uint8_t nr10 = 0;
        uint8_t nr11 = 0;
        uint8_t nr12 = 0;
        uint8_t nr13 = 0;
        uint8_t nr14 = 0;
        bool active = false;
        uint16_t frequency = 0;
        uint32_t timer = 0;
        uint8_t duty_step = 0;
        uint8_t length = 0;
        bool length_enabled = false;
        uint8_t volume = 0;
        uint8_t env_timer = 0;
        uint16_t shadow_freq = 0;
        uint8_t sweep_timer = 0;
        bool sweep_enabled = false;
    };

    struct Channel2State {
        uint8_t nr21 = 0;
        uint8_t nr22 = 0;
        uint8_t nr23 = 0;
        uint8_t nr24 = 0;
        bool active = false;
        uint16_t frequency = 0;
        uint32_t timer = 0;
        uint8_t duty_step = 0;
        uint8_t length = 0;
        bool length_enabled = false;
        uint8_t volume = 0;
        uint8_t env_timer = 0;
    };

    struct Channel3State {
        uint8_t nr30 = 0;
        uint8_t nr31 = 0;
        uint8_t nr32 = 0;
        uint8_t nr33 = 0;
        uint8_t nr34 = 0;
        bool active = false;
        uint16_t frequency = 0;
        uint32_t timer = 0;
        uint8_t sample_idx = 0;
        uint16_t length = 0;
        bool length_enabled = false;
        std::array<uint8_t, 16> wave_ram{};
    };

    struct Channel4State {
        uint8_t nr41 = 0;
        uint8_t nr42 = 0;
        uint8_t nr43 = 0;
        uint8_t nr44 = 0;
        bool active = false;
        uint32_t timer = 0;
        uint16_t lfsr = 0x7FFF;
        uint8_t length = 0;
        bool length_enabled = false;
        uint8_t volume = 0;
        uint8_t env_timer = 0;
    };

    struct Snapshot {
        bool enabled = true;
        uint8_t nr50 = 0x77U;
        uint8_t nr51 = 0xF3U;
        uint8_t nr52 = 0xF1U;
        uint32_t frame_seq_timer = 0;
        uint8_t frame_seq_step = 0;
        uint32_t sample_timer = 0;
        Channel1State ch1{};
        Channel2State ch2{};
        Channel3State ch3{};
        Channel4State ch4{};
    };

    void save_snapshot(Snapshot& snap) const noexcept;
    void load_snapshot(const Snapshot& snap) noexcept;

private:
    bool enabled_ = true;

    // Master controls
    uint8_t nr50_ = 0x77U;  // Volume L/R
    uint8_t nr51_ = 0xF3U;  // Panning
    uint8_t nr52_ = 0xF1U;  // Power & Status

    // Frame sequencer (512 Hz)
    uint32_t frame_seq_timer_ = 0;
    uint8_t frame_seq_step_ = 0;

    // Sample generation
    uint32_t sample_timer_ = 0;
    std::vector<int16_t> sample_buffer_;

    Channel1State ch1_{};
    Channel2State ch2_{};
    Channel3State ch3_{};
    Channel4State ch4_{};

    void step_frame_sequencer() noexcept;
    void step_length_counters() noexcept;
    void step_envelopes() noexcept;
    void step_sweep() noexcept;
    void mix_audio_sample() noexcept;

    void trigger_ch1() noexcept;
    void trigger_ch2() noexcept;
    void trigger_ch3() noexcept;
    void trigger_ch4() noexcept;
};

}  // namespace dragon::gb
