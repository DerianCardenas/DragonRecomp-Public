#pragma once

// Game Boy system Bus: 64 KiB address space, MBC1/3/5 mappers, RAM, timers, and I/O.
// Hot-path rules (AGENTS.md): no exceptions, no heap allocation, no virtual dispatch.

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <vector>

#include "gb/apu.h"
#include "gb/cartridge.h"
#include "gb/cpu_state.h"
#include "gb/ppu.h"

namespace dragon::gb {

enum JoypadButton : uint8_t {
    kButtonA = 1U << 0U,
    kButtonB = 1U << 1U,
    kButtonSelect = 1U << 2U,
    kButtonStart = 1U << 3U,
    kButtonRight = 1U << 4U,
    kButtonLeft = 1U << 5U,
    kButtonUp = 1U << 6U,
    kButtonDown = 1U << 7U,
};

class Bus {
public:
    Bus() noexcept;

    void load_cartridge(const Cartridge& cart) noexcept;
    void load_rom(std::span<const uint8_t> rom) noexcept;

    // Unit test helpers
    void enable_flat_ram(bool enabled = true) noexcept { flat_ram_mode_ = enabled; }
    bool is_flat_ram() const noexcept { return flat_ram_mode_; }
    void load_direct(uint16_t addr, std::span<const uint8_t> bytes) noexcept;

    uint8_t read(uint16_t addr) noexcept;
    void write(uint16_t addr, uint8_t value) noexcept;
    uint8_t peek(uint16_t addr) const noexcept;

    uint8_t current_rom_bank() const noexcept {
        if (mbc_ == MbcType::Mbc1 && mbc1_mode_ == 1) {
            return static_cast<uint8_t>((rom_bank_ & 0x1FU) | (ram_bank_ << 5U));
        }
        return static_cast<uint8_t>(rom_bank_ & 0xFFU);
    }
    uint8_t current_ram_bank() const noexcept { return ram_bank_; }

    // Timer advancement (in T-cycles)
    void step_timers(uint32_t t_cycles) noexcept;

    // Interrupt helpers
    uint8_t interrupt_flags() const noexcept {
        return flat_ram_mode_ ? flat_ram_[0xFF0FU] : if_reg_;
    }
    uint8_t interrupt_enable() const noexcept {
        return flat_ram_mode_ ? flat_ram_[0xFFFFU] : ie_reg_;
    }
    void request_interrupt(uint8_t bit) noexcept {
        if (flat_ram_mode_) {
            flat_ram_[0xFF0FU] |= (1U << bit);
        } else {
            if_reg_ |= (1U << bit);
        }
    }
    void clear_interrupt(uint8_t bit) noexcept {
        if (flat_ram_mode_) {
            flat_ram_[0xFF0FU] &= ~(1U << bit);
        } else {
            if_reg_ &= ~(1U << bit);
        }
    }

    // Direct memory access for CPU state initialization
    uint8_t* hram_data() noexcept { return hram_.data(); }
    const uint8_t* rom_data() const noexcept { return rom_.data(); }
    uint32_t rom_size() const noexcept { return static_cast<uint32_t>(rom_.size()); }

    std::span<uint8_t> sram() noexcept { return sram_; }
    std::span<const uint8_t> sram() const noexcept { return sram_; }
    bool has_battery() const noexcept { return has_battery_; }
    bool load_battery_ram(const std::filesystem::path& path) noexcept;
    bool save_battery_ram(const std::filesystem::path& path) const noexcept;

    bool save_state(const CpuState& cpu, const std::filesystem::path& path) const noexcept;
    bool load_state(CpuState& cpu, const std::filesystem::path& path) noexcept;

    // Joypad input (bitmask of JoypadButton)
    void set_joypad(uint8_t buttons) noexcept { joypad_state_ = buttons; }
    uint8_t joypad() const noexcept { return joypad_state_; }

    // Serial output log (used by Blargg test ROMs printing to serial port $FF01-$FF02)
    const std::string& serial_output() const noexcept { return serial_out_; }
    void clear_serial_output() noexcept { serial_out_.clear(); }

    bool is_cgb() const noexcept { return is_cgb_; }
    void set_cgb(bool cgb) noexcept {
        is_cgb_ = cgb;
        ppu_.set_cgb(cgb);
    }

    uint8_t read_vram(uint8_t bank, uint16_t addr) const noexcept {
        if (flat_ram_mode_) {
            return flat_ram_[addr];
        }
        uint16_t offset = (addr - 0x8000U) & 0x1FFFU;
        return (bank == 1 && is_cgb_) ? vram_bank1_[offset] : vram_bank0_[offset];
    }

    Ppu& ppu() noexcept { return ppu_; }
    const Ppu& ppu() const noexcept { return ppu_; }

    Apu& apu() noexcept { return apu_; }
    const Apu& apu() const noexcept { return apu_; }

    void sync_ppu(uint64_t cpu_cycles) noexcept;
    void sync_apu(uint64_t cpu_cycles) noexcept;

    // CGB Speed switch ($FF4D)
    void toggle_double_speed() noexcept { key1_ = (key1_ ^ 0x80U) & ~0x01U; }
    bool is_double_speed() const noexcept { return (key1_ & 0x80U) != 0; }

    // CGB HDMA ($FF51-$FF55)
    void step_hdma() noexcept;
    void trigger_hdma(uint8_t value) noexcept;

    // MBC3 RTC
    void step_rtc(uint32_t t_cycles) noexcept;

    using FrameCallback = void (*)(CpuState* cpu, Bus* bus, void* user_data);
    void set_frame_callback(FrameCallback cb, void* user_data) noexcept {
        frame_cb_ = cb;
        frame_user_data_ = user_data;
    }
    void on_frame_complete(CpuState* cpu) noexcept;

    // Recompiled dispatch table registration and lookup
    void set_dispatch_table(const RecompFuncEntry* entries, size_t count) noexcept {
        dispatch_entries_ = entries;
        dispatch_count_ = count;
    }
    RecompFunc lookup_function(uint16_t addr, uint8_t bank) const noexcept;
    void log_fallback(uint16_t addr, uint8_t bank, uint16_t from_pc);

private:
    bool flat_ram_mode_ = false;
    bool is_cgb_ = false;

    const RecompFuncEntry* dispatch_entries_ = nullptr;
    size_t dispatch_count_ = 0;
    std::set<std::pair<uint16_t, uint8_t>> logged_fallbacks_;

    Ppu ppu_;
    uint64_t last_ppu_sync_cycles_ = 0;
    Apu apu_;
    uint64_t last_apu_sync_cycles_ = 0;
    FrameCallback frame_cb_ = nullptr;
    void* frame_user_data_ = nullptr;

    // Cartridge and memory storage
    std::vector<uint8_t> rom_;
    std::vector<uint8_t> sram_;
    std::array<uint8_t, 65536> flat_ram_{};

    // Internal Game Boy RAMs
    std::array<uint8_t, 0x2000> vram_bank0_{};
    std::array<uint8_t, 0x2000> vram_bank1_{};  // CGB only
    std::array<uint8_t, 0x1000> wram_bank0_{};  // Fixed $C000-$CFFF
    std::array<uint8_t, 0x7000> wram_banks_{};  // Banks 1..7 ($D000-$DFFF)
    std::array<uint8_t, 0x00A0> oam_{};         // $FE00-$FE9F
    std::array<uint8_t, 0x007F> hram_{};        // $FF80-$FFFE

    // MBC and banking state
    MbcType mbc_ = MbcType::RomOnly;
    bool has_battery_ = false;
    bool ram_enabled_ = false;
    uint16_t rom_bank_ = 1;
    uint8_t ram_bank_ = 0;
    uint8_t mbc1_mode_ = 0;

    // MBC3 RTC registers
    uint8_t rtc_s_ = 0;
    uint8_t rtc_m_ = 0;
    uint8_t rtc_h_ = 0;
    uint8_t rtc_dl_ = 0;
    uint8_t rtc_dh_ = 0;
    uint8_t rtc_select_ = 0;
    uint8_t rtc_latch_prev_ = 0xFF;
    uint8_t latched_rtc_s_ = 0;
    uint8_t latched_rtc_m_ = 0;
    uint8_t latched_rtc_h_ = 0;
    uint8_t latched_rtc_dl_ = 0;
    uint8_t latched_rtc_dh_ = 0;
    uint32_t rtc_cycles_ = 0;

    // CGB HDMA ($FF51-$FF55)
    uint16_t hdma_src_ = 0;
    uint16_t hdma_dst_ = 0;
    uint8_t hdma5_ = 0xFF;
    bool hdma_active_ = false;

    // CGB Bank registers
    uint8_t vram_bank_idx_ = 0;
    uint8_t wram_bank_idx_ = 1;

    // Hardware Timers
    uint16_t div_counter_ = 0;
    uint8_t tima_ = 0;
    uint8_t tma_ = 0;
    uint8_t tac_ = 0;

    // Interrupt registers
    uint8_t if_reg_ = 0xE1U;  // Upper 3 bits unused, always 1
    uint8_t ie_reg_ = 0x00U;

    // Joypad
    uint8_t joyp_ = 0xCFU;
    uint8_t joypad_state_ = 0x00U;

    // Serial
    uint8_t sb_ = 0;
    uint8_t sc_ = 0;
    std::string serial_out_;

    // Speed switch ($FF4D KEY1)
    uint8_t key1_ = 0;

    void update_joyp() noexcept;
};

}  // namespace dragon::gb
