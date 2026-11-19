#pragma once

// Sega Master System & Game Gear VDP (Mode 4 — TMS9918A extended: 315-5124/5246/5378).
// Reference: https://www.smspower.org/uploads/Development/msvdp-20021112.txt, docs/SMS_SPEC.md §4.

#include <array>
#include <cstdint>

namespace dragon::sms {

struct VdpState {
    std::array<uint8_t, 16384> vram{};
    std::array<uint8_t, 64> cram{};  // 32B used on SMS, 64B used on Game Gear
    std::array<uint8_t, 16> regs{};

    uint16_t addr = 0;
    uint8_t code = 0;
    uint8_t addr_latch = 0;
    uint8_t second_byte = 0;
    uint8_t read_buffer = 0;
    uint8_t cram_latch = 0;  // Game Gear even-byte CRAM write latch

    uint8_t status = 0;           // Bit 7: Frame INT, Bit 6: Sprite Overflow, Bit 5: Collision
    uint8_t line_irq_pending = 0; // Pending line interrupt flag
    uint8_t line_counter = 0xFF;  // Internal line IRQ down-counter
    uint8_t latched_vscroll = 0;  // Reg 9 latched at start of active frame

    uint16_t scanline = 0;
    uint32_t line_cycles = 0;
    uint8_t h_counter = 0;
};

class Vdp {
public:
    static constexpr int kMaxWidth = 256;
    static constexpr int kMaxHeight = 224;
    static constexpr int kSmsHeight = 192;
    static constexpr int kGgWidth = 160;
    static constexpr int kGgHeight = 144;
    static constexpr int kGgOffsetX = 48;
    static constexpr int kGgOffsetY = 24;
    static constexpr uint32_t kCyclesPerScanline = 228U;  // NTSC 3.58 MHz / 59.92 Hz / 262
    static constexpr uint16_t kTotalScanlines = 262U;

    Vdp();

    void reset(bool game_gear_mode) noexcept;
    void set_game_gear(bool gg) noexcept { game_gear_ = gg; }
    bool is_game_gear() const noexcept { return game_gear_; }

    // Advance VDP by `cycles` Z80 T-cycles. Sets `frame_ready` when entering VBlank.
    void step(uint32_t cycles) noexcept;

    // CPU I/O ports ($BE Data, $BF Control/Status, $7E V-Counter, $7F H-Counter)
    uint8_t read_data() noexcept;
    void write_data(uint8_t value) noexcept;
    uint8_t read_status() noexcept;
    void write_control(uint8_t value) noexcept;
    uint8_t read_v_counter() const noexcept;
    uint8_t read_h_counter() const noexcept;

    // Active-high VDP IRQ output line connected to Z80 /INT
    bool irq_asserted() const noexcept;

    bool consume_frame_ready() noexcept {
        bool r = frame_ready_;
        frame_ready_ = false;
        return r;
    }

    int visible_width() const noexcept { return game_gear_ ? kGgWidth : kMaxWidth; }
    int visible_height() const noexcept;
    int viewport_x() const noexcept { return game_gear_ ? kGgOffsetX : 0; }
    int viewport_y() const noexcept { return game_gear_ ? kGgOffsetY : 0; }

    // Full 256x224 RGBA32 framebuffer (SDL_PIXELFORMAT_RGBA32: R in low byte)
    const std::array<uint32_t, kMaxWidth * kMaxHeight>& framebuffer() const noexcept {
        return fb_;
    }

    const VdpState& state() const noexcept { return st_; }
    void set_state(const VdpState& s) noexcept { st_ = s; }

    // Color lookup helper (exposed for tests)
    uint32_t cram_rgba32(uint8_t color_index) const noexcept;

private:
    bool is_224_mode() const noexcept;
    void render_scanline(int line) noexcept;
    uint16_t nametable_base() const noexcept;

    VdpState st_{};
    bool game_gear_ = false;
    bool frame_ready_ = false;
    std::array<uint32_t, kMaxWidth * kMaxHeight> fb_{};
};

}  // namespace dragon::sms
