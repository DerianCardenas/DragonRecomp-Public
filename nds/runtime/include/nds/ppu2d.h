#pragma once

#include <array>
#include <cstdint>

namespace dragon::nds {

class Bus;

/*
 * Nintendo DS 2D Graphics Engine (instantiated twice: Engine A and Engine B).
 * Each engine renders 256x192 RGBA8888 (ABGR8888 little-endian) scanline-by-scanline.
 */
class Engine2D {
public:
    static constexpr uint32_t kWidth = 256U;
    static constexpr uint32_t kHeight = 192U;

    explicit Engine2D(bool is_engine_a = true) noexcept : is_engine_a_(is_engine_a) {}

    void reset() noexcept;
    void render_scanline(uint32_t y, const Bus& bus, const uint32_t* gx3d_fb = nullptr) noexcept;

    uint32_t read32(uint32_t offset) const noexcept;
    void write32(uint32_t offset, uint32_t val, uint32_t mask) noexcept;
    void write_master_bright(uint16_t val) noexcept { master_bright_ = val; }
    uint16_t read_master_bright() const noexcept { return master_bright_; }

    const std::array<uint32_t, kWidth * kHeight>& framebuffer() const noexcept { return fb_; }

private:
    bool is_engine_a_ = true;

    uint32_t dispcnt_ = 0;
    std::array<uint16_t, 4> bgcnt_{};
    std::array<uint16_t, 4> bghofs_{};
    std::array<uint16_t, 4> bgvofs_{};

    // Affine parameters for BG2 (idx 0) and BG3 (idx 1)
    std::array<int16_t, 2> bg_pa_{0x100, 0x100};
    std::array<int16_t, 2> bg_pb_{0, 0};
    std::array<int16_t, 2> bg_pc_{0, 0};
    std::array<int16_t, 2> bg_pd_{0x100, 0x100};
    std::array<int32_t, 2> bg_x_{0, 0};
    std::array<int32_t, 2> bg_y_{0, 0};
    std::array<int32_t, 2> bg_ref_x_{0, 0};
    std::array<int32_t, 2> bg_ref_y_{0, 0};

    uint16_t bldcnt_ = 0;
    uint16_t bldalpha_ = 0;
    uint16_t bldy_ = 0;
    uint16_t master_bright_ = 0;

    std::array<uint32_t, kWidth * kHeight> fb_{};

    static uint32_t bgr555_to_rgba(uint16_t c) noexcept;
    uint32_t apply_master_bright(uint32_t rgba) const noexcept;
    void render_text_bg(uint32_t y, int bg, const Bus& bus, uint32_t* line_col,
                        uint8_t* line_prio) noexcept;
    void render_affine_or_ext_bg(uint32_t y, int bg, const Bus& bus, uint32_t* line_col,
                                 uint8_t* line_prio) noexcept;
    void render_sprites(uint32_t y, const Bus& bus, uint32_t* line_col,
                        uint8_t* line_prio) noexcept;
};

}  // namespace dragon::nds
