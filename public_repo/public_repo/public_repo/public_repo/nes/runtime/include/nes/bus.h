#pragma once

// NES system Bus: 2 KiB internal RAM, dot-accurate 2C02 PPU ($2000-$3FFF + $4014 OAM DMA),
// standard controllers ($4016/$4017), 2A03 APU ($4000-$4015, $4017), PRG RAM ($6000-$7FFF),
// Mappers 0 (NROM), 1 (MMC1), 2 (UxROM), 3 (CNROM), 4 (MMC3), and flat 64 KiB test mode.
// Hot-path rules (AGENTS.md): no exceptions, no heap allocation, no virtual dispatch.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "nes/apu.h"
#include "nes/cartridge.h"
#include "nes/cpu_state.h"
#include "nes/ppu.h"

namespace dragon::nes {

enum JoypadButton : uint8_t {
    kButtonA = 1U << 0U,
    kButtonB = 1U << 1U,
    kButtonSelect = 1U << 2U,
    kButtonStart = 1U << 3U,
    kButtonUp = 1U << 4U,
    kButtonDown = 1U << 5U,
    kButtonLeft = 1U << 6U,
    kButtonRight = 1U << 7U,
};

class Bus {
public:
    using FrameCallback = void (*)(CpuState* cpu, Bus* bus, void* user_data);

    static constexpr uint8_t kAnyBank = 0xFFU;

    Bus() noexcept;

    // Load a parsed iNES cartridge (Mappers 0, 1, 2, 3, 4).
    void load_cartridge(const Cartridge& cart) noexcept;

    // Load raw PRG ROM bytes directly (defaults to Mapper 0 unless `set_mapper` is called).
    void load_prg_rom(std::span<const uint8_t> prg) noexcept;

    void set_mapper(uint16_t mapper) noexcept;
    uint16_t mapper() const noexcept { return mapper_; }

    // Return the active PRG bank index mapped at CPU address `addr` ($8000-$FFFF).
    // Returns 16 KiB bank index for Mappers 1/2, 8 KiB bank index for Mapper 4, 0 for Mappers 0/3.
    uint8_t current_prg_bank(uint16_t addr) const noexcept;

    // Enable 64 KiB flat RAM mode for unit/differential tests.
    void enable_flat_ram(bool enabled = true) noexcept { flat_ram_mode_ = enabled; }
    bool is_flat_ram() const noexcept { return flat_ram_mode_; }

    uint8_t read(uint16_t addr) noexcept;
    void write(uint16_t addr, uint8_t value) noexcept;

    // Side-effect-free memory read used by OAM DMA, DMC DMA, and test inspection.
    uint8_t peek(uint16_t addr) const noexcept;

    uint8_t* ram_data() noexcept { return flat_ram_mode_ ? flat_ram_.data() : ram_.data(); }
    const uint8_t* prg_rom_data() const noexcept { return prg_rom_; }
    uint32_t prg_rom_size() const noexcept { return prg_rom_size_; }

    std::span<uint8_t> prg_ram() noexcept { return prg_ram_; }
    std::span<const uint8_t> prg_ram() const noexcept { return prg_ram_; }
    bool has_battery() const noexcept { return has_battery_; }
    bool load_battery_ram(const std::filesystem::path& path) noexcept;
    bool save_battery_ram(const std::filesystem::path& path) const noexcept;

    bool save_state(const CpuState& cpu, const std::filesystem::path& path) const noexcept;
    bool load_state(CpuState& cpu, const std::filesystem::path& path) noexcept;

    Ppu& ppu() noexcept { return ppu_; }
    const Ppu& ppu() const noexcept { return ppu_; }

    Apu& apu() noexcept { return apu_; }
    const Apu& apu() const noexcept { return apu_; }

    bool mapper_irq_pending() const noexcept { return mmc3_irq_pending_ || mapper40_irq_pending_; }
    bool irq_pending() const noexcept {
        return apu_.irq_pending() || mmc3_irq_pending_ || mapper40_irq_pending_;
    }

    // Controller state (port 0 or 1, bitmask of JoypadButton).
    void set_controller(uint8_t port, uint8_t buttons) noexcept;
    uint8_t controller(uint8_t port) const noexcept;

    // CPU attachment and PPU/APU catch-up synchronization.
    void attach_cpu(CpuState* cpu) noexcept;
    void sync_ppu(uint64_t cpu_cycles) noexcept;
    void update_next_event(CpuState* cpu) noexcept;

    // Frame callback and frame limit for headless / SDL3 runners.
    void set_frame_callback(FrameCallback cb, void* user_data = nullptr) noexcept {
        frame_callback_ = cb;
        frame_user_data_ = user_data;
    }
    void set_max_frames(uint64_t max_frames) noexcept { max_frames_ = max_frames; }
    uint64_t max_frames() const noexcept { return max_frames_; }
    uint64_t completed_frames() const noexcept { return completed_frames_; }
    void reset_frame_counter() noexcept { completed_frames_ = 0ULL; }
    void on_frame_complete(CpuState* cpu) noexcept;

    bool in_nmi() const noexcept { return in_nmi_; }
    void set_in_nmi(bool v) noexcept { in_nmi_ = v; }
    bool in_irq() const noexcept { return in_irq_; }
    void set_in_irq(bool v) noexcept { in_irq_ = v; }

    bool is_vs_system() const noexcept { return is_vs_system_; }
    void set_vs_system(bool vs) noexcept { is_vs_system_ = vs; }
    void insert_coin(uint8_t slot = 0) noexcept {
        if (slot < 2U) {
            coin_pulses_[slot] = 6U;
        }
        // Also pulse the service line ($4016 bit 2). Several Vs. games (e.g. Vs.
        // Battle City) credit from the service button and ignore the coin
        // acceptor bits under the default coinage DIP, so routing a coin to both
        // makes "insert coin" actually add a credit everywhere. Verified: the
        // Battle City credit counter at $03B only increments from bit 2.
        service_pulse_ = 6U;
    }
    // Vs. System service button ($4016 bit 2).
    void insert_service() noexcept { service_pulse_ = 6U; }
    void set_ppu_palette(PpuPalette pal) noexcept { ppu_.set_palette(pal); }

    // Write directly into flat RAM or PRG area for test setup.
    void write_direct(uint16_t addr, uint8_t value) noexcept;
    void load_direct(uint16_t start_addr, std::span<const uint8_t> data) noexcept;

    // Recompiler dispatch table registration and lookup.
    void set_dispatch_table(const RecompFuncEntry* entries, size_t count) noexcept {
        dispatch_entries_ = entries;
        dispatch_count_ = count;
    }

    RecompFunc lookup_function(uint16_t addr, uint8_t bank = 0) const noexcept;

    void set_log_fallbacks(bool enabled) noexcept { log_fallbacks_ = enabled; }
    bool should_log_fallback(uint16_t addr) noexcept;
    uint32_t fallback_count() const noexcept { return fallback_count_; }
    void reset_fallbacks() noexcept;

private:
    static uint8_t apu_memory_read(void* user_data, uint16_t addr) noexcept;
    static void on_ppu_scanline_a12(void* user_data) noexcept;

    void reset_mapper_state() noexcept;
    void update_mapper_banks() noexcept;
    void apply_mmc3_chr_banks() noexcept;
    void write_mapper(uint16_t addr, uint8_t value) noexcept;
    void clock_mmc3_scanline_irq() noexcept;
    void set_prg_bank_8k(uint8_t slot_8k, uint32_t bank_8k) noexcept;
    void set_prg_bank_16k(uint8_t slot_16k, uint32_t bank_16k) noexcept;
    void set_prg_bank_32k(uint32_t bank_32k) noexcept;
    RecompFunc lookup_exact_function(uint16_t addr, uint8_t bank) const noexcept;

    std::array<uint8_t, 0x0800> ram_{};
    std::array<uint8_t, 0x20> apu_io_regs_{};
    std::array<uint8_t, 0x2000> prg_ram_{};
    std::array<uint8_t, 0x8000> prg_rom_small_{};
    std::vector<uint8_t> prg_rom_large_{};
    std::array<uint8_t, 0x10000> flat_ram_{};

    Ppu ppu_{};
    Apu apu_{};
    CpuState* cpu_ = nullptr;
    uint64_t last_ppu_cpu_cycles_ = 0ULL;
    bool in_nmi_ = false;
    bool in_irq_ = false;

    std::array<uint8_t, 2> joypad_state_{0U, 0U};
    std::array<uint8_t, 2> joypad_shift_{0U, 0U};
    bool joypad_strobe_ = false;

    FrameCallback frame_callback_ = nullptr;
    void* frame_user_data_ = nullptr;
    uint64_t max_frames_ = 0ULL;
    uint64_t completed_frames_ = 0ULL;

    const uint8_t* prg_rom_ = nullptr;
    uint32_t prg_rom_size_ = 0;
    std::array<uint32_t, 4> prg_bank_offsets_{0U, 0x2000U, 0x4000U, 0x6000U};
    uint16_t mapper_ = 0U;
    Mirroring cart_mirroring_ = Mirroring::Horizontal;
    bool has_battery_ = false;
    bool flat_ram_mode_ = false;

    // Mapper 1 (MMC1) registers
    uint8_t mmc1_shift_reg_ = 0U;
    uint8_t mmc1_shift_count_ = 0U;
    uint8_t mmc1_control_ = 0x0CU;
    uint8_t mmc1_chr0_ = 0U;
    uint8_t mmc1_chr1_ = 0U;
    uint8_t mmc1_prg_ = 0U;

    // Mapper 2 (UxROM) & Mapper 3 (CNROM) registers
    uint8_t uxrom_prg_bank_ = 0U;
    uint8_t cnrom_chr_bank_ = 0U;

    // Mapper 4 (MMC3) registers
    uint8_t mmc3_bank_select_ = 0U;
    std::array<uint8_t, 8> mmc3_bank_regs_{};
    uint8_t mmc3_irq_latch_ = 0U;
    uint8_t mmc3_irq_counter_ = 0U;
    bool mmc3_irq_reload_ = false;
    bool mmc3_irq_enabled_ = false;
    bool mmc3_irq_pending_ = false;

    // Mapper 189 (TXC/MMC3 variant, e.g. Street Fighter II: The World Warrior).
    // CHR/IRQ/mirroring behave like MMC3; PRG is a single 32 KiB bank selected
    // by a register in $4100-$5FFF. https://www.nesdev.org/wiki/INES_Mapper_189
    uint8_t mapper189_prg_bank_ = 0U;

    // Mapper 40 (NTDEC 2722 / SMB2J pirate):
    // $6000-$7FFF: 8 KiB PRG ROM Bank 6 (fixed)
    // $8000-$9FFF: 8 KiB PRG ROM Bank 4 (fixed)
    // $A000-$BFFF: 8 KiB PRG ROM Bank 5 (fixed)
    // $C000-$DFFF: 8 KiB PRG ROM switchable bank ($E000-$FFFF)
    // $E000-$FFFF: 8 KiB PRG ROM Bank 7 (fixed)
    // IRQ: 13-bit M2 binary ripple counter (4096 cycles)
    // https://www.nesdev.org/wiki/INES_Mapper_040
    uint8_t mapper40_prg_bank_ = 0U;
    bool mapper40_irq_enabled_ = false;
    uint32_t mapper40_irq_counter_ = 0U;
    bool mapper40_irq_pending_ = false;

    // Nintendo Vs. System state & Mapper 99
    bool is_vs_system_ = false;
    std::array<uint8_t, 2> coin_pulses_{0U, 0U};
    uint8_t service_pulse_ = 0U;
    uint8_t vs_dip_ = 0U;
    uint32_t mapper99_chr_bank_ = 0U;

    const RecompFuncEntry* dispatch_entries_ = nullptr;
    size_t dispatch_count_ = 0;

    std::array<uint8_t, 8192> fallback_bitmap_{};
    uint32_t fallback_count_ = 0;
    bool log_fallbacks_ = true;
};

}  // namespace dragon::nes
