#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace dragon::gba {

class Apu {
public:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kCpuClockHz = 16777216;

    Apu();

    void reset();
    void step(uint32_t cycles);

    uint8_t read_io8(uint32_t offset) const noexcept;
    uint16_t read_io16(uint32_t offset) const noexcept;
    void write_io8(uint32_t offset, uint8_t value) noexcept;
    void write_io16(uint32_t offset, uint16_t value) noexcept;
    void write_fifo32(int fifo_idx, uint32_t value) noexcept;

    // Called when Timer 0 or Timer 1 overflows.
    // Returns bitmask: bit 0 = FIFO A needs DMA refill, bit 1 = FIFO B needs DMA refill.
    uint8_t on_timer_overflow(int timer_idx) noexcept;

    std::span<const int16_t> samples() const noexcept { return sample_buffer_; }
    void clear_samples() noexcept { sample_buffer_.clear(); }

    size_t fifo_size(int fifo_idx) const noexcept { return fifos_[fifo_idx & 1].count; }

    int8_t fifo_latch(int fifo_idx) const noexcept { return fifos_[fifo_idx & 1].latch; }

private:
    struct DmaFifo {
        std::array<int8_t, 32> buf{};
        uint8_t head = 0;
        uint8_t tail = 0;
        uint8_t count = 0;
        int8_t latch = 0;

        void clear() noexcept {
            head = 0;
            tail = 0;
            count = 0;
            latch = 0;
        }

        void push8(int8_t sample) noexcept {
            if (count < 32U) {
                buf[tail] = sample;
                tail = (tail + 1U) & 31U;
                ++count;
            }
        }

        int8_t pop8() noexcept {
            if (count > 0U) {
                latch = buf[head];
                head = (head + 1U) & 31U;
                --count;
            }
            return latch;
        }
    };

    struct SquareChannel {
        uint16_t cnt_l = 0;
        uint16_t cnt_h = 0;
        uint16_t cnt_x = 0;
        uint32_t phase_timer = 0;
        uint8_t duty_pos = 0;
        uint8_t volume = 0;
        uint8_t env_timer = 0;
        uint16_t length_counter = 0;
        uint16_t shadow_freq = 0;
        uint8_t sweep_timer = 0;
        bool sweep_enabled = false;
        bool enabled = false;
    };

    struct WaveChannel {
        uint16_t cnt_l = 0;
        uint16_t cnt_h = 0;
        uint16_t cnt_x = 0;
        uint32_t phase_timer = 0;
        uint8_t wave_pos = 0;
        uint16_t length_counter = 0;
        bool enabled = false;
    };

    struct NoiseChannel {
        uint16_t cnt_l = 0;
        uint16_t cnt_h = 0;
        uint32_t phase_timer = 0;
        uint16_t lfsr = 0x7FFFU;
        uint8_t volume = 0;
        uint8_t env_timer = 0;
        uint16_t length_counter = 0;
        bool enabled = false;
    };

    void step_frame_sequencer() noexcept;
    void sample_frame() noexcept;

    std::array<DmaFifo, 2> fifos_{};
    SquareChannel sq1_{};
    SquareChannel sq2_{};
    WaveChannel wave_{};
    NoiseChannel noise_{};
    std::array<std::array<uint8_t, 16>, 2> wave_ram_{};

    uint16_t soundcnt_l_ = 0;
    uint16_t soundcnt_h_ = 0;
    uint16_t soundcnt_x_ = 0;
    uint16_t soundbias_ = 0x0200;

    std::array<uint8_t, 0x50> io_regs_{};
    uint32_t frame_seq_cycles_ = 0;
    uint8_t frame_seq_step_ = 0;
    uint64_t sample_accum_ = 0;
    std::vector<int16_t> sample_buffer_;
};

}  // namespace dragon::gba
