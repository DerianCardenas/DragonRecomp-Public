#pragma once

#include <array>
#include <cstdint>

namespace dragon::gba {

class Ppu {
public:
    static constexpr uint32_t kScreenWidth = 240;
    static constexpr uint32_t kScreenHeight = 160;
    static constexpr uint32_t kTotalLines = 228;
    static constexpr uint32_t kHDrawCycles = 960;
    static constexpr uint32_t kCyclesPerLine = 1232;
    static constexpr uint32_t kCyclesPerFrame = kTotalLines * kCyclesPerLine;  // 280,896

    Ppu();

    void reset();

    struct StepEvents {
        uint16_t irq_flags = 0;  // Bit 0: VBlank, Bit 1: HBlank, Bit 2: VCount
        bool entered_hblank = false;
        bool entered_vblank = false;
    };

    StepEvents step(uint32_t cycles);

    uint8_t read_io8(uint32_t offset) const noexcept;
    uint16_t read_io16(uint32_t offset) const noexcept;
    void write_io8(uint32_t offset, uint8_t value) noexcept;
    void write_io16(uint32_t offset, uint16_t value) noexcept;

    uint8_t read_palette8(uint32_t offset) const noexcept { return palette_ram_[offset & 0x3FFU]; }
    uint16_t read_palette16(uint32_t offset) const noexcept;
    void write_palette16(uint32_t offset, uint16_t value) noexcept;

    uint8_t read_vram8(uint32_t offset) const noexcept;
    uint16_t read_vram16(uint32_t offset) const noexcept;
    void write_vram16(uint32_t offset, uint16_t value) noexcept;

    uint8_t read_oam8(uint32_t offset) const noexcept { return oam_[offset & 0x3FFU]; }
    uint16_t read_oam16(uint32_t offset) const noexcept;
    void write_oam16(uint32_t offset, uint16_t value) noexcept;

    bool consume_frame_complete() noexcept {
        const bool v = frame_complete_;
        frame_complete_ = false;
        return v;
    }

    uint16_t vcount() const noexcept { return vcount_; }
    uint16_t dispcnt() const noexcept { return dispcnt_; }

    const std::array<uint32_t, kScreenWidth * kScreenHeight>& framebuffer() const noexcept {
        return framebuffer_;
    }

    std::array<uint8_t, 0x400>& palette_ram() noexcept { return palette_ram_; }
    const std::array<uint8_t, 0x400>& palette_ram() const noexcept { return palette_ram_; }
    std::array<uint8_t, 0x18000>& vram() noexcept { return vram_; }
    const std::array<uint8_t, 0x18000>& vram() const noexcept { return vram_; }
    std::array<uint8_t, 0x400>& oam() noexcept { return oam_; }
    const std::array<uint8_t, 0x400>& oam() const noexcept { return oam_; }

    static uint32_t rgb555_to_rgba32(uint16_t color) noexcept;

    struct StatePayload {
        uint16_t dispcnt = 0;
        uint16_t dispstat = 0;
        uint16_t vcount = 0;
        uint32_t line_cycles = 0;
        uint8_t in_hblank = 0;
        uint8_t frame_complete = 0;
        std::array<uint16_t, 4> bgcnt{};
        std::array<uint16_t, 4> bghofs{};
        std::array<uint16_t, 4> bgvofs{};
        std::array<int16_t, 2> bgpa{};
        std::array<int16_t, 2> bgpb{};
        std::array<int16_t, 2> bgpc{};
        std::array<int16_t, 2> bgpd{};
        std::array<int32_t, 2> bgx_latch{};
        std::array<int32_t, 2> bgy_latch{};
        std::array<int32_t, 2> bgx_internal{};
        std::array<int32_t, 2> bgy_internal{};
        std::array<uint32_t, 2> bgx_raw{};
        std::array<uint32_t, 2> bgy_raw{};
        uint16_t win0h = 0;
        uint16_t win1h = 0;
        uint16_t win0v = 0;
        uint16_t win1v = 0;
        uint16_t winin = 0;
        uint16_t winout = 0;
        uint16_t mosaic = 0;
        uint16_t bldcnt = 0;
        uint16_t bldalpha = 0;
        uint16_t bldy = 0;
    };

    void save_state(StatePayload& p) const noexcept;
    void load_state(const StatePayload& p) noexcept;

private:
    void render_scanline(uint16_t line);
    void render_regular_bg(int bg_idx, uint16_t line, uint16_t* line_color, uint8_t* line_prio,
                           uint8_t* line_layer);
    void render_affine_bg(int bg_idx, uint16_t line, uint16_t* line_color, uint8_t* line_prio,
                          uint8_t* line_layer);
    void render_sprites(uint16_t line, uint16_t* obj_color, uint8_t* obj_prio, bool* obj_semi,
                        bool* obj_win);

    static uint32_t map_vram_offset(uint32_t offset) noexcept;

    uint16_t dispcnt_ = 0;
    uint16_t dispstat_ = 0;
    uint16_t vcount_ = 0;
    uint32_t line_cycles_ = 0;
    bool in_hblank_ = false;
    bool frame_complete_ = false;

    std::array<uint16_t, 4> bgcnt_{};
    std::array<uint16_t, 4> bghofs_{};
    std::array<uint16_t, 4> bgvofs_{};

    // Affine parameters for BG2 (index 0) and BG3 (index 1)
    std::array<int16_t, 2> bgpa_{0x0100, 0x0100};
    std::array<int16_t, 2> bgpb_{0, 0};
    std::array<int16_t, 2> bgpc_{0, 0};
    std::array<int16_t, 2> bgpd_{0x0100, 0x0100};
    std::array<int32_t, 2> bgx_latch_{0, 0};
    std::array<int32_t, 2> bgy_latch_{0, 0};
    std::array<int32_t, 2> bgx_internal_{0, 0};
    std::array<int32_t, 2> bgy_internal_{0, 0};
    std::array<uint32_t, 2> bgx_raw_{0, 0};
    std::array<uint32_t, 2> bgy_raw_{0, 0};

    uint16_t win0h_ = 0;
    uint16_t win1h_ = 0;
    uint16_t win0v_ = 0;
    uint16_t win1v_ = 0;
    uint16_t winin_ = 0;
    uint16_t winout_ = 0;
    uint16_t mosaic_ = 0;
    uint16_t bldcnt_ = 0;
    uint16_t bldalpha_ = 0;
    uint16_t bldy_ = 0;

    std::array<uint8_t, 0x400> palette_ram_{};
    std::array<uint8_t, 0x18000> vram_{};
    std::array<uint8_t, 0x400> oam_{};
    std::array<uint32_t, kScreenWidth * kScreenHeight> framebuffer_{};
};

}  // namespace dragon::gba
