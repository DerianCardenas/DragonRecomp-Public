#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "dragon/gpu3d.h"

namespace dragon::nds {

class Bus;

/*
 * Nintendo DS 3D GX Geometry Engine + Rendering Engine bridge to common/gpu3d.
 * Reference: GBATEK DS 3D Video (0x04000320 - 0x040006FF).
 */
class Gx3d {
public:
    static constexpr uint32_t kWidth = 256U;
    static constexpr uint32_t kHeight = 192U;

    Gx3d();
    void reset() noexcept;

    uint32_t read32(uint32_t offset) const noexcept;
    void write32(uint32_t offset, uint32_t val) noexcept;
    void write_disp3dcnt(uint16_t val) noexcept { disp3dcnt_ = val; }
    uint16_t read_disp3dcnt() const noexcept { return disp3dcnt_; }
    bool gxfifo_irq_wanted() const noexcept {
        return gxfifo_irq_mode_ == 1U || gxfifo_irq_mode_ == 2U;
    }

    // Called at VBlank to render queued polygons if SWAP_BUFFERS was issued
    void on_vblank(const Bus& bus) noexcept;

    const uint32_t* framebuffer() const noexcept { return gpu_.color_buffer(); }

private:
    struct Mat4 {
        std::array<int32_t, 16> m{};  // 1.19.12 fixed point
    };

    struct PolyVert {
        dragon::gpu3d::Vertex3D v{};
    };

    struct QueuedTri {
        dragon::gpu3d::Vertex3D v[3]{};
        dragon::gpu3d::DrawState state{};
        uint32_t tex_param = 0;
        uint32_t pltt_base = 0;
    };

    dragon::gpu3d::SoftGpu3d gpu_{kWidth, kHeight};

    uint16_t disp3dcnt_ = 0;
    uint8_t gxfifo_irq_mode_ = 0;
    uint32_t clear_color_ = 0;
    uint16_t clear_depth_ = 0x7FFFU;

    // Matrix state
    uint8_t mtx_mode_ = 0;
    Mat4 proj_mtx_{};
    Mat4 pos_mtx_{};
    Mat4 dir_mtx_{};
    Mat4 tex_mtx_{};
    mutable Mat4 clip_mtx_{};
    mutable bool clip_dirty_ = true;

    std::array<Mat4, 1> proj_stack_{};
    std::array<Mat4, 32> pos_stack_{};
    std::array<Mat4, 32> dir_stack_{};
    std::array<Mat4, 1> tex_stack_{};
    int32_t proj_sp_ = 0;
    int32_t pos_sp_ = 0;
    int32_t tex_sp_ = 0;

    // Command FIFO unpacker
    uint32_t packed_cmds_ = 0;
    uint8_t cur_cmd_ = 0;
    int32_t params_left_ = 0;
    int32_t param_count_ = 0;
    std::array<uint32_t, 32> param_buf_{};

    // Vertex / Primitive state
    bool in_begin_ = false;
    uint8_t prim_type_ = 0;
    int16_t vtx_x_ = 0;
    int16_t vtx_y_ = 0;
    int16_t vtx_z_ = 0;
    float cur_r_ = 1.0f;
    float cur_g_ = 1.0f;
    float cur_b_ = 1.0f;
    int16_t tex_s_ = 0;
    int16_t tex_t_ = 0;
    uint32_t poly_attr_ = 0;
    uint32_t active_poly_attr_ = 0;
    uint32_t tex_param_ = 0;
    uint32_t pltt_base_ = 0;

    // Viewport
    dragon::gpu3d::Viewport viewport_{0, 0, 256, 192};

    // Lighting
    std::array<int32_t, 4 * 3> light_dir_{};
    std::array<uint16_t, 4> light_col_{};
    uint16_t mat_diffuse_ = 0x7FFFU;
    uint16_t mat_ambient_ = 0;
    uint16_t mat_specular_ = 0;
    uint16_t mat_emission_ = 0;

    // Primitive assembly buffer
    std::array<dragon::gpu3d::Vertex3D, 4> strip_buf_{};
    int strip_count_ = 0;
    bool strip_odd_ = false;

    // Double-buffered triangle list
    std::vector<QueuedTri> build_list_;
    std::vector<QueuedTri> render_list_;
    bool swap_pending_ = false;
    bool w_buffer_ = false;

    // Scratch texture decode buffer
    std::vector<uint32_t> decoded_tex_;

    static Mat4 identity_mat() noexcept;
    static Mat4 mul_mat(const Mat4& a, const Mat4& b) noexcept;
    static int cmd_param_count(uint8_t cmd) noexcept;

    void push_fifo_word(uint32_t word) noexcept;
    void execute_command(uint8_t cmd, const uint32_t* params, int count) noexcept;
    void submit_vertex() noexcept;
    void update_clip_matrix() const noexcept;
    void decode_texture(const Bus& bus, uint32_t tex_param, uint32_t pltt_base,
                        dragon::gpu3d::Texture2D& out_tex) noexcept;
};

}  // namespace dragon::nds
