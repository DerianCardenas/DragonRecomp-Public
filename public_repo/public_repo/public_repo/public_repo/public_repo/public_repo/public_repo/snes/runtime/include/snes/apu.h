#pragma once

// SNES Audio Subsystem (S-SMP + S-DSP):
//   - SPC700 8-bit sound coprocessor (1.024 MHz, 256 opcodes, 64 KiB ARAM, 64-byte IPL ROM)
//   - 4 communication ports ($2140-$2143 <-> $F4-$F7)
//   - Timers 0, 1 (8 kHz) and 2 (64 kHz)
//   - S-DSP 8-voice polyphony:
//       * BRR ADPCM 4-bit decoding with 4 filters and loop/end flags
//       * ADSR and GAIN envelope generation
//       * Volume control (Left/Right per voice + Master Volume)
//       * Resampled 48 kHz 16-bit stereo PCM output for SDL3
// References:
//   https://snes.nesdev.org/wiki/Audio_subsystem
//   https://problemkaputt.de/fullsnes.htm#snesaudioprocessingunitapu
// Hot-path rules (AGENTS.md): no exceptions, no heap allocation, no virtual dispatch.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dragon::snes {

struct DspVoice {
    int8_t vol_l = 0;
    int8_t vol_r = 0;
    uint16_t pitch = 0U;
    uint8_t srcn = 0U;
    uint8_t adsr1 = 0U;
    uint8_t adsr2 = 0U;
    uint8_t gain = 0U;
    uint8_t envx = 0U;
    int8_t outx = 0;

    // Internal playback state
    bool active = false;
    uint16_t brr_block_addr = 0U;
    uint16_t loop_addr = 0U;
    uint8_t sample_pos = 0U;  // 0..15 within current decoded 16-sample buffer
    uint32_t frac_pos = 0U;   // 12-bit fractional accumulator
    std::array<int16_t, 16> decoded_samples{};
    int16_t prev1 = 0;
    int16_t prev2 = 0;

    enum class EnvState : uint8_t {
        Attack,
        Decay,
        Sustain,
        Release,
    };
    EnvState env_state = EnvState::Release;
    int32_t env_level = 0;  // 0..0x7FF
};

class Apu {
public:
    static constexpr size_t kAramSize = 0x10000U;  // 64 KiB
    static constexpr size_t kVoiceCount = 8U;
    static constexpr uint32_t kDspSampleRate = 32000U;
    static constexpr uint32_t kOutputSampleRate = 48000U;

    Apu() noexcept;

    void reset() noexcept;

    // Communication ports accessed by main 65816 CPU ($2140-$2143 -> port 0..3)
    uint8_t read_port(uint8_t port) noexcept;
    void write_port(uint8_t port, uint8_t value) noexcept;

    // Synchronize SPC700 execution up to CPU master cycles
    void catchup(uint64_t cpu_master_cycles) noexcept;

    // Step SPC700 by a number of SPC clock cycles (1.024 MHz)
    void step_cycles(uint64_t cycles) noexcept;

    // Render 48 kHz 16-bit interleaved stereo PCM audio samples
    void render_samples(std::span<int16_t> out_stereo) noexcept;

    // Inspection and test accessors
    const std::array<uint8_t, kAramSize>& aram() const noexcept { return aram_; }
    std::array<uint8_t, kAramSize>& aram() noexcept { return aram_; }
    bool ipl_rom_enabled() const noexcept { return ipl_rom_enabled_; }
    uint16_t pc() const noexcept { return pc_; }
    uint8_t sp() const noexcept { return sp_; }
    uint8_t psw() const noexcept { return psw_; }
    uint8_t a() const noexcept { return a_; }
    uint8_t x() const noexcept { return x_; }
    uint8_t y() const noexcept { return y_; }

    const std::array<DspVoice, kVoiceCount>& voices() const noexcept { return voices_; }
    std::array<DspVoice, kVoiceCount>& voices() noexcept { return voices_; }

    // Direct DSP register write for tests
    void write_dsp_register(uint8_t reg, uint8_t val) noexcept;
    uint8_t read_dsp_register(uint8_t reg) noexcept;

    // Key on a voice for tests/playback
    void key_on(uint8_t voice_mask) noexcept;

    struct Timer {
        uint8_t target = 0U;
        uint8_t counter = 0U;
        uint16_t divider = 0U;
        uint16_t divider_max = 128U;  // 128 for T0/T1 (8 kHz), 16 for T2 (64 kHz)
        uint8_t stage1 = 0U;
        bool enabled = false;
    };

    struct Snapshot {
        std::array<uint8_t, kAramSize> aram{};
        uint16_t pc = 0xFFC0U;
        uint8_t a = 0U;
        uint8_t x = 0U;
        uint8_t y = 0U;
        uint8_t sp = 0xEFU;
        uint8_t psw = 0U;
        bool halted = false;
        bool ipl_rom_enabled = true;
        uint8_t dsp_addr = 0U;
        std::array<uint8_t, 4> cpu_to_spc{};
        std::array<uint8_t, 4> spc_to_cpu{};
        std::array<Timer, 3> timers{};
        std::array<uint8_t, 128> dsp_regs{};
        std::array<DspVoice, kVoiceCount> voices{};
        int8_t mvol_l = 127;
        int8_t mvol_r = 127;
        uint8_t dir_page = 0U;
        uint8_t endx = 0U;
        uint64_t spc_cycles_executed = 0ULL;
        uint64_t last_cpu_master_cycles = 0ULL;
    };

    void save_snapshot(Snapshot& snap) const noexcept;
    void load_snapshot(const Snapshot& snap) noexcept;

private:
    uint8_t spc_read(uint16_t addr) noexcept;
    void spc_write(uint16_t addr, uint8_t val) noexcept;
    uint16_t spc_read16(uint16_t addr) noexcept;
    void spc_write16(uint16_t addr, uint16_t val) noexcept;

    uint8_t spc_pop() noexcept;
    void spc_push(uint8_t val) noexcept;

    void step_spc_instruction() noexcept;
    void step_timers(uint8_t cycles) noexcept;
    void decode_next_brr_block(DspVoice& v) noexcept;
    void step_dsp_32k() noexcept;

    std::array<uint8_t, kAramSize> aram_{};

    // SPC700 CPU Registers
    uint16_t pc_ = 0xFFC0U;
    uint8_t a_ = 0U;
    uint8_t x_ = 0U;
    uint8_t y_ = 0U;
    uint8_t sp_ = 0xEFU;
    uint8_t psw_ = 0U;
    bool halted_ = false;

    // I/O & Control
    bool ipl_rom_enabled_ = true;
    uint8_t dsp_addr_ = 0U;
    std::array<uint8_t, 4> cpu_to_spc_{};
    std::array<uint8_t, 4> spc_to_cpu_{};
    std::array<Timer, 3> timers_{};

    // DSP State
    std::array<uint8_t, 128> dsp_regs_{};
    std::array<DspVoice, kVoiceCount> voices_{};
    int8_t mvol_l_ = 127;
    int8_t mvol_r_ = 127;
    uint8_t dir_page_ = 0U;
    uint8_t endx_ = 0U;

    // Synchronization with main CPU
    uint64_t spc_cycles_executed_ = 0ULL;
    uint64_t last_cpu_master_cycles_ = 0ULL;

    // Resampler accumulator for 32 kHz -> 48 kHz
    uint32_t resample_phase_ = 0U;
    int16_t current_dsp_l_ = 0;
    int16_t current_dsp_r_ = 0;
};

}  // namespace dragon::snes
