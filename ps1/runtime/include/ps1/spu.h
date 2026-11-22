#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dragon::ps1 {

class Ps1Spu {
public:
    static constexpr size_t kSpuRamSize = 512U * 1024U;
    static constexpr int kNumVoices = 24;
    static constexpr uint32_t kSampleRate = 44100U;

    Ps1Spu();
    void reset();

    uint16_t read16(uint32_t addr) noexcept;
    void write16(uint32_t addr, uint16_t val) noexcept;

    // DMA channel 4 transfer helpers
    void dma_write16(uint16_t val) noexcept;
    uint16_t dma_read16() noexcept;

    // Synthesize stereo frames at 44.1 kHz into ring buffer
    void step_samples(size_t stereo_frames);
    void pop_stereo(int16_t* dst, size_t stereo_frames);
    size_t queued_frames() const;

    uint8_t* ram() noexcept { return ram_.data(); }
    const uint8_t* ram() const noexcept { return ram_.data(); }

private:
    struct Voice {
        int16_t vol_l = 0;
        int16_t vol_r = 0;
        uint16_t pitch = 0x1000;
        uint16_t start_addr = 0;   // x8 bytes
        uint16_t repeat_addr = 0;  // x8 bytes
        uint16_t adsr_lo = 0;
        uint16_t adsr_hi = 0;
        int16_t adsr_vol = 0;

        bool active = false;
        uint32_t cur_addr = 0;
        uint32_t pitch_counter = 0;
        int sample_idx = 28;
        std::array<int16_t, 28> decoded{};
        int32_t hist1 = 0;
        int32_t hist2 = 0;
        bool endx = false;
    };

    void decode_adpcm_block(Voice& v) noexcept;
    void key_on(uint32_t mask) noexcept;
    void key_off(uint32_t mask) noexcept;

    std::vector<uint8_t> ram_;
    std::array<Voice, kNumVoices> voices_{};
    int16_t main_vol_l_ = 0x3FFF;
    int16_t main_vol_r_ = 0x3FFF;
    uint16_t spucnt_ = 0;
    uint16_t spustat_ = 0;
    uint16_t transfer_addr_reg_ = 0;
    uint32_t transfer_ptr_ = 0;
    uint16_t transfer_ctrl_ = 4;
    uint32_t key_on_reg_ = 0;
    uint32_t key_off_reg_ = 0;

    mutable std::mutex mutex_;
    std::vector<int16_t> ring_;
    size_t read_pos_ = 0;
    size_t write_pos_ = 0;
    size_t count_ = 0;
};

}  // namespace dragon::ps1
