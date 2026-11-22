#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "dragon/gpu3d.h"

namespace dragon::psp {

class PspBus;

class PspGe {
public:
    static constexpr uint32_t kScreenWidth = 480U;
    static constexpr uint32_t kScreenHeight = 272U;

    PspGe();
    void reset();

    // Enqueue and immediately execute a GE display list (up to stall_addr, or END/FINISH).
    int enqueue_list(PspBus& bus, uint32_t list_addr, uint32_t stall_addr, int cb_id);
    int update_stall_addr(PspBus& bus, int list_id, uint32_t stall_addr);
    int list_sync(int list_id) const;
    int draw_sync() const;

    // Display configuration (from sceDisplaySetFrameBuf)
    void set_display_framebuf(uint32_t top_addr, uint32_t buf_width, uint32_t pixel_format);
    uint32_t display_top_addr() const noexcept { return display_top_addr_; }
    uint32_t display_buf_width() const noexcept { return display_buf_width_; }
    uint32_t display_pixel_format() const noexcept { return display_pixel_format_; }

    // Composite final 480x272 ABGR8888 screen from VRAM framebuffer or GE SoftGpu3d output.
    void present_frame(const PspBus& bus, uint32_t* out_abgr480x272) const;

    uint32_t commands_executed() const noexcept { return commands_executed_; }
    uint32_t triangles_submitted() const noexcept { return triangles_submitted_; }
    bool has_ge_drawn() const noexcept { return has_ge_drawn_; }

    const dragon::gpu3d::SoftGpu3d& gpu3d() const noexcept { return gpu3d_; }

private:
    struct GeListState {
        bool active = false;
        bool finished = false;
        uint32_t pc = 0;
        uint32_t stall_addr = 0;
        std::array<uint32_t, 8> call_stack{};
        uint8_t call_depth = 0;
    };

    void execute_list(PspBus& bus, GeListState& list);
    void execute_command(PspBus& bus, GeListState& list, uint8_t cmd, uint32_t arg);
    void draw_primitive(PspBus& bus, uint32_t prim_type, uint32_t vertex_count);
    void copy_gpu3d_to_vram(PspBus& bus) const;
    void decode_texture(const PspBus& bus, dragon::gpu3d::Texture2D& out_tex);
    void execute_block_transfer(PspBus& bus, uint32_t arg);

    dragon::gpu3d::SoftGpu3d gpu3d_;

    // Display registers
    uint32_t display_top_addr_ = 0x04000000U;
    uint32_t display_buf_width_ = 512U;
    uint32_t display_pixel_format_ = 3U;  // 0=565, 1=5551, 2=4444, 3=8888

    // GE state
    uint32_t base_addr_ = 0;
    uint32_t offset_addr_ = 0;
    uint32_t vaddr_ = 0;
    uint32_t iaddr_ = 0;
    uint32_t vtype_ = 0;
    uint32_t fb_ptr_ = 0x04000000U;
    uint32_t fb_width_ = 512U;
    uint32_t fb_pixel_format_ = 3U;
    uint32_t zbuf_ptr_ = 0x04088000U;
    uint32_t zbuf_width_ = 512U;

    bool clear_mode_ = false;
    uint8_t clear_flags_ = 0;
    bool cull_enable_ = false;
    uint32_t cull_dir_ = 0;
    bool texture_enable_ = false;
    bool alpha_blend_enable_ = false;
    bool alpha_test_enable_ = false;
    uint8_t alpha_test_ref_ = 0;
    bool depth_test_enable_ = false;
    uint32_t depth_func_ = 2U;
    bool depth_write_disable_ = false;
    uint32_t blend_mode_ = 0;
    uint32_t tex_func_ = 0;

    // Matrices (column-major 4x4)
    std::array<float, 16> world_mtx_{};
    std::array<float, 16> view_mtx_{};
    std::array<float, 16> proj_mtx_{};
    uint32_t world_idx_ = 0;
    uint32_t view_idx_ = 0;
    uint32_t proj_idx_ = 0;

    // Viewport & offset
    float vp_xscale_ = 240.0f;
    float vp_yscale_ = -136.0f;
    float vp_zscale_ = 32767.0f;
    float vp_xcenter_ = 2048.0f;
    float vp_ycenter_ = 2048.0f;
    float vp_zcenter_ = 32768.0f;
    float offset_x_ = 1808.0f;  // 2048 - 240
    float offset_y_ = 1912.0f;  // 2048 - 136
    float tex_scale_u_ = 1.0f;
    float tex_scale_v_ = 1.0f;
    float tex_offset_u_ = 0.0f;
    float tex_offset_v_ = 0.0f;

    // Texture & CLUT state
    uint32_t tex_addr_ = 0;
    uint32_t tex_buf_width_ = 64U;
    uint32_t tex_width_ = 64U;
    uint32_t tex_height_ = 64U;
    uint32_t tex_format_ = 3U;
    bool tex_swizzled_ = false;
    bool tex_clamp_u_ = false;
    bool tex_clamp_v_ = false;
    uint32_t clut_addr_ = 0;
    uint32_t clut_format_ = 3U;
    std::array<uint32_t, 256> clut_rgba_{};
    std::vector<uint32_t> decoded_tex_;

    // Block transfer registers
    uint32_t xfer_src_ = 0;
    uint32_t xfer_src_w_ = 512U;
    uint32_t xfer_dst_ = 0;
    uint32_t xfer_dst_w_ = 512U;
    uint32_t xfer_src_pos_ = 0;
    uint32_t xfer_dst_pos_ = 0;
    uint32_t xfer_size_ = 0;

    std::array<GeListState, 16> lists_{};
    int next_list_id_ = 1;
    uint32_t commands_executed_ = 0;
    uint32_t triangles_submitted_ = 0;
    bool has_ge_drawn_ = false;
};

}  // namespace dragon::psp
