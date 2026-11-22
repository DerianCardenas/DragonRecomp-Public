#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace dragon::ps1 {

class Ps1Gpu {
public:
    static constexpr uint32_t kVramWidth = 1024U;
    static constexpr uint32_t kVramHeight = 512U;
    static constexpr size_t kVramPixels = kVramWidth * kVramHeight;

    Ps1Gpu();
    void reset();

    // MMIO ports 0x1F801810 (GP0/GPUREAD) and 0x1F801814 (GP1/GPUSTAT)
    void write_gp0(uint32_t val) noexcept;
    void write_gp1(uint32_t val) noexcept;
    uint32_t read_gpuread() noexcept;
    uint32_t read_gpustat() const noexcept;

    // Render active display region into ABGR8888 buffer (updates out_w / out_h)
    void present_frame(std::vector<uint32_t>& out_abgr, uint32_t& out_w, uint32_t& out_h) const;

    uint16_t* vram() noexcept { return vram_.data(); }
    const uint16_t* vram() const noexcept { return vram_.data(); }

    uint32_t display_width() const noexcept;
    uint32_t display_height() const noexcept;
    uint32_t primitives_drawn() const noexcept { return primitives_drawn_; }
    void toggle_odd_line() noexcept { odd_line_ = !odd_line_; }

private:
    enum class TransferMode : uint8_t {
        None = 0,
        CpuToVram,
        VramToCpu,
    };

    void dispatch_gp0_command() noexcept;
    void cmd_fill_rect() noexcept;
    void cmd_polygon(uint8_t cmd) noexcept;
    void cmd_line(uint8_t cmd) noexcept;
    void cmd_rect(uint8_t cmd) noexcept;
    void cmd_vram_to_vram() noexcept;
    void cmd_cpu_to_vram() noexcept;
    void cmd_vram_to_cpu() noexcept;

    struct Vertex {
        int32_t x = 0;
        int32_t y = 0;
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        uint8_t u = 0;
        uint8_t v = 0;
    };

    void rasterize_triangle(const Vertex& a, const Vertex& b, const Vertex& c, bool shaded,
                            bool textured, bool semi_trans, bool raw_tex, uint16_t clut,
                            uint16_t tpage) noexcept;
    void plot_pixel(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b, bool semi_trans,
                    uint8_t semi_mode, bool mask_bit) noexcept;
    bool sample_texel(uint16_t tpage, uint16_t clut, uint32_t u, uint32_t v, uint8_t& out_r,
                      uint8_t& out_g, uint8_t& out_b, bool& out_stp) const noexcept;

    std::vector<uint16_t> vram_;  // 1024 x 512 BGR555 + STP bit15

    // GP0 command FIFO state
    std::array<uint32_t, 16> cmd_fifo_{};
    uint32_t cmd_words_received_ = 0;
    uint32_t cmd_words_expected_ = 0;
    bool polyline_active_ = false;
    uint8_t polyline_cmd_ = 0;
    bool polyline_expect_color_ = false;
    Vertex polyline_prev_{};

    // VRAM CPU transfer state
    TransferMode xfer_mode_ = TransferMode::None;
    uint32_t xfer_x_ = 0;
    uint32_t xfer_y_ = 0;
    uint32_t xfer_w_ = 0;
    uint32_t xfer_h_ = 0;
    uint32_t xfer_cur_x_ = 0;
    uint32_t xfer_cur_y_ = 0;

    // GPU registers
    uint32_t gpuread_latch_ = 0;
    uint32_t texpage_ = 0;
    uint32_t tex_window_ = 0;
    int32_t draw_area_x1_ = 0;
    int32_t draw_area_y1_ = 0;
    int32_t draw_area_x2_ = 1023;
    int32_t draw_area_y2_ = 511;
    int32_t draw_offset_x_ = 0;
    int32_t draw_offset_y_ = 0;
    bool set_mask_bit_ = false;
    bool check_mask_bit_ = false;
    bool dither_ = false;
    bool draw_to_display_ = false;

    // Display registers (GP1)
    bool display_disabled_ = false;
    uint32_t dma_dir_ = 0;
    uint32_t disp_x_ = 0;
    uint32_t disp_y_ = 0;
    uint32_t disp_h_mode_ = 1;  // 0=256, 1=320, 2=512, 3=640
    bool disp_h_368_ = false;
    bool disp_v_480_ = false;
    bool disp_24bit_ = false;
    bool disp_pal_ = false;
    bool odd_line_ = false;
    uint32_t primitives_drawn_ = 0;
};

}  // namespace dragon::ps1
