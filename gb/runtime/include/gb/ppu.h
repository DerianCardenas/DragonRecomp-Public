#pragma once

// Game Boy / Game Boy Color PPU (Picture Processing Unit) scanline-based emulation.
// References:
//   https://gbdev.io/pandocs/Rendering.html
//   https://gbdev.io/pandocs/Palettes.html
//   https://gbdev.io/pandocs/STAT.html
// Hot-path rules (AGENTS.md): no exceptions, no heap allocation, no virtual dispatch.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dragon::gb {

class Bus;

class Ppu {
public:
    static constexpr uint32_t kScreenWidth = 160;
    static constexpr uint32_t kScreenHeight = 144;
    static constexpr uint16_t kDotsPerScanline = 456;
    static constexpr uint16_t kScanlinesPerFrame = 154;

    Ppu() noexcept;

    void reset() noexcept;

    // Advance PPU timing by master T-cycles (4 T-cycles = 1 M-cycle).
    void advance(uint32_t t_cycles, Bus* bus) noexcept;

    // Register access
    uint8_t read_register(uint16_t addr) const noexcept;
    void write_register(uint16_t addr, uint8_t value, Bus* bus) noexcept;

    // Framebuffer access: 160x144 RGBA 32-bit (0xAABBGGRR)
    const std::array<uint32_t, kScreenWidth * kScreenHeight>& framebuffer() const noexcept {
        return framebuffer_;
    }

    bool consume_frame_complete() noexcept {
        bool val = frame_complete_;
        frame_complete_ = false;
        return val;
    }

    uint8_t ly() const noexcept { return ly_; }
    uint8_t stat() const noexcept { return stat_; }
    uint8_t lcdc() const noexcept { return lcdc_; }

    void set_cgb(bool cgb) noexcept { is_cgb_ = cgb; }
    bool is_cgb() const noexcept { return is_cgb_; }

    // Palette setting for DMG mode
    void set_dmg_palette_classic(bool green_tint) noexcept { classic_palette_ = green_tint; }

    struct Snapshot {
        bool is_cgb = false;
        bool classic_palette = false;
        bool frame_complete = false;
        uint16_t dot = 0;
        uint8_t ly = 0;
        uint8_t lyc = 0;
        uint8_t lcdc = 0x91U;
        uint8_t stat = 0x85U;
        uint8_t scy = 0;
        uint8_t scx = 0;
        uint8_t wy = 0;
        uint8_t wx = 0;
        uint8_t window_line = 0;
        uint8_t bgp = 0xFCU;
        uint8_t obp0 = 0xFFU;
        uint8_t obp1 = 0xFFU;
        uint8_t bcps = 0;
        uint8_t ocps = 0;
        std::array<uint8_t, 64> cgb_bg_pal{};
        std::array<uint8_t, 64> cgb_obj_pal{};
    };

    void save_snapshot(Snapshot& snap) const noexcept;
    void load_snapshot(const Snapshot& snap) noexcept;

private:
    bool is_cgb_ = false;
    bool classic_palette_ = false;
    bool frame_complete_ = false;
    bool stat_irq_line_ = false;

    uint16_t dot_ = 0;
    uint8_t ly_ = 0;
    uint8_t lyc_ = 0;
    uint8_t lcdc_ = 0x91U;
    uint8_t stat_ = 0x85U;
    uint8_t scy_ = 0;
    uint8_t scx_ = 0;
    uint8_t wy_ = 0;
    uint8_t wx_ = 0;
    uint8_t window_line_ = 0;

    // DMG Palettes
    uint8_t bgp_ = 0xFCU;
    uint8_t obp0_ = 0xFFU;
    uint8_t obp1_ = 0xFFU;

    // CGB Palettes: 64 bytes each (8 palettes * 4 colors * 2 bytes BGR555)
    uint8_t bcps_ = 0;
    uint8_t ocps_ = 0;
    std::array<uint8_t, 64> cgb_bg_pal_{};
    std::array<uint8_t, 64> cgb_obj_pal_{};

    // Framebuffer storage (160x144 pixels RGBA8888)
    std::array<uint32_t, kScreenWidth * kScreenHeight> framebuffer_{};

    // Scanline depth/priority buffer to determine sprite vs BG occlusion
    std::array<uint8_t, kScreenWidth> bg_color_ids_{};
    std::array<uint8_t, kScreenWidth> bg_priority_flags_{};

    void render_scanline(Bus* bus) noexcept;
    void render_background_and_window(Bus* bus) noexcept;
    void render_sprites(Bus* bus) noexcept;

    uint32_t color_to_rgba_dmg(uint8_t color_id, uint8_t palette_reg) const noexcept;
    uint32_t color_to_rgba_cgb(uint8_t palette_idx, uint8_t color_id, bool is_obj) const noexcept;
    void update_stat_interrupts(Bus* bus) noexcept;
};

}  // namespace dragon::gb
