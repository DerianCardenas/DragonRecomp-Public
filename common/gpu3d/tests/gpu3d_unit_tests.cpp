#include <cassert>
#include <cstdio>
#include <vector>

#include "dragon/gpu3d.h"

using namespace dragon::gpu3d;

namespace {

void test_basic_triangle_and_zbuffer() {
    std::printf("Running test_basic_triangle_and_zbuffer...\n");
    SoftGpu3d gpu(64, 64);
    gpu.clear(0xFF000000U, 1.0f);

    DrawState st{};
    st.depth_func = DepthFunc::Less;
    st.depth_write = true;

    // Far red triangle covering center (z = 0.5)
    Vertex3D r0{-0.8f, -0.8f, 0.5f, 1.0f, 0, 0, 1, 0, 0, 1};
    Vertex3D r1{0.8f, -0.8f, 0.5f, 1.0f, 0, 0, 1, 0, 0, 1};
    Vertex3D r2{0.0f, 0.8f, 0.5f, 1.0f, 0, 0, 1, 0, 0, 1};
    gpu.draw_triangle(r0, r1, r2, st);

    const uint32_t center_px = gpu.color_buffer()[32 * 64 + 32];
    assert((center_px & 0xFFU) == 255U);         // Red
    assert(((center_px >> 8U) & 0xFFU) == 0U);   // Green
    assert(((center_px >> 16U) & 0xFFU) == 0U);  // Blue

    // Near green triangle covering center (z = 0.0 -> closer)
    Vertex3D g0{-0.5f, -0.5f, 0.0f, 1.0f, 0, 0, 0, 1, 0, 1};
    Vertex3D g1{0.5f, -0.5f, 0.0f, 1.0f, 0, 0, 0, 1, 0, 1};
    Vertex3D g2{0.0f, 0.5f, 0.0f, 1.0f, 0, 0, 0, 1, 0, 1};
    gpu.draw_triangle(g0, g1, g2, st);

    const uint32_t center_after = gpu.color_buffer()[32 * 64 + 32];
    assert((center_after & 0xFFU) == 0U);
    assert(((center_after >> 8U) & 0xFFU) == 255U);

    // Behind blue triangle (z = 0.8) must NOT overwrite green
    Vertex3D b0{-0.9f, -0.9f, 0.8f, 1.0f, 0, 0, 0, 0, 1, 1};
    Vertex3D b1{0.9f, -0.9f, 0.8f, 1.0f, 0, 0, 0, 0, 1, 1};
    Vertex3D b2{0.0f, 0.9f, 0.8f, 1.0f, 0, 0, 0, 0, 1, 1};
    gpu.draw_triangle(b0, b1, b2, st);
    assert(((gpu.color_buffer()[32 * 64 + 32] >> 8U) & 0xFFU) == 255U);
    std::printf("  passed!\n");
}

void test_textured_quad_and_alpha_blend() {
    std::printf("Running test_textured_quad_and_alpha_blend...\n");
    SoftGpu3d gpu(32, 32);
    gpu.clear(0xFF000000U, 1.0f);

    // 2x2 checkerboard texture: (0,0)=white, (1,0)=blue, (0,1)=green, (1,1)=red
    const uint32_t texels[4] = {0xFFFFFFFFU, 0xFFFF0000U, 0xFF00FF00U, 0xFF0000FFU};
    Texture2D tex{texels, 2, 2, true, true, false, false};

    DrawState st{};
    st.textured = true;
    st.modulate = true;

    Vertex3D v0{-1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1, 1, 1, 1};
    Vertex3D v1{1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1, 1, 1, 1};
    Vertex3D v2{1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1, 1, 1, 1};
    Vertex3D v3{-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1, 1, 1, 1};
    gpu.draw_triangle(v0, v1, v2, st, &tex);
    gpu.draw_triangle(v0, v2, v3, st, &tex);

    assert(gpu.color_buffer()[4 * 32 + 4] == 0xFFFFFFFFU);    // top-left: white
    assert(gpu.color_buffer()[4 * 32 + 24] == 0xFFFF0000U);   // top-right: blue
    assert(gpu.color_buffer()[24 * 32 + 4] == 0xFF00FF00U);   // bottom-left: green
    assert(gpu.color_buffer()[24 * 32 + 24] == 0xFF0000FFU);  // bottom-right: red
    std::printf("  passed!\n");
}

}  // namespace

int main() {
    test_basic_triangle_and_zbuffer();
    test_textured_quad_and_alpha_blend();
    std::printf("All dragon_gpu3d unit tests passed!\n");
    return 0;
}
