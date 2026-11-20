#pragma once

// Hudson Soft HuC6270 VDC (Video Display Controller) & HuC6260 VCE (Video Color Encoder).
// Reference: docs/PCE_SPEC.md §3, Charles MacDonald pcetech.txt.

#include <array>
#include <cstdint>

namespace dragon::pce {

struct VdcState {
    uint8_t ar = 0;                 // Selected VDC register ($00-$1F)
    uint8_t status = 0;             // Status flags (CR/OR/RR/DS/DV/VD/BSY)
    std::array<uint16_t, 32> regs{};
    uint16_t read_buffer = 0;
    uint16_t write_latch = 0;
    uint16_t byr_latch = 0;         // Active scanline Y scroll counter
    uint8_t satb_dma_pending = 0;
    uint8_t satb_auto_dma = 0;
    uint16_t vce_cta = 0;           // 9-bit VCE color table address (0..511)
    uint8_t vce_ctrl = 0;
    uint16_t scanline = 0;
    uint32_t line_cycles = 0;
    uint8_t irq_line = 0;
};

class Vdc {
public:
    static constexpr int kMaxWidth = 512;
    static constexpr int kMaxHeight = 256;
    static constexpr uint16_t kTotalScanlines = 262U;
    static constexpr uint32_t kCyclesPerScanline = 455U;  // ~7.15909 MHz / 15.734 kHz

    // Status bits ($0000 read)
    static constexpr uint8_t kStatCollision = 0x01U;  // Bit 0: Sprite 0 collision
    static constexpr uint8_t kStatOverflow  = 0x02U;  // Bit 1: >16 sprite cells on scanline
    static constexpr uint8_t kStatRcr       = 0x04U;  // Bit 2: Raster compare match
    static constexpr uint8_t kStatSatbDma   = 0x08U;  // Bit 3: VRAM->SATB DMA complete
    static constexpr uint8_t kStatVramDma   = 0x10U;  // Bit 4: VRAM->VRAM DMA complete
    static constexpr uint8_t kStatVblank    = 0x20U;  // Bit 5: Vertical blank

    Vdc();
    void reset() noexcept;

    // HuC6270 VDC I/O ($0000-$03FF, port = addr & 3)
    uint8_t read_vdc(uint8_t port) noexcept;
    void write_vdc(uint8_t port, uint8_t val) noexcept;

    // HuC6260 VCE I/O ($0400-$07FF, port = addr & 7)
    uint8_t read_vce(uint8_t port) noexcept;
    void write_vce(uint8_t port, uint8_t val) noexcept;

    void step(uint32_t cpu_cycles) noexcept;
    uint32_t cycles_until_next_scanline() const noexcept {
        return (st_.line_cycles < kCyclesPerScanline) ? (kCyclesPerScanline - st_.line_cycles) : 1U;
    }

    bool irq_pending() const noexcept { return st_.irq_line != 0U; }
    bool consume_frame_ready() noexcept {
        const bool r = frame_ready_;
        frame_ready_ = false;
        return r;
    }

    int visible_width() const noexcept { return vis_width_; }
    int visible_height() const noexcept { return vis_height_; }
    const std::array<uint32_t, kMaxWidth * kMaxHeight>& framebuffer() const noexcept { return fb_; }

    // Serialization helpers
    VdcState state() const noexcept { return st_; }
    void set_state(const VdcState& s) noexcept;
    const std::array<uint16_t, 32768>& vram() const noexcept { return vram_; }
    std::array<uint16_t, 32768>& vram_mut() noexcept { return vram_; }
    const std::array<uint16_t, 256>& satb() const noexcept { return satb_; }
    std::array<uint16_t, 256>& satb_mut() noexcept { return satb_; }
    const std::array<uint16_t, 512>& palette() const noexcept { return palette_; }
    std::array<uint16_t, 512>& palette_mut() noexcept { return palette_; }

    static uint32_t vce_to_rgba32(uint16_t grb9) noexcept;

private:
    uint16_t vram_inc_step() const noexcept;
    void execute_vram_dma() noexcept;
    void execute_satb_dma() noexcept;
    void enter_scanline(int line) noexcept;
    void render_scanline(int line) noexcept;
    void update_visible_dimensions() noexcept;

    VdcState st_{};
    std::array<uint16_t, 32768> vram_{};     // 64 KiB VRAM (32K x 16-bit words)
    std::array<uint16_t, 256> satb_{};       // 64 sprites x 4 words
    std::array<uint16_t, 512> palette_{};    // 512 x 9-bit GRB VCE palette
    std::array<uint32_t, 512> palette_rgba_{};
    std::array<uint32_t, kMaxWidth * kMaxHeight> fb_{};

    int vis_width_ = 256;
    int vis_height_ = 224;
    bool frame_ready_ = false;
};

}  // namespace dragon::pce
