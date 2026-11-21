#pragma once

// Sega Genesis / Mega Drive 24-bit Main Bus connecting M68000, Work RAM, Cartridge,
// VDP Mode 5, Z80 Sound CPU, YM2612, SN76489 PSG, and 3/6-button Controllers.
// Reference: docs/GENESIS_SPEC.md §1.

#include <array>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include "md/cartridge.h"
#include "md/cpu_state.h"
#include "md/vdp.h"
#include "md/ym2612.h"
#include "md/z80.h"
#include "sms/psg.h"

namespace dragon::md {

// 6-button Genesis controller bitmask (active high in host API)
enum MdButton : uint16_t {
    kBtnUp = 1U << 0,
    kBtnDown = 1U << 1,
    kBtnLeft = 1U << 2,
    kBtnRight = 1U << 3,
    kBtnB = 1U << 4,
    kBtnC = 1U << 5,
    kBtnA = 1U << 6,
    kBtnStart = 1U << 7,
    kBtnZ = 1U << 8,
    kBtnY = 1U << 9,
    kBtnX = 1U << 10,
    kBtnMode = 1U << 11,
};

struct FallbackEntry {
    uint32_t target = 0;
    uint32_t from_pc = 0;
};

class Bus {
public:
    using FrameCallback = void (*)(MdCpuState* cpu, Bus* bus, void* user_data);

    Bus() noexcept;

    bool load_rom(const std::filesystem::path& path, std::string* err = nullptr);
    bool load_rom_bytes(const std::vector<uint8_t>& raw, std::string* err = nullptr);
    void reset(MdCpuState& cpu) noexcept;

    // M68000 24-bit Big-Endian memory access
    uint8_t read8(uint32_t addr) noexcept;
    uint16_t read16(uint32_t addr) noexcept;
    uint32_t read32(uint32_t addr) noexcept;
    void write8(uint32_t addr, uint8_t value) noexcept;
    void write16(uint32_t addr, uint16_t value) noexcept;
    void write32(uint32_t addr, uint32_t value) noexcept;

    // Hardware synchronization (advances VDP, Z80, YM2612, PSG, controller timeout)
    void sync_hardware(uint64_t m68k_cycles) noexcept;

    // Input (port 0 = P1, port 1 = P2)
    void set_controller_state(int port, uint16_t buttons) noexcept;
    void set_six_button_mode(bool enabled) noexcept { six_button_ = enabled; }

    // Audio mixer: drains 48 kHz stereo PCM (YM2612 + SN76489 PSG mixed)
    size_t drain_audio_samples(int16_t* dst, size_t max_frames) noexcept;

    // Static recompilation function registry & fallback tracking
    void register_recomp_functions(const MdRecompFuncEntry* entries, size_t count);
    MdRecompFunc lookup_function(uint32_t addr) const noexcept;
    void log_fallback(uint32_t target, uint32_t from_pc) noexcept;
    const std::vector<FallbackEntry>& fallbacks() const noexcept { return fallbacks_; }

    // Frame completion callback
    void set_frame_callback(FrameCallback cb, void* user_data) noexcept {
        frame_cb_ = cb;
        frame_cb_user_ = user_data;
    }
    void on_frame_complete(MdCpuState* cpu) noexcept;

    // Save States (`DRST` container) & SRAM `.sav`
    bool save_state(const MdCpuState& cpu, const std::filesystem::path& path) const;
    bool load_state(MdCpuState& cpu, const std::filesystem::path& path);
    bool load_sram(const std::filesystem::path& path) { return cart_.load_sram(path); }
    bool save_sram(const std::filesystem::path& path) const { return cart_.save_sram(path); }

    // Subsystem accessors
    Cartridge& cartridge() noexcept { return cart_; }
    const Cartridge& cartridge() const noexcept { return cart_; }
    Vdp& vdp() noexcept { return vdp_; }
    const Vdp& vdp() const noexcept { return vdp_; }
    Ym2612& ym2612() noexcept { return ym2612_; }
    const Ym2612& ym2612() const noexcept { return ym2612_; }
    dragon::sms::Psg& psg() noexcept { return psg_; }
    MdZ80& z80() noexcept { return z80_; }
    const std::array<uint8_t, 65536>& wram() const noexcept { return wram_; }
    std::array<uint8_t, 65536>& wram_mut() noexcept { return wram_; }

    // Flat RAM mode for isolated M68000 CPU unit tests
    void set_flat_ram_mode(bool enabled) noexcept { flat_ram_mode_ = enabled; }
    bool is_flat_ram_mode() const noexcept { return flat_ram_mode_; }

private:
    uint8_t read_io(uint32_t reg) noexcept;
    void write_io(uint32_t reg, uint8_t value) noexcept;

    Cartridge cart_;
    Vdp vdp_;
    Ym2612 ym2612_;
    dragon::sms::Psg psg_;
    MdZ80 z80_;
    std::array<uint8_t, 65536> wram_{};

    // Controller state (2 ports)
    struct ControllerPort {
        uint16_t buttons = 0;
        uint8_t data = 0x7FU;
        uint8_t ctrl = 0x00U;
        uint8_t th_step = 0;
        uint32_t th_timeout = 0;
    };
    ControllerPort ctrl_[2]{};
    bool six_button_ = true;

    uint64_t last_sync_cycles_ = 0;
    uint32_t z80_cycle_rem_ = 0;
    bool flat_ram_mode_ = false;

    std::unordered_map<uint32_t, MdRecompFunc> recomp_table_;
    std::vector<FallbackEntry> fallbacks_;

    FrameCallback frame_cb_ = nullptr;
    void* frame_cb_user_ = nullptr;
};

}  // namespace dragon::md
