#pragma once

// Sega 315-5313 Video Display Processor (VDP Mode 5 — Genesis / Mega Drive).
// Reference: https://plutiedev.com/, https://segaretro.org/Sega_Mega_Drive/VDP_registers,
//            docs/GENESIS_SPEC.md §3.
// Hot-path rules (AGENTS.md): no exceptions, no heap allocation, no virtual dispatch.

#include <array>
#include <cstdint>

namespace dragon::md {

class Bus;

class Vdp {
public:
    static constexpr uint32_t kMaxWidth = 320;
    static constexpr uint32_t kMaxHeight = 240;
    static constexpr uint32_t kCyclesPerScanline = 488;
    static constexpr uint32_t kScanlinesNtsc = 262;
    static constexpr uint32_t kScanlinesPal = 313;

    Vdp() noexcept;

    void reset(bool pal = false) noexcept;

    // Advance VDP by `m68k_cycles` and fire H-Int (L4) / V-Int (L6) via `bus`.
    void step(uint32_t m68k_cycles, Bus* bus) noexcept;

    // Control & Data port access ($C00000-$C0000F)
    uint16_t read_data() noexcept;
    void write_data(uint16_t value, Bus* bus) noexcept;
    uint16_t read_control() noexcept;
    void write_control(uint16_t value, Bus* bus) noexcept;
    uint16_t read_hv_counter() const noexcept;

    // Framebuffer: always stored at stride kMaxWidth (320), active region [active_width x
    // active_height] in 0xAABBGGRR (RGBA8888 little-endian)
    const std::array<uint32_t, kMaxWidth * kMaxHeight>& framebuffer() const noexcept {
        return framebuffer_;
    }
    uint32_t active_width() const noexcept { return (regs_[12] & 0x01U) ? 320U : 256U; }
    uint32_t active_height() const noexcept { return (pal_ && (regs_[1] & 0x08U)) ? 240U : 224U; }

    bool consume_frame_complete() noexcept {
        bool v = frame_complete_;
        frame_complete_ = false;
        return v;
    }

    bool vint_pending() const noexcept { return vint_pending_; }
    bool hint_pending() const noexcept { return hint_pending_; }
    void ack_vint() noexcept { vint_pending_ = false; }
    void ack_hint() noexcept { hint_pending_ = false; }
    bool vint_enabled() const noexcept { return (regs_[1] & 0x20U) != 0U; }
    bool hint_enabled() const noexcept { return (regs_[0] & 0x10U) != 0U; }
    uint32_t scanline() const noexcept { return scanline_; }
    bool is_pal() const noexcept { return pal_; }
    uint8_t reg(uint8_t idx) const noexcept { return idx < 24 ? regs_[idx] : 0; }

    // Direct memory accessors for unit tests & save states
    const std::array<uint8_t, 65536>& vram() const noexcept { return vram_; }
    std::array<uint8_t, 65536>& vram_mut() noexcept { return vram_; }
    const std::array<uint16_t, 64>& cram() const noexcept { return cram_; }
    std::array<uint16_t, 64>& cram_mut() noexcept { return cram_; }
    const std::array<uint16_t, 40>& vsram() const noexcept { return vsram_; }
    std::array<uint16_t, 40>& vsram_mut() noexcept { return vsram_; }
    const std::array<uint8_t, 24>& regs() const noexcept { return regs_; }
    std::array<uint8_t, 24>& regs_mut() noexcept { return regs_; }

    struct Snapshot {
        std::array<uint8_t, 65536> vram{};
        std::array<uint16_t, 64> cram{};
        std::array<uint16_t, 40> vsram{};
        std::array<uint8_t, 24> regs{};
        uint16_t addr_reg = 0;
        uint8_t code_reg = 0;
        uint8_t pending_ctrl = 0;
        uint8_t dma_fill_pending = 0;
        uint16_t ctrl_latch = 0;
        uint32_t scanline = 0;
        uint32_t line_cycles = 0;
        int32_t hint_counter = 0;
        uint8_t vint_pending = 0;
        uint8_t hint_pending = 0;
        uint8_t sprite_overflow = 0;
        uint8_t sprite_collision = 0;
        uint8_t pal = 0;
    };

    void save_snapshot(Snapshot& s) const noexcept;
    void load_snapshot(const Snapshot& s) noexcept;

private:
    void execute_dma(Bus* bus) noexcept;
    void execute_dma_fill(uint16_t fill_word) noexcept;
    void execute_dma_copy() noexcept;
    void render_scanline(uint32_t y) noexcept;
    uint32_t cram_to_rgba(uint8_t index, uint8_t sh_mode) const noexcept;
    void update_cram_cache(uint8_t index) noexcept;

    std::array<uint8_t, 65536> vram_{};
    std::array<uint16_t, 64> cram_{};
    std::array<uint32_t, 64> cram_rgba_normal_{};
    std::array<uint32_t, 64> cram_rgba_shadow_{};
    std::array<uint32_t, 64> cram_rgba_highlight_{};
    std::array<uint16_t, 40> vsram_{};
    std::array<uint8_t, 24> regs_{};

    uint16_t addr_reg_ = 0;
    uint8_t code_reg_ = 0;
    bool pending_ctrl_ = false;
    bool dma_fill_pending_ = false;
    uint16_t ctrl_latch_ = 0;

    uint32_t scanline_ = 0;
    uint32_t line_cycles_ = 0;
    int32_t hint_counter_ = 0;
    bool vint_pending_ = false;
    bool hint_pending_ = false;
    bool sprite_overflow_ = false;
    bool sprite_collision_ = false;
    bool frame_complete_ = false;
    bool pal_ = false;

    std::array<uint32_t, kMaxWidth * kMaxHeight> framebuffer_{};
};

}  // namespace dragon::md
