#include "dragon/gpu3d.h"

#include <algorithm>
#include <cmath>

namespace dragon::gpu3d {

namespace {

inline float clamp01(float x) noexcept {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

inline uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) noexcept {
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8U) |
           (static_cast<uint32_t>(b) << 16U) | (static_cast<uint32_t>(a) << 24U);
}

inline Vertex3D lerp_vertex(const Vertex3D& a, const Vertex3D& b, float t) noexcept {
    Vertex3D o{};
    o.x = a.x + (b.x - a.x) * t;
    o.y = a.y + (b.y - a.y) * t;
    o.z = a.z + (b.z - a.z) * t;
    o.w = a.w + (b.w - a.w) * t;
    o.u = a.u + (b.u - a.u) * t;
    o.v = a.v + (b.v - a.v) * t;
    o.r = a.r + (b.r - a.r) * t;
    o.g = a.g + (b.g - a.g) * t;
    o.b = a.b + (b.b - a.b) * t;
    o.a = a.a + (b.a - a.a) * t;
    return o;
}

inline int32_t wrap_coord(int32_t coord, int32_t size, bool repeat, bool flip) noexcept {
    if (size <= 0)
        return 0;
    if (!repeat) {
        return std::clamp(coord, 0, size - 1);
    }
    if (flip) {
        const int32_t period = size * 2;
        int32_t m = coord % period;
        if (m < 0)
            m += period;
        return (m < size) ? m : (period - 1 - m);
    }
    int32_t m = coord % size;
    if (m < 0)
        m += size;
    return m;
}

inline bool pass_depth(DepthFunc fn, float z, float old_z) noexcept {
    switch (fn) {
        case DepthFunc::Less:
            return z < old_z;
        case DepthFunc::LessEqual:
            return z <= old_z;
        case DepthFunc::Equal:
            return std::fabs(z - old_z) <= 1e-5f;
        case DepthFunc::Always:
            return true;
    }
    return z < old_z;
}

}  // namespace

SoftGpu3d::SoftGpu3d(uint32_t width, uint32_t height) {
    resize(width, height);
}

void SoftGpu3d::resize(uint32_t width, uint32_t height) {
    width_ = std::max(1U, width);
    height_ = std::max(1U, height);
    viewport_ = Viewport{0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_)};
    color_.assign(static_cast<size_t>(width_) * height_, 0x00000000U);
    depth_.assign(static_cast<size_t>(width_) * height_, 1.0f);
}

void SoftGpu3d::clear(uint32_t clear_rgba, float clear_depth) noexcept {
    std::fill(color_.begin(), color_.end(), clear_rgba);
    std::fill(depth_.begin(), depth_.end(), clear_depth);
}

uint32_t SoftGpu3d::sample_texture(const Texture2D& tex, float u, float v) noexcept {
    if (!tex.rgba || tex.width == 0U || tex.height == 0U) {
        return 0xFFFFFFFFU;
    }
    const int32_t iu = static_cast<int32_t>(std::floor(u * static_cast<float>(tex.width)));
    const int32_t iv = static_cast<int32_t>(std::floor(v * static_cast<float>(tex.height)));
    const int32_t tx = wrap_coord(iu, static_cast<int32_t>(tex.width), tex.repeat_u, tex.flip_u);
    const int32_t ty = wrap_coord(iv, static_cast<int32_t>(tex.height), tex.repeat_v, tex.flip_v);
    return tex.rgba[static_cast<size_t>(ty) * tex.width + static_cast<size_t>(tx)];
}

void SoftGpu3d::draw_triangle(const Vertex3D& v0, const Vertex3D& v1, const Vertex3D& v2,
                              const DrawState& state, const Texture2D* tex) noexcept {
    // Clip against near plane: z + w >= 0 and w >= epsilon
    constexpr float kNearEps = 1e-4f;
    Vertex3D poly[8] = {v0, v1, v2};
    int count = 3;

    // 1) Clip against w >= kNearEps
    Vertex3D tmp[8];
    int out_count = 0;
    for (int i = 0; i < count; ++i) {
        const Vertex3D& a = poly[i];
        const Vertex3D& b = poly[(i + 1) % count];
        const float da = a.w - kNearEps;
        const float db = b.w - kNearEps;
        const bool ina = da >= 0.0f;
        const bool inb = db >= 0.0f;
        if (ina) {
            tmp[out_count++] = a;
        }
        if (ina != inb) {
            const float t = da / (da - db);
            tmp[out_count++] = lerp_vertex(a, b, t);
        }
    }
    if (out_count < 3)
        return;

    // 2) Clip against z + w >= 0 (near plane)
    count = 0;
    for (int i = 0; i < out_count; ++i) {
        const Vertex3D& a = tmp[i];
        const Vertex3D& b = tmp[(i + 1) % out_count];
        const float da = a.z + a.w;
        const float db = b.z + b.w;
        const bool ina = da >= 0.0f;
        const bool inb = db >= 0.0f;
        if (ina) {
            poly[count++] = a;
        }
        if (ina != inb) {
            const float t = da / (da - db);
            poly[count++] = lerp_vertex(a, b, t);
        }
    }
    if (count < 3)
        return;

    // Fan-triangulate clipped polygon
    for (int i = 1; i + 1 < count; ++i) {
        rasterize_clipped(poly[0], poly[i], poly[i + 1], state, tex);
    }
}

void SoftGpu3d::rasterize_clipped(const Vertex3D& v0, const Vertex3D& v1, const Vertex3D& v2,
                                  const DrawState& state, const Texture2D* tex) noexcept {
    const float inv_w0 = 1.0f / v0.w;
    const float inv_w1 = 1.0f / v1.w;
    const float inv_w2 = 1.0f / v2.w;

    const float half_w = static_cast<float>(viewport_.width) * 0.5f;
    const float half_h = static_cast<float>(viewport_.height) * 0.5f;
    const float vp_x = static_cast<float>(viewport_.x);
    const float vp_y = static_cast<float>(viewport_.y);

    const float sx0 = vp_x + (v0.x * inv_w0 + 1.0f) * half_w;
    const float sy0 = vp_y + (1.0f - v0.y * inv_w0) * half_h;
    const float sz0 = (v0.z * inv_w0 + 1.0f) * 0.5f;

    const float sx1 = vp_x + (v1.x * inv_w1 + 1.0f) * half_w;
    const float sy1 = vp_y + (1.0f - v1.y * inv_w1) * half_h;
    const float sz1 = (v1.z * inv_w1 + 1.0f) * 0.5f;

    const float sx2 = vp_x + (v2.x * inv_w2 + 1.0f) * half_w;
    const float sy2 = vp_y + (1.0f - v2.y * inv_w2) * half_h;
    const float sz2 = (v2.z * inv_w2 + 1.0f) * 0.5f;

    // Signed area in screen space (Y-down: CCW in NDC gives area < 0)
    float area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
    if (std::fabs(area) < 1e-6f) {
        return;
    }

    if (state.cull == CullMode::Back && area > 0.0f) {
        return;
    }
    if (state.cull == CullMode::Front && area < 0.0f) {
        return;
    }

    // Normalize winding so edge functions are >= 0 inside
    float x0 = sx0, y0 = sy0, z0 = sz0, w0 = inv_w0;
    float x1 = sx1, y1 = sy1, z1 = sz1, w1 = inv_w1;
    float x2 = sx2, y2 = sy2, z2 = sz2, w2 = inv_w2;
    Vertex3D a = v0, b = v1, c = v2;
    if (area < 0.0f) {
        std::swap(x1, x2);
        std::swap(y1, y2);
        std::swap(z1, z2);
        std::swap(w1, w2);
        std::swap(b, c);
        area = -area;
    }

    const int32_t min_x =
        std::max<int32_t>(0, static_cast<int32_t>(std::floor(std::min({x0, x1, x2}))));
    const int32_t max_x = std::min<int32_t>(
        static_cast<int32_t>(width_) - 1, static_cast<int32_t>(std::ceil(std::max({x0, x1, x2}))));
    const int32_t min_y =
        std::max<int32_t>(0, static_cast<int32_t>(std::floor(std::min({y0, y1, y2}))));
    const int32_t max_y = std::min<int32_t>(
        static_cast<int32_t>(height_) - 1, static_cast<int32_t>(std::ceil(std::max({y0, y1, y2}))));

    if (min_x > max_x || min_y > max_y) {
        return;
    }

    const float inv_area = 1.0f / area;
    const float u0w = a.u * w0, v0w = a.v * w0;
    const float u1w = b.u * w1, v1w = b.v * w1;
    const float u2w = c.u * w2, v2w = c.v * w2;

    for (int32_t py = min_y; py <= max_y; ++py) {
        const float fy = static_cast<float>(py) + 0.5f;
        for (int32_t px = min_x; px <= max_x; ++px) {
            const float fx = static_cast<float>(px) + 0.5f;

            const float e0 = (x2 - x1) * (fy - y1) - (y2 - y1) * (fx - x1);
            const float e1 = (x0 - x2) * (fy - y2) - (y0 - y2) * (fx - x2);
            const float e2 = (x1 - x0) * (fy - y0) - (y1 - y0) * (fx - x0);
            if (e0 < 0.0f || e1 < 0.0f || e2 < 0.0f) {
                continue;
            }

            const float b0 = e0 * inv_area;
            const float b1 = e1 * inv_area;
            const float b2 = e2 * inv_area;

            const float interp_w = b0 * w0 + b1 * w1 + b2 * w2;
            float z = 0.0f;
            if (state.w_buffer) {
                z = (interp_w > 1e-8f) ? (1.0f / interp_w) : 1e9f;
            } else {
                z = b0 * z0 + b1 * z1 + b2 * z2;
                if (z < -1e-4f || z > 1.0001f) {
                    continue;
                }
            }

            const size_t idx = static_cast<size_t>(py) * width_ + static_cast<size_t>(px);
            if (!pass_depth(state.depth_func, z, depth_[idx])) {
                continue;
            }

            float cr = b0 * a.r + b1 * b.r + b2 * c.r;
            float cg = b0 * a.g + b1 * b.g + b2 * c.g;
            float cb = b0 * a.b + b1 * b.b + b2 * c.b;
            float ca = b0 * a.a + b1 * b.a + b2 * c.a;

            if (state.textured && tex && tex->rgba) {
                const float inv_interp_w = (interp_w > 1e-8f) ? (1.0f / interp_w) : 1.0f;
                const float u = (b0 * u0w + b1 * u1w + b2 * u2w) * inv_interp_w;
                const float v = (b0 * v0w + b1 * v1w + b2 * v2w) * inv_interp_w;
                const uint32_t tpx = sample_texture(*tex, u, v);
                const float tr = static_cast<float>(tpx & 0xFFU) / 255.0f;
                const float tg = static_cast<float>((tpx >> 8U) & 0xFFU) / 255.0f;
                const float tb = static_cast<float>((tpx >> 16U) & 0xFFU) / 255.0f;
                const float ta = static_cast<float>((tpx >> 24U) & 0xFFU) / 255.0f;
                if (state.modulate) {
                    cr *= tr;
                    cg *= tg;
                    cb *= tb;
                    ca *= ta;
                } else {
                    cr = tr;
                    cg = tg;
                    cb = tb;
                    ca = ta;
                }
            }

            const uint8_t out_a = static_cast<uint8_t>(clamp01(ca) * 255.0f + 0.5f);
            if (out_a == 0U) {
                continue;
            }
            if (state.alpha_test_ref > 0U && out_a <= state.alpha_test_ref) {
                continue;
            }

            uint8_t out_r = static_cast<uint8_t>(clamp01(cr) * 255.0f + 0.5f);
            uint8_t out_g = static_cast<uint8_t>(clamp01(cg) * 255.0f + 0.5f);
            uint8_t out_b = static_cast<uint8_t>(clamp01(cb) * 255.0f + 0.5f);

            if (state.blend == BlendMode::AlphaBlend && out_a < 255U) {
                const uint32_t dst = color_[idx];
                const float sa = static_cast<float>(out_a) / 255.0f;
                const float da = 1.0f - sa;
                out_r = static_cast<uint8_t>(
                    clamp01(cr * sa + (static_cast<float>(dst & 0xFFU) / 255.0f) * da) * 255.0f);
                out_g = static_cast<uint8_t>(
                    clamp01(cg * sa + (static_cast<float>((dst >> 8U) & 0xFFU) / 255.0f) * da) *
                    255.0f);
                out_b = static_cast<uint8_t>(
                    clamp01(cb * sa + (static_cast<float>((dst >> 16U) & 0xFFU) / 255.0f) * da) *
                    255.0f);
            } else if (state.blend == BlendMode::Additive) {
                const uint32_t dst = color_[idx];
                out_r = static_cast<uint8_t>(
                    std::min<uint32_t>(255U, out_r + static_cast<uint32_t>(dst & 0xFFU)));
                out_g = static_cast<uint8_t>(
                    std::min<uint32_t>(255U, out_g + static_cast<uint32_t>((dst >> 8U) & 0xFFU)));
                out_b = static_cast<uint8_t>(
                    std::min<uint32_t>(255U, out_b + static_cast<uint32_t>((dst >> 16U) & 0xFFU)));
            }

            color_[idx] = pack_rgba(out_r, out_g, out_b, out_a);
            if (state.depth_write) {
                depth_[idx] = z;
            }
        }
    }
}

}  // namespace dragon::gpu3d
