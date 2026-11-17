#pragma once

// Ricoh 2A03 APU emulation for the NES runtime.
// References:
//   https://www.nesdev.org/wiki/APU
//   https://www.nesdev.org/wiki/APU_Pulse
//   https://www.nesdev.org/wiki/APU_Triangle
//   https://www.nesdev.org/wiki/APU_Noise
//   https://www.nesdev.org/wiki/APU_DMC
//   https://www.nesdev.org/wiki/APU_Frame_Counter
//   https://www.nesdev.org/wiki/APU_Mixer
// Hot-path rules (AGENTS.md): no exceptions, no heap allocation, no virtual dispatch.

#include <array>
#include <cstddef>
#include <cstdint>

namespace dragon::nes {

class Apu {
public:
    using MemoryReadCallback = uint8_t (*)(void* user_data, uint16_t addr) noexcept;

    static constexpr uint32_t kCpuClockHz = 1789773U;
    static constexpr uint32_t kSampleRate = 48000U;
    static constexpr size_t kSampleBufferCapacity = 16384U;

    Apu() noexcept;

    void reset() noexcept;

    void set_memory_reader(MemoryReadCallback cb, void* user_data) noexcept {
        mem_read_cb_ = cb;
        mem_read_user_ = user_data;
    }

    // CPU-facing register write ($4000-$4013, $4015, $4017).
    void write_register(uint16_t addr, uint8_t value) noexcept;

    // CPU-facing status read ($4015): returns status byte and clears frame IRQ flag.
    uint8_t read_status() noexcept;

    // Side-effect-free status inspection ($4015).
    uint8_t peek_status() const noexcept;

    // Advance APU by 1 CPU cycle or N CPU cycles.
    void step_cycle() noexcept;
    void step_cycles(uint64_t cpu_cycles) noexcept;

    // Interrupt state (level-triggered: active when frame_irq_ or dmc_irq_ is set).
    bool frame_irq() const noexcept { return frame_irq_; }
    bool dmc_irq() const noexcept { return dmc_irq_; }
    bool irq_pending() const noexcept { return frame_irq_ || dmc_irq_; }

    // Number of CPU cycles until the next potential frame IRQ edge (or 0 if already pending,
    // or a large value if frame IRQ is inhibited / 5-step mode).
    uint32_t cycles_until_next_irq() const noexcept;

    // Channel length counters (0 = Pulse 1, 1 = Pulse 2, 2 = Triangle, 3 = Noise) and DMC bytes.
    uint8_t length_counter(uint8_t channel) const noexcept;
    uint16_t dmc_bytes_remaining() const noexcept { return dmc_.bytes_remaining; }
    uint8_t dmc_output_level() const noexcept { return dmc_.output_level; }

    // Instantaneous non-linear mixer output in [0.0f, 1.0f].
    float current_mixer_output() const noexcept;

    // Downsampled 48 kHz mono audio stream accessors.
    size_t available_samples() const noexcept { return sample_count_; }
    uint64_t total_samples_generated() const noexcept { return total_samples_generated_; }
    size_t pop_samples(int16_t* dst, size_t max_count) noexcept;
    size_t pop_float_samples(float* dst, size_t max_count) noexcept;
    void clear_samples() noexcept;

    struct Envelope {
        bool start_flag = false;
        bool loop_flag = false;
        bool constant_volume = false;
        uint8_t volume_or_period = 0;
        uint8_t divider = 0;
        uint8_t decay_level = 0;

        void clock() noexcept;
        uint8_t output() const noexcept { return constant_volume ? volume_or_period : decay_level; }
    };

    struct PulseChannel {
        bool enabled = false;
        bool is_pulse1 = false;
        uint8_t duty = 0;
        uint8_t duty_pos = 0;
        uint8_t length_counter = 0;
        uint16_t timer_period = 0;
        uint16_t timer_value = 0;

        Envelope env{};

        bool sweep_enabled = false;
        uint8_t sweep_period = 0;
        bool sweep_negate = false;
        uint8_t sweep_shift = 0;
        uint8_t sweep_divider = 0;
        bool sweep_reload = false;

        uint16_t sweep_target_period() const noexcept;
        bool is_sweep_muted() const noexcept;
        void clock_timer() noexcept;
        void clock_length() noexcept;
        void clock_sweep() noexcept;
        uint8_t output() const noexcept;
    };

    struct TriangleChannel {
        bool enabled = false;
        bool control_flag = false;
        uint8_t linear_reload_value = 0;
        uint8_t linear_counter = 0;
        bool linear_reload_flag = false;
        uint8_t length_counter = 0;
        uint16_t timer_period = 0;
        uint16_t timer_value = 0;
        uint8_t seq_pos = 0;

        void clock_timer() noexcept;
        void clock_linear() noexcept;
        void clock_length() noexcept;
        uint8_t output() const noexcept;
    };

    struct NoiseChannel {
        bool enabled = false;
        bool mode = false;
        uint8_t length_counter = 0;
        uint16_t timer_period = 4;
        uint16_t timer_value = 0;
        uint16_t shift_reg = 1;

        Envelope env{};

        void clock_timer() noexcept;
        void clock_length() noexcept;
        uint8_t output() const noexcept;
    };

    struct DmcChannel {
        bool enabled = false;
        bool irq_enabled = false;
        bool loop = false;
        uint16_t timer_period = 428;
        uint16_t timer_value = 428;
        uint8_t output_level = 0;
        uint16_t sample_address = 0xC000;
        uint16_t sample_length = 1;
        uint16_t current_address = 0xC000;
        uint16_t bytes_remaining = 0;
        uint8_t sample_buffer = 0;
        bool sample_buffer_empty = true;
        uint8_t shift_reg = 0;
        uint8_t bits_remaining = 8;
        bool silence_flag = true;
    };

    struct Snapshot {
        uint64_t total_cpu_cycles = 0;
        uint32_t frame_counter_cycles = 0;
        bool five_step_mode = false;
        bool irq_inhibit = false;
        bool frame_irq = false;
        bool dmc_irq = false;
        PulseChannel pulse1{};
        PulseChannel pulse2{};
        TriangleChannel triangle{};
        NoiseChannel noise{};
        DmcChannel dmc{};
    };

    void save_snapshot(Snapshot& snap) const noexcept;
    void load_snapshot(const Snapshot& snap) noexcept;

private:
    void clock_quarter_frame() noexcept;
    void clock_half_frame() noexcept;
    void clock_dmc_timer() noexcept;
    void fetch_dmc_sample_byte() noexcept;
    void push_output_sample(float mixed) noexcept;

    PulseChannel pulse1_{};
    PulseChannel pulse2_{};
    TriangleChannel triangle_{};
    NoiseChannel noise_{};
    DmcChannel dmc_{};

    MemoryReadCallback mem_read_cb_ = nullptr;
    void* mem_read_user_ = nullptr;

    uint64_t total_cpu_cycles_ = 0ULL;
    uint32_t frame_counter_cycles_ = 0U;
    bool five_step_mode_ = false;
    bool irq_inhibit_ = false;
    bool frame_irq_ = false;
    bool dmc_irq_ = false;

    // Resampling & high-pass filtering state (1.789773 MHz -> 48 kHz)
    uint32_t resample_phase_ = 0U;
    float mix_accum_ = 0.0f;
    uint32_t mix_accum_count_ = 0U;
    float hp_prev_in_ = 0.0f;
    float hp_prev_out_ = 0.0f;

    std::array<int16_t, kSampleBufferCapacity> sample_buf_i16_{};
    std::array<float, kSampleBufferCapacity> sample_buf_f32_{};
    size_t sample_read_idx_ = 0U;
    size_t sample_write_idx_ = 0U;
    size_t sample_count_ = 0U;
    uint64_t total_samples_generated_ = 0ULL;

    std::array<float, 31> pulse_mixer_table_{};
    std::array<float, 203> tnd_mixer_table_{};
};

}  // namespace dragon::nes
