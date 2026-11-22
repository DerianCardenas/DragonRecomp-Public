#include "psp/interpreter.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "dragon/mips.h"
#include "psp/runtime.h"

namespace dragon::psp {

namespace {

inline uint32_t f32_to_u32(float f) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

inline float u32_to_f32(uint32_t u) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

inline uint32_t rotr32(uint32_t v, uint32_t sh) noexcept {
    sh &= 31U;
    return (sh == 0U) ? v : ((v >> sh) | (v << (32U - sh)));
}

inline uint32_t bitrev32(uint32_t x) noexcept {
    x = ((x >> 1U) & 0x55555555U) | ((x & 0x55555555U) << 1U);
    x = ((x >> 2U) & 0x33333333U) | ((x & 0x33333333U) << 2U);
    x = ((x >> 4U) & 0x0F0F0F0FU) | ((x & 0x0F0F0F0FU) << 4U);
    x = ((x >> 8U) & 0x00FF00FFU) | ((x & 0x00FF00FFU) << 8U);
    return (x >> 16U) | (x << 16U);
}

inline uint32_t vfpu_reg_index(uint8_t reg) noexcept {
    // Allegrex VFPU register index:
    // bits 4..2 = matrix (0..7), bit 5 = transpose (row vs col), bits 1..0 = col/row, bits 6..5 =
    // offset
    const uint32_t mtx = (reg >> 2U) & 7U;
    const uint32_t idx = reg & 3U;
    const uint32_t off = (reg >> 5U) & 3U;
    return (mtx * 16U) + (idx * 4U) + off;
}

inline void vfpu_vec_indices(uint8_t reg, uint32_t size, uint32_t out[4]) noexcept {
    const uint32_t mtx = (reg >> 2U) & 7U;
    const uint32_t idx = reg & 3U;
    const bool transposed = ((reg >> 5U) & 1U) != 0U;
    const uint32_t off = (size == 4U) ? 0U : (((reg >> 6U) & 1U) * 2U);
    for (uint32_t i = 0; i < size; ++i) {
        const uint32_t c = transposed ? ((off + i) & 3U) : idx;
        const uint32_t r = transposed ? idx : ((off + i) & 3U);
        out[i] = mtx * 16U + c * 4U + r;
    }
}

void exec_vfpu_core(PspRuntime& rt, PspCpuState& cpu, const dragon::mips::DecodedMips& d) noexcept {
    PspBus& bus = rt.bus();
    const uint32_t raw = d.raw;

    switch (d.op) {
        case dragon::mips::MipsOp::Lvs: {
            const int32_t off = static_cast<int32_t>(static_cast<int16_t>(raw & 0xFFFCU));
            const uint32_t addr = cpu.gpr[d.rs] + static_cast<uint32_t>(off);
            const uint8_t vt = static_cast<uint8_t>(((raw & 3U) << 5U) | d.rt);
            cpu.vfpu[vfpu_reg_index(vt) & 127U] = u32_to_f32(bus.read32(addr));
            break;
        }
        case dragon::mips::MipsOp::Svs: {
            const int32_t off = static_cast<int32_t>(static_cast<int16_t>(raw & 0xFFFCU));
            const uint32_t addr = cpu.gpr[d.rs] + static_cast<uint32_t>(off);
            const uint8_t vt = static_cast<uint8_t>(((raw & 3U) << 5U) | d.rt);
            bus.write32(addr, f32_to_u32(cpu.vfpu[vfpu_reg_index(vt) & 127U]));
            break;
        }
        case dragon::mips::MipsOp::Lvq: {
            const int32_t off = static_cast<int32_t>(static_cast<int16_t>(raw & 0xFFFCU));
            const uint32_t addr = cpu.gpr[d.rs] + static_cast<uint32_t>(off);
            const uint8_t vt = static_cast<uint8_t>(((raw & 1U) << 5U) | d.rt);
            uint32_t idx[4];
            vfpu_vec_indices(vt, 4U, idx);
            for (uint32_t i = 0; i < 4U; ++i) {
                cpu.vfpu[idx[i] & 127U] = u32_to_f32(bus.read32(addr + i * 4U));
            }
            break;
        }
        case dragon::mips::MipsOp::Svq: {
            const int32_t off = static_cast<int32_t>(static_cast<int16_t>(raw & 0xFFFCU));
            const uint32_t addr = cpu.gpr[d.rs] + static_cast<uint32_t>(off);
            const uint8_t vt = static_cast<uint8_t>(((raw & 1U) << 5U) | d.rt);
            uint32_t idx[4];
            vfpu_vec_indices(vt, 4U, idx);
            for (uint32_t i = 0; i < 4U; ++i) {
                bus.write32(addr + i * 4U, f32_to_u32(cpu.vfpu[idx[i] & 127U]));
            }
            break;
        }
        case dragon::mips::MipsOp::Vfpu0: {  // vadd / vsub / vdiv / vsbn
            const uint32_t sub = (raw >> 23U) & 7U;
            const uint32_t sz = 1U + ((raw >> 7U) & 1U) + (((raw >> 15U) & 1U) << 1U);
            const uint8_t vd = static_cast<uint8_t>(raw & 0x7FU);
            const uint8_t vs = static_cast<uint8_t>((raw >> 8U) & 0x7FU);
            const uint8_t vt = static_cast<uint8_t>((raw >> 16U) & 0x7FU);
            uint32_t id[4], is[4], it[4];
            vfpu_vec_indices(vd, sz, id);
            vfpu_vec_indices(vs, sz, is);
            vfpu_vec_indices(vt, sz, it);
            float res[4];
            for (uint32_t i = 0; i < sz; ++i) {
                const float a = cpu.vfpu[is[i] & 127U];
                const float b = cpu.vfpu[it[i] & 127U];
                if (sub == 0U)
                    res[i] = a + b;
                else if (sub == 1U)
                    res[i] = a - b;
                else if (sub == 7U)
                    res[i] = (b != 0.0f) ? (a / b) : 0.0f;
                else
                    res[i] = a;
            }
            for (uint32_t i = 0; i < sz; ++i)
                cpu.vfpu[id[i] & 127U] = res[i];
            break;
        }
        case dragon::mips::MipsOp::Vfpu1: {  // vmul / vdot / vscl
            const uint32_t sub = (raw >> 23U) & 7U;
            const uint32_t sz = 1U + ((raw >> 7U) & 1U) + (((raw >> 15U) & 1U) << 1U);
            const uint8_t vd = static_cast<uint8_t>(raw & 0x7FU);
            const uint8_t vs = static_cast<uint8_t>((raw >> 8U) & 0x7FU);
            const uint8_t vt = static_cast<uint8_t>((raw >> 16U) & 0x7FU);
            uint32_t id[4], is[4], it[4];
            vfpu_vec_indices(vd, sz, id);
            vfpu_vec_indices(vs, sz, is);
            vfpu_vec_indices(vt, sz, it);
            if (sub == 0U) {  // vmul
                float res[4];
                for (uint32_t i = 0; i < sz; ++i) {
                    res[i] = cpu.vfpu[is[i] & 127U] * cpu.vfpu[it[i] & 127U];
                }
                for (uint32_t i = 0; i < sz; ++i)
                    cpu.vfpu[id[i] & 127U] = res[i];
            } else if (sub == 1U) {  // vdot
                float sum = 0.0f;
                for (uint32_t i = 0; i < sz; ++i) {
                    sum += cpu.vfpu[is[i] & 127U] * cpu.vfpu[it[i] & 127U];
                }
                cpu.vfpu[vfpu_reg_index(vd) & 127U] = sum;
            } else if (sub == 2U) {  // vscl
                const float s = cpu.vfpu[vfpu_reg_index(vt) & 127U];
                float res[4];
                for (uint32_t i = 0; i < sz; ++i)
                    res[i] = cpu.vfpu[is[i] & 127U] * s;
                for (uint32_t i = 0; i < sz; ++i)
                    cpu.vfpu[id[i] & 127U] = res[i];
            }
            break;
        }
        case dragon::mips::MipsOp::Vfpu4: {  // vmov / vabs / vneg / vsqrt / vrcp / vrsq / vsin /
                                             // vcos / vzero / vone / vidt
            const uint32_t sub = (raw >> 21U) & 0x1FU;
            const uint32_t op2 = (raw >> 16U) & 0x1FU;
            const uint32_t sz = 1U + ((raw >> 7U) & 1U) + (((raw >> 15U) & 1U) << 1U);
            const uint8_t vd = static_cast<uint8_t>(raw & 0x7FU);
            const uint8_t vs = static_cast<uint8_t>((raw >> 8U) & 0x7FU);
            uint32_t id[4], is[4];
            vfpu_vec_indices(vd, sz, id);
            vfpu_vec_indices(vs, sz, is);
            if (sub == 0x00U) {
                float res[4];
                for (uint32_t i = 0; i < sz; ++i) {
                    const float a = cpu.vfpu[is[i] & 127U];
                    switch (op2) {
                        case 0x00U:
                            res[i] = a;
                            break;  // vmov
                        case 0x01U:
                            res[i] = std::fabs(a);
                            break;  // vabs
                        case 0x02U:
                            res[i] = -a;
                            break;  // vneg
                        case 0x03U:
                            res[i] = (a >= 0.0f) ? 1.0f : -1.0f;
                            break;  // vsgn
                        case 0x10U:
                            res[i] = (a != 0.0f) ? (1.0f / a) : 0.0f;
                            break;  // vrcp
                        case 0x11U:
                            res[i] = (a > 0.0f) ? (1.0f / std::sqrt(a)) : 0.0f;
                            break;  // vrsq
                        case 0x12U:
                            res[i] = std::sin(a * 1.57079632679f);
                            break;  // vsin (half-pi)
                        case 0x13U:
                            res[i] = std::cos(a * 1.57079632679f);
                            break;  // vcos (half-pi)
                        case 0x16U:
                            res[i] = (a >= 0.0f) ? std::sqrt(a) : 0.0f;
                            break;  // vsqrt
                        default:
                            res[i] = a;
                            break;
                    }
                }
                for (uint32_t i = 0; i < sz; ++i)
                    cpu.vfpu[id[i] & 127U] = res[i];
            } else if (sub == 0x03U) {  // vzero / vone / vidt
                if (op2 == 0x06U) {     // vzero
                    for (uint32_t i = 0; i < sz; ++i)
                        cpu.vfpu[id[i] & 127U] = 0.0f;
                } else if (op2 == 0x07U) {  // vone
                    for (uint32_t i = 0; i < sz; ++i)
                        cpu.vfpu[id[i] & 127U] = 1.0f;
                } else if (op2 == 0x03U) {  // vidt
                    const uint32_t lane = vd & 3U;
                    for (uint32_t i = 0; i < sz; ++i) {
                        cpu.vfpu[id[i] & 127U] = (i == lane) ? 1.0f : 0.0f;
                    }
                }
            }
            break;
        }
        case dragon::mips::MipsOp::Cop2: {  // mfv / mtv
            const uint32_t sub = (raw >> 21U) & 0x1FU;
            const uint8_t vreg = static_cast<uint8_t>(raw & 0x7FU);
            if (sub == 0x03U) {  // mfv rt, vs
                if (d.rt != 0)
                    cpu.gpr[d.rt] = f32_to_u32(cpu.vfpu[vfpu_reg_index(vreg) & 127U]);
            } else if (sub == 0x07U) {  // mtv rt, vd
                cpu.vfpu[vfpu_reg_index(vreg) & 127U] = u32_to_f32(cpu.gpr[d.rt]);
            }
            break;
        }
        default:
            break;
    }
}

void exec_non_branch(PspRuntime& rt, PspCpuState& cpu,
                     const dragon::mips::DecodedMips& d) noexcept {
    using dragon::mips::MipsOp;
    PspBus& bus = rt.bus();
    auto set_gpr = [&](uint8_t reg, uint32_t val) noexcept {
        if (reg != 0)
            cpu.gpr[reg] = val;
    };

    switch (d.op) {
        case MipsOp::Nop:
        case MipsOp::Sync:
        case MipsOp::Cache:
        case MipsOp::Vflush:
        case MipsOp::Break:
            break;

        case MipsOp::Halt:
            cpu.halted = 1U;
            break;

        // Shifts
        case MipsOp::Sll:
            set_gpr(d.rd, cpu.gpr[d.rt] << d.sa);
            break;
        case MipsOp::Srl:
            set_gpr(d.rd, cpu.gpr[d.rt] >> d.sa);
            break;
        case MipsOp::Sra:
            set_gpr(d.rd, static_cast<uint32_t>(static_cast<int32_t>(cpu.gpr[d.rt]) >> d.sa));
            break;
        case MipsOp::Sllv:
            set_gpr(d.rd, cpu.gpr[d.rt] << (cpu.gpr[d.rs] & 31U));
            break;
        case MipsOp::Srlv:
            set_gpr(d.rd, cpu.gpr[d.rt] >> (cpu.gpr[d.rs] & 31U));
            break;
        case MipsOp::Srav:
            set_gpr(d.rd, static_cast<uint32_t>(static_cast<int32_t>(cpu.gpr[d.rt]) >>
                                                (cpu.gpr[d.rs] & 31U)));
            break;
        case MipsOp::Rotr:
            set_gpr(d.rd, rotr32(cpu.gpr[d.rt], d.sa));
            break;
        case MipsOp::Rotrv:
            set_gpr(d.rd, rotr32(cpu.gpr[d.rt], cpu.gpr[d.rs]));
            break;

        // ALU
        case MipsOp::Add:
        case MipsOp::Addu:
            set_gpr(d.rd, cpu.gpr[d.rs] + cpu.gpr[d.rt]);
            break;
        case MipsOp::Sub:
        case MipsOp::Subu:
            set_gpr(d.rd, cpu.gpr[d.rs] - cpu.gpr[d.rt]);
            break;
        case MipsOp::Addi:
        case MipsOp::Addiu:
            set_gpr(d.rt, cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm)));
            break;
        case MipsOp::And:
            set_gpr(d.rd, cpu.gpr[d.rs] & cpu.gpr[d.rt]);
            break;
        case MipsOp::Or:
            set_gpr(d.rd, cpu.gpr[d.rs] | cpu.gpr[d.rt]);
            break;
        case MipsOp::Xor:
            set_gpr(d.rd, cpu.gpr[d.rs] ^ cpu.gpr[d.rt]);
            break;
        case MipsOp::Nor:
            set_gpr(d.rd, ~(cpu.gpr[d.rs] | cpu.gpr[d.rt]));
            break;
        case MipsOp::Andi:
            set_gpr(d.rt, cpu.gpr[d.rs] & static_cast<uint32_t>(d.imm));
            break;
        case MipsOp::Ori:
            set_gpr(d.rt, cpu.gpr[d.rs] | static_cast<uint32_t>(d.imm));
            break;
        case MipsOp::Xori:
            set_gpr(d.rt, cpu.gpr[d.rs] ^ static_cast<uint32_t>(d.imm));
            break;
        case MipsOp::Lui:
            set_gpr(d.rt, static_cast<uint32_t>(d.imm) << 16U);
            break;

        case MipsOp::Slt:
            set_gpr(d.rd, static_cast<int32_t>(cpu.gpr[d.rs]) < static_cast<int32_t>(cpu.gpr[d.rt])
                              ? 1U
                              : 0U);
            break;
        case MipsOp::Sltu:
            set_gpr(d.rd, cpu.gpr[d.rs] < cpu.gpr[d.rt] ? 1U : 0U);
            break;
        case MipsOp::Slti:
            set_gpr(d.rt,
                    static_cast<int32_t>(cpu.gpr[d.rs]) < static_cast<int32_t>(d.simm) ? 1U : 0U);
            break;
        case MipsOp::Sltiu:
            set_gpr(d.rt,
                    cpu.gpr[d.rs] < static_cast<uint32_t>(static_cast<int32_t>(d.simm)) ? 1U : 0U);
            break;

        // Allegrex conditional moves & extensions
        case MipsOp::Movz:
            if (cpu.gpr[d.rt] == 0U)
                set_gpr(d.rd, cpu.gpr[d.rs]);
            break;
        case MipsOp::Movn:
            if (cpu.gpr[d.rt] != 0U)
                set_gpr(d.rd, cpu.gpr[d.rs]);
            break;
        case MipsOp::Clz:
            set_gpr(d.rd, static_cast<uint32_t>(std::countl_zero(cpu.gpr[d.rs])));
            break;
        case MipsOp::Clo:
            set_gpr(d.rd, static_cast<uint32_t>(std::countl_one(cpu.gpr[d.rs])));
            break;
        case MipsOp::Max:
            set_gpr(d.rd, static_cast<uint32_t>(std::max(static_cast<int32_t>(cpu.gpr[d.rs]),
                                                         static_cast<int32_t>(cpu.gpr[d.rt]))));
            break;
        case MipsOp::Min:
            set_gpr(d.rd, static_cast<uint32_t>(std::min(static_cast<int32_t>(cpu.gpr[d.rs]),
                                                         static_cast<int32_t>(cpu.gpr[d.rt]))));
            break;
        case MipsOp::Ext: {
            const uint32_t pos = d.sa;
            const uint32_t sz = static_cast<uint32_t>(d.rd) + 1U;
            const uint32_t mask = (sz >= 32U) ? 0xFFFFFFFFU : ((1U << sz) - 1U);
            set_gpr(d.rt, (cpu.gpr[d.rs] >> pos) & mask);
            break;
        }
        case MipsOp::Ins: {
            const uint32_t pos = d.sa;
            const uint32_t msb = d.rd;
            const uint32_t sz = (msb >= pos) ? (msb - pos + 1U) : 0U;
            if (sz > 0U) {
                const uint32_t mask = (sz >= 32U) ? 0xFFFFFFFFU : ((1U << sz) - 1U);
                const uint32_t cleared = cpu.gpr[d.rt] & ~(mask << pos);
                set_gpr(d.rt, cleared | ((cpu.gpr[d.rs] & mask) << pos));
            }
            break;
        }
        case MipsOp::Wsbh: {
            const uint32_t v = cpu.gpr[d.rt];
            set_gpr(d.rd, ((v & 0x00FF00FFU) << 8U) | ((v & 0xFF00FF00U) >> 8U));
            break;
        }
        case MipsOp::Wsbw: {
            const uint32_t v = cpu.gpr[d.rt];
            set_gpr(d.rd, ((v & 0x000000FFU) << 24U) | ((v & 0x0000FF00U) << 8U) |
                              ((v & 0x00FF0000U) >> 8U) | ((v & 0xFF000000U) >> 24U));
            break;
        }
        case MipsOp::Seb:
            set_gpr(d.rd, static_cast<uint32_t>(
                              static_cast<int32_t>(static_cast<int8_t>(cpu.gpr[d.rt]))));
            break;
        case MipsOp::Seh:
            set_gpr(d.rd, static_cast<uint32_t>(
                              static_cast<int32_t>(static_cast<int16_t>(cpu.gpr[d.rt]))));
            break;
        case MipsOp::Bitrev:
            set_gpr(d.rd, bitrev32(cpu.gpr[d.rt]));
            break;

        // Multiply / Divide / Accumulate
        case MipsOp::Mult: {
            const int64_t p = static_cast<int64_t>(static_cast<int32_t>(cpu.gpr[d.rs])) *
                              static_cast<int64_t>(static_cast<int32_t>(cpu.gpr[d.rt]));
            cpu.lo = static_cast<uint32_t>(p & 0xFFFFFFFFLL);
            cpu.hi = static_cast<uint32_t>((static_cast<uint64_t>(p) >> 32U) & 0xFFFFFFFFULL);
            break;
        }
        case MipsOp::Multu: {
            const uint64_t p =
                static_cast<uint64_t>(cpu.gpr[d.rs]) * static_cast<uint64_t>(cpu.gpr[d.rt]);
            cpu.lo = static_cast<uint32_t>(p & 0xFFFFFFFFULL);
            cpu.hi = static_cast<uint32_t>(p >> 32U);
            break;
        }
        case MipsOp::Madd: {
            int64_t acc = static_cast<int64_t>((static_cast<uint64_t>(cpu.hi) << 32U) | cpu.lo);
            acc += static_cast<int64_t>(static_cast<int32_t>(cpu.gpr[d.rs])) *
                   static_cast<int64_t>(static_cast<int32_t>(cpu.gpr[d.rt]));
            cpu.lo = static_cast<uint32_t>(acc & 0xFFFFFFFFLL);
            cpu.hi = static_cast<uint32_t>(static_cast<uint64_t>(acc) >> 32U);
            break;
        }
        case MipsOp::Maddu: {
            uint64_t acc = (static_cast<uint64_t>(cpu.hi) << 32U) | cpu.lo;
            acc += static_cast<uint64_t>(cpu.gpr[d.rs]) * static_cast<uint64_t>(cpu.gpr[d.rt]);
            cpu.lo = static_cast<uint32_t>(acc & 0xFFFFFFFFULL);
            cpu.hi = static_cast<uint32_t>(acc >> 32U);
            break;
        }
        case MipsOp::Msub: {
            int64_t acc = static_cast<int64_t>((static_cast<uint64_t>(cpu.hi) << 32U) | cpu.lo);
            acc -= static_cast<int64_t>(static_cast<int32_t>(cpu.gpr[d.rs])) *
                   static_cast<int64_t>(static_cast<int32_t>(cpu.gpr[d.rt]));
            cpu.lo = static_cast<uint32_t>(acc & 0xFFFFFFFFLL);
            cpu.hi = static_cast<uint32_t>(static_cast<uint64_t>(acc) >> 32U);
            break;
        }
        case MipsOp::Msubu: {
            uint64_t acc = (static_cast<uint64_t>(cpu.hi) << 32U) | cpu.lo;
            acc -= static_cast<uint64_t>(cpu.gpr[d.rs]) * static_cast<uint64_t>(cpu.gpr[d.rt]);
            cpu.lo = static_cast<uint32_t>(acc & 0xFFFFFFFFULL);
            cpu.hi = static_cast<uint32_t>(acc >> 32U);
            break;
        }
        case MipsOp::Div: {
            const int32_t a = static_cast<int32_t>(cpu.gpr[d.rs]);
            const int32_t b = static_cast<int32_t>(cpu.gpr[d.rt]);
            if (b != 0) {
                if (a == INT32_MIN && b == -1) {
                    cpu.lo = static_cast<uint32_t>(INT32_MIN);
                    cpu.hi = 0U;
                } else {
                    cpu.lo = static_cast<uint32_t>(a / b);
                    cpu.hi = static_cast<uint32_t>(a % b);
                }
            }
            break;
        }
        case MipsOp::Divu: {
            const uint32_t a = cpu.gpr[d.rs];
            const uint32_t b = cpu.gpr[d.rt];
            if (b != 0U) {
                cpu.lo = a / b;
                cpu.hi = a % b;
            }
            break;
        }
        case MipsOp::Mfhi:
            set_gpr(d.rd, cpu.hi);
            break;
        case MipsOp::Mthi:
            cpu.hi = cpu.gpr[d.rs];
            break;
        case MipsOp::Mflo:
            set_gpr(d.rd, cpu.lo);
            break;
        case MipsOp::Mtlo:
            cpu.lo = cpu.gpr[d.rs];
            break;

        // Loads & Stores
        case MipsOp::Lb: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            set_gpr(d.rt, static_cast<uint32_t>(
                              static_cast<int32_t>(static_cast<int8_t>(bus.read8(addr)))));
            break;
        }
        case MipsOp::Lbu: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            set_gpr(d.rt, bus.read8(addr));
            break;
        }
        case MipsOp::Lh: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            set_gpr(d.rt, static_cast<uint32_t>(
                              static_cast<int32_t>(static_cast<int16_t>(bus.read16(addr)))));
            break;
        }
        case MipsOp::Lhu: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            set_gpr(d.rt, bus.read16(addr));
            break;
        }
        case MipsOp::Lw:
        case MipsOp::Ll: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            set_gpr(d.rt, bus.read32(addr));
            break;
        }
        case MipsOp::Lwl: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            set_gpr(d.rt, psp_lwl(&cpu, addr, cpu.gpr[d.rt]));
            break;
        }
        case MipsOp::Lwr: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            set_gpr(d.rt, psp_lwr(&cpu, addr, cpu.gpr[d.rt]));
            break;
        }
        case MipsOp::Sb: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            bus.write8(addr, static_cast<uint8_t>(cpu.gpr[d.rt] & 0xFFU));
            break;
        }
        case MipsOp::Sh: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            bus.write16(addr, static_cast<uint16_t>(cpu.gpr[d.rt] & 0xFFFFU));
            break;
        }
        case MipsOp::Sw: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            bus.write32(addr, cpu.gpr[d.rt]);
            break;
        }
        case MipsOp::Sc: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            bus.write32(addr, cpu.gpr[d.rt]);
            set_gpr(d.rt, 1U);
            break;
        }
        case MipsOp::Swl: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            psp_swl(&cpu, addr, cpu.gpr[d.rt]);
            break;
        }
        case MipsOp::Swr: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            psp_swr(&cpu, addr, cpu.gpr[d.rt]);
            break;
        }

        // System
        case MipsOp::Syscall:
            rt.hle().handle_syscall(rt, cpu, d.syscall_code);
            break;
        case MipsOp::Mfic:
            set_gpr(d.rt, 1U);
            break;
        case MipsOp::Mtic:
        case MipsOp::Mfc0:
        case MipsOp::Mtc0:
            break;

        // COP1 FPU
        case MipsOp::Mfc1:
            set_gpr(d.rt, f32_to_u32(cpu.f[d.rd]));
            break;
        case MipsOp::Mtc1:
            cpu.f[d.rd] = u32_to_f32(cpu.gpr[d.rt]);
            break;
        case MipsOp::Cfc1:
            set_gpr(d.rt, (d.rd == 31) ? cpu.fcr31 : 0x00003351U);
            break;
        case MipsOp::Ctc1:
            if (d.rd == 31)
                cpu.fcr31 = cpu.gpr[d.rt];
            break;
        case MipsOp::Lwc1: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            cpu.f[d.rt] = u32_to_f32(bus.read32(addr));
            break;
        }
        case MipsOp::Swc1: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            bus.write32(addr, f32_to_u32(cpu.f[d.rt]));
            break;
        }
        case MipsOp::AddS:
            cpu.f[d.sa] = cpu.f[d.rd] + cpu.f[d.rt];
            break;
        case MipsOp::SubS:
            cpu.f[d.sa] = cpu.f[d.rd] - cpu.f[d.rt];
            break;
        case MipsOp::MulS:
            cpu.f[d.sa] = cpu.f[d.rd] * cpu.f[d.rt];
            break;
        case MipsOp::DivS:
            cpu.f[d.sa] = cpu.f[d.rd] / cpu.f[d.rt];
            break;
        case MipsOp::SqrtS:
            cpu.f[d.sa] = std::sqrt(cpu.f[d.rd]);
            break;
        case MipsOp::AbsS:
            cpu.f[d.sa] = std::fabs(cpu.f[d.rd]);
            break;
        case MipsOp::MovS:
            cpu.f[d.sa] = cpu.f[d.rd];
            break;
        case MipsOp::NegS:
            cpu.f[d.sa] = -cpu.f[d.rd];
            break;
        case MipsOp::RoundWS:
            cpu.f[d.sa] = u32_to_f32(
                static_cast<uint32_t>(static_cast<int32_t>(std::nearbyint(cpu.f[d.rd]))));
            break;
        case MipsOp::TruncWS:
        case MipsOp::CvtWS:
            cpu.f[d.sa] =
                u32_to_f32(static_cast<uint32_t>(static_cast<int32_t>(std::trunc(cpu.f[d.rd]))));
            break;
        case MipsOp::CeilWS:
            cpu.f[d.sa] =
                u32_to_f32(static_cast<uint32_t>(static_cast<int32_t>(std::ceil(cpu.f[d.rd]))));
            break;
        case MipsOp::FloorWS:
            cpu.f[d.sa] =
                u32_to_f32(static_cast<uint32_t>(static_cast<int32_t>(std::floor(cpu.f[d.rd]))));
            break;
        case MipsOp::CvtSW:
            cpu.f[d.sa] = static_cast<float>(static_cast<int32_t>(f32_to_u32(cpu.f[d.rd])));
            break;
        case MipsOp::CCondS: {
            const float a = cpu.f[d.rd];
            const float b = cpu.f[d.rt];
            const bool unord = std::isnan(a) || std::isnan(b);
            bool cond = false;
            if (unord) {
                cond = (d.funct & 1U) != 0U;
            } else {
                const bool eq = (a == b);
                const bool lt = (a < b);
                cond = (((d.funct & 2U) != 0U) && eq) || (((d.funct & 4U) != 0U) && lt);
            }
            if (cond)
                cpu.fcr31 |= (1U << 23U);
            else
                cpu.fcr31 &= ~(1U << 23U);
            break;
        }

        // VFPU & COP2
        case MipsOp::Lvs:
        case MipsOp::Svs:
        case MipsOp::Lvq:
        case MipsOp::Svq:
        case MipsOp::Vfpu0:
        case MipsOp::Vfpu1:
        case MipsOp::Vfpu3:
        case MipsOp::Vfpu4:
        case MipsOp::Vfpu5:
        case MipsOp::Vfpu6:
        case MipsOp::Cop2:
            exec_vfpu_core(rt, cpu, d);
            break;

        default:
            break;
    }
    cpu.gpr[0] = 0U;
}

}  // namespace

void PspInterpreter::exec_single_non_branch(PspRuntime& rt, PspCpuState& cpu, uint32_t raw,
                                            uint32_t pc) noexcept {
    const auto d = dragon::mips::decode_mips(raw, pc, dragon::mips::MipsProfile::Allegrex);
    exec_non_branch(rt, cpu, d);
    ++cpu.cycles;
}

void PspInterpreter::step(PspRuntime& rt, PspCpuState& cpu) noexcept {
    using dragon::mips::MipsOp;
    const uint32_t pc = cpu.pc;
    const uint32_t raw = rt.bus().read32(pc);
    const auto d = dragon::mips::decode_mips(raw, pc, dragon::mips::MipsProfile::Allegrex);

    // Advance pipeline: current npc becomes next pc, npc advances to npc + 4 unless branch/jump
    const uint32_t cur_npc = cpu.npc;
    uint32_t next_pc = cur_npc;
    uint32_t next_npc = cur_npc + 4U;

    if (!d.has_delay_slot) {
        exec_non_branch(rt, cpu, d);
        cpu.pc = next_pc;
        cpu.npc = next_npc;
        cpu.gpr[0] = 0U;
        ++cpu.cycles;
        return;
    }

    // Evaluate branch / jump condition
    bool take = false;
    uint32_t target = d.target;
    const uint32_t rs_v = cpu.gpr[d.rs];
    const uint32_t rt_v = cpu.gpr[d.rt];
    const int32_t rs_s = static_cast<int32_t>(rs_v);
    const bool fcc = ((cpu.fcr31 >> 23U) & 1U) != 0U;

    switch (d.op) {
        case MipsOp::J:
            take = true;
            break;
        case MipsOp::Jal:
            take = true;
            cpu.gpr[31] = pc + 8U;
            break;
        case MipsOp::Jr:
            take = true;
            target = rs_v;
            break;
        case MipsOp::Jalr:
            take = true;
            target = rs_v;
            if (d.rd != 0)
                cpu.gpr[d.rd] = pc + 8U;
            break;
        case MipsOp::Beq:
        case MipsOp::Beql:
            take = (rs_v == rt_v);
            break;
        case MipsOp::Bne:
        case MipsOp::Bnel:
            take = (rs_v != rt_v);
            break;
        case MipsOp::Blez:
        case MipsOp::Blezl:
            take = (rs_s <= 0);
            break;
        case MipsOp::Bgtz:
        case MipsOp::Bgtzl:
            take = (rs_s > 0);
            break;
        case MipsOp::Bltz:
        case MipsOp::Bltzl:
            take = (rs_s < 0);
            break;
        case MipsOp::Bgez:
        case MipsOp::Bgezl:
            take = (rs_s >= 0);
            break;
        case MipsOp::Bltzal:
        case MipsOp::Bltzall:
            cpu.gpr[31] = pc + 8U;
            take = (rs_s < 0);
            break;
        case MipsOp::Bgezal:
        case MipsOp::Bgezall:
            cpu.gpr[31] = pc + 8U;
            take = (rs_s >= 0);
            break;
        case MipsOp::Bc1f:
        case MipsOp::Bc1fl:
            take = !fcc;
            break;
        case MipsOp::Bc1t:
        case MipsOp::Bc1tl:
            take = fcc;
            break;
        default:
            break;
    }

    if (take) {
        // Execute delay slot at cur_npc, then jump to target
        cpu.pc = cur_npc;
        cpu.npc = target;
    } else if (d.is_branch_likely) {
        // Branch-likely NOT taken: nullify delay slot!
        cpu.pc = pc + 8U;
        cpu.npc = pc + 12U;
    } else {
        // Normal branch NOT taken: execute delay slot then continue sequentially
        cpu.pc = cur_npc;
        cpu.npc = cur_npc + 4U;
    }

    cpu.gpr[0] = 0U;
    ++cpu.cycles;
}

uint64_t PspInterpreter::run_slice(PspRuntime& rt, PspCpuState& cpu,
                                   uint64_t max_instructions) noexcept {
    uint64_t executed = 0;
    while (executed < max_instructions && !cpu.halted && !cpu.vblank_yield) {
        if (cpu.pc == 0x08800000U || cpu.pc == 0U) {
            cpu.halted = 1U;
            break;
        }
        step(rt, cpu);
        ++executed;
    }
    return executed;
}

void PspInterpreter::run_until_return(PspRuntime& rt, PspCpuState& cpu, uint32_t target_pc,
                                      uint32_t return_pc, uint64_t max_instructions) noexcept {
    cpu.pc = target_pc;
    cpu.npc = target_pc + 4U;
    uint64_t executed = 0;
    while (executed < max_instructions && !cpu.halted && !cpu.vblank_yield) {
        if (cpu.pc == return_pc || cpu.pc == 0x08800000U || cpu.pc == 0U) {
            break;
        }
        step(rt, cpu);
        ++executed;
    }
}

}  // namespace dragon::psp

// =========================================================================
// extern "C" Runtime Helpers for C99 Static Recompiled Code
// =========================================================================
extern "C" {

static inline dragon::psp::PspRuntime* get_rt(PspCpuState* cpu) noexcept {
    return static_cast<dragon::psp::PspRuntime*>(cpu->runtime);
}

uint8_t psp_read8(PspCpuState* cpu, uint32_t addr) {
    return get_rt(cpu)->bus().read8(addr);
}

uint16_t psp_read16(PspCpuState* cpu, uint32_t addr) {
    return get_rt(cpu)->bus().read16(addr);
}

uint32_t psp_read32(PspCpuState* cpu, uint32_t addr) {
    return get_rt(cpu)->bus().read32(addr);
}

void psp_write8(PspCpuState* cpu, uint32_t addr, uint8_t val) {
    get_rt(cpu)->bus().write8(addr, val);
}

void psp_write16(PspCpuState* cpu, uint32_t addr, uint16_t val) {
    get_rt(cpu)->bus().write16(addr, val);
}

void psp_write32(PspCpuState* cpu, uint32_t addr, uint32_t val) {
    get_rt(cpu)->bus().write32(addr, val);
}

uint32_t psp_lwl(PspCpuState* cpu, uint32_t addr, uint32_t rt_val) {
    const uint32_t word = get_rt(cpu)->bus().read32(addr & ~3U);
    switch (addr & 3U) {
        case 0:
            return (rt_val & 0x00FFFFFFU) | (word << 24U);
        case 1:
            return (rt_val & 0x0000FFFFU) | (word << 16U);
        case 2:
            return (rt_val & 0x000000FFU) | (word << 8U);
        default:
            return word;
    }
}

uint32_t psp_lwr(PspCpuState* cpu, uint32_t addr, uint32_t rt_val) {
    const uint32_t word = get_rt(cpu)->bus().read32(addr & ~3U);
    switch (addr & 3U) {
        case 0:
            return word;
        case 1:
            return (rt_val & 0xFF000000U) | (word >> 8U);
        case 2:
            return (rt_val & 0xFFFF0000U) | (word >> 16U);
        default:
            return (rt_val & 0xFFFFFF00U) | (word >> 24U);
    }
}

void psp_swl(PspCpuState* cpu, uint32_t addr, uint32_t rt_val) {
    auto& bus = get_rt(cpu)->bus();
    const uint32_t aligned = addr & ~3U;
    uint32_t word = bus.read32(aligned);
    switch (addr & 3U) {
        case 0:
            word = (word & 0xFFFFFF00U) | (rt_val >> 24U);
            break;
        case 1:
            word = (word & 0xFFFF0000U) | (rt_val >> 16U);
            break;
        case 2:
            word = (word & 0xFF000000U) | (rt_val >> 8U);
            break;
        default:
            word = rt_val;
            break;
    }
    bus.write32(aligned, word);
}

void psp_swr(PspCpuState* cpu, uint32_t addr, uint32_t rt_val) {
    auto& bus = get_rt(cpu)->bus();
    const uint32_t aligned = addr & ~3U;
    uint32_t word = bus.read32(aligned);
    switch (addr & 3U) {
        case 0:
            word = rt_val;
            break;
        case 1:
            word = (word & 0x000000FFU) | (rt_val << 8U);
            break;
        case 2:
            word = (word & 0x0000FFFFU) | (rt_val << 16U);
            break;
        default:
            word = (word & 0x00FFFFFFU) | (rt_val << 24U);
            break;
    }
    bus.write32(aligned, word);
}

void psp_hle_syscall(PspCpuState* cpu, uint32_t code) {
    auto* rt = get_rt(cpu);
    rt->hle().handle_syscall(*rt, *cpu, code);
}

void psp_exec_fallback_insn(PspCpuState* cpu, uint32_t raw, uint32_t pc) {
    auto* rt = get_rt(cpu);
    dragon::psp::PspInterpreter::exec_single_non_branch(*rt, *cpu, raw, pc);
}

void psp_runtime_dispatch(PspCpuState* cpu, uint32_t target_pc) {
    auto* rt = get_rt(cpu);
    if (target_pc == 0x08800000U || target_pc == 0U) {
        cpu->halted = 1U;
        return;
    }
    if (auto fn = rt->lookup_recomp(target_pc)) {
        rt->note_recomp_call();
        fn(cpu);
        return;
    }
    // Check if target is an imported HLE stub (`jr $ra; syscall <code>`)
    const uint32_t w0 = rt->bus().read32(target_pc);
    const uint32_t w1 = rt->bus().read32(target_pc + 4U);
    if (w0 == 0x03E00008U && (w1 & 0xFC00003FU) == 0x0000000CU) {
        rt->hle().handle_syscall(*rt, *cpu, (w1 >> 6U) & 0xFFFFFU);
        return;
    }
    // Transparent interpreter fallback for uncompiled targets
    rt->note_interp_fallback();
    const uint32_t ret_pc = cpu->gpr[31];
    dragon::psp::PspInterpreter::run_until_return(*rt, *cpu, target_pc, ret_pc);
}

}  // extern "C"
