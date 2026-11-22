#pragma once

#include <cstdint>
#include <vector>

namespace dragon::gpu3d {

struct Vertex3D {
    float x = 0.0f;  // Clip-space X
    float y = 0.0f;  // Clip-space Y
    float z = 0.0f;  // Clip-space Z
    float w = 1.0f;  // Clip-space W
    float u = 0.0f;  // Texture U (normalized 0..1)
    float v = 0.0f;  // Texture V (normalized 0..1)
    float r = 1.0f;  // Vertex color R (0..1)
    float g = 1.0f;  // Vertex color G (0..1)
    float b = 1.0f;  // Vertex color B (0..1)
    float a = 1.0f;  // Vertex color A (0..1)
};

struct Texture2D {
    const uint32_t* rgba = nullptr;  // ABGR8888 (R in low byte)
    uint32_t width = 0;
    uint32_t height = 0;
    bool repeat_u = true;
    bool repeat_v = true;
    bool flip_u = false;
    bool flip_v = false;
};

enum class CullMode : uint8_t {
    None = 0,
    Back = 1,
    Front = 2,
};

enum class DepthFunc : uint8_t {
    Less = 0,
    LessEqual = 1,
    Equal = 2,
    Always = 3,
};

enum class BlendMode : uint8_t {
    Opaque = 0,
    AlphaBlend = 1,
    Additive = 2,
};

struct DrawState {
    CullMode cull = CullMode::None;
    DepthFunc depth_func = DepthFunc::Less;
    bool depth_write = true;
    BlendMode blend = BlendMode::Opaque;
    uint8_t alpha_test_ref = 0;  // Discard pixel if alpha <= alpha_test_ref (when > 0)
    bool textured = false;
    bool modulate = true;   // Multiply texture color by vertex color
    bool w_buffer = false;  // Use linear W depth instead of Z/W (NDS SWAP_BUFFERS bit 1)
    uint8_t poly_id = 0;
};

struct Viewport {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 256;
    int32_t height = 192;
};

/*
 * Shared deterministic software 3D rasterizer backend (NDS, PSP, PS1, N64).
 * Hot-path draw_triangle() performs zero heap allocations.
 */
class SoftGpu3d {
public:
    SoftGpu3d(uint32_t width = 256U, uint32_t height = 192U);

    void resize(uint32_t width, uint32_t height);
    void clear(uint32_t clear_rgba = 0x00000000U, float clear_depth = 1.0f) noexcept;
    void set_viewport(const Viewport& vp) noexcept { viewport_ = vp; }

    void draw_triangle(const Vertex3D& v0, const Vertex3D& v1, const Vertex3D& v2,
                       const DrawState& state, const Texture2D* tex = nullptr) noexcept;

    uint32_t width() const noexcept { return width_; }
    uint32_t height() const noexcept { return height_; }
    const uint32_t* color_buffer() const noexcept { return color_.data(); }
    const float* depth_buffer() const noexcept { return depth_.data(); }

private:
    uint32_t width_ = 256U;
    uint32_t height_ = 192U;
    Viewport viewport_{0, 0, 256, 192};
    std::vector<uint32_t> color_;
    std::vector<float> depth_;

    static uint32_t sample_texture(const Texture2D& tex, float u, float v) noexcept;
    void rasterize_clipped(const Vertex3D& v0, const Vertex3D& v1, const Vertex3D& v2,
                           const DrawState& state, const Texture2D* tex) noexcept;
};

}  // namespace dragon::gpu3d
