#pragma once

// Dot-accurate Ricoh 2C02 PPU emulation for the NES runtime.
// References:
//   https://www.nesdev.org/wiki/PPU_registers
//   https://www.nesdev.org/wiki/PPU_rendering
//   https://www.nesdev.org/wiki/PPU_scrolling
//   https://www.nesdev.org/wiki/PPU_sprite_evaluation
//   https://www.nesdev.org/wiki/PPU_palettes
// Hot-path rules (AGENTS.md): no exceptions, no heap allocation, no virtual dispatch.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "nes/cartridge.h"

namespace dragon::nes {

enum class PpuPalette : uint8_t {
    Ntsc2C02,
    Vs0001,
    Vs0002,
    Vs0003,
    Vs0004,
    Vs2C03,
    SmoothFbx,
};

class Ppu {
public:
    using ScanlineIrqCallback = void (*)(void* user_data) noexcept;

    static constexpr uint32_t kScreenWidth = 256;
    static constexpr uint32_t kScreenHeight = 240;
    static constexpr uint16_t kDotsPerScanline = 341;
    static constexpr uint16_t kScanlinesPerFrame = 262;

    Ppu() noexcept;

    void reset() noexcept;

    // Load CHR ROM or enable 8 KiB writable CHR RAM when `chr_rom` is empty or `is_ram` is true.
    void load_chr(std::span<const uint8_t> chr_rom, bool is_ram = false) noexcept;
    void set_mirroring(Mirroring mode) noexcept { mirroring_ = mode; }
    Mirroring mirroring() const noexcept { return mirroring_; }

    // CHR bank switching (1 KiB, 2 KiB, 4 KiB, or 8 KiB granularity) for Mappers 1, 3, and 4.
    void set_chr_bank_1k(uint8_t slot_1k, uint32_t bank_1k) noexcept;
    void set_chr_bank_2k(uint8_t slot_1k_base, uint32_t bank_1k_aligned) noexcept;
    void set_chr_bank_4k(uint8_t slot_4k, uint32_t bank_4k) noexcept;
    void set_chr_bank_8k(uint32_t bank_8k) noexcept;
    uint32_t chr_bank_1k(uint8_t slot_1k) const noexcept {
        return chr_bank_offsets_[slot_1k & 0x07U] >> 10U;
    }

    // MMC3 scanline A12 notification hook (called at dot 260 of visible scanlines 0..239 when
    // rendering is enabled and BG or sprites fetch from $1000).
    void set_scanline_irq_callback(ScanlineIrqCallback cb, void* user_data,
                                   bool watch_enabled = false) noexcept {
        scanline_irq_cb_ = cb;
        scanline_irq_user_ = user_data;
        scanline_irq_watch_ = watch_enabled;
    }
    void set_scanline_irq_watch(bool enabled) noexcept { scanline_irq_watch_ = enabled; }

    // CPU-facing register access ($2000-$2007 -> reg 0..7).
    uint8_t read_register(uint16_t reg) noexcept;
    void write_register(uint16_t reg, uint8_t value) noexcept;

    // OAM DMA byte write at (oam_addr + offset) & 0xFF.
    void write_oam_dma(uint8_t offset, uint8_t value) noexcept;

    // PPU internal bus ($0000-$3FFF) read/write.
    uint8_t read_vram(uint16_t addr) const noexcept;
    void write_vram(uint16_t addr, uint8_t value) noexcept;

    // Advance PPU by 1 dot or N dots.
    void step_dot() noexcept;
    void step_dots(uint64_t dots) noexcept;

    // Number of PPU dots until the next frame/VBlank/pre-render/scanline-IRQ boundary.
    uint32_t dots_until_next_event() const noexcept;

    // Interrupt & frame completion latches.
    bool nmi_pending() const noexcept { return nmi_pending_; }
    bool consume_nmi() noexcept {
        const bool pending = nmi_pending_;
        nmi_pending_ = false;
        return pending;
    }

    bool frame_complete() const noexcept { return frame_complete_; }
    bool consume_frame_complete() noexcept {
        const bool done = frame_complete_;
        frame_complete_ = false;
        return done;
    }

    // State accessors for testing and frontend presentation.
    uint16_t v() const noexcept { return v_; }
    uint16_t t() const noexcept { return t_; }
    uint8_t fine_x() const noexcept { return x_; }
    uint8_t w() const noexcept { return w_; }
    uint8_t ctrl() const noexcept { return ctrl_; }
    uint8_t mask() const noexcept { return mask_; }
    uint8_t status() const noexcept { return status_; }
    uint8_t oam_addr() const noexcept { return oam_addr_; }
    uint8_t read_buffer() const noexcept { return read_buffer_; }
    uint8_t open_bus() const noexcept { return open_bus_; }

    uint16_t scanline() const noexcept { return scanline_; }
    uint16_t dot() const noexcept { return dot_; }
    uint64_t frame_count() const noexcept { return frame_count_; }
    uint64_t total_dots() const noexcept { return total_dots_; }

    const std::array<uint32_t, kScreenWidth * kScreenHeight>& framebuffer() const noexcept {
        return framebuffer_;
    }
    const std::array<uint8_t, 0x100>& oam() const noexcept { return oam_; }
    const std::array<uint8_t, 0x20>& palette_ram() const noexcept { return palette_ram_; }

    void set_palette(PpuPalette palette) noexcept { palette_ = palette; }
    PpuPalette palette() const noexcept { return palette_; }

    static uint32_t palette_rgba(uint8_t color_id, uint8_t emphasis_bits = 0,
                                 PpuPalette pal = PpuPalette::Ntsc2C02) noexcept;

    struct Snapshot {
        uint8_t ctrl = 0;
        uint8_t mask = 0;
        uint8_t status = 0;
        uint8_t oam_addr = 0;
        uint8_t read_buffer = 0;
        uint8_t open_bus = 0;
        uint16_t v = 0;
        uint16_t t = 0;
        uint8_t x = 0;
        uint8_t w = 0;
        uint16_t scanline = 0;
        uint16_t dot = 0;
        uint64_t frame_count = 0;
        uint64_t total_dots = 0;
        bool nmi_pending = false;
        bool frame_complete = false;
        bool suppress_vbl = false;
        bool suppress_nmi = false;
        bool sprite_zero_hit_latched = false;
        uint8_t bg_next_tile_id = 0;
        uint8_t bg_next_tile_attr = 0;
        uint8_t bg_next_tile_lsb = 0;
        uint8_t bg_next_tile_msb = 0;
        uint16_t bg_shift_pat_lo = 0;
        uint16_t bg_shift_pat_hi = 0;
        uint16_t bg_shift_attr_lo = 0;
        uint16_t bg_shift_attr_hi = 0;
        Mirroring mirroring = Mirroring::Horizontal;
        bool chr_is_ram = true;
        std::array<uint32_t, 8> chr_bank_offsets{};
        std::array<uint8_t, 0x1000> ciram{};
        std::array<uint8_t, 0x20> palette_ram{};
        std::array<uint8_t, 0x100> oam{};
        std::array<uint8_t, 0x2000> chr_ram{};
    };

    void save_snapshot(Snapshot& snap) const noexcept;
    void load_snapshot(const Snapshot& snap) noexcept;

private:
    struct ActiveSprite {
        uint8_t x = 0;
        uint8_t pat_lo = 0;
        uint8_t pat_hi = 0;
        uint8_t palette = 0;
        uint8_t priority = 0;
        bool is_sprite_zero = false;
    };

    bool rendering_enabled() const noexcept { return (mask_ & 0x18U) != 0U; }
    bool uses_pattern_table_1000() const noexcept { return (ctrl_ & 0x38U) != 0U; }
    uint16_t map_nametable_addr(uint16_t addr) const noexcept;
    static uint8_t map_palette_addr(uint16_t addr) noexcept;

    void increment_scroll_x() noexcept;
    void increment_scroll_y() noexcept;
    void transfer_address_x() noexcept;
    void transfer_address_y() noexcept;
    void load_background_shifters() noexcept;
    void update_background_shifters() noexcept;
    void run_background_fetch_step(uint16_t phase) noexcept;
    void evaluate_sprites_for_next_scanline(uint16_t eval_scanline) noexcept;
    void render_pixel() noexcept;

    std::array<uint8_t, 0x2000> chr_ram_{};
    std::vector<uint8_t> chr_rom_storage_{};
    uint8_t* chr_data_ = nullptr;
    uint32_t chr_size_ = 0x2000U;
    std::array<uint32_t, 8> chr_bank_offsets_{};

    std::array<uint8_t, 0x1000> ciram_{};
    std::array<uint8_t, 0x20> palette_ram_{};
    std::array<uint8_t, 0x100> oam_{};
    std::array<uint32_t, kScreenWidth * kScreenHeight> framebuffer_{};

    Mirroring mirroring_ = Mirroring::Horizontal;
    bool chr_is_ram_ = true;

    ScanlineIrqCallback scanline_irq_cb_ = nullptr;
    void* scanline_irq_user_ = nullptr;
    bool scanline_irq_watch_ = false;

    uint8_t ctrl_ = 0;
    uint8_t mask_ = 0;
    uint8_t status_ = 0;
    uint8_t oam_addr_ = 0;
    uint8_t read_buffer_ = 0;
    uint8_t open_bus_ = 0;

    // Loopy scrolling registers (https://www.nesdev.org/wiki/PPU_scrolling)
    uint16_t v_ = 0;
    uint16_t t_ = 0;
    uint8_t x_ = 0;
    uint8_t w_ = 0;

    uint16_t scanline_ = 0;
    uint16_t dot_ = 0;
    uint64_t frame_count_ = 0;
    uint64_t total_dots_ = 0;

    bool nmi_pending_ = false;
    bool frame_complete_ = false;
    bool suppress_vbl_ = false;
    bool suppress_nmi_ = false;
    bool sprite_zero_hit_latched_ = false;

    // Background fetch latches & 16-bit shift registers
    uint8_t bg_next_tile_id_ = 0;
    uint8_t bg_next_tile_attr_ = 0;
    uint8_t bg_next_tile_lsb_ = 0;
    uint8_t bg_next_tile_msb_ = 0;
    uint16_t bg_shift_pat_lo_ = 0;
    uint16_t bg_shift_pat_hi_ = 0;
    uint16_t bg_shift_attr_lo_ = 0;
    uint16_t bg_shift_attr_hi_ = 0;

    // Secondary OAM evaluation results for the active scanline
    std::array<ActiveSprite, 8> active_sprites_{};
    uint8_t active_sprite_count_ = 0;

    PpuPalette palette_ = PpuPalette::Ntsc2C02;
};

}  // namespace dragon::nes
