#include "psp/ge.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "psp/bus.h"

namespace dragon::psp {

namespace {

inline float ge_float24(uint32_t arg) noexcept {
    const uint32_t bits = (arg & 0x00FFFFFFU) << 8U;
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

inline void set_identity(std::array<float, 16>& m) noexcept {
    m.fill(0.0f);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Column-major 4x4 * vec4
inline void mul_mat4_vec4(const std::array<float, 16>& m, const float v[4], float out[4]) noexcept {
    out[0] = m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12] * v[3];
    out[1] = m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13] * v[3];
    out[2] = m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] * v[3];
    out[3] = m[3] * v[0] + m[7] * v[1] + m[11] * v[2] + m[15] * v[3];
}

inline uint32_t conv_565_to_abgr(uint16_t c) noexcept {
    const uint32_t r = ((c & 0x001FU) * 255U) / 31U;
    const uint32_t g = (((c >> 5U) & 0x003FU) * 255U) / 63U;
    const uint32_t b = (((c >> 11U) & 0x001FU) * 255U) / 31U;
    return 0xFF000000U | (b << 16U) | (g << 8U) | r;
}

inline uint32_t conv_5551_to_abgr(uint16_t c) noexcept {
    const uint32_t r = ((c & 0x001FU) * 255U) / 31U;
    const uint32_t g = (((c >> 5U) & 0x001FU) * 255U) / 31U;
    const uint32_t b = (((c >> 10U) & 0x001FU) * 255U) / 31U;
    const uint32_t a = (c & 0x8000U) ? 255U : 0U;
    return (a << 24U) | (b << 16U) | (g << 8U) | r;
}

inline uint32_t conv_4444_to_abgr(uint16_t c) noexcept {
    const uint32_t r = (c & 0x000FU) * 17U;
    const uint32_t g = ((c >> 4U) & 0x000FU) * 17U;
    const uint32_t b = ((c >> 8U) & 0x000FU) * 17U;
    const uint32_t a = ((c >> 12U) & 0x000FU) * 17U;
    return (a << 24U) | (b << 16U) | (g << 8U) | r;
}

inline uint16_t conv_abgr_to_565(uint32_t c) noexcept {
    const uint32_t r = (c & 0xFFU) >> 3U;
    const uint32_t g = ((c >> 8U) & 0xFFU) >> 2U;
    const uint32_t b = ((c >> 16U) & 0xFFU) >> 3U;
    return static_cast<uint16_t>(r | (g << 5U) | (b << 11U));
}

inline uint16_t conv_abgr_to_5551(uint32_t c) noexcept {
    const uint32_t r = (c & 0xFFU) >> 3U;
    const uint32_t g = ((c >> 8U) & 0xFFU) >> 3U;
    const uint32_t b = ((c >> 16U) & 0xFFU) >> 3U;
    const uint32_t a = ((c >> 24U) & 0x80U) ? 1U : 0U;
    return static_cast<uint16_t>(r | (g << 5U) | (b << 10U) | (a << 15U));
}

inline uint16_t conv_abgr_to_4444(uint32_t c) noexcept {
    const uint32_t r = (c & 0xFFU) >> 4U;
    const uint32_t g = ((c >> 8U) & 0xFFU) >> 4U;
    const uint32_t b = ((c >> 16U) & 0xFFU) >> 4U;
    const uint32_t a = ((c >> 24U) & 0xFFU) >> 4U;
    return static_cast<uint16_t>(r | (g << 4U) | (b << 8U) | (a << 12U));
}

inline uint32_t align_up(uint32_t v, uint32_t a) noexcept {
    return (v + (a - 1U)) & ~(a - 1U);
}

inline float read_f32(const PspBus& bus, uint32_t addr) noexcept {
    const uint32_t raw = bus.read32(addr);
    float f = 0.0f;
    std::memcpy(&f, &raw, sizeof(f));
    return f;
}

}  // namespace

PspGe::PspGe() : gpu3d_(kScreenWidth, kScreenHeight) {
    reset();
}

void PspGe::reset() {
    gpu3d_.resize(kScreenWidth, kScreenHeight);
    gpu3d_.set_viewport(
        {0, 0, static_cast<int32_t>(kScreenWidth), static_cast<int32_t>(kScreenHeight)});
    gpu3d_.clear(0xFF000000U, 1.0f);

    display_top_addr_ = 0x04000000U;
    display_buf_width_ = 512U;
    display_pixel_format_ = 3U;

    base_addr_ = 0;
    offset_addr_ = 0;
    vaddr_ = 0;
    iaddr_ = 0;
    vtype_ = 0;
    fb_ptr_ = 0x04000000U;
    fb_width_ = 512U;
    fb_pixel_format_ = 3U;
    zbuf_ptr_ = 0x04088000U;
    zbuf_width_ = 512U;

    clear_mode_ = false;
    clear_flags_ = 0;
    cull_enable_ = false;
    cull_dir_ = 0;
    texture_enable_ = false;
    alpha_blend_enable_ = false;
    alpha_test_enable_ = false;
    alpha_test_ref_ = 0;
    depth_test_enable_ = false;
    depth_func_ = 2U;
    depth_write_disable_ = false;
    blend_mode_ = 0;
    tex_func_ = 0;

    set_identity(world_mtx_);
    set_identity(view_mtx_);
    set_identity(proj_mtx_);
    world_idx_ = view_idx_ = proj_idx_ = 0;

    vp_xscale_ = 240.0f;
    vp_yscale_ = -136.0f;
    vp_zscale_ = 32767.0f;
    vp_xcenter_ = 2048.0f;
    vp_ycenter_ = 2048.0f;
    vp_zcenter_ = 32768.0f;
    offset_x_ = 1808.0f;
    offset_y_ = 1912.0f;
    tex_scale_u_ = 1.0f;
    tex_scale_v_ = 1.0f;
    tex_offset_u_ = 0.0f;
    tex_offset_v_ = 0.0f;

    tex_addr_ = 0;
    tex_buf_width_ = 64U;
    tex_width_ = 64U;
    tex_height_ = 64U;
    tex_format_ = 3U;
    tex_swizzled_ = false;
    tex_clamp_u_ = false;
    tex_clamp_v_ = false;
    clut_addr_ = 0;
    clut_format_ = 3U;
    clut_rgba_.fill(0xFFFFFFFFU);
    decoded_tex_.clear();

    for (auto& l : lists_)
        l = GeListState{};
    next_list_id_ = 1;
    commands_executed_ = 0;
    triangles_submitted_ = 0;
    has_ge_drawn_ = false;
}

void PspGe::set_display_framebuf(uint32_t top_addr, uint32_t buf_width, uint32_t pixel_format) {
    if (top_addr != 0U)
        display_top_addr_ = top_addr;
    if (buf_width != 0U)
        display_buf_width_ = buf_width;
    display_pixel_format_ = pixel_format & 3U;
}

int PspGe::enqueue_list(PspBus& bus, uint32_t list_addr, uint32_t stall_addr, int cb_id) {
    (void)cb_id;
    const int id = next_list_id_++;
    auto& slot = lists_[static_cast<size_t>(id) % lists_.size()];
    slot = GeListState{};
    slot.active = true;
    slot.finished = false;
    slot.pc = list_addr;
    slot.stall_addr = stall_addr;
    execute_list(bus, slot);
    return id;
}

int PspGe::update_stall_addr(PspBus& bus, int list_id, uint32_t stall_addr) {
    auto& slot = lists_[static_cast<size_t>(list_id) % lists_.size()];
    slot.stall_addr = stall_addr;
    if (slot.active && !slot.finished) {
        execute_list(bus, slot);
    }
    return 0;
}

int PspGe::list_sync(int list_id) const {
    (void)list_id;
    return 0;  // Completed
}

int PspGe::draw_sync() const {
    return 0;  // Completed
}

void PspGe::execute_list(PspBus& bus, GeListState& list) {
    constexpr uint32_t kMaxCommandsPerKick = 262144U;
    uint32_t count = 0;
    while (list.active && !list.finished && count < kMaxCommandsPerKick) {
        if (list.stall_addr != 0U &&
            PspBus::normalize_addr(list.pc) == PspBus::normalize_addr(list.stall_addr)) {
            break;
        }
        const uint32_t word = bus.read32(list.pc);
        list.pc += 4U;
        const uint8_t cmd = static_cast<uint8_t>((word >> 24U) & 0xFFU);
        const uint32_t arg = word & 0x00FFFFFFU;
        ++commands_executed_;
        ++count;
        execute_command(bus, list, cmd, arg);
    }
    if (has_ge_drawn_) {
        copy_gpu3d_to_vram(bus);
    }
}

void PspGe::execute_command(PspBus& bus, GeListState& list, uint8_t cmd, uint32_t arg) {
    switch (cmd) {
        case 0x00:
            break;  // NOP
        case 0x01:
            vaddr_ = (base_addr_ | arg) + offset_addr_;
            break;  // VADDR
        case 0x02:
            iaddr_ = (base_addr_ | arg) + offset_addr_;
            break;    // IADDR
        case 0x04: {  // PRIM
            const uint32_t prim_type = (arg >> 16U) & 0x07U;
            const uint32_t vcount = arg & 0xFFFFU;
            draw_primitive(bus, prim_type, vcount);
            break;
        }
        case 0x08:  // JUMP
            list.pc = ((base_addr_ | arg) + offset_addr_) & ~3U;
            break;
        case 0x0A:  // CALL
            if (list.call_depth < list.call_stack.size()) {
                list.call_stack[list.call_depth++] = list.pc;
                list.pc = ((base_addr_ | arg) + offset_addr_) & ~3U;
            }
            break;
        case 0x0B:  // RET
            if (list.call_depth > 0) {
                list.pc = list.call_stack[--list.call_depth];
            }
            break;
        case 0x0C:  // END
            list.finished = true;
            list.active = false;
            break;
        case 0x0F:  // FINISH
            break;
        case 0x10:  // BASE
            base_addr_ = (arg & 0x000F0000U) << 8U;
            break;
        case 0x12:
            vtype_ = arg;
            break;  // VERTEXTYPE
        case 0x13:
            offset_addr_ = arg << 8U;
            break;  // OFFSETADDR
        case 0x14:
            offset_addr_ = list.pc - 4U;
            break;  // ORIGIN

        case 0x1D:
            cull_enable_ = (arg & 1U) != 0U;
            break;
        case 0x1E:
            texture_enable_ = (arg & 1U) != 0U;
            break;
        case 0x21:
            alpha_blend_enable_ = (arg & 1U) != 0U;
            break;
        case 0x22:
            alpha_test_enable_ = (arg & 1U) != 0U;
            break;
        case 0x23:
            depth_test_enable_ = (arg & 1U) != 0U;
            break;

        // World 3x4 matrix (12 floats -> column-major 4x4 with row 3 = [0,0,0,1])
        case 0x3A:
            world_idx_ = arg & 0x0FU;
            break;
        case 0x3B:
            if (world_idx_ < 12U) {
                const uint32_t col = world_idx_ / 3U;
                const uint32_t row = world_idx_ % 3U;
                world_mtx_[col * 4U + row] = ge_float24(arg);
                world_mtx_[3] = world_mtx_[7] = world_mtx_[11] = 0.0f;
                world_mtx_[15] = 1.0f;
                ++world_idx_;
            }
            break;

        // View 3x4 matrix
        case 0x3C:
            view_idx_ = arg & 0x0FU;
            break;
        case 0x3D:
            if (view_idx_ < 12U) {
                const uint32_t col = view_idx_ / 3U;
                const uint32_t row = view_idx_ % 3U;
                view_mtx_[col * 4U + row] = ge_float24(arg);
                view_mtx_[3] = view_mtx_[7] = view_mtx_[11] = 0.0f;
                view_mtx_[15] = 1.0f;
                ++view_idx_;
            }
            break;

        // Projection 4x4 matrix
        case 0x3E:
            proj_idx_ = arg & 0x0FU;
            break;
        case 0x3F:
            if (proj_idx_ < 16U) {
                proj_mtx_[proj_idx_++] = ge_float24(arg);
            }
            break;

        // Viewport & offsets
        case 0x42:
            vp_xscale_ = ge_float24(arg);
            break;
        case 0x43:
            vp_yscale_ = ge_float24(arg);
            break;
        case 0x44:
            vp_zscale_ = ge_float24(arg);
            break;
        case 0x45:
            vp_xcenter_ = ge_float24(arg);
            break;
        case 0x46:
            vp_ycenter_ = ge_float24(arg);
            break;
        case 0x47:
            vp_zcenter_ = ge_float24(arg);
            break;
        case 0x48:
            tex_scale_u_ = ge_float24(arg);
            break;
        case 0x49:
            tex_scale_v_ = ge_float24(arg);
            break;
        case 0x4A:
            tex_offset_u_ = ge_float24(arg);
            break;
        case 0x4B:
            tex_offset_v_ = ge_float24(arg);
            break;
        case 0x4C:
            offset_x_ = static_cast<float>(arg & 0xFFFFU) / 16.0f;
            break;
        case 0x4D:
            offset_y_ = static_cast<float>(arg & 0xFFFFU) / 16.0f;
            break;

        case 0x9B:
            cull_dir_ = arg & 1U;
            break;
        case 0x9C:
            fb_ptr_ = (fb_ptr_ & 0xFF000000U) | (arg & 0x00FFFFFFU);
            break;
        case 0x9D:
            fb_width_ = arg & 0xFFFFU;
            fb_ptr_ = ((arg & 0x00FF0000U) << 8U) | (fb_ptr_ & 0x00FFFFFFU);
            if ((fb_ptr_ & 0xFF000000U) == 0U)
                fb_ptr_ |= 0x04000000U;
            break;
        case 0x9E:
            zbuf_ptr_ = (zbuf_ptr_ & 0xFF000000U) | (arg & 0x00FFFFFFU);
            break;
        case 0x9F:
            zbuf_width_ = arg & 0xFFFFU;
            zbuf_ptr_ = ((arg & 0x00FF0000U) << 8U) | (zbuf_ptr_ & 0x00FFFFFFU);
            break;

        // Texture 0
        case 0xA0:
            tex_addr_ = (tex_addr_ & 0xFF000000U) | (arg & 0x00FFFFFFU);
            break;
        case 0xA8:
            tex_buf_width_ = arg & 0xFFFFU;
            tex_addr_ = ((arg & 0x00FF0000U) << 8U) | (tex_addr_ & 0x00FFFFFFU);
            break;
        case 0xB0:
            clut_addr_ = (clut_addr_ & 0xFF000000U) | (arg & 0x00FFFFFFU);
            break;
        case 0xB1:
            clut_addr_ = ((arg & 0x00FF0000U) << 8U) | (clut_addr_ & 0x00FFFFFFU);
            break;
        case 0xB8:
            tex_width_ = 1U << std::min<uint32_t>(arg & 0x0FU, 9U);
            tex_height_ = 1U << std::min<uint32_t>((arg >> 8U) & 0x0FU, 9U);
            break;
        case 0xC2:
            tex_swizzled_ = (arg & 1U) != 0U;
            break;
        case 0xC3:
            tex_format_ = arg & 0x0FU;
            break;
        case 0xC4: {  // LOADCLUT
            const uint32_t blocks = arg & 0x3FU;
            const uint32_t byte_count = blocks * 32U;
            const uint32_t bpp = (clut_format_ == 3U) ? 4U : 2U;
            const uint32_t entries = std::min<uint32_t>(byte_count / bpp, 256U);
            for (uint32_t i = 0; i < entries; ++i) {
                if (clut_format_ == 3U) {
                    clut_rgba_[i] = bus.read32(clut_addr_ + i * 4U);
                } else {
                    const uint16_t raw = bus.read16(clut_addr_ + i * 2U);
                    if (clut_format_ == 0U)
                        clut_rgba_[i] = conv_565_to_abgr(raw);
                    else if (clut_format_ == 1U)
                        clut_rgba_[i] = conv_5551_to_abgr(raw);
                    else
                        clut_rgba_[i] = conv_4444_to_abgr(raw);
                }
            }
            break;
        }
        case 0xC5:
            clut_format_ = arg & 3U;
            break;
        case 0xC7:
            tex_clamp_u_ = (arg & 1U) != 0U;
            tex_clamp_v_ = ((arg >> 8U) & 1U) != 0U;
            break;
        case 0xC9:
            tex_func_ = arg & 0x07U;
            break;
        case 0xD2:
            fb_pixel_format_ = arg & 3U;
            break;
        case 0xD3:
            clear_mode_ = (arg & 1U) != 0U;
            clear_flags_ = static_cast<uint8_t>((arg >> 8U) & 0x07U);
            break;
        case 0xDB:
            alpha_test_ref_ = static_cast<uint8_t>((arg >> 8U) & 0xFFU);
            break;
        case 0xDE:
            depth_func_ = arg & 0x07U;
            break;
        case 0xDF:
            blend_mode_ = arg;
            break;
        case 0xE7:
            depth_write_disable_ = (arg & 1U) != 0U;
            break;

        // Block transfer (sceGuCopyImage)
        case 0xEA:
            execute_block_transfer(bus, arg);
            break;
        case 0xEB:
            xfer_src_ = (xfer_src_ & 0xFF000000U) | (arg & 0x00FFFFFFU);
            break;
        case 0xEC:
            xfer_src_w_ = arg & 0xFFFFU;
            xfer_src_ = ((arg & 0x00FF0000U) << 8U) | (xfer_src_ & 0x00FFFFFFU);
            break;
        case 0xED:
            xfer_dst_ = (xfer_dst_ & 0xFF000000U) | (arg & 0x00FFFFFFU);
            break;
        case 0xEE:
            xfer_dst_w_ = arg & 0xFFFFU;
            xfer_dst_ = ((arg & 0x00FF0000U) << 8U) | (xfer_dst_ & 0x00FFFFFFU);
            break;
        case 0xB2:
            xfer_src_pos_ = arg;
            break;
        case 0xB3:
            xfer_dst_pos_ = arg;
            break;
        case 0xB4:
            xfer_size_ = arg;
            break;

        default:
            break;
    }
}

void PspGe::decode_texture(const PspBus& bus, dragon::gpu3d::Texture2D& out_tex) {
    const uint32_t w = std::clamp(tex_width_, 1U, 512U);
    const uint32_t h = std::clamp(tex_height_, 1U, 512U);
    const uint32_t stride = std::max(tex_buf_width_, w);
    decoded_tex_.resize(static_cast<size_t>(w) * h);

    auto read_tex_byte = [&](uint32_t byte_x, uint32_t y, uint32_t row_bytes) -> uint8_t {
        if (!tex_swizzled_) {
            return bus.read8(tex_addr_ + y * row_bytes + byte_x);
        }
        // PSP 16x8 byte swizzle
        const uint32_t block_x = byte_x >> 4U;
        const uint32_t block_y = y >> 3U;
        const uint32_t blocks_per_row = std::max(1U, row_bytes >> 4U);
        const uint32_t block_addr = (block_y * blocks_per_row + block_x) * 128U;
        const uint32_t in_block = ((y & 7U) << 4U) | (byte_x & 15U);
        return bus.read8(tex_addr_ + block_addr + in_block);
    };

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t rgba = 0xFFFFFFFFU;
            switch (tex_format_) {
                case 0: {  // RGB565
                    const uint32_t rb = stride * 2U;
                    const uint16_t lo = read_tex_byte(x * 2U, y, rb);
                    const uint16_t hi = read_tex_byte(x * 2U + 1U, y, rb);
                    rgba = conv_565_to_abgr(static_cast<uint16_t>(lo | (hi << 8U)));
                    break;
                }
                case 1: {  // RGBA5551
                    const uint32_t rb = stride * 2U;
                    const uint16_t lo = read_tex_byte(x * 2U, y, rb);
                    const uint16_t hi = read_tex_byte(x * 2U + 1U, y, rb);
                    rgba = conv_5551_to_abgr(static_cast<uint16_t>(lo | (hi << 8U)));
                    break;
                }
                case 2: {  // RGBA4444
                    const uint32_t rb = stride * 2U;
                    const uint16_t lo = read_tex_byte(x * 2U, y, rb);
                    const uint16_t hi = read_tex_byte(x * 2U + 1U, y, rb);
                    rgba = conv_4444_to_abgr(static_cast<uint16_t>(lo | (hi << 8U)));
                    break;
                }
                case 3: {  // RGBA8888
                    const uint32_t rb = stride * 4U;
                    const uint32_t b0 = read_tex_byte(x * 4U + 0U, y, rb);
                    const uint32_t b1 = read_tex_byte(x * 4U + 1U, y, rb);
                    const uint32_t b2 = read_tex_byte(x * 4U + 2U, y, rb);
                    const uint32_t b3 = read_tex_byte(x * 4U + 3U, y, rb);
                    rgba = b0 | (b1 << 8U) | (b2 << 16U) | (b3 << 24U);
                    break;
                }
                case 4: {  // T4 (4-bit indexed)
                    const uint32_t rb = std::max(1U, stride / 2U);
                    const uint8_t b = read_tex_byte(x >> 1U, y, rb);
                    const uint8_t idx = (x & 1U) ? ((b >> 4U) & 0x0FU) : (b & 0x0FU);
                    rgba = clut_rgba_[idx];
                    break;
                }
                case 5: {  // T8 (8-bit indexed)
                    const uint8_t idx = read_tex_byte(x, y, stride);
                    rgba = clut_rgba_[idx];
                    break;
                }
                default:
                    break;
            }
            decoded_tex_[static_cast<size_t>(y) * w + x] = rgba;
        }
    }

    out_tex.rgba = decoded_tex_.data();
    out_tex.width = w;
    out_tex.height = h;
    out_tex.repeat_u = !tex_clamp_u_;
    out_tex.repeat_v = !tex_clamp_v_;
}

void PspGe::draw_primitive(PspBus& bus, uint32_t prim_type, uint32_t vertex_count) {
    if (vertex_count == 0U)
        return;

    const uint32_t tc_fmt = vtype_ & 0x03U;
    const uint32_t col_fmt = (vtype_ >> 2U) & 0x07U;
    const uint32_t nrm_fmt = (vtype_ >> 5U) & 0x03U;
    const uint32_t pos_fmt = (vtype_ >> 7U) & 0x03U;
    const uint32_t wgt_fmt = (vtype_ >> 9U) & 0x03U;
    const uint32_t idx_fmt = (vtype_ >> 11U) & 0x03U;
    const uint32_t wgt_cnt = ((vtype_ >> 14U) & 0x07U) + 1U;
    const bool through = ((vtype_ >> 23U) & 1U) != 0U;

    auto comp_size = [](uint32_t f) -> uint32_t {
        if (f == 1U)
            return 1U;
        if (f == 2U)
            return 2U;
        if (f == 3U)
            return 4U;
        return 0U;
    };

    // Calculate vertex stride and field offsets according to PSP GE alignment rules
    uint32_t off = 0;
    uint32_t max_align = 1;
    auto place = [&](uint32_t sz, uint32_t cnt) -> uint32_t {
        if (sz == 0)
            return off;
        max_align = std::max(max_align, sz);
        off = align_up(off, sz);
        const uint32_t start = off;
        off += sz * cnt;
        return start;
    };

    const uint32_t wgt_off = (wgt_fmt != 0) ? place(comp_size(wgt_fmt), wgt_cnt) : 0U;
    (void)wgt_off;
    const uint32_t tc_off = (tc_fmt != 0) ? place(comp_size(tc_fmt), 2U) : 0U;
    uint32_t col_sz = 0;
    if (col_fmt >= 4U && col_fmt <= 6U)
        col_sz = 2U;
    else if (col_fmt == 7U)
        col_sz = 4U;
    const uint32_t col_off = (col_sz != 0) ? place(col_sz, 1U) : 0U;
    const uint32_t nrm_off = (nrm_fmt != 0) ? place(comp_size(nrm_fmt), 3U) : 0U;
    (void)nrm_off;
    const uint32_t pos_off = (pos_fmt != 0) ? place(comp_size(pos_fmt), 3U) : 0U;
    const uint32_t vstride = align_up(off, max_align);
    if (vstride == 0U || pos_fmt == 0U)
        return;

    auto decode_vertex = [&](uint32_t vidx) -> dragon::gpu3d::Vertex3D {
        const uint32_t base = vaddr_ + vidx * vstride;
        dragon::gpu3d::Vertex3D v{};

        // Texture coordinates
        if (tc_fmt == 1U) {
            v.u = static_cast<float>(bus.read8(base + tc_off)) /
                  (through ? static_cast<float>(tex_width_) : 128.0f);
            v.v = static_cast<float>(bus.read8(base + tc_off + 1U)) /
                  (through ? static_cast<float>(tex_height_) : 128.0f);
        } else if (tc_fmt == 2U) {
            v.u = static_cast<float>(bus.read16(base + tc_off)) /
                  (through ? static_cast<float>(tex_width_) : 32768.0f);
            v.v = static_cast<float>(bus.read16(base + tc_off + 2U)) /
                  (through ? static_cast<float>(tex_height_) : 32768.0f);
        } else if (tc_fmt == 3U) {
            v.u = read_f32(bus, base + tc_off);
            v.v = read_f32(bus, base + tc_off + 4U);
            if (through) {
                if (tex_width_ > 0)
                    v.u /= static_cast<float>(tex_width_);
                if (tex_height_ > 0)
                    v.v /= static_cast<float>(tex_height_);
            }
        }
        if (!through) {
            v.u = v.u * tex_scale_u_ + tex_offset_u_;
            v.v = v.v * tex_scale_v_ + tex_offset_v_;
        }

        // Color
        uint32_t abgr = 0xFFFFFFFFU;
        if (col_fmt == 4U)
            abgr = conv_565_to_abgr(bus.read16(base + col_off));
        else if (col_fmt == 5U)
            abgr = conv_5551_to_abgr(bus.read16(base + col_off));
        else if (col_fmt == 6U)
            abgr = conv_4444_to_abgr(bus.read16(base + col_off));
        else if (col_fmt == 7U)
            abgr = bus.read32(base + col_off);
        v.r = static_cast<float>(abgr & 0xFFU) / 255.0f;
        v.g = static_cast<float>((abgr >> 8U) & 0xFFU) / 255.0f;
        v.b = static_cast<float>((abgr >> 16U) & 0xFFU) / 255.0f;
        v.a = static_cast<float>((abgr >> 24U) & 0xFFU) / 255.0f;

        // Position
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        if (pos_fmt == 1U) {
            px = static_cast<float>(static_cast<int8_t>(bus.read8(base + pos_off))) /
                 (through ? 1.0f : 128.0f);
            py = static_cast<float>(static_cast<int8_t>(bus.read8(base + pos_off + 1U))) /
                 (through ? 1.0f : 128.0f);
            pz = static_cast<float>(static_cast<int8_t>(bus.read8(base + pos_off + 2U))) /
                 (through ? 1.0f : 128.0f);
        } else if (pos_fmt == 2U) {
            px = static_cast<float>(static_cast<int16_t>(bus.read16(base + pos_off))) /
                 (through ? 1.0f : 32768.0f);
            py = static_cast<float>(static_cast<int16_t>(bus.read16(base + pos_off + 2U))) /
                 (through ? 1.0f : 32768.0f);
            pz = static_cast<float>(static_cast<int16_t>(bus.read16(base + pos_off + 4U))) /
                 (through ? 1.0f : 32768.0f);
        } else {
            px = read_f32(bus, base + pos_off);
            py = read_f32(bus, base + pos_off + 4U);
            pz = read_f32(bus, base + pos_off + 8U);
        }

        if (through) {
            // Convert screen-space (0..480, 0..272) directly to NDC (-1..1, 1..-1)
            v.x = (px / (static_cast<float>(kScreenWidth) * 0.5f)) - 1.0f;
            v.y = 1.0f - (py / (static_cast<float>(kScreenHeight) * 0.5f));
            v.z = pz / 65535.0f;
            v.w = 1.0f;
        } else {
            float p0[4] = {px, py, pz, 1.0f};
            float p1[4], p2[4], p3[4];
            mul_mat4_vec4(world_mtx_, p0, p1);
            mul_mat4_vec4(view_mtx_, p1, p2);
            mul_mat4_vec4(proj_mtx_, p2, p3);
            v.x = p3[0];
            v.y = p3[1];
            v.z = p3[2];
            v.w = (std::fabs(p3[3]) < 1e-6f) ? 1.0f : p3[3];
        }
        return v;
    };

    std::vector<dragon::gpu3d::Vertex3D> verts;
    verts.reserve(vertex_count);
    for (uint32_t i = 0; i < vertex_count; ++i) {
        uint32_t idx = i;
        if (idx_fmt == 1U)
            idx = bus.read8(iaddr_ + i);
        else if (idx_fmt == 2U)
            idx = bus.read16(iaddr_ + i * 2U);
        verts.push_back(decode_vertex(idx));
    }
    if (idx_fmt == 0U)
        vaddr_ += vertex_count * vstride;
    else
        iaddr_ += vertex_count * ((idx_fmt == 1U) ? 1U : 2U);

    // Clear mode fast-path (GU_CLEAR_COLOR_BUFFER_BIT / GU_CLEAR_DEPTH_BUFFER_BIT)
    if (clear_mode_) {
        if (!verts.empty()) {
            const auto& c = verts.back();
            const uint32_t r = static_cast<uint32_t>(std::clamp(c.r, 0.0f, 1.0f) * 255.0f);
            const uint32_t g = static_cast<uint32_t>(std::clamp(c.g, 0.0f, 1.0f) * 255.0f);
            const uint32_t b = static_cast<uint32_t>(std::clamp(c.b, 0.0f, 1.0f) * 255.0f);
            const uint32_t a = static_cast<uint32_t>(std::clamp(c.a, 0.0f, 1.0f) * 255.0f);
            const uint32_t abgr = (a << 24U) | (b << 16U) | (g << 8U) | r;
            gpu3d_.clear(abgr, 1.0f);
            has_ge_drawn_ = true;
        }
        return;
    }

    dragon::gpu3d::DrawState state{};
    if (cull_enable_ && !through) {
        state.cull =
            (cull_dir_ == 0U) ? dragon::gpu3d::CullMode::Back : dragon::gpu3d::CullMode::Front;
    }
    state.depth_write = !depth_write_disable_;
    if (!depth_test_enable_) {
        state.depth_func = dragon::gpu3d::DepthFunc::Always;
    } else if (depth_func_ == 2U) {
        state.depth_func = dragon::gpu3d::DepthFunc::Equal;
    } else if (depth_func_ == 5U) {
        state.depth_func = dragon::gpu3d::DepthFunc::LessEqual;
    } else {
        state.depth_func = dragon::gpu3d::DepthFunc::Less;
    }
    if (alpha_blend_enable_) {
        state.blend = dragon::gpu3d::BlendMode::AlphaBlend;
    }
    if (alpha_test_enable_) {
        state.alpha_test_ref = alpha_test_ref_;
    }

    dragon::gpu3d::Texture2D tex{};
    const dragon::gpu3d::Texture2D* tex_ptr = nullptr;
    if (texture_enable_ && tex_addr_ != 0U) {
        decode_texture(bus, tex);
        tex_ptr = &tex;
        state.textured = true;
        state.modulate = (tex_func_ != 1U);  // 1 = GU_TFX_DECAL / REPLACE
    }

    auto submit_tri = [&](const dragon::gpu3d::Vertex3D& a, const dragon::gpu3d::Vertex3D& b,
                          const dragon::gpu3d::Vertex3D& c) {
        gpu3d_.draw_triangle(a, b, c, state, tex_ptr);
        ++triangles_submitted_;
        has_ge_drawn_ = true;
    };

    switch (prim_type) {
        case 3:  // GU_TRIANGLES
            for (size_t i = 0; i + 2U < verts.size(); i += 3U) {
                submit_tri(verts[i], verts[i + 1U], verts[i + 2U]);
            }
            break;
        case 4:  // GU_TRIANGLE_STRIP
            for (size_t i = 0; i + 2U < verts.size(); ++i) {
                if ((i & 1U) == 0U)
                    submit_tri(verts[i], verts[i + 1U], verts[i + 2U]);
                else
                    submit_tri(verts[i + 1U], verts[i], verts[i + 2U]);
            }
            break;
        case 5:  // GU_TRIANGLE_FAN
            for (size_t i = 1; i + 1U < verts.size(); ++i) {
                submit_tri(verts[0], verts[i], verts[i + 1U]);
            }
            break;
        case 6:  // GU_SPRITES (axis-aligned rectangles defined by pairs of diagonal corners)
            for (size_t i = 0; i + 1U < verts.size(); i += 2U) {
                const auto& a = verts[i];
                const auto& b = verts[i + 1U];
                dragon::gpu3d::Vertex3D tr = b;
                tr.y = a.y;
                tr.v = a.v;
                dragon::gpu3d::Vertex3D bl = a;
                bl.y = b.y;
                bl.v = b.v;
                submit_tri(a, tr, b);
                submit_tri(a, b, bl);
            }
            break;
        default:
            break;
    }
}

void PspGe::copy_gpu3d_to_vram(PspBus& bus) const {
    const uint32_t* src = gpu3d_.color_buffer();
    if (!src || fb_width_ == 0U)
        return;
    const uint32_t stride = fb_width_;
    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        const uint32_t* row = src + static_cast<size_t>(y) * kScreenWidth;
        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            const uint32_t abgr = row[x];
            if (fb_pixel_format_ == 3U) {
                bus.write32(fb_ptr_ + (y * stride + x) * 4U, abgr);
            } else if (fb_pixel_format_ == 0U) {
                bus.write16(fb_ptr_ + (y * stride + x) * 2U, conv_abgr_to_565(abgr));
            } else if (fb_pixel_format_ == 1U) {
                bus.write16(fb_ptr_ + (y * stride + x) * 2U, conv_abgr_to_5551(abgr));
            } else {
                bus.write16(fb_ptr_ + (y * stride + x) * 2U, conv_abgr_to_4444(abgr));
            }
        }
    }
}

void PspGe::execute_block_transfer(PspBus& bus, uint32_t arg) {
    const uint32_t bpp = ((arg & 1U) != 0U) ? 4U : 2U;
    const uint32_t sx = xfer_src_pos_ & 0x3FFU;
    const uint32_t sy = (xfer_src_pos_ >> 10U) & 0x3FFU;
    const uint32_t dx = xfer_dst_pos_ & 0x3FFU;
    const uint32_t dy = (xfer_dst_pos_ >> 10U) & 0x3FFU;
    const uint32_t w = (xfer_size_ & 0x3FFU) + 1U;
    const uint32_t h = ((xfer_size_ >> 10U) & 0x3FFU) + 1U;

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t s_addr = xfer_src_ + ((sy + y) * xfer_src_w_ + (sx + x)) * bpp;
            const uint32_t d_addr = xfer_dst_ + ((dy + y) * xfer_dst_w_ + (dx + x)) * bpp;
            if (bpp == 4U)
                bus.write32(d_addr, bus.read32(s_addr));
            else
                bus.write16(d_addr, bus.read16(s_addr));
        }
    }
}

void PspGe::present_frame(const PspBus& bus, uint32_t* out_abgr480x272) const {
    if (!out_abgr480x272)
        return;
    const uint32_t stride = (display_buf_width_ != 0U) ? display_buf_width_ : 512U;
    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        uint32_t* dst = out_abgr480x272 + static_cast<size_t>(y) * kScreenWidth;
        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            uint32_t abgr = 0xFF000000U;
            if (display_pixel_format_ == 3U) {
                abgr = bus.read32(display_top_addr_ + (y * stride + x) * 4U) | 0xFF000000U;
            } else if (display_pixel_format_ == 0U) {
                abgr = conv_565_to_abgr(bus.read16(display_top_addr_ + (y * stride + x) * 2U));
            } else if (display_pixel_format_ == 1U) {
                abgr = conv_5551_to_abgr(bus.read16(display_top_addr_ + (y * stride + x) * 2U)) |
                       0xFF000000U;
            } else {
                abgr = conv_4444_to_abgr(bus.read16(display_top_addr_ + (y * stride + x) * 2U)) |
                       0xFF000000U;
            }
            dst[x] = abgr;
        }
    }
}

}  // namespace dragon::psp
