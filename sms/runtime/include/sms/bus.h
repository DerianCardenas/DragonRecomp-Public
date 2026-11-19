#pragma once

// Sega Master System & Game Gear memory bus, I/O port decoder, input, and save states.
// Reference: https://www.smspower.org/Development/PortMapping, docs/SMS_SPEC.md §3.

#include <array>
#include <cstdint>
#include <filesystem>

#include "sms/cartridge.h"
#include "sms/cpu_state.h"
#include "sms/psg.h"
#include "sms/vdp.h"

namespace dragon::sms {

struct InputState {
    // Player 1
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool button1 = false;  // Fire 1 / A
    bool button2 = false;  // Fire 2 / B
    bool pause_or_start = false;  // SMS: triggers NMI; GG: Port $00 bit 7

    // Player 2 (SMS)
    bool p2_up = false;
    bool p2_down = false;
    bool p2_left = false;
    bool p2_right = false;
    bool p2_button1 = false;
    bool p2_button2 = false;
};

class Bus {
public:
    static constexpr size_t kWramSize = 8192U;

    Bus();

    void reset(SmsCpuState& cpu) noexcept;

    uint8_t read(uint16_t addr) const noexcept;
    void write(uint16_t addr, uint8_t value) noexcept;

    uint8_t io_read(uint8_t port) noexcept;
    void io_write(uint8_t port, uint8_t value) noexcept;

    // Advance VDP & PSG to catch up with `cpu.cycles`
    void sync_hardware(SmsCpuState& cpu) noexcept;

    // Update input state; on SMS, rising edge of Pause asserts NMI
    void set_input(const InputState& in, SmsCpuState& cpu) noexcept;

    // Recompiled dispatch table registration
    void set_dispatch_table(const SmsRecompFuncEntry* table, size_t count) noexcept {
        dispatch_table_ = table;
        dispatch_count_ = count;
    }
    SmsRecompFunc lookup_recomp(uint8_t bank, uint16_t addr) const noexcept;

    void set_log_fallbacks(bool enabled) noexcept { log_fallbacks_ = enabled; }
    bool log_fallbacks() const noexcept { return log_fallbacks_; }
    uint64_t fallback_count() const noexcept { return fallback_count_; }
    void record_fallback(uint8_t bank, uint16_t addr, uint16_t from_pc) noexcept;

    // Save States (DRST + SMS1 format)
    bool save_state(const std::filesystem::path& path, const SmsCpuState& cpu) const;
    bool load_state(const std::filesystem::path& path, SmsCpuState& cpu);

    Cartridge& cartridge() noexcept { return cart_; }
    const Cartridge& cartridge() const noexcept { return cart_; }
    Vdp& vdp() noexcept { return vdp_; }
    const Vdp& vdp() const noexcept { return vdp_; }
    Psg& psg() noexcept { return psg_; }
    const Psg& psg() const noexcept { return psg_; }
    uint8_t* wram() noexcept { return wram_.data(); }

    void set_frame_stop(bool stop) noexcept { stop_requested_ = stop; }
    bool stop_requested() const noexcept { return stop_requested_; }

    using FrameCallback = void (*)(SmsCpuState* cpu, Bus* bus, void* user_data);
    void set_frame_callback(FrameCallback cb, void* user_data) noexcept {
        frame_callback_ = cb;
        frame_callback_user_ = user_data;
    }

private:
    Cartridge cart_;
    Vdp vdp_;
    Psg psg_;
    std::array<uint8_t, kWramSize> wram_{};

    InputState input_{};
    bool prev_pause_ = false;
    uint8_t mem_control_ = 0xABU;
    uint8_t io_control_ = 0xFFU;  // Port $3F (TH/TR pins for Export region check)
    std::array<uint8_t, 6> gg_regs_{{0xC0U, 0x7FU, 0xFFU, 0x00U, 0xFFU, 0x00U}};

    uint64_t last_sync_cycles_ = 0;

    const SmsRecompFuncEntry* dispatch_table_ = nullptr;
    size_t dispatch_count_ = 0;
    bool log_fallbacks_ = true;
    uint64_t fallback_count_ = 0;
    bool stop_requested_ = false;
    FrameCallback frame_callback_ = nullptr;
    void* frame_callback_user_ = nullptr;
};

}  // namespace dragon::sms
