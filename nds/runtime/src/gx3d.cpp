#include "nds/gx3d.h"

#include <algorithm>
#include <cstring>

#include "nds/bus.h"

namespace dragon::nds {

namespace {

inline int32_t sext10(uint32_t v) noexcept {
    return (v & 0x200U) ? static_cast<int32_t>(v | 0xFFFFFC00U) : static_cast<int32_t>(v & 0x3FFU);
}

inline uint32_t rgb555_to_rgba(uint16_t c, uint8_t alpha = 255U) noexcept {
    const uint8_t r = static_cast<uint8_t>(((c & 0x1FU) << 3U) | ((c & 0x1FU) >> 2U));
    const uint8_t g =
        static_cast<uint8_t>((((c >> 5U) & 0x1FU) << 3U) | (((c >> 5U) & 0x1FU) >> 2U));
    const uint8_t b =
        static_cast<uint8_t>((((c >> 10U) & 0x1FU) << 3U) | (((c >> 10U) & 0x1FU) >> 2U));
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8U) |
           (static_cast<uint32_t>(b) << 16U) | (static_cast<uint32_t>(alpha) << 24U);
}

}  // namespace

Gx3d::Gx3d() {
    build_list_.reserve(4096);
    render_list_.reserve(4096);
    decoded_tex_.reserve(256 * 256);
    reset();
}

Gx3d::Mat4 Gx3d::identity_mat() noexcept {
    Mat4 m{};
    m.m[0] = 0x1000;
    m.m[5] = 0x1000;
    m.m[10] = 0x1000;
    m.m[15] = 0x1000;
    return m;
}

Gx3d::Mat4 Gx3d::mul_mat(const Mat4& a, const Mat4& b) noexcept {
    Mat4 r{};
    // Row-major NDS convention: result[row*4 + col] = sum_k a[row*4+k] * b[k*4+col] >> 12
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            int64_t acc = 0;
            for (int k = 0; k < 4; ++k) {
                acc += static_cast<int64_t>(a.m[row * 4 + k]) * b.m[k * 4 + col];
            }
            r.m[row * 4 + col] = static_cast<int32_t>(acc >> 12);
        }
    }
    return r;
}

int Gx3d::cmd_param_count(uint8_t cmd) noexcept {
    switch (cmd) {
        case 0x00U:
        case 0x11U:
        case 0x15U:
        case 0x41U:
            return 0;
        case 0x10U:
        case 0x12U:
        case 0x13U:
        case 0x14U:
        case 0x20U:
        case 0x21U:
        case 0x22U:
        case 0x24U:
        case 0x25U:
        case 0x26U:
        case 0x27U:
        case 0x28U:
        case 0x29U:
        case 0x2AU:
        case 0x2BU:
        case 0x30U:
        case 0x31U:
        case 0x32U:
        case 0x33U:
        case 0x40U:
        case 0x50U:
        case 0x60U:
        case 0x72U:
            return 1;
        case 0x23U:
        case 0x71U:
            return 2;
        case 0x1BU:
        case 0x1CU:
        case 0x70U:
            return 3;
        case 0x17U:
        case 0x19U:
            return 12;
        case 0x1AU:
            return 9;
        case 0x16U:
        case 0x18U:
            return 16;
        case 0x34U:
            return 32;
        default:
            return 0;
    }
}

void Gx3d::reset() noexcept {
    disp3dcnt_ = 0;
    gxfifo_irq_mode_ = 0;
    clear_color_ = 0;
    clear_depth_ = 0x7FFFU;
    mtx_mode_ = 0;
    proj_mtx_ = identity_mat();
    pos_mtx_ = identity_mat();
    dir_mtx_ = identity_mat();
    tex_mtx_ = identity_mat();
    clip_mtx_ = identity_mat();
    proj_stack_.fill(identity_mat());
    pos_stack_.fill(identity_mat());
    dir_stack_.fill(identity_mat());
    tex_stack_.fill(identity_mat());
    clip_dirty_ = false;
    proj_sp_ = 0;
    pos_sp_ = 0;
    tex_sp_ = 0;
    packed_cmds_ = 0;
    cur_cmd_ = 0;
    params_left_ = 0;
    param_count_ = 0;
    in_begin_ = false;
    prim_type_ = 0;
    vtx_x_ = vtx_y_ = vtx_z_ = 0;
    cur_r_ = cur_g_ = cur_b_ = 1.0f;
    tex_s_ = tex_t_ = 0;
    poly_attr_ = 0x001F00C0U;
    active_poly_attr_ = poly_attr_;
    tex_param_ = 0;
    pltt_base_ = 0;
    viewport_ = dragon::gpu3d::Viewport{0, 0, 256, 192};
    strip_count_ = 0;
    strip_odd_ = false;
    build_list_.clear();
    render_list_.clear();
    swap_pending_ = false;
    w_buffer_ = false;
    gpu_.clear(0x00000000U, 1.0f);
}

uint32_t Gx3d::read32(uint32_t offset) const noexcept {
    if (offset == 0x600U) {
        // GXSTAT: FIFO empty (bit 26), less than half full (bit 25), not busy, IRQ mode (bits
        // 31:30)
        uint32_t st = (1U << 26) | (1U << 25) | (static_cast<uint32_t>(gxfifo_irq_mode_) << 30U);
        st |= (static_cast<uint32_t>(pos_sp_ & 0x1F) << 8U);
        st |= (static_cast<uint32_t>(proj_sp_ & 1) << 13U);
        return st;
    }
    if (offset == 0x604U) {
        return static_cast<uint32_t>(build_list_.size()) |
               (static_cast<uint32_t>(build_list_.size() * 3U) << 16U);
    }
    if (offset >= 0x640U && offset < 0x680U) {
        // CLIPMTX_RESULT
        update_clip_matrix();
        const size_t idx = (offset - 0x640U) >> 2U;
        return static_cast<uint32_t>(clip_mtx_.m[idx & 15U]);
    }
    if (offset >= 0x680U && offset < 0x6A4U) {
        // VECMTX_RESULT (3x3 from dir_mtx_)
        const size_t idx = (offset - 0x680U) >> 2U;
        const size_t r = idx / 3U;
        const size_t c = idx % 3U;
        return static_cast<uint32_t>(dir_mtx_.m[r * 4U + c]);
    }
    return 0U;
}

void Gx3d::write32(uint32_t offset, uint32_t val) noexcept {
    if (offset == 0x600U) {
        gxfifo_irq_mode_ = static_cast<uint8_t>((val >> 30U) & 3U);
        return;
    }
    if (offset == 0x350U) {
        clear_color_ = val;
        return;
    }
    if (offset == 0x354U) {
        clear_depth_ = static_cast<uint16_t>(val & 0x7FFFU);
        return;
    }
    if (offset >= 0x400U && offset < 0x440U) {
        // GXFIFO packed command port
        push_fifo_word(val);
        return;
    }
    if (offset >= 0x440U && offset < 0x600U) {
        // Direct command port: cmd = (offset - 0x400) >> 2
        const uint8_t cmd = static_cast<uint8_t>((offset - 0x400U) >> 2U);
        const int need = cmd_param_count(cmd);
        if (need == 0) {
            execute_command(cmd, nullptr, 0);
        } else {
            if (cur_cmd_ != cmd || params_left_ == 0) {
                cur_cmd_ = cmd;
                param_count_ = 0;
                params_left_ = need;
            }
            if (param_count_ < 32) {
                param_buf_[param_count_++] = val;
            }
            --params_left_;
            if (params_left_ <= 0) {
                execute_command(cur_cmd_, param_buf_.data(), param_count_);
                cur_cmd_ = 0;
                param_count_ = 0;
                params_left_ = 0;
            }
        }
    }
}

void Gx3d::push_fifo_word(uint32_t word) noexcept {
    if (params_left_ > 0) {
        if (param_count_ < 32) {
            param_buf_[param_count_++] = word;
        }
        --params_left_;
        if (params_left_ == 0) {
            execute_command(cur_cmd_, param_buf_.data(), param_count_);
            param_count_ = 0;
            // Drain remaining 0-param commands in packed_cmds_
            while (packed_cmds_ != 0U) {
                cur_cmd_ = static_cast<uint8_t>(packed_cmds_ & 0xFFU);
                packed_cmds_ >>= 8U;
                if (cur_cmd_ == 0U)
                    continue;
                params_left_ = cmd_param_count(cur_cmd_);
                if (params_left_ == 0) {
                    execute_command(cur_cmd_, nullptr, 0);
                } else {
                    break;
                }
            }
        }
        return;
    }

    packed_cmds_ = word;
    while (packed_cmds_ != 0U) {
        cur_cmd_ = static_cast<uint8_t>(packed_cmds_ & 0xFFU);
        packed_cmds_ >>= 8U;
        if (cur_cmd_ == 0U)
            continue;
        params_left_ = cmd_param_count(cur_cmd_);
        param_count_ = 0;
        if (params_left_ == 0) {
            execute_command(cur_cmd_, nullptr, 0);
        } else {
            break;
        }
    }
}

void Gx3d::update_clip_matrix() const noexcept {
    if (clip_dirty_) {
        clip_mtx_ = mul_mat(pos_mtx_, proj_mtx_);
        clip_dirty_ = false;
    }
}

void Gx3d::execute_command(uint8_t cmd, const uint32_t* p, int count) noexcept {
    (void)count;
    auto apply_mtx = [&](const Mat4& m, bool is_load) {
        switch (mtx_mode_) {
            case 0:
                proj_mtx_ = is_load ? m : mul_mat(m, proj_mtx_);
                clip_dirty_ = true;
                break;
            case 1:
                pos_mtx_ = is_load ? m : mul_mat(m, pos_mtx_);
                clip_dirty_ = true;
                break;
            case 2:
                pos_mtx_ = is_load ? m : mul_mat(m, pos_mtx_);
                dir_mtx_ = is_load ? m : mul_mat(m, dir_mtx_);
                clip_dirty_ = true;
                break;
            case 3:
                tex_mtx_ = is_load ? m : mul_mat(m, tex_mtx_);
                break;
            default:
                break;
        }
    };

    switch (cmd) {
        case 0x10U:  // MTX_MODE
            mtx_mode_ = static_cast<uint8_t>(p[0] & 3U);
            break;
        case 0x11U:  // MTX_PUSH
            if (mtx_mode_ == 0) {
                proj_stack_[0] = proj_mtx_;
                proj_sp_ = 1;
            } else if (mtx_mode_ == 1 || mtx_mode_ == 2) {
                pos_stack_[pos_sp_ & 31] = pos_mtx_;
                dir_stack_[pos_sp_ & 31] = dir_mtx_;
                pos_sp_ = (pos_sp_ + 1) & 31;
            } else if (mtx_mode_ == 3) {
                tex_stack_[0] = tex_mtx_;
                tex_sp_ = 1;
            }
            break;
        case 0x12U: {  // MTX_POP
            int32_t off = static_cast<int32_t>(p[0] & 0x3FU);
            if (off & 0x20)
                off -= 64;
            if (mtx_mode_ == 0) {
                proj_mtx_ = proj_stack_[0];
                proj_sp_ = 0;
                clip_dirty_ = true;
            } else if (mtx_mode_ == 1 || mtx_mode_ == 2) {
                pos_sp_ = (pos_sp_ - off) & 31;
                pos_mtx_ = pos_stack_[pos_sp_];
                dir_mtx_ = dir_stack_[pos_sp_];
                clip_dirty_ = true;
            } else if (mtx_mode_ == 3) {
                tex_mtx_ = tex_stack_[0];
                tex_sp_ = 0;
            }
            break;
        }
        case 0x13U: {  // MTX_STORE
            const int idx = static_cast<int>(p[0] & 31U);
            if (mtx_mode_ == 0)
                proj_stack_[0] = proj_mtx_;
            else if (mtx_mode_ == 1 || mtx_mode_ == 2) {
                pos_stack_[idx] = pos_mtx_;
                dir_stack_[idx] = dir_mtx_;
            } else if (mtx_mode_ == 3)
                tex_stack_[0] = tex_mtx_;
            break;
        }
        case 0x14U: {  // MTX_RESTORE
            const int idx = static_cast<int>(p[0] & 31U);
            if (mtx_mode_ == 0) {
                proj_mtx_ = proj_stack_[0];
                clip_dirty_ = true;
            } else if (mtx_mode_ == 1 || mtx_mode_ == 2) {
                pos_mtx_ = pos_stack_[idx];
                dir_mtx_ = dir_stack_[idx];
                clip_dirty_ = true;
            } else if (mtx_mode_ == 3) {
                tex_mtx_ = tex_stack_[0];
            }
            break;
        }
        case 0x15U:  // MTX_IDENTITY
            apply_mtx(identity_mat(), true);
            break;
        case 0x16U: {  // MTX_LOAD_4x4
            Mat4 m{};
            for (int i = 0; i < 16; ++i)
                m.m[i] = static_cast<int32_t>(p[i]);
            apply_mtx(m, true);
            break;
        }
        case 0x17U: {  // MTX_LOAD_4x3
            Mat4 m = identity_mat();
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 3; ++c) {
                    m.m[r * 4 + c] = static_cast<int32_t>(p[r * 3 + c]);
                }
            }
            apply_mtx(m, true);
            break;
        }
        case 0x18U: {  // MTX_MULT_4x4
            Mat4 m{};
            for (int i = 0; i < 16; ++i)
                m.m[i] = static_cast<int32_t>(p[i]);
            apply_mtx(m, false);
            break;
        }
        case 0x19U: {  // MTX_MULT_4x3
            Mat4 m = identity_mat();
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 3; ++c) {
                    m.m[r * 4 + c] = static_cast<int32_t>(p[r * 3 + c]);
                }
            }
            apply_mtx(m, false);
            break;
        }
        case 0x1AU: {  // MTX_MULT_3x3
            Mat4 m = identity_mat();
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    m.m[r * 4 + c] = static_cast<int32_t>(p[r * 3 + c]);
                }
            }
            apply_mtx(m, false);
            break;
        }
        case 0x1BU: {  // MTX_SCALE
            Mat4 m = identity_mat();
            m.m[0] = static_cast<int32_t>(p[0]);
            m.m[5] = static_cast<int32_t>(p[1]);
            m.m[10] = static_cast<int32_t>(p[2]);
            if (mtx_mode_ == 2) {
                pos_mtx_ = mul_mat(m, pos_mtx_);
                clip_dirty_ = true;
            } else {
                apply_mtx(m, false);
            }
            break;
        }
        case 0x1CU: {  // MTX_TRANS
            Mat4 m = identity_mat();
            m.m[12] = static_cast<int32_t>(p[0]);
            m.m[13] = static_cast<int32_t>(p[1]);
            m.m[14] = static_cast<int32_t>(p[2]);
            apply_mtx(m, false);
            break;
        }
        case 0x20U: {  // COLOR
            const uint16_t c = static_cast<uint16_t>(p[0] & 0x7FFFU);
            cur_r_ = static_cast<float>(c & 0x1FU) / 31.0f;
            cur_g_ = static_cast<float>((c >> 5U) & 0x1FU) / 31.0f;
            cur_b_ = static_cast<float>((c >> 10U) & 0x1FU) / 31.0f;
            break;
        }
        case 0x21U: {  // NORMAL
            // Transform normal (1.0.9) by 3x3 direction matrix (1.19.12)
            const int64_t rnx = sext10(p[0] & 0x3FFU);
            const int64_t rny = sext10((p[0] >> 10U) & 0x3FFU);
            const int64_t rnz = sext10((p[0] >> 20U) & 0x3FFU);
            const auto& dm = dir_mtx_.m;
            const int32_t nx =
                static_cast<int32_t>((rnx * dm[0] + rny * dm[4] + rnz * dm[8]) >> 12);
            const int32_t ny =
                static_cast<int32_t>((rnx * dm[1] + rny * dm[5] + rnz * dm[9]) >> 12);
            const int32_t nz =
                static_cast<int32_t>((rnx * dm[2] + rny * dm[6] + rnz * dm[10]) >> 12);
            float rr = static_cast<float>(mat_emission_ & 0x1FU) / 31.0f;
            float gg = static_cast<float>((mat_emission_ >> 5U) & 0x1FU) / 31.0f;
            float bb = static_cast<float>((mat_emission_ >> 10U) & 0x1FU) / 31.0f;
            for (int l = 0; l < 4; ++l) {
                if ((active_poly_attr_ & (1U << l)) == 0U)
                    continue;
                const int64_t dot = -(static_cast<int64_t>(nx) * light_dir_[l * 3 + 0] +
                                      static_cast<int64_t>(ny) * light_dir_[l * 3 + 1] +
                                      static_cast<int64_t>(nz) * light_dir_[l * 3 + 2]);
                const float df = std::max(0.0f, static_cast<float>(dot) / (512.0f * 512.0f));
                const uint16_t lc = light_col_[l];
                const float lr = static_cast<float>(lc & 0x1FU) / 31.0f;
                const float lg = static_cast<float>((lc >> 5U) & 0x1FU) / 31.0f;
                const float lb = static_cast<float>((lc >> 10U) & 0x1FU) / 31.0f;
                rr += lr * (static_cast<float>(mat_ambient_ & 0x1FU) / 31.0f +
                            df * static_cast<float>(mat_diffuse_ & 0x1FU) / 31.0f);
                gg += lg * (static_cast<float>((mat_ambient_ >> 5U) & 0x1FU) / 31.0f +
                            df * static_cast<float>((mat_diffuse_ >> 5U) & 0x1FU) / 31.0f);
                bb += lb * (static_cast<float>((mat_ambient_ >> 10U) & 0x1FU) / 31.0f +
                            df * static_cast<float>((mat_diffuse_ >> 10U) & 0x1FU) / 31.0f);
            }
            cur_r_ = std::min(1.0f, rr);
            cur_g_ = std::min(1.0f, gg);
            cur_b_ = std::min(1.0f, bb);
            break;
        }
        case 0x22U: {  // TEXCOORD
            const int16_t raw_s = static_cast<int16_t>(p[0] & 0xFFFFU);
            const int16_t raw_t = static_cast<int16_t>(p[0] >> 16U);
            if (((tex_param_ >> 30U) & 3U) == 1U) {
                const auto& tm = tex_mtx_.m;
                const int64_t s = raw_s;
                const int64_t t = raw_t;
                tex_s_ = static_cast<int16_t>(
                    (s * tm[0] + t * tm[4] + (tm[8] << 4) + (tm[12] << 4)) >> 12);
                tex_t_ = static_cast<int16_t>(
                    (s * tm[1] + t * tm[5] + (tm[9] << 4) + (tm[13] << 4)) >> 12);
            } else {
                tex_s_ = raw_s;
                tex_t_ = raw_t;
            }
            break;
        }
        case 0x23U:  // VTX_16
            vtx_x_ = static_cast<int16_t>(p[0] & 0xFFFFU);
            vtx_y_ = static_cast<int16_t>(p[0] >> 16U);
            vtx_z_ = static_cast<int16_t>(p[1] & 0xFFFFU);
            submit_vertex();
            break;
        case 0x24U:  // VTX_10
            vtx_x_ = static_cast<int16_t>(sext10(p[0] & 0x3FFU) << 6);
            vtx_y_ = static_cast<int16_t>(sext10((p[0] >> 10U) & 0x3FFU) << 6);
            vtx_z_ = static_cast<int16_t>(sext10((p[0] >> 20U) & 0x3FFU) << 6);
            submit_vertex();
            break;
        case 0x25U:  // VTX_XY
            vtx_x_ = static_cast<int16_t>(p[0] & 0xFFFFU);
            vtx_y_ = static_cast<int16_t>(p[0] >> 16U);
            submit_vertex();
            break;
        case 0x26U:  // VTX_XZ
            vtx_x_ = static_cast<int16_t>(p[0] & 0xFFFFU);
            vtx_z_ = static_cast<int16_t>(p[0] >> 16U);
            submit_vertex();
            break;
        case 0x27U:  // VTX_YZ
            vtx_y_ = static_cast<int16_t>(p[0] & 0xFFFFU);
            vtx_z_ = static_cast<int16_t>(p[0] >> 16U);
            submit_vertex();
            break;
        case 0x28U:  // VTX_DIFF
            vtx_x_ = static_cast<int16_t>(vtx_x_ + sext10(p[0] & 0x3FFU));
            vtx_y_ = static_cast<int16_t>(vtx_y_ + sext10((p[0] >> 10U) & 0x3FFU));
            vtx_z_ = static_cast<int16_t>(vtx_z_ + sext10((p[0] >> 20U) & 0x3FFU));
            submit_vertex();
            break;
        case 0x29U:  // POLYGON_ATTR
            poly_attr_ = p[0];
            break;
        case 0x2AU:  // TEXIMAGE_PARAM
            tex_param_ = p[0];
            break;
        case 0x2BU:  // PLTT_BASE
            pltt_base_ = p[0] & 0x1FFFU;
            break;
        case 0x30U:  // DIF_AMB
            mat_diffuse_ = static_cast<uint16_t>(p[0] & 0x7FFFU);
            mat_ambient_ = static_cast<uint16_t>((p[0] >> 16U) & 0x7FFFU);
            if (p[0] & 0x8000U) {
                cur_r_ = static_cast<float>(mat_diffuse_ & 0x1FU) / 31.0f;
                cur_g_ = static_cast<float>((mat_diffuse_ >> 5U) & 0x1FU) / 31.0f;
                cur_b_ = static_cast<float>((mat_diffuse_ >> 10U) & 0x1FU) / 31.0f;
            }
            break;
        case 0x31U:  // SPE_EMI
            mat_specular_ = static_cast<uint16_t>(p[0] & 0x7FFFU);
            mat_emission_ = static_cast<uint16_t>((p[0] >> 16U) & 0x7FFFU);
            break;
        case 0x32U: {  // LIGHT_VECTOR
            const int l = static_cast<int>((p[0] >> 30U) & 3U);
            const int64_t rlx = sext10(p[0] & 0x3FFU);
            const int64_t rly = sext10((p[0] >> 10U) & 0x3FFU);
            const int64_t rlz = sext10((p[0] >> 20U) & 0x3FFU);
            const auto& dm = dir_mtx_.m;
            light_dir_[l * 3 + 0] =
                static_cast<int32_t>((rlx * dm[0] + rly * dm[4] + rlz * dm[8]) >> 12);
            light_dir_[l * 3 + 1] =
                static_cast<int32_t>((rlx * dm[1] + rly * dm[5] + rlz * dm[9]) >> 12);
            light_dir_[l * 3 + 2] =
                static_cast<int32_t>((rlx * dm[2] + rly * dm[6] + rlz * dm[10]) >> 12);
            break;
        }
        case 0x33U: {  // LIGHT_COLOR
            const int l = static_cast<int>((p[0] >> 30U) & 3U);
            light_col_[l] = static_cast<uint16_t>(p[0] & 0x7FFFU);
            break;
        }
        case 0x40U:  // BEGIN_VTXS
            in_begin_ = true;
            prim_type_ = static_cast<uint8_t>(p[0] & 3U);
            active_poly_attr_ = poly_attr_;
            strip_count_ = 0;
            strip_odd_ = false;
            break;
        case 0x41U:  // END_VTXS
            in_begin_ = false;
            strip_count_ = 0;
            strip_odd_ = false;
            break;
        case 0x50U:  // SWAP_BUFFERS
            render_list_.swap(build_list_);
            build_list_.clear();
            w_buffer_ = (p[0] & 2U) != 0U;
            swap_pending_ = true;
            break;
        case 0x60U: {  // VIEWPORT
            const int32_t x1 = static_cast<int32_t>(p[0] & 0xFFU);
            const int32_t y1 = static_cast<int32_t>((p[0] >> 8U) & 0xFFU);
            const int32_t x2 = static_cast<int32_t>((p[0] >> 16U) & 0xFFU);
            const int32_t y2 = static_cast<int32_t>((p[0] >> 24U) & 0xFFU);
            viewport_.x = x1;
            viewport_.y = 191 - y2;
            viewport_.width = std::max(1, x2 - x1 + 1);
            viewport_.height = std::max(1, y2 - y1 + 1);
            break;
        }
        default:
            break;
    }
}

void Gx3d::submit_vertex() noexcept {
    if (!in_begin_)
        return;
    update_clip_matrix();

    // Transform (vtx_x_, vtx_y_, vtx_z_, 1.0) in 1.19.12 fixed point by clip_mtx_
    const int64_t vx = vtx_x_;
    const int64_t vy = vtx_y_;
    const int64_t vz = vtx_z_;

    const auto& m = clip_mtx_.m;
    const float cx =
        static_cast<float>((vx * m[0] + vy * m[4] + vz * m[8] + (int64_t{m[12]} << 12)) >> 12) /
        4096.0f;
    const float cy =
        static_cast<float>((vx * m[1] + vy * m[5] + vz * m[9] + (int64_t{m[13]} << 12)) >> 12) /
        4096.0f;
    const float cz =
        static_cast<float>((vx * m[2] + vy * m[6] + vz * m[10] + (int64_t{m[14]} << 12)) >> 12) /
        4096.0f;
    const float cw =
        static_cast<float>((vx * m[3] + vy * m[7] + vz * m[11] + (int64_t{m[15]} << 12)) >> 12) /
        4096.0f;

    const uint32_t tw = 8U << ((tex_param_ >> 20U) & 7U);
    const uint32_t th = 8U << ((tex_param_ >> 23U) & 7U);
    const float u = static_cast<float>(tex_s_) / (16.0f * static_cast<float>(tw));
    const float v = static_cast<float>(tex_t_) / (16.0f * static_cast<float>(th));
    const float alpha = static_cast<float>((active_poly_attr_ >> 16U) & 0x1FU) / 31.0f;

    dragon::gpu3d::Vertex3D vert{cx, cy,     cz,     cw,     u,
                                 v,  cur_r_, cur_g_, cur_b_, alpha > 0.0f ? alpha : 1.0f};

    auto emit_tri = [&](const dragon::gpu3d::Vertex3D& a, const dragon::gpu3d::Vertex3D& b,
                        const dragon::gpu3d::Vertex3D& c) {
        if (build_list_.size() >= 8192U)
            return;
        QueuedTri qt{};
        qt.v[0] = a;
        qt.v[1] = b;
        qt.v[2] = c;
        const bool show_back = (active_poly_attr_ & (1U << 6)) != 0U;
        const bool show_front = (active_poly_attr_ & (1U << 7)) != 0U;
        if (show_back && show_front)
            qt.state.cull = dragon::gpu3d::CullMode::None;
        else if (show_front)
            qt.state.cull = dragon::gpu3d::CullMode::Back;
        else if (show_back)
            qt.state.cull = dragon::gpu3d::CullMode::Front;
        else
            return;
        qt.state.depth_func = (active_poly_attr_ & (1U << 14)) ? dragon::gpu3d::DepthFunc::Equal
                                                               : dragon::gpu3d::DepthFunc::Less;
        const uint32_t tex_fmt = (tex_param_ >> 26U) & 7U;
        const bool tex_translucent = ((disp3dcnt_ & 1U) != 0U) && (tex_fmt == 1U || tex_fmt == 6U);
        qt.state.blend = ((alpha > 0.0f && alpha < 0.99f) || tex_translucent)
                             ? dragon::gpu3d::BlendMode::AlphaBlend
                             : dragon::gpu3d::BlendMode::Opaque;
        qt.state.textured = ((disp3dcnt_ & 1U) != 0U) && (tex_fmt != 0U);
        qt.state.modulate = ((active_poly_attr_ >> 4U) & 3U) == 0U;
        qt.tex_param = tex_param_;
        qt.pltt_base = pltt_base_;
        build_list_.push_back(qt);
    };

    if (prim_type_ == 0U) {  // Triangles
        strip_buf_[strip_count_++] = vert;
        if (strip_count_ == 3) {
            emit_tri(strip_buf_[0], strip_buf_[1], strip_buf_[2]);
            strip_count_ = 0;
        }
    } else if (prim_type_ == 1U) {  // Quads
        strip_buf_[strip_count_++] = vert;
        if (strip_count_ == 4) {
            emit_tri(strip_buf_[0], strip_buf_[1], strip_buf_[2]);
            emit_tri(strip_buf_[0], strip_buf_[2], strip_buf_[3]);
            strip_count_ = 0;
        }
    } else if (prim_type_ == 2U) {  // Triangle Strip
        if (strip_count_ < 3) {
            strip_buf_[strip_count_++] = vert;
            if (strip_count_ == 3) {
                emit_tri(strip_buf_[0], strip_buf_[1], strip_buf_[2]);
                strip_odd_ = true;
            }
        } else {
            strip_buf_[0] = strip_buf_[1];
            strip_buf_[1] = strip_buf_[2];
            strip_buf_[2] = vert;
            if (strip_odd_) {
                emit_tri(strip_buf_[0], strip_buf_[2], strip_buf_[1]);
            } else {
                emit_tri(strip_buf_[0], strip_buf_[1], strip_buf_[2]);
            }
            strip_odd_ = !strip_odd_;
        }
    } else if (prim_type_ == 3U) {  // Quad Strip
        if (strip_count_ < 4) {
            strip_buf_[strip_count_++] = vert;
            if (strip_count_ == 4) {
                emit_tri(strip_buf_[0], strip_buf_[1], strip_buf_[3]);
                emit_tri(strip_buf_[0], strip_buf_[3], strip_buf_[2]);
            }
        } else {
            strip_buf_[0] = strip_buf_[2];
            strip_buf_[1] = strip_buf_[3];
            strip_buf_[2] = vert;
            strip_count_ = 3;
        }
    }
}

void Gx3d::decode_texture(const Bus& bus, uint32_t tex_param, uint32_t pltt_base,
                          dragon::gpu3d::Texture2D& out_tex) noexcept {
    const uint32_t fmt = (tex_param >> 26U) & 7U;
    const uint32_t w = 8U << ((tex_param >> 20U) & 7U);
    const uint32_t h = 8U << ((tex_param >> 23U) & 7U);
    const uint32_t vram_off = (tex_param & 0xFFFFU) << 3U;
    const bool color0_trans = (tex_param & (1U << 29)) != 0U;
    const uint32_t pal_off = (fmt == 2U) ? (pltt_base << 3U) : (pltt_base << 4U);

    if (w > 1024U || h > 1024U || fmt == 0U) {
        out_tex.rgba = nullptr;
        return;
    }

    decoded_tex_.resize(static_cast<size_t>(w) * h);
    auto read_pal = [&](uint32_t idx) -> uint16_t {
        const uint32_t a = pal_off + idx * 2U;
        return static_cast<uint16_t>(bus.read_vram_tex_pal(a) |
                                     (static_cast<uint16_t>(bus.read_vram_tex_pal(a + 1U)) << 8U));
    };

    const size_t total = static_cast<size_t>(w) * h;
    switch (fmt) {
        case 1U:  // A3I5
            for (size_t i = 0; i < total; ++i) {
                const uint8_t b = bus.read_vram_tex(vram_off + static_cast<uint32_t>(i));
                const uint8_t a = static_cast<uint8_t>(((b >> 5U) * 255U) / 7U);
                decoded_tex_[i] = rgb555_to_rgba(read_pal(b & 0x1FU), a);
            }
            break;
        case 2U:  // 4-color (2bpp)
            for (size_t i = 0; i < total; ++i) {
                const uint8_t b = bus.read_vram_tex(vram_off + static_cast<uint32_t>(i >> 2U));
                const uint8_t idx = (b >> ((i & 3U) * 2U)) & 3U;
                decoded_tex_[i] =
                    (color0_trans && idx == 0U) ? 0x00000000U : rgb555_to_rgba(read_pal(idx));
            }
            break;
        case 3U:  // 16-color (4bpp)
            for (size_t i = 0; i < total; ++i) {
                const uint8_t b = bus.read_vram_tex(vram_off + static_cast<uint32_t>(i >> 1U));
                const uint8_t idx = (i & 1U) ? (b >> 4U) : (b & 0x0FU);
                decoded_tex_[i] =
                    (color0_trans && idx == 0U) ? 0x00000000U : rgb555_to_rgba(read_pal(idx));
            }
            break;
        case 4U:  // 256-color (8bpp)
            for (size_t i = 0; i < total; ++i) {
                const uint8_t idx = bus.read_vram_tex(vram_off + static_cast<uint32_t>(i));
                decoded_tex_[i] =
                    (color0_trans && idx == 0U) ? 0x00000000U : rgb555_to_rgba(read_pal(idx));
            }
            break;
        case 5U: {  // Compressed 4x4-Texel Texture (GBATEK)
            const uint32_t blocks_x = w >> 2U;
            const uint32_t blocks_y = h >> 2U;
            auto blend555 = [](uint16_t c0, uint16_t c1, int w0, int w1, int div) -> uint32_t {
                const int r =
                    (static_cast<int>(c0 & 0x1FU) * w0 + static_cast<int>(c1 & 0x1FU) * w1) / div;
                const int g = (static_cast<int>((c0 >> 5U) & 0x1FU) * w0 +
                               static_cast<int>((c1 >> 5U) & 0x1FU) * w1) /
                              div;
                const int b = (static_cast<int>((c0 >> 10U) & 0x1FU) * w0 +
                               static_cast<int>((c1 >> 10U) & 0x1FU) * w1) /
                              div;
                return rgb555_to_rgba(
                    static_cast<uint16_t>((r & 0x1F) | ((g & 0x1F) << 5) | ((b & 0x1F) << 10)));
            };
            for (uint32_t by = 0; by < blocks_y; ++by) {
                for (uint32_t bx = 0; bx < blocks_x; ++bx) {
                    const uint32_t blk = by * blocks_x + bx;
                    const uint32_t tex_addr = vram_off + blk * 4U;
                    // Palette index slot is VRAM Slot 1 (0x20000 + (addr & 0x1FFFF)/2 +
                    // slot2*0x10000)
                    const uint32_t idx_addr = 0x20000U + ((tex_addr & 0x1FFFFU) >> 1U) +
                                              ((tex_addr >= 0x40000U) ? 0x10000U : 0U);
                    const uint16_t pinfo = static_cast<uint16_t>(
                        bus.read_vram_tex(idx_addr) |
                        (static_cast<uint16_t>(bus.read_vram_tex(idx_addr + 1U)) << 8U));
                    const uint32_t pbase = pal_off + ((pinfo & 0x3FFFU) << 2U);
                    const uint32_t mode = (pinfo >> 14U) & 3U;
                    auto rpal = [&](uint32_t k) -> uint16_t {
                        const uint32_t a = pbase + k * 2U;
                        return static_cast<uint16_t>(
                            bus.read_vram_tex_pal(a) |
                            (static_cast<uint16_t>(bus.read_vram_tex_pal(a + 1U)) << 8U));
                    };
                    const uint16_t c0 = rpal(0U);
                    const uint16_t c1 = rpal(1U);
                    uint32_t cols[4];
                    cols[0] = rgb555_to_rgba(c0);
                    cols[1] = rgb555_to_rgba(c1);
                    if (mode == 0U) {
                        cols[2] = rgb555_to_rgba(rpal(2U));
                        cols[3] = 0x00000000U;
                    } else if (mode == 1U) {
                        cols[2] = blend555(c0, c1, 1, 1, 2);
                        cols[3] = 0x00000000U;
                    } else if (mode == 2U) {
                        cols[2] = rgb555_to_rgba(rpal(2U));
                        cols[3] = rgb555_to_rgba(rpal(3U));
                    } else {
                        cols[2] = blend555(c0, c1, 5, 3, 8);
                        cols[3] = blend555(c0, c1, 3, 5, 8);
                    }
                    for (uint32_t ty = 0; ty < 4U; ++ty) {
                        const uint8_t row = bus.read_vram_tex(tex_addr + ty);
                        for (uint32_t tx = 0; tx < 4U; ++tx) {
                            const uint8_t code = (row >> (tx * 2U)) & 3U;
                            decoded_tex_[(by * 4U + ty) * w + (bx * 4U + tx)] = cols[code];
                        }
                    }
                }
            }
            break;
        }
        case 6U:  // A5I3
            for (size_t i = 0; i < total; ++i) {
                const uint8_t b = bus.read_vram_tex(vram_off + static_cast<uint32_t>(i));
                const uint8_t a = static_cast<uint8_t>(((b >> 3U) * 255U) / 31U);
                decoded_tex_[i] = rgb555_to_rgba(read_pal(b & 7U), a);
            }
            break;
        case 7U:  // Direct 16-bit RGBA5551
            for (size_t i = 0; i < total; ++i) {
                const uint32_t a = vram_off + static_cast<uint32_t>(i * 2U);
                const uint16_t c =
                    static_cast<uint16_t>(bus.read_vram_tex(a) |
                                          (static_cast<uint16_t>(bus.read_vram_tex(a + 1U)) << 8U));
                decoded_tex_[i] = (c & 0x8000U) ? rgb555_to_rgba(c, 255U) : 0x00000000U;
            }
            break;
        default:
            std::fill(decoded_tex_.begin(), decoded_tex_.end(), 0xFFFFFFFFU);
            break;
    }

    out_tex.rgba = decoded_tex_.data();
    out_tex.width = w;
    out_tex.height = h;
    out_tex.repeat_u = (tex_param & (1U << 16)) != 0U;
    out_tex.repeat_v = (tex_param & (1U << 17)) != 0U;
    out_tex.flip_u = (tex_param & (1U << 18)) != 0U;
    out_tex.flip_v = (tex_param & (1U << 19)) != 0U;
}

void Gx3d::on_vblank(const Bus& bus) noexcept {
    if (!swap_pending_) {
        return;
    }
    swap_pending_ = false;

    const uint16_t c15 = static_cast<uint16_t>(clear_color_ & 0x7FFFU);
    const uint8_t ca = static_cast<uint8_t>((((clear_color_ >> 16U) & 0x1FU) * 255U) / 31U);
    const float clear_z =
        w_buffer_
            ? 1e9f
            : (clear_depth_ == 0x7FFFU ? 1.0001f : static_cast<float>(clear_depth_) / 32767.0f);
    gpu_.clear(rgb555_to_rgba(c15, ca), clear_z);
    gpu_.set_viewport(viewport_);

    uint32_t last_tex_param = 0xFFFFFFFFU;
    uint32_t last_pltt_base = 0xFFFFFFFFU;
    dragon::gpu3d::Texture2D cached_tex{};

    for (const auto& tri : render_list_) {
        const dragon::gpu3d::Texture2D* tex_ptr = nullptr;
        if (tri.state.textured) {
            if (tri.tex_param != last_tex_param || tri.pltt_base != last_pltt_base) {
                decode_texture(bus, tri.tex_param, tri.pltt_base, cached_tex);
                last_tex_param = tri.tex_param;
                last_pltt_base = tri.pltt_base;
            }
            if (cached_tex.rgba) {
                tex_ptr = &cached_tex;
            }
        }
        auto st = tri.state;
        st.w_buffer = w_buffer_;
        gpu_.draw_triangle(tri.v[0], tri.v[1], tri.v[2], st, tex_ptr);
    }
}

}  // namespace dragon::nds
