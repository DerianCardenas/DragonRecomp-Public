#pragma once

// SNES system Bus:
//   - 128 KiB WRAM ($7E:0000-$7F:FFFF, low 8 KiB mirrored at $00-$3F/$80-$BF:0000-1FFF)
//   - WRAM Data Port ($2180) & 17-bit Address Registers ($2181-$2183)
//   - LoROM / HiROM / ExHiROM cartridge ROM mapping + SRAM mapping
//   - Hardware 8x8->16 Multiply ($4202-$4203 -> $4216-$4217)
//   - Hardware 16/8->16 Divide ($4204-$4206 -> $4214-$4215 quotient, $4216-$4217 remainder)
//   - 8-channel General-Purpose DMA ($43x0-$43xA, triggered via $420B, +8 master cycles/byte)
//   - Minimal S-PPU / APU / Controller / Interrupt register stubs
// Hot-path rules (AGENTS.md): no exceptions, no heap allocation, no virtual dispatch.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "snes/apu.h"
#include "snes/cartridge.h"
#include "snes/cpu_state.h"
#include "snes/ppu.h"

namespace dragon::snes {

struct DmaChannel {
    uint8_t dmap = 0xFFU;    // $43x0: direction (bit7), step (bits 3-4), mode (bits 0-2)
    uint8_t bbad = 0xFFU;    // $43x1: B-bus register ($2100 + bbad)
    uint16_t a1t = 0xFFFFU;  // $43x2-$43x3: A-bus 16-bit offset
    uint8_t a1b = 0xFFU;     // $43x4: A-bus bank
    uint16_t das = 0xFFFFU;  // $43x5-$43x6: transfer size (0 = 65536 bytes)
    uint8_t dasb = 0xFFU;    // $43x7: HDMA indirect bank
    uint16_t a2a = 0xFFFFU;  // $43x8-$43x9: HDMA table current address
    uint8_t ntrl = 0xFFU;    // $43xA: HDMA line counter
    uint8_t unused = 0xFFU;  // $43xB / $43xF
};

class Bus {
public:
    static constexpr size_t kWramSize = 0x20000U;        // 128 KiB
    static constexpr size_t kDefaultSramSize = 0x8000U;  // 32 KiB

    Bus() noexcept;

    void reset() noexcept;

    void load_cartridge(const Cartridge& cart) noexcept;
    void load_rom(std::span<const uint8_t> rom, MapMode mode = MapMode::LoROM) noexcept;

    void set_map_mode(MapMode mode) noexcept { map_mode_ = mode; }
    MapMode map_mode() const noexcept { return map_mode_; }

    void attach_cpu(CpuState* cpu) noexcept;
    CpuState* cpu() const noexcept { return cpu_; }

    uint8_t read(uint32_t addr24) noexcept;
    void write(uint32_t addr24, uint8_t value) noexcept;

    uint8_t read(uint8_t bank, uint16_t offset) noexcept {
        return read((static_cast<uint32_t>(bank) << 16U) | offset);
    }
    void write(uint8_t bank, uint16_t offset, uint8_t value) noexcept {
        write((static_cast<uint32_t>(bank) << 16U) | offset, value);
    }

    uint16_t read16(uint32_t addr24) noexcept {
        const uint8_t lo = read(addr24 & 0xFFFFFFU);
        const uint8_t hi = read((addr24 + 1U) & 0xFFFFFFU);
        return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
    }
    void write16(uint32_t addr24, uint16_t value) noexcept {
        write(addr24 & 0xFFFFFFU, static_cast<uint8_t>(value & 0xFFU));
        write((addr24 + 1U) & 0xFFFFFFU, static_cast<uint8_t>(value >> 8U));
    }

    // Side-effect-free read for disassembler, fallback inspection, and tests.
    uint8_t peek(uint32_t addr24) const noexcept;

    // Write directly into ROM or WRAM/SRAM for unit test setup.
    void write_direct(uint32_t addr24, uint8_t value) noexcept;
    void load_direct(uint32_t start_addr24, std::span<const uint8_t> data) noexcept;

    uint8_t* wram_data() noexcept { return wram_.data(); }
    const uint8_t* wram_data() const noexcept { return wram_.data(); }
    uint32_t wram_port_address() const noexcept { return wram_addr_; }

    uint8_t* sram_data() noexcept { return sram_.data(); }
    const uint8_t* sram_data() const noexcept { return sram_.data(); }
    size_t sram_size() const noexcept { return sram_.size(); }
    bool has_battery() const noexcept { return has_battery_; }
    bool load_battery_ram(const std::filesystem::path& path) noexcept;
    bool save_battery_ram(const std::filesystem::path& path) const noexcept;

    bool save_state(const CpuState& cpu, const std::filesystem::path& path) const noexcept;
    bool load_state(CpuState& cpu, const std::filesystem::path& path) noexcept;

    const uint8_t* rom_data() const noexcept { return rom_.data(); }
    size_t rom_size() const noexcept { return rom_.size(); }

    const DmaChannel& dma_channel(size_t ch) const noexcept { return dma_[ch & 7U]; }
    uint64_t total_dma_master_cycles() const noexcept { return total_dma_master_cycles_; }

    bool fastrom_enabled() const noexcept { return fastrom_enabled_; }
    uint8_t nmitimen() const noexcept { return nmitimen_; }
    uint8_t hdmaen() const noexcept { return hdmaen_; }

    Ppu& ppu() noexcept { return ppu_; }
    const Ppu& ppu() const noexcept { return ppu_; }

    Apu& apu() noexcept { return apu_; }
    const Apu& apu() const noexcept { return apu_; }

    void set_controller_state(uint16_t joy1, uint16_t joy2 = 0U) noexcept {
        joy1_state_ = joy1;
        joy2_state_ = joy2;
        if (joy_strobe_) {
            joy1_shift_ = joy1_state_;
            joy2_shift_ = joy2_state_;
        }
    }
    uint16_t controller_state(size_t port = 0U) const noexcept {
        return (port == 0U) ? joy1_state_ : joy2_state_;
    }

    void init_hdma_frame() noexcept;
    void step_hdma_scanline(uint16_t scanline) noexcept;

    void on_scanline(uint16_t scanline) noexcept;
    void on_frame_start() noexcept;

    bool nmi_requested() const noexcept { return nmi_requested_; }
    void acknowledge_nmi() noexcept { nmi_requested_ = false; }
    bool irq_requested() const noexcept { return irq_requested_; }
    void acknowledge_irq() noexcept { irq_requested_ = false; }

    bool in_nmi() const noexcept { return in_nmi_; }
    void set_in_nmi(bool v) noexcept { in_nmi_ = v; }
    bool in_irq() const noexcept { return in_irq_; }
    void set_in_irq(bool v) noexcept { in_irq_ = v; }
    bool in_context_switch() const noexcept { return in_context_switch_; }
    void set_in_context_switch(bool v) noexcept { in_context_switch_ = v; }

    uint64_t next_scanline_cycle() const noexcept { return next_scanline_cycle_; }
    uint16_t current_scanline() const noexcept { return current_scanline_; }
    uint64_t current_frame() const noexcept { return current_frame_; }

    using FrameCallback = void (*)(void* user_data, uint64_t frame_index);
    void set_frame_callback(FrameCallback cb, void* user_data = nullptr) noexcept {
        frame_callback_ = cb;
        frame_user_data_ = user_data;
    }
    void set_max_frames(uint64_t max_frames) noexcept { max_frames_ = max_frames; }
    uint64_t max_frames() const noexcept { return max_frames_; }

    void sync_scanlines(CpuState* cpu) noexcept;

    // Recompiler dispatch table registration and lookup.
    void set_dispatch_table(const RecompFuncEntry* entries, size_t count) noexcept {
        dispatch_entries_ = entries;
        dispatch_count_ = count;
    }

    RecompFunc lookup_function(uint8_t bank, uint16_t addr, uint8_t mode_flags) const noexcept;

    // Stack-trick / jump-engine dispatch targets (config-driven, replaces a
    // former hardcoded SMB-SNES address). A target < 0x10000 matches by 16-bit
    // address in any bank; a 24-bit target matches (bank, addr) exactly.
    void set_stack_trick_targets(const uint32_t* targets, size_t count) noexcept {
        stack_trick_targets_ = targets;
        stack_trick_count_ = count;
    }

    bool is_stack_trick_target(uint8_t bank, uint16_t addr) const noexcept;

    void set_log_fallbacks(bool enabled) noexcept { log_fallbacks_ = enabled; }
    bool should_log_fallback(uint16_t addr) noexcept;
    uint32_t fallback_count() const noexcept { return fallback_count_; }
    void reset_fallbacks() noexcept;

private:
    uint8_t read_io(uint16_t offset) noexcept;
    void write_io(uint16_t offset, uint8_t value) noexcept;
    uint8_t read_b_bus(uint8_t reg) noexcept;
    void write_b_bus(uint8_t reg, uint8_t value) noexcept;
    void execute_dma(uint8_t channels_mask) noexcept;
    int64_t map_sram_address(uint8_t bank, uint16_t offset) const noexcept;
    RecompFunc lookup_exact_function(uint8_t bank, uint16_t addr,
                                     uint8_t mode_flags) const noexcept;

    std::array<uint8_t, kWramSize> wram_{};
    std::vector<uint8_t> sram_{};
    std::vector<uint8_t> rom_{};

    MapMode map_mode_ = MapMode::LoROM;
    bool has_battery_ = false;
    CpuState* cpu_ = nullptr;

    // WRAM port ($2180-$2183)
    uint32_t wram_addr_ = 0U;

    // Hardware multiply/divide ($4202-$4206, $4214-$4217)
    uint8_t wr_mpya_ = 0xFFU;
    uint8_t wr_mpyb_ = 0xFFU;
    uint16_t wr_diva_ = 0xFFFFU;
    uint8_t wr_divb_ = 0xFFU;
    uint16_t rd_div_ = 0U;
    uint16_t rd_mpy_ = 0U;

    // System / PPU / APU / Joypad registers
    Ppu ppu_{};
    Apu apu_{};
    uint8_t nmitimen_ = 0U;
    uint8_t hdmaen_ = 0U;
    bool fastrom_enabled_ = false;
    bool rdnmi_flag_ = false;
    bool timeup_flag_ = false;
    bool nmi_requested_ = false;
    bool irq_requested_ = false;
    bool in_nmi_ = false;
    bool in_irq_ = false;
    bool in_context_switch_ = false;
    uint64_t next_scanline_cycle_ = 1364ULL;
    uint16_t current_scanline_ = 0U;
    uint64_t current_frame_ = 0ULL;
    uint64_t max_frames_ = 0ULL;
    FrameCallback frame_callback_ = nullptr;
    void* frame_user_data_ = nullptr;
    bool auto_joypad_busy_ = false;
    uint16_t htime_ = 0x01FFU;
    uint16_t vtime_ = 0x01FFU;
    uint16_t joy1_state_ = 0U;
    uint16_t joy2_state_ = 0U;
    uint16_t joy1_data_ = 0U;
    uint16_t joy2_data_ = 0U;
    uint16_t joy1_shift_ = 0U;
    uint16_t joy2_shift_ = 0U;
    bool joy_strobe_ = false;
    std::array<bool, 8> hdma_active_{};
    std::array<bool, 8> hdma_do_transfer_{};
    std::array<bool, 8> hdma_repeat_{};
    std::array<uint8_t, 8> hdma_line_counter_{};
    uint8_t open_bus_ = 0U;

    // 8 DMA channels ($4300-$437F)
    std::array<DmaChannel, 8> dma_{};
    uint64_t total_dma_master_cycles_ = 0ULL;

    // Dispatch & fallback tracking
    const RecompFuncEntry* dispatch_entries_ = nullptr;
    size_t dispatch_count_ = 0U;
    const uint32_t* stack_trick_targets_ = nullptr;
    size_t stack_trick_count_ = 0U;
    std::array<uint8_t, 8192> fallback_bitmap_{};
    uint32_t fallback_count_ = 0U;
    bool log_fallbacks_ = true;
};

}  // namespace dragon::snes
