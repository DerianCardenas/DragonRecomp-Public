#include "ps1/gte.h"

#include <algorithm>
#include <bit>

namespace dragon::ps1 {

namespace {

// Official PS1 GTE UNR (Unsigned Newton-Raphson) lookup table (257 entries, psx-spx)
constexpr uint8_t kUnrTable[257] = {
    0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA, 0xE8, 0xE6, 0xE4, 0xE3,
    0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5, 0xD3, 0xD1, 0xD0, 0xCE, 0xCD, 0xCB, 0xC9, 0xC8,
    0xC6, 0xC5, 0xC3, 0xC1, 0xC0, 0xBE, 0xBD, 0xBB, 0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xB0,
    0xAE, 0xAD, 0xAB, 0xAA, 0xA9, 0xA7, 0xA6, 0xA4, 0xA3, 0xA2, 0xA0, 0x9F, 0x9E, 0x9C, 0x9B, 0x9A,
    0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8F, 0x8D, 0x8C, 0x8B, 0x8A, 0x89, 0x87, 0x86,
    0x85, 0x84, 0x83, 0x82, 0x81, 0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x78, 0x77, 0x75, 0x74,
    0x73, 0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
    0x63, 0x62, 0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5D, 0x5C, 0x5B, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x55,
    0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x48,
    0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3F, 0x3F, 0x3E, 0x3D, 0x3C, 0x3C, 0x3B,
    0x3A, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2F,
    0x2E, 0x2E, 0x2D, 0x2C, 0x2C, 0x2B, 0x2A, 0x2A, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
    0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1F, 0x1E, 0x1E, 0x1D, 0x1D, 0x1C, 0x1B, 0x1B, 0x1A,
    0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
    0x10, 0x0F, 0x0F, 0x0E, 0x0E, 0x0D, 0x0D, 0x0C, 0x0C, 0x0B, 0x0A, 0x0A, 0x09, 0x09, 0x08, 0x08,
    0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,
    0x00,
};

inline int16_t lo16s(uint32_t v) noexcept {
    return static_cast<int16_t>(v & 0xFFFFU);
}
inline int16_t hi16s(uint32_t v) noexcept {
    return static_cast<int16_t>(v >> 16U);
}
inline uint32_t pack16(int16_t lo, int16_t hi) noexcept {
    return static_cast<uint32_t>(static_cast<uint16_t>(lo)) |
           (static_cast<uint32_t>(static_cast<uint16_t>(hi)) << 16U);
}

inline int32_t clamp_ir(int64_t val, bool lm, uint32_t& flag, uint32_t bit) noexcept {
    const int64_t lo = lm ? 0 : -32768;
    const int64_t hi = 32767;
    if (val < lo) {
        flag |= (1U << bit);
        return static_cast<int32_t>(lo);
    }
    if (val > hi) {
        flag |= (1U << bit);
        return static_cast<int32_t>(hi);
    }
    return static_cast<int32_t>(val);
}

inline int16_t clamp_sxy(int64_t val, uint32_t& flag, uint32_t bit) noexcept {
    if (val < -1024) {
        flag |= (1U << bit);
        return -1024;
    }
    if (val > 1023) {
        flag |= (1U << bit);
        return 1023;
    }
    return static_cast<int16_t>(val);
}

inline uint16_t clamp_sz(int64_t val, uint32_t& flag) noexcept {
    if (val < 0) {
        flag |= (1U << 18U);
        return 0U;
    }
    if (val > 0xFFFF) {
        flag |= (1U << 18U);
        return 0xFFFFU;
    }
    return static_cast<uint16_t>(val);
}

inline uint8_t clamp_rgb(int64_t val, uint32_t& flag, uint32_t bit) noexcept {
    if (val < 0) {
        flag |= (1U << bit);
        return 0U;
    }
    if (val > 255) {
        flag |= (1U << bit);
        return 255U;
    }
    return static_cast<uint8_t>(val);
}

void push_sxy(Ps1CpuState& cpu, int16_t sx, int16_t sy) noexcept {
    cpu.gte_data[12] = cpu.gte_data[13];
    cpu.gte_data[13] = cpu.gte_data[14];
    cpu.gte_data[14] = pack16(sx, sy);
    cpu.gte_data[15] = cpu.gte_data[14];
}

void push_sz(Ps1CpuState& cpu, uint16_t sz) noexcept {
    cpu.gte_data[16] = cpu.gte_data[17];
    cpu.gte_data[17] = cpu.gte_data[18];
    cpu.gte_data[18] = cpu.gte_data[19];
    cpu.gte_data[19] = sz;
}

void push_rgb(Ps1CpuState& cpu, uint8_t r, uint8_t g, uint8_t b, uint8_t code) noexcept {
    cpu.gte_data[20] = cpu.gte_data[21];
    cpu.gte_data[21] = cpu.gte_data[22];
    cpu.gte_data[22] = static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8U) |
                       (static_cast<uint32_t>(b) << 16U) | (static_cast<uint32_t>(code) << 24U);
}

void get_matrix(const Ps1CpuState& cpu, uint32_t mx, int16_t m[3][3]) noexcept {
    uint32_t base = 0;
    if (mx == 0U)
        base = 0;  // RT (ctrl 0..4)
    else if (mx == 1U)
        base = 8;  // LLM (ctrl 8..12)
    else if (mx == 2U)
        base = 16;  // LCM (ctrl 16..20)
    else {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                m[i][j] = 0;
        return;
    }
    m[0][0] = lo16s(cpu.gte_ctrl[base + 0]);
    m[0][1] = hi16s(cpu.gte_ctrl[base + 0]);
    m[0][2] = lo16s(cpu.gte_ctrl[base + 1]);
    m[1][0] = hi16s(cpu.gte_ctrl[base + 1]);
    m[1][1] = lo16s(cpu.gte_ctrl[base + 2]);
    m[1][2] = hi16s(cpu.gte_ctrl[base + 2]);
    m[2][0] = lo16s(cpu.gte_ctrl[base + 3]);
    m[2][1] = hi16s(cpu.gte_ctrl[base + 3]);
    m[2][2] = lo16s(cpu.gte_ctrl[base + 4]);
}

void get_vector(const Ps1CpuState& cpu, uint32_t vx, int16_t v[3]) noexcept {
    if (vx <= 2U) {
        v[0] = lo16s(cpu.gte_data[vx * 2U]);
        v[1] = hi16s(cpu.gte_data[vx * 2U]);
        v[2] = lo16s(cpu.gte_data[vx * 2U + 1U]);
    } else {
        v[0] = lo16s(cpu.gte_data[9]);
        v[1] = lo16s(cpu.gte_data[10]);
        v[2] = lo16s(cpu.gte_data[11]);
    }
}

void get_translation(const Ps1CpuState& cpu, uint32_t tx, int64_t tr[3]) noexcept {
    if (tx == 0U) {  // TRX/Y/Z
        tr[0] = static_cast<int64_t>(static_cast<int32_t>(cpu.gte_ctrl[5])) << 12;
        tr[1] = static_cast<int64_t>(static_cast<int32_t>(cpu.gte_ctrl[6])) << 12;
        tr[2] = static_cast<int64_t>(static_cast<int32_t>(cpu.gte_ctrl[7])) << 12;
    } else if (tx == 1U) {  // RBK/GBK/BBK
        tr[0] = static_cast<int64_t>(static_cast<int32_t>(cpu.gte_ctrl[13])) << 12;
        tr[1] = static_cast<int64_t>(static_cast<int32_t>(cpu.gte_ctrl[14])) << 12;
        tr[2] = static_cast<int64_t>(static_cast<int32_t>(cpu.gte_ctrl[15])) << 12;
    } else if (tx == 2U) {  // RFC/GFC/BFC
        tr[0] = static_cast<int64_t>(static_cast<int32_t>(cpu.gte_ctrl[21])) << 12;
        tr[1] = static_cast<int64_t>(static_cast<int32_t>(cpu.gte_ctrl[22])) << 12;
        tr[2] = static_cast<int64_t>(static_cast<int32_t>(cpu.gte_ctrl[23])) << 12;
    } else {
        tr[0] = tr[1] = tr[2] = 0;
    }
}

void rtp_vertex(Ps1CpuState& cpu, uint32_t vidx, int shift, bool lm, bool calc_dq,
                uint32_t& flag) noexcept {
    int16_t m[3][3], v[3];
    int64_t tr[3];
    get_matrix(cpu, 0, m);
    get_vector(cpu, vidx, v);
    get_translation(cpu, 0, tr);

    int64_t mac[3];
    for (int i = 0; i < 3; ++i) {
        const int64_t sum = tr[i] + static_cast<int64_t>(m[i][0]) * v[0] +
                            static_cast<int64_t>(m[i][1]) * v[1] +
                            static_cast<int64_t>(m[i][2]) * v[2];
        mac[i] = sum >> shift;
        cpu.gte_data[25 + i] = static_cast<uint32_t>(static_cast<int32_t>(mac[i]));
    }

    const int64_t raw_z =
        (tr[2] + static_cast<int64_t>(m[2][0]) * v[0] + static_cast<int64_t>(m[2][1]) * v[1] +
         static_cast<int64_t>(m[2][2]) * v[2]) >>
        12;

    cpu.gte_data[9] = static_cast<uint32_t>(clamp_ir(mac[0], lm, flag, 24));
    cpu.gte_data[10] = static_cast<uint32_t>(clamp_ir(mac[1], lm, flag, 23));
    cpu.gte_data[11] = static_cast<uint32_t>(clamp_ir(mac[2], lm, flag, 22));
    const uint16_t sz3 = clamp_sz(raw_z, flag);
    push_sz(cpu, sz3);

    const uint16_t h = static_cast<uint16_t>(cpu.gte_ctrl[26] & 0xFFFFU);
    uint32_t h_div_sz = Ps1Gte::unr_divide(h, sz3);
    if (sz3 <= (h / 2U)) {
        flag |= (1U << 17U);
        h_div_sz = 0x1FFFFU;
    }

    const int32_t ofx = static_cast<int32_t>(cpu.gte_ctrl[24]);
    const int32_t ofy = static_cast<int32_t>(cpu.gte_ctrl[25]);
    const int64_t sx =
        (static_cast<int64_t>(static_cast<int16_t>(cpu.gte_data[9])) * h_div_sz + ofx) >> 16;
    const int64_t sy =
        (static_cast<int64_t>(static_cast<int16_t>(cpu.gte_data[10])) * h_div_sz + ofy) >> 16;
    push_sxy(cpu, clamp_sxy(sx, flag, 14), clamp_sxy(sy, flag, 13));

    if (calc_dq) {
        const int32_t dqa = lo16s(cpu.gte_ctrl[27]);
        const int32_t dqb = static_cast<int32_t>(cpu.gte_ctrl[28]);
        const int64_t p = static_cast<int64_t>(dqa) * h_div_sz + dqb;
        cpu.gte_data[24] = static_cast<uint32_t>(static_cast<int32_t>(p));
        const int64_t ir0 = p >> 12;
        if (ir0 < 0) {
            flag |= (1U << 12U);
            cpu.gte_data[8] = 0U;
        } else if (ir0 > 0x1000) {
            flag |= (1U << 12U);
            cpu.gte_data[8] = 0x1000U;
        } else {
            cpu.gte_data[8] = static_cast<uint32_t>(ir0);
        }
    }
}

void set_mac_ir_rgb(Ps1CpuState& cpu, int64_t r, int64_t g, int64_t b, int shift, bool lm,
                    uint32_t& flag) noexcept {
    const int64_t m1 = r >> shift;
    const int64_t m2 = g >> shift;
    const int64_t m3 = b >> shift;
    cpu.gte_data[25] = static_cast<uint32_t>(static_cast<int32_t>(m1));
    cpu.gte_data[26] = static_cast<uint32_t>(static_cast<int32_t>(m2));
    cpu.gte_data[27] = static_cast<uint32_t>(static_cast<int32_t>(m3));
    cpu.gte_data[9] = static_cast<uint32_t>(clamp_ir(m1, lm, flag, 24));
    cpu.gte_data[10] = static_cast<uint32_t>(clamp_ir(m2, lm, flag, 23));
    cpu.gte_data[11] = static_cast<uint32_t>(clamp_ir(m3, lm, flag, 22));
    const uint8_t cr = clamp_rgb(m1 >> 4, flag, 21);
    const uint8_t cg = clamp_rgb(m2 >> 4, flag, 20);
    const uint8_t cb = clamp_rgb(m3 >> 4, flag, 19);
    const uint8_t code = static_cast<uint8_t>((cpu.gte_data[6] >> 24U) & 0xFFU);
    push_rgb(cpu, cr, cg, cb, code);
}

}  // namespace

uint32_t Ps1Gte::unr_divide(uint16_t h, uint16_t sz3) noexcept {
    if (sz3 == 0U || static_cast<uint32_t>(h) >= static_cast<uint32_t>(sz3) * 2U) {
        return 0x1FFFFU;
    }
    const uint32_t z = static_cast<uint32_t>(std::countl_zero(sz3));
    const uint32_t n = static_cast<uint32_t>(h) << z;
    const uint32_t d = static_cast<uint32_t>(sz3) << z;
    const uint32_t u = static_cast<uint32_t>(kUnrTable[((d & 0x7FFFU) + 0x40U) >> 7U]) + 0x101U;
    const uint32_t d2 = (0x2000080U - (d * u)) >> 8U;
    const uint32_t recip = (0x80U + (d2 * u)) >> 8U;
    const uint32_t res =
        static_cast<uint32_t>((static_cast<uint64_t>(n) * recip + 0x8000ULL) >> 16U);
    return std::min<uint32_t>(res, 0x1FFFFU);
}

uint32_t Ps1Gte::read_data(const Ps1CpuState& cpu, uint8_t reg) noexcept {
    reg &= 31U;
    switch (reg) {
        case 1:
        case 3:
        case 5:
        case 8:
        case 9:
        case 10:
        case 11:
            return static_cast<uint32_t>(static_cast<int32_t>(lo16s(cpu.gte_data[reg])));
        case 7:
        case 16:
        case 17:
        case 18:
        case 19:
            return cpu.gte_data[reg] & 0xFFFFU;
        case 15:
            return cpu.gte_data[14];  // SXYP mirrors SXY2 on read
        case 28:
        case 29: {
            // ORGB: pack clamped IR1/2/3 >> 7 into 5:5:5 RGB
            const uint32_t r =
                static_cast<uint32_t>(std::clamp<int32_t>(lo16s(cpu.gte_data[9]) >> 7, 0, 31));
            const uint32_t g =
                static_cast<uint32_t>(std::clamp<int32_t>(lo16s(cpu.gte_data[10]) >> 7, 0, 31));
            const uint32_t b =
                static_cast<uint32_t>(std::clamp<int32_t>(lo16s(cpu.gte_data[11]) >> 7, 0, 31));
            return r | (g << 5U) | (b << 10U);
        }
        default:
            return cpu.gte_data[reg];
    }
}

void Ps1Gte::write_data(Ps1CpuState& cpu, uint8_t reg, uint32_t val) noexcept {
    reg &= 31U;
    switch (reg) {
        case 1:
        case 3:
        case 5:
        case 8:
        case 9:
        case 10:
        case 11:
            cpu.gte_data[reg] = static_cast<uint32_t>(static_cast<int32_t>(lo16s(val)));
            break;
        case 7:
        case 16:
        case 17:
        case 18:
        case 19:
            cpu.gte_data[reg] = val & 0xFFFFU;
            break;
        case 15:  // SXYP pushes onto SXY FIFO
            push_sxy(cpu, lo16s(val), hi16s(val));
            break;
        case 28:  // IRGB expands 5:5:5 into IR1/IR2/IR3
            cpu.gte_data[28] = val & 0x7FFFU;
            cpu.gte_data[9] = (val & 0x1FU) << 7U;
            cpu.gte_data[10] = ((val >> 5U) & 0x1FU) << 7U;
            cpu.gte_data[11] = ((val >> 10U) & 0x1FU) << 7U;
            break;
        case 29:  // ORGB is read-only
            break;
        case 30: {  // LZCS -> updates LZCR (leading bits matching sign bit)
            cpu.gte_data[30] = val;
            const uint32_t v = (val & 0x80000000U) ? ~val : val;
            cpu.gte_data[31] = static_cast<uint32_t>(std::countl_zero(v));
            break;
        }
        case 31:  // LZCR is read-only
            break;
        default:
            cpu.gte_data[reg] = val;
            break;
    }
}

uint32_t Ps1Gte::read_ctrl(const Ps1CpuState& cpu, uint8_t reg) noexcept {
    reg &= 31U;
    switch (reg) {
        case 4:
        case 12:
        case 20:
        case 26:
        case 27:
        case 29:
        case 30:
            return static_cast<uint32_t>(static_cast<int32_t>(lo16s(cpu.gte_ctrl[reg])));
        case 31: {
            uint32_t f = cpu.gte_ctrl[31] & 0x7FFFF000U;
            if ((f & 0x7F87E000U) != 0U)
                f |= 0x80000000U;
            return f;
        }
        default:
            return cpu.gte_ctrl[reg];
    }
}

void Ps1Gte::write_ctrl(Ps1CpuState& cpu, uint8_t reg, uint32_t val) noexcept {
    reg &= 31U;
    switch (reg) {
        case 4:
        case 12:
        case 20:
        case 27:
        case 29:
        case 30:
            cpu.gte_ctrl[reg] = static_cast<uint32_t>(static_cast<int32_t>(lo16s(val)));
            break;
        case 26:  // H (unsigned 16-bit, sign-extended on read)
            cpu.gte_ctrl[reg] = val & 0xFFFFU;
            break;
        case 31:
            cpu.gte_ctrl[31] = val & 0x7FFFF000U;
            break;
        default:
            cpu.gte_ctrl[reg] = val;
            break;
    }
}

void Ps1Gte::execute_command(Ps1CpuState& cpu, uint32_t raw) noexcept {
    const uint8_t op = static_cast<uint8_t>(raw & 0x3FU);
    const int shift = ((raw >> 19U) & 1U) ? 12 : 0;
    const bool lm = ((raw >> 10U) & 1U) != 0U;
    const uint32_t mx = (raw >> 17U) & 3U;
    const uint32_t vx = (raw >> 15U) & 3U;
    const uint32_t tx = (raw >> 13U) & 3U;

    uint32_t flag = 0U;

    switch (op) {
        case 0x01:  // RTPS
            rtp_vertex(cpu, 0, shift, lm, true, flag);
            break;

        case 0x30:  // RTPT
            rtp_vertex(cpu, 0, shift, lm, false, flag);
            rtp_vertex(cpu, 1, shift, lm, false, flag);
            rtp_vertex(cpu, 2, shift, lm, true, flag);
            break;

        case 0x06: {  // NCLIP
            const int64_t sx0 = lo16s(cpu.gte_data[12]);
            const int64_t sy0 = hi16s(cpu.gte_data[12]);
            const int64_t sx1 = lo16s(cpu.gte_data[13]);
            const int64_t sy1 = hi16s(cpu.gte_data[13]);
            const int64_t sx2 = lo16s(cpu.gte_data[14]);
            const int64_t sy2 = hi16s(cpu.gte_data[14]);
            const int64_t res =
                sx0 * sy1 + sx1 * sy2 + sx2 * sy0 - sx0 * sy2 - sx1 * sy0 - sx2 * sy1;
            cpu.gte_data[24] = static_cast<uint32_t>(static_cast<int32_t>(res));
            break;
        }

        case 0x12: {  // MVMVA
            int16_t m[3][3], v[3];
            int64_t tr[3];
            get_matrix(cpu, mx, m);
            get_vector(cpu, vx, v);
            get_translation(cpu, tx, tr);
            for (int i = 0; i < 3; ++i) {
                const int64_t mac =
                    (tr[i] + static_cast<int64_t>(m[i][0]) * v[0] +
                     static_cast<int64_t>(m[i][1]) * v[1] + static_cast<int64_t>(m[i][2]) * v[2]) >>
                    shift;
                cpu.gte_data[25 + i] = static_cast<uint32_t>(static_cast<int32_t>(mac));
                cpu.gte_data[9 + i] = static_cast<uint32_t>(clamp_ir(mac, lm, flag, 24U - i));
            }
            break;
        }

        case 0x0C: {  // OP (cross product of RT diagonal and IR)
            const int64_t d1 = lo16s(cpu.gte_ctrl[0]);
            const int64_t d2 = hi16s(cpu.gte_ctrl[2]);
            const int64_t d3 = lo16s(cpu.gte_ctrl[4]);
            const int64_t ir1 = lo16s(cpu.gte_data[9]);
            const int64_t ir2 = lo16s(cpu.gte_data[10]);
            const int64_t ir3 = lo16s(cpu.gte_data[11]);
            const int64_t m1 = (ir3 * d2 - ir2 * d3) >> shift;
            const int64_t m2 = (ir1 * d3 - ir3 * d1) >> shift;
            const int64_t m3 = (ir2 * d1 - ir1 * d2) >> shift;
            cpu.gte_data[25] = static_cast<uint32_t>(static_cast<int32_t>(m1));
            cpu.gte_data[26] = static_cast<uint32_t>(static_cast<int32_t>(m2));
            cpu.gte_data[27] = static_cast<uint32_t>(static_cast<int32_t>(m3));
            cpu.gte_data[9] = static_cast<uint32_t>(clamp_ir(m1, lm, flag, 24));
            cpu.gte_data[10] = static_cast<uint32_t>(clamp_ir(m2, lm, flag, 23));
            cpu.gte_data[11] = static_cast<uint32_t>(clamp_ir(m3, lm, flag, 22));
            break;
        }

        case 0x28: {  // SQR
            for (int i = 0; i < 3; ++i) {
                const int64_t v = lo16s(cpu.gte_data[9 + i]);
                const int64_t mac = (v * v) >> shift;
                cpu.gte_data[25 + i] = static_cast<uint32_t>(static_cast<int32_t>(mac));
                cpu.gte_data[9 + i] = static_cast<uint32_t>(clamp_ir(mac, lm, flag, 24U - i));
            }
            break;
        }

        case 0x2D: {  // AVSZ3
            const int64_t zsf3 = lo16s(cpu.gte_ctrl[29]);
            const int64_t sum = zsf3 * (static_cast<int64_t>(cpu.gte_data[17] & 0xFFFFU) +
                                        static_cast<int64_t>(cpu.gte_data[18] & 0xFFFFU) +
                                        static_cast<int64_t>(cpu.gte_data[19] & 0xFFFFU));
            cpu.gte_data[24] = static_cast<uint32_t>(static_cast<int32_t>(sum));
            cpu.gte_data[7] = clamp_sz(sum >> 12, flag);
            break;
        }

        case 0x2E: {  // AVSZ4
            const int64_t zsf4 = lo16s(cpu.gte_ctrl[30]);
            const int64_t sum = zsf4 * (static_cast<int64_t>(cpu.gte_data[16] & 0xFFFFU) +
                                        static_cast<int64_t>(cpu.gte_data[17] & 0xFFFFU) +
                                        static_cast<int64_t>(cpu.gte_data[18] & 0xFFFFU) +
                                        static_cast<int64_t>(cpu.gte_data[19] & 0xFFFFU));
            cpu.gte_data[24] = static_cast<uint32_t>(static_cast<int32_t>(sum));
            cpu.gte_data[7] = clamp_sz(sum >> 12, flag);
            break;
        }

        case 0x10:    // DPCS
        case 0x2A: {  // DPCT
            const int n = (op == 0x2A) ? 3 : 1;
            const int64_t ir0 = lo16s(cpu.gte_data[8]);
            int64_t fc[3];
            get_translation(cpu, 2, fc);
            for (int k = 0; k < n; ++k) {
                const uint32_t src = (op == 0x2A) ? cpu.gte_data[20] : cpu.gte_data[6];
                const int64_t r = static_cast<int64_t>(src & 0xFFU) << 16;
                const int64_t g = static_cast<int64_t>((src >> 8U) & 0xFFU) << 16;
                const int64_t b = static_cast<int64_t>((src >> 16U) & 0xFFU) << 16;
                const int64_t dr = clamp_ir((fc[0] - r) >> shift, false, flag, 24);
                const int64_t dg = clamp_ir((fc[1] - g) >> shift, false, flag, 23);
                const int64_t db = clamp_ir((fc[2] - b) >> shift, false, flag, 22);
                set_mac_ir_rgb(cpu, r + ir0 * dr, g + ir0 * dg, b + ir0 * db, shift, lm, flag);
            }
            break;
        }

        case 0x11: {  // INTPL
            const int64_t ir0 = lo16s(cpu.gte_data[8]);
            int64_t fc[3];
            get_translation(cpu, 2, fc);
            const int64_t r = static_cast<int64_t>(lo16s(cpu.gte_data[9])) << 12;
            const int64_t g = static_cast<int64_t>(lo16s(cpu.gte_data[10])) << 12;
            const int64_t b = static_cast<int64_t>(lo16s(cpu.gte_data[11])) << 12;
            const int64_t dr = clamp_ir((fc[0] - r) >> shift, false, flag, 24);
            const int64_t dg = clamp_ir((fc[1] - g) >> shift, false, flag, 23);
            const int64_t db = clamp_ir((fc[2] - b) >> shift, false, flag, 22);
            set_mac_ir_rgb(cpu, r + ir0 * dr, g + ir0 * dg, b + ir0 * db, shift, lm, flag);
            break;
        }

        case 0x13:    // NCDS
        case 0x16:    // NCDT
        case 0x1B:    // NCCS
        case 0x3F:    // NCCT
        case 0x1E:    // NCS
        case 0x20: {  // NCT
            const bool triple = (op == 0x16 || op == 0x3F || op == 0x20);
            const bool mul_col = (op != 0x1E && op != 0x20);
            const bool depth_cue = (op == 0x13 || op == 0x16);
            int16_t llm[3][3], lcm[3][3];
            int64_t bk[3], fc[3];
            get_matrix(cpu, 1, llm);
            get_matrix(cpu, 2, lcm);
            get_translation(cpu, 1, bk);
            get_translation(cpu, 2, fc);
            const int64_t cr = static_cast<int64_t>(cpu.gte_data[6] & 0xFFU);
            const int64_t cg = static_cast<int64_t>((cpu.gte_data[6] >> 8U) & 0xFFU);
            const int64_t cb = static_cast<int64_t>((cpu.gte_data[6] >> 16U) & 0xFFU);
            const int64_t ir0 = lo16s(cpu.gte_data[8]);

            const int count = triple ? 3 : 1;
            for (int k = 0; k < count; ++k) {
                int16_t v[3];
                get_vector(cpu, static_cast<uint32_t>(k), v);
                int32_t ir[3];
                for (int i = 0; i < 3; ++i) {
                    const int64_t m = (static_cast<int64_t>(llm[i][0]) * v[0] +
                                       static_cast<int64_t>(llm[i][1]) * v[1] +
                                       static_cast<int64_t>(llm[i][2]) * v[2]) >>
                                      shift;
                    ir[i] = clamp_ir(m, lm, flag, 24U - i);
                }
                int64_t col[3];
                for (int i = 0; i < 3; ++i) {
                    const int64_t m = (bk[i] + static_cast<int64_t>(lcm[i][0]) * ir[0] +
                                       static_cast<int64_t>(lcm[i][1]) * ir[1] +
                                       static_cast<int64_t>(lcm[i][2]) * ir[2]) >>
                                      shift;
                    col[i] = clamp_ir(m, lm, flag, 24U - i);
                }
                if (mul_col) {
                    col[0] = (cr * col[0]) << 4;
                    col[1] = (cg * col[1]) << 4;
                    col[2] = (cb * col[2]) << 4;
                } else {
                    col[0] <<= shift;
                    col[1] <<= shift;
                    col[2] <<= shift;
                }
                if (depth_cue) {
                    const int64_t dr = clamp_ir((fc[0] - col[0]) >> shift, false, flag, 24);
                    const int64_t dg = clamp_ir((fc[1] - col[1]) >> shift, false, flag, 23);
                    const int64_t db = clamp_ir((fc[2] - col[2]) >> shift, false, flag, 22);
                    col[0] += ir0 * dr;
                    col[1] += ir0 * dg;
                    col[2] += ir0 * db;
                }
                set_mac_ir_rgb(cpu, col[0], col[1], col[2], shift, lm, flag);
            }
            break;
        }

        case 0x1C:    // CC
        case 0x14: {  // CDP
            int16_t lcm[3][3];
            int64_t bk[3], fc[3];
            get_matrix(cpu, 2, lcm);
            get_translation(cpu, 1, bk);
            get_translation(cpu, 2, fc);
            const int32_t ir[3] = {lo16s(cpu.gte_data[9]), lo16s(cpu.gte_data[10]),
                                   lo16s(cpu.gte_data[11])};
            const int64_t cr = static_cast<int64_t>(cpu.gte_data[6] & 0xFFU);
            const int64_t cg = static_cast<int64_t>((cpu.gte_data[6] >> 8U) & 0xFFU);
            const int64_t cb = static_cast<int64_t>((cpu.gte_data[6] >> 16U) & 0xFFU);
            int64_t col[3];
            for (int i = 0; i < 3; ++i) {
                const int64_t m = (bk[i] + static_cast<int64_t>(lcm[i][0]) * ir[0] +
                                   static_cast<int64_t>(lcm[i][1]) * ir[1] +
                                   static_cast<int64_t>(lcm[i][2]) * ir[2]) >>
                                  shift;
                col[i] = clamp_ir(m, lm, flag, 24U - i);
            }
            col[0] = (cr * col[0]) << 4;
            col[1] = (cg * col[1]) << 4;
            col[2] = (cb * col[2]) << 4;
            if (op == 0x14) {
                const int64_t ir0 = lo16s(cpu.gte_data[8]);
                const int64_t dr = clamp_ir((fc[0] - col[0]) >> shift, false, flag, 24);
                const int64_t dg = clamp_ir((fc[1] - col[1]) >> shift, false, flag, 23);
                const int64_t db = clamp_ir((fc[2] - col[2]) >> shift, false, flag, 22);
                col[0] += ir0 * dr;
                col[1] += ir0 * dg;
                col[2] += ir0 * db;
            }
            set_mac_ir_rgb(cpu, col[0], col[1], col[2], shift, lm, flag);
            break;
        }

        case 0x29: {  // DCPL
            const int64_t ir0 = lo16s(cpu.gte_data[8]);
            int64_t fc[3];
            get_translation(cpu, 2, fc);
            const int64_t r =
                (static_cast<int64_t>(cpu.gte_data[6] & 0xFFU) * lo16s(cpu.gte_data[9])) << 4;
            const int64_t g =
                (static_cast<int64_t>((cpu.gte_data[6] >> 8U) & 0xFFU) * lo16s(cpu.gte_data[10]))
                << 4;
            const int64_t b =
                (static_cast<int64_t>((cpu.gte_data[6] >> 16U) & 0xFFU) * lo16s(cpu.gte_data[11]))
                << 4;
            const int64_t dr = clamp_ir((fc[0] - r) >> shift, false, flag, 24);
            const int64_t dg = clamp_ir((fc[1] - g) >> shift, false, flag, 23);
            const int64_t db = clamp_ir((fc[2] - b) >> shift, false, flag, 22);
            set_mac_ir_rgb(cpu, r + ir0 * dr, g + ir0 * dg, b + ir0 * db, shift, lm, flag);
            break;
        }

        case 0x3D:    // GPF
        case 0x3E: {  // GPL
            const int64_t ir0 = lo16s(cpu.gte_data[8]);
            int64_t c[3];
            for (int i = 0; i < 3; ++i) {
                const int64_t base =
                    (op == 0x3E) ? (static_cast<int64_t>(static_cast<int32_t>(cpu.gte_data[25 + i]))
                                    << shift)
                                 : 0;
                c[i] = base + ir0 * lo16s(cpu.gte_data[9 + i]);
            }
            set_mac_ir_rgb(cpu, c[0], c[1], c[2], shift, lm, flag);
            break;
        }

        default:
            break;
    }

    if ((flag & 0x7F87E000U) != 0U)
        flag |= 0x80000000U;
    cpu.gte_ctrl[31] = flag;
}

}  // namespace dragon::ps1
