#pragma once

// PC Engine / TurboGrafx-16 2 MiB MMU bus, Hardware I/O ($FF), Timer, Joypad, IRQ, and Save States.
// Reference: docs/PCE_SPEC.md §1, Charles MacDonald pcetech.txt.

#include <array>
#include <cstdint>
#include <filesystem>

#include "pce/cartridge.h"
#include "pce/cpu_state.h"
#include "pce/psg.h"
#include "pce/vdc.h"

namespace dragon::pce {

struct InputState {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool button1 = false;  // I
    bool button2 = false;  // II
    bool select = false;   // Select
    bool run = false;      // Run (Start)
};

class Bus {
public:
    static constexpr size_t kWramSize = 32768U;  // 8 KiB PCE ($F8) + 24 KiB SuperGrafx ($F9-$FB)

    // IRQ controller bits ($1402 mask / $1403 status)
    static constexpr uint8_t kIrq2Bit = 0x01U;  // External / BRK
    static constexpr uint8_t kIrq1Bit = 0x02U;  // VDC
    static constexpr uint8_t kTiqBit  = 0x04U;  // Internal Timer

    Bus();

    void reset(PceCpuState& cpu) noexcept;

    uint8_t read(const PceCpuState& cpu, uint16_t addr) noexcept;
    void write(PceCpuState& cpu, uint16_t addr, uint8_t value) noexcept;

    // Direct VDC port writes for HuC6280 ST0 / ST1 / ST2 (bypass MPR0)
    void write_vdc_direct(uint8_t port, uint8_t value) noexcept;

    // Advance VDC, PSG, and 7-bit Timer to catch up with cpu.cycles
    void sync_hardware(PceCpuState& cpu) noexcept;

    void set_input(const InputState& in) noexcept { input_ = in; }

    // Pending unmasked interrupt bits (bit 0=IRQ2, bit 1=IRQ1, bit 2=TIQ)
    uint8_t pending_irqs() const noexcept;

    // Recompiled dispatch table registration
    void set_dispatch_table(const PceRecompFuncEntry* table, size_t count) noexcept {
        dispatch_table_ = table;
        dispatch_count_ = count;
    }
    PceRecompFunc lookup_recomp(uint8_t bank, uint16_t addr) const noexcept;

    void set_log_fallbacks(bool enabled) noexcept { log_fallbacks_ = enabled; }
    bool log_fallbacks() const noexcept { return log_fallbacks_; }
    uint64_t fallback_count() const noexcept { return fallback_count_; }
    void record_fallback(uint8_t bank, uint16_t addr, uint16_t from_pc) noexcept;

    // Save States (DRST + PCE1 format)
    bool save_state(const std::filesystem::path& path, const PceCpuState& cpu) const;
    bool load_state(const std::filesystem::path& path, PceCpuState& cpu);

    Cartridge& cartridge() noexcept { return cart_; }
    const Cartridge& cartridge() const noexcept { return cart_; }
    Vdc& vdc() noexcept { return vdc_; }
    const Vdc& vdc() const noexcept { return vdc_; }
    Psg& psg() noexcept { return psg_; }
    const Psg& psg() const noexcept { return psg_; }
    uint8_t* wram() noexcept { return wram_.data(); }

    void set_frame_stop(bool stop) noexcept { stop_requested_ = stop; }
    bool stop_requested() const noexcept { return stop_requested_; }

    using FrameCallback = void (*)(PceCpuState* cpu, Bus* bus, void* user_data);
    void set_frame_callback(FrameCallback cb, void* user_data) noexcept {
        frame_callback_ = cb;
        frame_callback_user_ = user_data;
    }

private:
    uint8_t io_read(uint16_t offset) noexcept;
    void io_write(uint16_t offset, uint8_t value) noexcept;
    void step_timer(uint32_t cpu_cycles) noexcept;

    Cartridge cart_;
    Vdc vdc_;
    Psg psg_;
    std::array<uint8_t, kWramSize> wram_{};

    InputState input_{};
    uint8_t joy_sel_ = 0;      // Bit 0: SEL, Bit 1: CLR
    uint8_t io_buffer_ = 0;    // Last I/O data bus value for open-bus bits

    // Hardware Timer ($0C00-$0C01)
    uint8_t timer_reload_ = 0;
    uint8_t timer_counter_ = 0;
    uint8_t timer_enabled_ = 0;
    uint32_t timer_prescaler_ = 0;

    // Interrupt Controller ($1402-$1403)
    uint8_t irq_disable_ = 0;  // 1 = masked/disabled
    uint8_t irq_status_ = 0;   // Bit 2: TIQ pending latch

    uint64_t last_sync_cycles_ = 0;

    const PceRecompFuncEntry* dispatch_table_ = nullptr;
    size_t dispatch_count_ = 0;
    bool log_fallbacks_ = false;
    uint64_t fallback_count_ = 0;
    bool stop_requested_ = false;
    FrameCallback frame_callback_ = nullptr;
    void* frame_callback_user_ = nullptr;
};

}  // namespace dragon::pce
