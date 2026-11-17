#pragma once

// SNES S-PPU (5C77 + 5C78) emulation:
//   - 64 KiB VRAM (32K 16-bit words, $2115 VMAIN increment + address translation, $2116-$2119,
//     $2139-$213A read with prefetch latch)
//   - 512 B CGRAM (256 15-bit BGR555 colors, $2121-$2122, $213B)
//   - 544 B OAM (128 sprites + 32 B high table, $2101-$2104, $2138)
//   - Scanline renderer (256x224 RGBA8888): BG Modes 0-7 (2bpp/4bpp/8bpp tiles, 8x8 & 16x16,
//     Mode 1 BG3 priority, Mode 7 affine matrix + $2134-$2136 signed 16x8 hardware multiply),
//     OBJ sprites (all 8 size modes, priorities 0-3, H/V flip), Window clipping, Main/Sub screen
//     compositing, Color Math (add/subtract/half/fixed color), and Master Brightness (0..15).
// References:
//   https://snes.nesdev.org/wiki/PPU_registers
//   https://snes.nesdev.org/wiki/Backgrounds
//   https://snes.nesdev.org/wiki/Sprites
//   https://snes.nesdev.org/wiki/Color_math
// Hot-path rules (AGENTS.md): no exceptions, no heap allocation, no virtual dispatch.

#include <array>
#include <cstddef>
#include <cstdint>

namespace dragon::snes {

class Ppu {
public:
    static constexpr uint32_t kScreenWidth = 256U;
    static constexpr uint32_t kScreenHeight = 224U;
    static constexpr size_t kVramWordCount = 0x8000U;  // 32K words = 64 KiB
    static constexpr size_t kCgramColorCount = 256U;   // 256 words = 512 B
    static constexpr size_t kOamByteCount = 544U;      // 512 B low + 32 B high

    Ppu() noexcept;

    void reset() noexcept;

    // B-bus register access ($2100-$213F -> reg 0x00..0x3F).
    uint8_t read_register(uint8_t reg) noexcept;
    void write_register(uint8_t reg, uint8_t value) noexcept;

    // Render one visible scanline (y in 0..223) into the RGBA8888 framebuffer.
    void render_scanline(uint16_t y) noexcept;

    // Called at VBlank start (scanline 225) to reload OAM address when not in forced blank.
    void on_vblank_start() noexcept;

    // Update H/V counter reference for $2137 SLHV latch.
    void latch_hv_counter(uint16_t h_dot, uint16_t v_line) noexcept;

    // Direct memory accessors for testing & inspection.
    const std::array<uint16_t, kVramWordCount>& vram() const noexcept { return vram_; }
    std::array<uint16_t, kVramWordCount>& vram() noexcept { return vram_; }
    uint16_t read_vram_word(uint16_t word_addr) const noexcept {
        return vram_[word_addr & 0x7FFFU];
    }
    void write_vram_word(uint16_t word_addr, uint16_t value) noexcept {
        vram_[word_addr & 0x7FFFU] = value;
    }

    const std::array<uint16_t, kCgramColorCount>& cgram() const noexcept { return cgram_; }
    std::array<uint16_t, kCgramColorCount>& cgram() noexcept { return cgram_; }
    uint16_t read_cgram_color(uint8_t index) const noexcept { return cgram_[index]; }
    void write_cgram_color(uint8_t index, uint16_t bgr555) noexcept {
        cgram_[index] = static_cast<uint16_t>(bgr555 & 0x7FFFU);
    }

    const std::array<uint8_t, kOamByteCount>& oam() const noexcept { return oam_; }
    std::array<uint8_t, kOamByteCount>& oam() noexcept { return oam_; }

    const std::array<uint32_t, kScreenWidth * kScreenHeight>& framebuffer() const noexcept {
        return framebuffer_;
    }

    uint8_t inidisp() const noexcept { return inidisp_; }
    uint8_t bgmode() const noexcept { return bgmode_; }
    uint8_t tm() const noexcept { return tm_; }
    uint8_t ts() const noexcept { return ts_; }
    int32_t mpy_result() const noexcept { return mpy_result_; }

    uint8_t bg_sc(size_t i) const noexcept { return bg_sc_[i & 3U]; }
    uint8_t bg12nba() const noexcept { return bg12nba_; }
    uint8_t bg34nba() const noexcept { return bg34nba_; }
    uint16_t bg_hofs(size_t i) const noexcept { return bg_hofs_[i & 3U]; }
    uint16_t bg_vofs(size_t i) const noexcept { return bg_vofs_[i & 3U]; }

    static uint32_t bgr555_to_rgba8888(uint16_t bgr555, uint8_t brightness = 15U) noexcept;

    struct Snapshot {
        std::array<uint16_t, kVramWordCount> vram{};
        std::array<uint16_t, kCgramColorCount> cgram{};
        std::array<uint8_t, kOamByteCount> oam{};
        uint8_t vmain = 0U;
        uint16_t vram_addr = 0U;
        uint16_t vram_prefetch = 0U;
        uint16_t oam_addr = 0U;
        bool oam_priority = false;
        uint8_t cgram_addr = 0U;
        bool cgram_second_write = false;
        bool cgram_read_second = false;
        uint8_t cgram_latch = 0U;
        uint8_t inidisp = 0x80U;
        uint8_t obsel = 0U;
        uint8_t bgmode = 0U;
        uint8_t mosaic = 0U;
        std::array<uint8_t, 4> bg_sc{};
        uint8_t bg12nba = 0U;
        uint8_t bg34nba = 0U;
        std::array<uint16_t, 4> bg_hofs{};
        std::array<uint16_t, 4> bg_vofs{};
        uint8_t bg_ofs_latch = 0U;
        uint8_t bg_hofs_latch = 0U;
        uint8_t m7sel = 0U;
        uint8_t m7_latch = 0U;
        int16_t m7a = 0;
        int16_t m7b = 0;
        int16_t m7c = 0;
        int16_t m7d = 0;
        int16_t m7x = 0;
        int16_t m7y = 0;
        int16_t m7_hofs = 0;
        int16_t m7_vofs = 0;
        int32_t mpy_result = 0;
        uint8_t w12sel = 0U;
        uint8_t w34sel = 0U;
        uint8_t wobjsel = 0U;
        uint8_t wh0 = 0U;
        uint8_t wh1 = 0U;
        uint8_t wh2 = 0U;
        uint8_t wh3 = 0U;
        uint8_t wbglog = 0U;
        uint8_t wobjlog = 0U;
        uint8_t tm = 0U;
        uint8_t ts = 0U;
        uint8_t tmw = 0U;
        uint8_t tsw = 0U;
        uint8_t cgwsel = 0U;
        uint8_t cgadsub = 0U;
        uint8_t setini = 0U;
        uint8_t fixed_r = 0U;
        uint8_t fixed_g = 0U;
        uint8_t fixed_b = 0U;
        uint16_t latched_h = 0U;
        uint16_t latched_v = 0U;
        bool ophct_second = false;
        bool opvct_second = false;
        uint8_t stat77 = 0x01U;
        uint8_t stat78 = 0x01U;
    };

    void save_snapshot(Snapshot& snap) const noexcept;
    void load_snapshot(const Snapshot& snap) noexcept;

private:
    struct LayerPixel {
        uint16_t color = 0U;    // 15-bit BGR555
        uint8_t priority = 0U;  // 0..1 for BG, 0..3 for OBJ
        uint8_t palette = 0U;   // 0..7 for OBJ color math check
        bool valid = false;
    };

    struct ScreenPixel {
        uint8_t r = 0U;            // 5-bit (0..31)
        uint8_t g = 0U;            // 5-bit (0..31)
        uint8_t b = 0U;            // 5-bit (0..31)
        uint8_t layer = 5U;        // 0=BG1, 1=BG2, 2=BG3, 3=BG4, 4=OBJ, 5=Backdrop
        uint8_t obj_palette = 0U;  // 0..7 when layer == 4
    };

    uint16_t translate_vram_addr(uint16_t word_addr) const noexcept;
    uint16_t vram_step() const noexcept;

    void evaluate_sprites_on_scanline(uint16_t y,
                                      std::array<LayerPixel, kScreenWidth>& out_obj) noexcept;
    void render_bg_scanline(uint8_t bg_idx, uint8_t bpp, uint16_t y,
                            std::array<LayerPixel, kScreenWidth>& out_bg) const noexcept;
    void render_mode7_scanline(uint16_t y,
                               std::array<LayerPixel, kScreenWidth>& out_bg1) const noexcept;

    bool eval_layer_window(uint8_t layer_idx, uint16_t x) const noexcept;
    bool eval_color_window(uint16_t x) const noexcept;

    std::array<uint16_t, kVramWordCount> vram_{};
    std::array<uint16_t, kCgramColorCount> cgram_{};
    std::array<uint8_t, kOamByteCount> oam_{};
    std::array<uint32_t, kScreenWidth * kScreenHeight> framebuffer_{};

    // VRAM state ($2115-$2119, $2139-$213A)
    uint8_t vmain_ = 0U;
    uint16_t vram_addr_ = 0U;
    uint16_t vram_prefetch_ = 0U;

    // CGRAM state ($2121-$2122, $213B)
    uint8_t cgram_addr_ = 0U;
    uint8_t cgram_latch_ = 0U;
    bool cgram_second_write_ = false;
    bool cgram_read_second_ = false;

    // OAM state ($2101-$2104, $2138)
    uint8_t obsel_ = 0U;
    uint16_t oam_reload_ = 0U;
    uint16_t oam_addr_ = 0U;
    uint8_t oam_latch_ = 0U;
    bool oam_priority_rotation_ = false;

    // Display & BG registers ($2100, $2105-$2114)
    uint8_t inidisp_ = 0x80U;
    uint8_t bgmode_ = 0U;
    uint8_t mosaic_ = 0U;
    std::array<uint8_t, 4> bg_sc_{};
    uint8_t bg12nba_ = 0U;
    uint8_t bg34nba_ = 0U;
    std::array<uint16_t, 4> bg_hofs_{};
    std::array<uint16_t, 4> bg_vofs_{};
    uint8_t bg_ofs_latch_ = 0U;
    uint8_t bg_hofs_latch_ = 0U;

    // Mode 7 registers & hardware signed 16x8 multiply ($211A-$2120, $2134-$2136)
    uint8_t m7sel_ = 0U;
    uint8_t m7_latch_ = 0U;
    int16_t m7a_ = 0;
    int16_t m7b_ = 0;
    int16_t m7c_ = 0;
    int16_t m7d_ = 0;
    int16_t m7x_ = 0;
    int16_t m7y_ = 0;
    int16_t m7_hofs_ = 0;
    int16_t m7_vofs_ = 0;
    int32_t mpy_result_ = 0;

    // Window registers ($2123-$212B)
    uint8_t w12sel_ = 0U;
    uint8_t w34sel_ = 0U;
    uint8_t wobjsel_ = 0U;
    uint8_t wh0_ = 0U;
    uint8_t wh1_ = 0U;
    uint8_t wh2_ = 0U;
    uint8_t wh3_ = 0U;
    uint8_t wbglog_ = 0U;
    uint8_t wobjlog_ = 0U;

    // Screen designation & color math ($212C-$2133)
    uint8_t tm_ = 0U;
    uint8_t ts_ = 0U;
    uint8_t tmw_ = 0U;
    uint8_t tsw_ = 0U;
    uint8_t cgwsel_ = 0U;
    uint8_t cgadsub_ = 0U;
    uint8_t setini_ = 0U;
    uint8_t fixed_r_ = 0U;
    uint8_t fixed_g_ = 0U;
    uint8_t fixed_b_ = 0U;

    // H/V latch ($2137, $213C-$213F)
    uint16_t latched_h_ = 0U;
    uint16_t latched_v_ = 0U;
    bool ophct_second_ = false;
    bool opvct_second_ = false;
    uint8_t stat77_ = 0x01U;
    uint8_t stat78_ = 0x01U;
};

}  // namespace dragon::snes
