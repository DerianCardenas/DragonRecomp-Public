#include "nds/interpreter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dragon::nds {

namespace {

inline uint8_t mem_r8(NdsCpuState& cpu, Bus& bus, uint32_t addr) noexcept {
    return cpu.is_arm9 ? bus.arm9_read8(addr, &cpu) : bus.arm7_read8(addr);
}
inline uint16_t mem_r16(NdsCpuState& cpu, Bus& bus, uint32_t addr) noexcept {
    return cpu.is_arm9 ? bus.arm9_read16(addr, &cpu) : bus.arm7_read16(addr);
}
inline uint32_t mem_r32(NdsCpuState& cpu, Bus& bus, uint32_t addr) noexcept {
    return cpu.is_arm9 ? bus.arm9_read32(addr, &cpu) : bus.arm7_read32(addr);
}
inline void mem_w8(NdsCpuState& cpu, Bus& bus, uint32_t addr, uint8_t v) noexcept {
    if (cpu.is_arm9)
        bus.arm9_write8(addr, v, &cpu);
    else
        bus.arm7_write8(addr, v);
}
inline void mem_w16(NdsCpuState& cpu, Bus& bus, uint32_t addr, uint16_t v) noexcept {
    if (cpu.is_arm9)
        bus.arm9_write16(addr, v, &cpu);
    else
        bus.arm7_write16(addr, v);
}
inline void mem_w32(NdsCpuState& cpu, Bus& bus, uint32_t addr, uint32_t v) noexcept {
    if (cpu.is_arm9)
        bus.arm9_write32(addr, v, &cpu);
    else
        bus.arm7_write32(addr, v);
}

inline uint32_t read_reg(const NdsCpuState& cpu, uint32_t reg, uint32_t pc_val) noexcept {
    return (reg == 15U) ? pc_val : cpu.r[reg];
}

inline uint32_t shift_imm(uint32_t val, uint32_t type, uint32_t amt, uint8_t carry_in,
                          uint8_t& carry_out) noexcept {
    carry_out = carry_in;
    switch (type & 3U) {
        case 0U:  // LSL
            if (amt == 0U)
                return val;
            carry_out = static_cast<uint8_t>((val >> (32U - amt)) & 1U);
            return val << amt;
        case 1U:  // LSR
            if (amt == 0U) {
                carry_out = static_cast<uint8_t>(val >> 31U);
                return 0U;
            }
            carry_out = static_cast<uint8_t>((val >> (amt - 1U)) & 1U);
            return val >> amt;
        case 2U:  // ASR
            if (amt == 0U) {
                carry_out = static_cast<uint8_t>(val >> 31U);
                return (val & 0x80000000U) ? 0xFFFFFFFFU : 0U;
            }
            carry_out = static_cast<uint8_t>((val >> (amt - 1U)) & 1U);
            return static_cast<uint32_t>(static_cast<int32_t>(val) >> amt);
        case 3U:  // ROR / RRX
            if (amt == 0U) {
                carry_out = static_cast<uint8_t>(val & 1U);
                return (static_cast<uint32_t>(carry_in) << 31U) | (val >> 1U);
            }
            carry_out = static_cast<uint8_t>((val >> (amt - 1U)) & 1U);
            return nds_ror32(val, amt);
        default:
            return val;
    }
}

inline uint32_t shift_reg(uint32_t val, uint32_t type, uint32_t amt, uint8_t carry_in,
                          uint8_t& carry_out) noexcept {
    carry_out = carry_in;
    amt &= 0xFFU;
    if (amt == 0U)
        return val;
    switch (type & 3U) {
        case 0U:  // LSL
            if (amt < 32U) {
                carry_out = static_cast<uint8_t>((val >> (32U - amt)) & 1U);
                return val << amt;
            }
            if (amt == 32U) {
                carry_out = static_cast<uint8_t>(val & 1U);
                return 0U;
            }
            carry_out = 0U;
            return 0U;
        case 1U:  // LSR
            if (amt < 32U) {
                carry_out = static_cast<uint8_t>((val >> (amt - 1U)) & 1U);
                return val >> amt;
            }
            if (amt == 32U) {
                carry_out = static_cast<uint8_t>(val >> 31U);
                return 0U;
            }
            carry_out = 0U;
            return 0U;
        case 2U:  // ASR
            if (amt < 32U) {
                carry_out = static_cast<uint8_t>((val >> (amt - 1U)) & 1U);
                return static_cast<uint32_t>(static_cast<int32_t>(val) >> amt);
            }
            carry_out = static_cast<uint8_t>(val >> 31U);
            return (val & 0x80000000U) ? 0xFFFFFFFFU : 0U;
        case 3U: {  // ROR
            const uint32_t r = amt & 31U;
            if (r == 0U) {
                carry_out = static_cast<uint8_t>(val >> 31U);
                return val;
            }
            carry_out = static_cast<uint8_t>((val >> (r - 1U)) & 1U);
            return nds_ror32(val, r);
        }
        default:
            return val;
    }
}

void write_pc_interwork(NdsCpuState& cpu, uint32_t val, bool allow_interwork) noexcept {
    if (allow_interwork && cpu.is_arm9) {
        cpu.thumb = static_cast<uint8_t>(val & 1U);
    }
    cpu.r[15] = cpu.thumb ? (val & ~1U) : (val & ~3U);
}

void exec_arm(NdsCpuState& cpu, Bus& bus, uint32_t insn, uint32_t pc_cur) noexcept {
    const uint32_t cond = insn >> 28U;
    const uint32_t pc8 = pc_cur + 8U;

    // Unconditional ARMv5TE space (cond == 0xF)
    if (cond == 0xFU) {
        if (cpu.is_arm9 && ((insn & 0xFE000000U) == 0xFA000000U)) {
            // BLX <label> (ARM -> Thumb)
            int32_t off = static_cast<int32_t>(insn & 0x00FFFFFFU);
            if (off & 0x00800000)
                off |= static_cast<int32_t>(0xFF000000U);
            const uint32_t h = (insn >> 24U) & 1U;
            cpu.r[14] = pc_cur + 4U;
            cpu.thumb = 1U;
            cpu.r[15] = static_cast<uint32_t>(static_cast<int32_t>(pc8) + (off << 2) +
                                              static_cast<int32_t>(h << 1));
        }
        // PLD and other 0xF hints are NOPs
        return;
    }

    if (!nds_cond_passed(&cpu, cond)) {
        return;
    }

    // Branch / Branch with Link: 101L
    if ((insn & 0x0E000000U) == 0x0A000000U) {
        int32_t off = static_cast<int32_t>(insn & 0x00FFFFFFU);
        if (off & 0x00800000)
            off |= static_cast<int32_t>(0xFF000000U);
        if (insn & 0x01000000U) {
            cpu.r[14] = pc_cur + 4U;
        }
        cpu.r[15] = static_cast<uint32_t>(static_cast<int32_t>(pc8) + (off << 2));
        return;
    }

    // SWI
    if ((insn & 0x0F000000U) == 0x0F000000U) {
        const uint8_t num = static_cast<uint8_t>((insn >> 16U) & 0xFFU);
        cpu_handle_swi(cpu, bus, num);
        return;
    }

    // Coprocessor MRC / MCR (CP15 on ARM9)
    if ((insn & 0x0F000010U) == 0x0E000010U) {
        const uint32_t cp_num = (insn >> 8U) & 0xFU;
        if (cp_num == 15U && cpu.is_arm9) {
            const uint32_t cn = (insn >> 16U) & 0xFU;
            const uint32_t rd = (insn >> 12U) & 0xFU;
            const uint32_t cp = (insn >> 5U) & 7U;
            const uint32_t cm = insn & 0xFU;
            if (insn & (1U << 20)) {
                const uint32_t v = nds_cp15_read(&cpu, cn, cm, cp);
                if (rd == 15U) {
                    cpu.n = static_cast<uint8_t>((v >> 31U) & 1U);
                    cpu.z = static_cast<uint8_t>((v >> 30U) & 1U);
                    cpu.c = static_cast<uint8_t>((v >> 29U) & 1U);
                    cpu.v = static_cast<uint8_t>((v >> 28U) & 1U);
                } else {
                    cpu.r[rd] = v;
                }
            } else {
                nds_cp15_write(&cpu, cn, cm, cp, read_reg(cpu, rd, pc8));
            }
        }
        return;
    }

    // BX / BLX Rm
    if ((insn & 0x0FFFFFF0U) == 0x012FFF10U ||
        (cpu.is_arm9 && ((insn & 0x0FFFFFF0U) == 0x012FFF30U))) {
        const uint32_t rm = insn & 0xFU;
        const uint32_t target = read_reg(cpu, rm, pc8);
        if ((insn & 0x30U) == 0x30U) {
            cpu.r[14] = pc_cur + 4U;
        }
        cpu.thumb = static_cast<uint8_t>(target & 1U);
        cpu.r[15] = cpu.thumb ? (target & ~1U) : (target & ~3U);
        return;
    }

    // CLZ (ARMv5TE)
    if (cpu.is_arm9 && ((insn & 0x0FFF0FF0U) == 0x016F0F10U)) {
        const uint32_t rd = (insn >> 12U) & 0xFU;
        const uint32_t rm = insn & 0xFU;
        cpu.r[rd] = nds_clz32(read_reg(cpu, rm, pc8));
        return;
    }

    // QADD / QSUB / QDADD / QDSUB (ARMv5TE)
    if (cpu.is_arm9 && ((insn & 0x0F900FF0U) == 0x01000050U)) {
        const uint32_t op = (insn >> 21U) & 3U;
        const uint32_t rn = (insn >> 16U) & 0xFU;
        const uint32_t rd = (insn >> 12U) & 0xFU;
        const uint32_t rm = insn & 0xFU;
        int64_t a = static_cast<int32_t>(cpu.r[rm]);
        int64_t b = static_cast<int32_t>(cpu.r[rn]);
        if (op == 2U || op == 3U) {
            b = nds_sat32(&cpu, b * 2LL);
        }
        const int64_t res = (op == 0U || op == 2U) ? (a + b) : (a - b);
        cpu.r[rd] = static_cast<uint32_t>(nds_sat32(&cpu, res));
        return;
    }

    // Signed 16x16 / 32x16 DSP Multiplies (ARMv5TE): bits[27:25]=000, [24:23]=10, [20]=0, [7]=1,
    // [4]=0
    if (cpu.is_arm9 && ((insn & 0x0F900090U) == 0x01000080U)) {
        const uint32_t op = (insn >> 21U) & 3U;
        const uint32_t rd = (insn >> 16U) & 0xFU;
        const uint32_t rn = (insn >> 12U) & 0xFU;
        const uint32_t rs = (insn >> 8U) & 0xFU;
        const uint32_t rm = insn & 0xFU;
        const bool x_hi = (insn & (1U << 5)) != 0U;
        const bool y_hi = (insn & (1U << 6)) != 0U;
        const int16_t sx = static_cast<int16_t>(x_hi ? (cpu.r[rm] >> 16U) : (cpu.r[rm] & 0xFFFFU));
        const int16_t sy = static_cast<int16_t>(y_hi ? (cpu.r[rs] >> 16U) : (cpu.r[rs] & 0xFFFFU));

        if (op == 0U) {  // SMLAxy
            const int64_t p = static_cast<int64_t>(sx) * sy + static_cast<int32_t>(cpu.r[rn]);
            if (p > 0x7FFFFFFFLL || p < -0x80000000LL)
                cpu.q = 1U;
            cpu.r[rd] = static_cast<uint32_t>(p);
        } else if (op == 1U) {
            const int64_t p = (static_cast<int64_t>(static_cast<int32_t>(cpu.r[rm])) * sy) >> 16;
            if (!x_hi) {  // SMLAWy
                const int64_t acc = p + static_cast<int32_t>(cpu.r[rn]);
                if (acc > 0x7FFFFFFFLL || acc < -0x80000000LL)
                    cpu.q = 1U;
                cpu.r[rd] = static_cast<uint32_t>(acc);
            } else {  // SMULWy
                cpu.r[rd] = static_cast<uint32_t>(p);
            }
        } else if (op == 2U) {  // SMLALxy
            const int64_t cur =
                static_cast<int64_t>((static_cast<uint64_t>(cpu.r[rd]) << 32U) | cpu.r[rn]);
            const int64_t res = cur + static_cast<int64_t>(sx) * sy;
            cpu.r[rn] = static_cast<uint32_t>(res & 0xFFFFFFFFULL);
            cpu.r[rd] = static_cast<uint32_t>(static_cast<uint64_t>(res) >> 32U);
        } else {  // SMULxy
            cpu.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(sx) * sy);
        }
        return;
    }

    // SWP / SWPB
    if ((insn & 0x0FB00FF0U) == 0x01000090U) {
        const bool is_byte = (insn & (1U << 22)) != 0U;
        const uint32_t rn = (insn >> 16U) & 0xFU;
        const uint32_t rd = (insn >> 12U) & 0xFU;
        const uint32_t rm = insn & 0xFU;
        const uint32_t addr = cpu.r[rn];
        if (is_byte) {
            const uint8_t old = mem_r8(cpu, bus, addr);
            mem_w8(cpu, bus, addr, static_cast<uint8_t>(cpu.r[rm] & 0xFFU));
            cpu.r[rd] = old;
        } else {
            const uint32_t old = nds_ldr32(&cpu, addr);
            mem_w32(cpu, bus, addr & ~3U, cpu.r[rm]);
            cpu.r[rd] = old;
        }
        return;
    }

    // Multiply (MUL/MLA)
    if ((insn & 0x0FC000F0U) == 0x00000090U) {
        const bool acc = (insn & (1U << 21)) != 0U;
        const bool set_f = (insn & (1U << 20)) != 0U;
        const uint32_t rd = (insn >> 16U) & 0xFU;
        const uint32_t rn = (insn >> 12U) & 0xFU;
        const uint32_t rs = (insn >> 8U) & 0xFU;
        const uint32_t rm = insn & 0xFU;
        uint32_t res = cpu.r[rm] * cpu.r[rs];
        if (acc)
            res += cpu.r[rn];
        cpu.r[rd] = res;
        if (set_f)
            nds_set_nz(&cpu, res);
        return;
    }

    // Multiply Long (UMULL/UMLAL/SMULL/SMLAL)
    if ((insn & 0x0F8000F0U) == 0x00800090U) {
        const bool is_signed = (insn & (1U << 22)) != 0U;
        const bool acc = (insn & (1U << 21)) != 0U;
        const bool set_f = (insn & (1U << 20)) != 0U;
        const uint32_t rd_hi = (insn >> 16U) & 0xFU;
        const uint32_t rd_lo = (insn >> 12U) & 0xFU;
        const uint32_t rs = (insn >> 8U) & 0xFU;
        const uint32_t rm = insn & 0xFU;
        uint64_t res =
            is_signed
                ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(cpu.r[rm])) *
                                        static_cast<int64_t>(static_cast<int32_t>(cpu.r[rs])))
                : (static_cast<uint64_t>(cpu.r[rm]) * static_cast<uint64_t>(cpu.r[rs]));
        if (acc) {
            res += (static_cast<uint64_t>(cpu.r[rd_hi]) << 32U) | cpu.r[rd_lo];
        }
        cpu.r[rd_lo] = static_cast<uint32_t>(res & 0xFFFFFFFFULL);
        cpu.r[rd_hi] = static_cast<uint32_t>(res >> 32U);
        if (set_f) {
            cpu.n = static_cast<uint8_t>((res >> 63U) & 1U);
            cpu.z = static_cast<uint8_t>(res == 0ULL);
        }
        return;
    }

    // Halfword / Signed / Doubleword Data Transfer (bits[27:25]=000, bit[7]=1, bit[4]=1)
    if ((insn & 0x0E000090U) == 0x00000090U) {
        const bool p = (insn & (1U << 24)) != 0U;
        const bool u = (insn & (1U << 23)) != 0U;
        const bool i_imm = (insn & (1U << 22)) != 0U;
        const bool w = (insn & (1U << 21)) != 0U;
        const bool l = (insn & (1U << 20)) != 0U;
        const uint32_t rn = (insn >> 16U) & 0xFU;
        const uint32_t rd = (insn >> 12U) & 0xFU;
        const uint32_t sh = (insn >> 5U) & 3U;
        const uint32_t off = i_imm ? (((insn >> 4U) & 0xF0U) | (insn & 0x0FU)) : cpu.r[insn & 0xFU];
        const uint32_t base = read_reg(cpu, rn, pc8);
        const uint32_t ea = u ? (base + off) : (base - off);
        const uint32_t addr = p ? ea : base;

        if (!l && (sh == 2U || sh == 3U) && cpu.is_arm9) {
            // LDRD (sh==2) / STRD (sh==3)
            if (sh == 2U) {
                cpu.r[rd] = mem_r32(cpu, bus, addr & ~3U);
                cpu.r[(rd + 1U) & 15U] = mem_r32(cpu, bus, (addr & ~3U) + 4U);
            } else {
                mem_w32(cpu, bus, addr & ~3U, cpu.r[rd]);
                mem_w32(cpu, bus, (addr & ~3U) + 4U, cpu.r[(rd + 1U) & 15U]);
            }
            if ((!p || w) && rn != 15U)
                cpu.r[rn] = ea;
            return;
        }

        if (l) {
            uint32_t val = 0U;
            if (sh == 1U)
                val = nds_ldrh16(&cpu, addr);
            else if (sh == 2U)
                val = nds_ldrsb8(&cpu, addr);
            else if (sh == 3U)
                val = nds_ldrsh16(&cpu, addr);
            if ((!p || w) && rn != rd)
                cpu.r[rn] = ea;
            if (rd == 15U)
                write_pc_interwork(cpu, val, false);
            else
                cpu.r[rd] = val;
        } else {
            const uint32_t val = read_reg(cpu, rd, pc_cur + 12U);
            if (sh == 1U)
                mem_w16(cpu, bus, addr & ~1U, static_cast<uint16_t>(val & 0xFFFFU));
            if (!p || w)
                cpu.r[rn] = ea;
        }
        return;
    }

    // MRS / MSR
    if ((insn & 0x0FBF0FFFU) == 0x010F0000U) {
        const bool use_spsr = (insn & (1U << 22)) != 0U;
        const uint32_t rd = (insn >> 12U) & 0xFU;
        cpu.r[rd] = use_spsr ? cpu.spsr : nds_pack_cpsr(&cpu);
        return;
    }
    if ((insn & 0x0DB0F000U) == 0x0120F000U) {
        const bool is_imm = (insn & (1U << 25)) != 0U;
        const bool use_spsr = (insn & (1U << 22)) != 0U;
        uint32_t val =
            is_imm ? nds_ror32(insn & 0xFFU, ((insn >> 8U) & 0xFU) * 2U) : cpu.r[insn & 0xFU];
        uint32_t mask = 0U;
        if (insn & (1U << 16))
            mask |= 0x000000FFU;
        if (insn & (1U << 17))
            mask |= 0x0000FF00U;
        if (insn & (1U << 18))
            mask |= 0x00FF0000U;
        if (insn & (1U << 19))
            mask |= 0xFF000000U;
        if (use_spsr) {
            cpu.spsr = (cpu.spsr & ~mask) | (val & mask);
        } else {
            const uint32_t cur = nds_pack_cpsr(&cpu);
            const uint32_t next = (cur & ~mask) | (val & mask);
            if (mask & 0x1FU)
                nds_switch_mode(&cpu, next & 0x1FU);
            nds_unpack_cpsr(&cpu, next);
        }
        return;
    }

    // Data Processing (bits[27:26] == 00)
    if ((insn & 0x0C000000U) == 0x00000000U) {
        const bool is_imm = (insn & (1U << 25)) != 0U;
        const uint32_t op = (insn >> 21U) & 0xFU;
        const bool set_f = (insn & (1U << 20)) != 0U;
        const uint32_t rn = (insn >> 16U) & 0xFU;
        const uint32_t rd = (insn >> 12U) & 0xFU;

        uint8_t sh_c = cpu.c;
        uint32_t op2 = 0U;
        uint32_t op1 = read_reg(cpu, rn, pc8);

        if (is_imm) {
            const uint32_t rot = ((insn >> 8U) & 0xFU) * 2U;
            op2 = nds_ror32(insn & 0xFFU, rot);
            if (rot != 0U)
                sh_c = static_cast<uint8_t>(op2 >> 31U);
        } else {
            const uint32_t rm = insn & 0xFU;
            const uint32_t stype = (insn >> 5U) & 3U;
            if (insn & (1U << 4)) {
                const uint32_t rs = (insn >> 8U) & 0xFU;
                op1 = read_reg(cpu, rn, pc_cur + 12U);
                op2 = shift_reg(read_reg(cpu, rm, pc_cur + 12U), stype, cpu.r[rs], cpu.c, sh_c);
            } else {
                op2 = shift_imm(read_reg(cpu, rm, pc8), stype, (insn >> 7U) & 0x1FU, cpu.c, sh_c);
            }
        }

        const bool flag_wr = set_f && (rd != 15U);
        uint32_t res = 0U;
        bool write_rd = true;

        switch (op) {
            case 0x0U:
                res = op1 & op2;
                break;
            case 0x1U:
                res = op1 ^ op2;
                break;
            case 0x2U:
                res = nds_sub_flags(&cpu, op1, op2, flag_wr ? 1 : 0);
                break;
            case 0x3U:
                res = nds_sub_flags(&cpu, op2, op1, flag_wr ? 1 : 0);
                break;
            case 0x4U:
                res = nds_add_flags(&cpu, op1, op2, flag_wr ? 1 : 0);
                break;
            case 0x5U:
                res = nds_adc_flags(&cpu, op1, op2, flag_wr ? 1 : 0);
                break;
            case 0x6U:
                res = nds_sbc_flags(&cpu, op1, op2, flag_wr ? 1 : 0);
                break;
            case 0x7U:
                res = nds_sbc_flags(&cpu, op2, op1, flag_wr ? 1 : 0);
                break;
            case 0x8U:
                res = op1 & op2;
                write_rd = false;
                break;
            case 0x9U:
                res = op1 ^ op2;
                write_rd = false;
                break;
            case 0xAU:
                nds_sub_flags(&cpu, op1, op2, 1);
                write_rd = false;
                break;
            case 0xBU:
                nds_add_flags(&cpu, op1, op2, 1);
                write_rd = false;
                break;
            case 0xCU:
                res = op1 | op2;
                break;
            case 0xDU:
                res = op2;
                break;
            case 0xEU:
                res = op1 & ~op2;
                break;
            case 0xFU:
                res = ~op2;
                break;
            default:
                break;
        }

        const bool is_logic = (op == 0U || op == 1U || op == 8U || op == 9U || op >= 12U);
        if (is_logic && (flag_wr || !write_rd)) {
            nds_set_nz(&cpu, res);
            cpu.c = sh_c;
        }

        if (write_rd) {
            if (rd == 15U) {
                if (set_f) {
                    const uint32_t s = cpu.spsr;
                    nds_switch_mode(&cpu, s & 0x1FU);
                    nds_unpack_cpsr(&cpu, s);
                }
                cpu.r[15] = cpu.thumb ? (res & ~1U) : (res & ~3U);
            } else {
                cpu.r[rd] = res;
            }
        }
        return;
    }

    // Single Data Transfer (LDR/STR/LDRB/STRB)
    if ((insn & 0x0C000000U) == 0x04000000U) {
        const bool is_reg = (insn & (1U << 25)) != 0U;
        const bool p = (insn & (1U << 24)) != 0U;
        const bool u = (insn & (1U << 23)) != 0U;
        const bool b = (insn & (1U << 22)) != 0U;
        const bool w = (insn & (1U << 21)) != 0U;
        const bool l = (insn & (1U << 20)) != 0U;
        const uint32_t rn = (insn >> 16U) & 0xFU;
        const uint32_t rd = (insn >> 12U) & 0xFU;

        uint32_t off = 0U;
        if (is_reg) {
            uint8_t dummy = cpu.c;
            off = shift_imm(cpu.r[insn & 0xFU], (insn >> 5U) & 3U, (insn >> 7U) & 0x1FU, cpu.c,
                            dummy);
        } else {
            off = insn & 0xFFFU;
        }
        const uint32_t base = read_reg(cpu, rn, pc8);
        const uint32_t ea = u ? (base + off) : (base - off);
        const uint32_t addr = p ? ea : base;

        if (l) {
            const uint32_t val = b ? mem_r8(cpu, bus, addr) : nds_ldr32(&cpu, addr);
            if ((!p || w) && rn != rd)
                cpu.r[rn] = ea;
            if (rd == 15U)
                write_pc_interwork(cpu, val, !b);
            else
                cpu.r[rd] = val;
        } else {
            const uint32_t val = read_reg(cpu, rd, pc_cur + 12U);
            if (b)
                mem_w8(cpu, bus, addr, static_cast<uint8_t>(val & 0xFFU));
            else
                mem_w32(cpu, bus, addr & ~3U, val);
            if (!p || w)
                cpu.r[rn] = ea;
        }
        return;
    }

    // Block Data Transfer (LDM / STM)
    if ((insn & 0x0E000000U) == 0x08000000U) {
        const bool p = (insn & (1U << 24)) != 0U;
        const bool u = (insn & (1U << 23)) != 0U;
        const bool s_bit = (insn & (1U << 22)) != 0U;
        const bool w = (insn & (1U << 21)) != 0U;
        const bool l = (insn & (1U << 20)) != 0U;
        const uint32_t rn = (insn >> 16U) & 0xFU;
        const uint16_t rlist = static_cast<uint16_t>(insn & 0xFFFFU);

        uint32_t count = 0U;
        for (int i = 0; i < 16; ++i) {
            if (rlist & (1U << i))
                ++count;
        }
        const uint32_t base = cpu.r[rn];
        uint32_t addr =
            u ? (p ? base + 4U : base) : (p ? base - count * 4U : base - count * 4U + 4U);
        const uint32_t new_base = u ? (base + count * 4U) : (base - count * 4U);

        const bool user_bank = s_bit && (!l || ((rlist & 0x8000U) == 0U));
        uint32_t saved_mode = cpu.cpsr & 0x1FU;
        if (user_bank && saved_mode != NDS_MODE_USR && saved_mode != NDS_MODE_SYS) {
            nds_switch_mode(&cpu, NDS_MODE_USR);
        }

        if (l) {
            if (w && ((rlist & (1U << rn)) == 0U)) {
                cpu.r[rn] = new_base;
            }
            for (int i = 0; i < 15; ++i) {
                if (rlist & (1U << i)) {
                    cpu.r[i] = mem_r32(cpu, bus, addr & ~3U);
                    addr += 4U;
                }
            }
            if (rlist & 0x8000U) {
                const uint32_t pc_val = mem_r32(cpu, bus, addr & ~3U);
                if (s_bit) {
                    const uint32_t sp = cpu.spsr;
                    nds_switch_mode(&cpu, sp & 0x1FU);
                    nds_unpack_cpsr(&cpu, sp);
                    cpu.r[15] = cpu.thumb ? (pc_val & ~1U) : (pc_val & ~3U);
                } else {
                    write_pc_interwork(cpu, pc_val, true);
                }
            }
        } else {
            for (int i = 0; i < 16; ++i) {
                if (rlist & (1U << i)) {
                    const uint32_t v = (i == 15) ? (pc_cur + 12U) : cpu.r[i];
                    mem_w32(cpu, bus, addr & ~3U, v);
                    addr += 4U;
                }
            }
            if (w)
                cpu.r[rn] = new_base;
        }

        if (user_bank && saved_mode != NDS_MODE_USR && saved_mode != NDS_MODE_SYS) {
            nds_switch_mode(&cpu, saved_mode);
        }
    }
}

void exec_thumb(NdsCpuState& cpu, Bus& bus, uint16_t insn, uint32_t pc_cur) noexcept {
    const uint32_t pc4 = pc_cur + 4U;
    const uint32_t top5 = insn >> 11U;

    // Format 1/2: Move shifted register / Add & Sub
    if (top5 <= 3U) {
        const uint32_t op = (insn >> 11U) & 3U;
        const uint32_t rd = insn & 7U;
        const uint32_t rs = (insn >> 3U) & 7U;
        if (op == 3U) {
            const uint32_t sub = (insn >> 9U) & 1U;
            const uint32_t imm = (insn >> 10U) & 1U;
            const uint32_t rn_imm = (insn >> 6U) & 7U;
            const uint32_t rhs = imm ? rn_imm : cpu.r[rn_imm];
            cpu.r[rd] = sub ? nds_sub_flags(&cpu, cpu.r[rs], rhs, 1)
                            : nds_add_flags(&cpu, cpu.r[rs], rhs, 1);
        } else {
            const uint32_t amt = (insn >> 6U) & 0x1FU;
            uint8_t c_out = cpu.c;
            if (op == 0U && amt == 0U) {
                cpu.r[rd] = cpu.r[rs];
                nds_set_nz(&cpu, cpu.r[rd]);
            } else {
                cpu.r[rd] = shift_imm(cpu.r[rs], op, amt, cpu.c, c_out);
                nds_set_nz(&cpu, cpu.r[rd]);
                cpu.c = c_out;
            }
        }
        return;
    }

    // Format 3: MOV/CMP/ADD/SUB immediate 8
    if (top5 <= 7U) {
        const uint32_t op = (insn >> 11U) & 3U;
        const uint32_t rd = (insn >> 8U) & 7U;
        const uint32_t imm = insn & 0xFFU;
        if (op == 0U) {
            cpu.r[rd] = imm;
            nds_set_nz(&cpu, imm);
        } else if (op == 1U) {
            nds_sub_flags(&cpu, cpu.r[rd], imm, 1);
        } else if (op == 2U) {
            cpu.r[rd] = nds_add_flags(&cpu, cpu.r[rd], imm, 1);
        } else {
            cpu.r[rd] = nds_sub_flags(&cpu, cpu.r[rd], imm, 1);
        }
        return;
    }

    // Format 4: ALU operations (010000)
    if ((insn >> 10U) == 0x10U) {
        const uint32_t op = (insn >> 6U) & 0xFU;
        const uint32_t rs = (insn >> 3U) & 7U;
        const uint32_t rd = insn & 7U;
        uint8_t c_out = cpu.c;
        switch (op) {
            case 0x0U:
                cpu.r[rd] &= cpu.r[rs];
                nds_set_nz(&cpu, cpu.r[rd]);
                break;
            case 0x1U:
                cpu.r[rd] ^= cpu.r[rs];
                nds_set_nz(&cpu, cpu.r[rd]);
                break;
            case 0x2U:
                cpu.r[rd] = shift_reg(cpu.r[rd], 0U, cpu.r[rs], cpu.c, c_out);
                nds_set_nz(&cpu, cpu.r[rd]);
                cpu.c = c_out;
                break;
            case 0x3U:
                cpu.r[rd] = shift_reg(cpu.r[rd], 1U, cpu.r[rs], cpu.c, c_out);
                nds_set_nz(&cpu, cpu.r[rd]);
                cpu.c = c_out;
                break;
            case 0x4U:
                cpu.r[rd] = shift_reg(cpu.r[rd], 2U, cpu.r[rs], cpu.c, c_out);
                nds_set_nz(&cpu, cpu.r[rd]);
                cpu.c = c_out;
                break;
            case 0x5U:
                cpu.r[rd] = nds_adc_flags(&cpu, cpu.r[rd], cpu.r[rs], 1);
                break;
            case 0x6U:
                cpu.r[rd] = nds_sbc_flags(&cpu, cpu.r[rd], cpu.r[rs], 1);
                break;
            case 0x7U:
                cpu.r[rd] = shift_reg(cpu.r[rd], 3U, cpu.r[rs], cpu.c, c_out);
                nds_set_nz(&cpu, cpu.r[rd]);
                cpu.c = c_out;
                break;
            case 0x8U:
                nds_set_nz(&cpu, cpu.r[rd] & cpu.r[rs]);
                break;
            case 0x9U:
                cpu.r[rd] = nds_sub_flags(&cpu, 0U, cpu.r[rs], 1);
                break;
            case 0xAU:
                nds_sub_flags(&cpu, cpu.r[rd], cpu.r[rs], 1);
                break;
            case 0xBU:
                nds_add_flags(&cpu, cpu.r[rd], cpu.r[rs], 1);
                break;
            case 0xCU:
                cpu.r[rd] |= cpu.r[rs];
                nds_set_nz(&cpu, cpu.r[rd]);
                break;
            case 0xDU:
                cpu.r[rd] *= cpu.r[rs];
                nds_set_nz(&cpu, cpu.r[rd]);
                break;
            case 0xEU:
                cpu.r[rd] &= ~cpu.r[rs];
                nds_set_nz(&cpu, cpu.r[rd]);
                break;
            case 0xFU:
                cpu.r[rd] = ~cpu.r[rs];
                nds_set_nz(&cpu, cpu.r[rd]);
                break;
            default:
                break;
        }
        return;
    }

    // Format 5: Hi register ops / BX / BLX Rm (010001)
    if ((insn >> 10U) == 0x11U) {
        const uint32_t op = (insn >> 8U) & 3U;
        const uint32_t rd = (insn & 7U) | (((insn >> 7U) & 1U) << 3U);
        const uint32_t rs = ((insn >> 3U) & 0xFU);
        const uint32_t val = read_reg(cpu, rs, pc4);
        if (op == 0U) {
            const uint32_t r = read_reg(cpu, rd, pc4) + val;
            if (rd == 15U)
                cpu.r[15] = r & ~1U;
            else
                cpu.r[rd] = r;
        } else if (op == 1U) {
            nds_sub_flags(&cpu, read_reg(cpu, rd, pc4), val, 1);
        } else if (op == 2U) {
            if (rd == 15U)
                cpu.r[15] = val & ~1U;
            else
                cpu.r[rd] = val;
        } else {
            if (cpu.is_arm9 && (insn & (1U << 7))) {
                cpu.r[14] = (pc_cur + 2U) | 1U;  // Thumb BLX Rm
            }
            cpu.thumb = static_cast<uint8_t>(val & 1U);
            cpu.r[15] = cpu.thumb ? (val & ~1U) : (val & ~3U);
        }
        return;
    }

    // Format 6: PC-relative load (01001)
    if (top5 == 0x09U) {
        const uint32_t rd = (insn >> 8U) & 7U;
        const uint32_t addr = (pc4 & ~3U) + ((insn & 0xFFU) << 2U);
        cpu.r[rd] = mem_r32(cpu, bus, addr);
        return;
    }

    // Format 7 & 8: Load/Store register offset (0101)
    if ((insn >> 12U) == 0x05U) {
        const uint32_t ro = (insn >> 6U) & 7U;
        const uint32_t rb = (insn >> 3U) & 7U;
        const uint32_t rd = insn & 7U;
        const uint32_t addr = cpu.r[rb] + cpu.r[ro];
        const uint32_t op = (insn >> 9U) & 7U;
        switch (op) {
            case 0U:
                mem_w32(cpu, bus, addr & ~3U, cpu.r[rd]);
                break;
            case 1U:
                mem_w16(cpu, bus, addr & ~1U, static_cast<uint16_t>(cpu.r[rd]));
                break;
            case 2U:
                mem_w8(cpu, bus, addr, static_cast<uint8_t>(cpu.r[rd]));
                break;
            case 3U:
                cpu.r[rd] = nds_ldrsb8(&cpu, addr);
                break;
            case 4U:
                cpu.r[rd] = nds_ldr32(&cpu, addr);
                break;
            case 5U:
                cpu.r[rd] = nds_ldrh16(&cpu, addr);
                break;
            case 6U:
                cpu.r[rd] = mem_r8(cpu, bus, addr);
                break;
            case 7U:
                cpu.r[rd] = nds_ldrsh16(&cpu, addr);
                break;
            default:
                break;
        }
        return;
    }

    // Format 9: Load/Store immediate offset (011)
    if ((insn >> 13U) == 0x03U) {
        const bool b = (insn & (1U << 12)) != 0U;
        const bool l = (insn & (1U << 11)) != 0U;
        const uint32_t off = (insn >> 6U) & 0x1FU;
        const uint32_t rb = (insn >> 3U) & 7U;
        const uint32_t rd = insn & 7U;
        const uint32_t addr = cpu.r[rb] + (b ? off : (off << 2U));
        if (l)
            cpu.r[rd] = b ? mem_r8(cpu, bus, addr) : nds_ldr32(&cpu, addr);
        else if (b)
            mem_w8(cpu, bus, addr, static_cast<uint8_t>(cpu.r[rd]));
        else
            mem_w32(cpu, bus, addr & ~3U, cpu.r[rd]);
        return;
    }

    // Format 10: Load/Store halfword (1000)
    if ((insn >> 12U) == 0x08U) {
        const bool l = (insn & (1U << 11)) != 0U;
        const uint32_t off = ((insn >> 6U) & 0x1FU) << 1U;
        const uint32_t rb = (insn >> 3U) & 7U;
        const uint32_t rd = insn & 7U;
        const uint32_t addr = cpu.r[rb] + off;
        if (l)
            cpu.r[rd] = nds_ldrh16(&cpu, addr);
        else
            mem_w16(cpu, bus, addr & ~1U, static_cast<uint16_t>(cpu.r[rd]));
        return;
    }

    // Format 11: SP-relative Load/Store (1001)
    if ((insn >> 12U) == 0x09U) {
        const bool l = (insn & (1U << 11)) != 0U;
        const uint32_t rd = (insn >> 8U) & 7U;
        const uint32_t addr = cpu.r[13] + ((insn & 0xFFU) << 2U);
        if (l)
            cpu.r[rd] = nds_ldr32(&cpu, addr);
        else
            mem_w32(cpu, bus, addr & ~3U, cpu.r[rd]);
        return;
    }

    // Format 12: Load address (1010)
    if ((insn >> 12U) == 0x0AU) {
        const bool sp = (insn & (1U << 11)) != 0U;
        const uint32_t rd = (insn >> 8U) & 7U;
        const uint32_t imm = (insn & 0xFFU) << 2U;
        cpu.r[rd] = (sp ? cpu.r[13] : (pc4 & ~3U)) + imm;
        return;
    }

    // Format 13/14: Add offset to SP / PUSH / POP (1011)
    if ((insn >> 12U) == 0x0BU) {
        if ((insn & 0xFF00U) == 0xB000U) {
            const uint32_t imm = (insn & 0x7FU) << 2U;
            cpu.r[13] = (insn & 0x80U) ? (cpu.r[13] - imm) : (cpu.r[13] + imm);
        } else if ((insn & 0x0600U) == 0x0400U) {
            const bool l = (insn & (1U << 11)) != 0U;
            const bool pc_lr = (insn & (1U << 8)) != 0U;
            const uint8_t rlist = static_cast<uint8_t>(insn & 0xFFU);
            if (!l) {
                if (pc_lr) {
                    cpu.r[13] -= 4U;
                    mem_w32(cpu, bus, cpu.r[13] & ~3U, cpu.r[14]);
                }
                for (int i = 7; i >= 0; --i) {
                    if (rlist & (1U << i)) {
                        cpu.r[13] -= 4U;
                        mem_w32(cpu, bus, cpu.r[13] & ~3U, cpu.r[i]);
                    }
                }
            } else {
                for (int i = 0; i < 8; ++i) {
                    if (rlist & (1U << i)) {
                        cpu.r[i] = mem_r32(cpu, bus, cpu.r[13] & ~3U);
                        cpu.r[13] += 4U;
                    }
                }
                if (pc_lr) {
                    const uint32_t pv = mem_r32(cpu, bus, cpu.r[13] & ~3U);
                    cpu.r[13] += 4U;
                    write_pc_interwork(cpu, pv, true);
                }
            }
        }
        return;
    }

    // Format 15: LDMIA / STMIA (1100)
    if ((insn >> 12U) == 0x0CU) {
        const bool l = (insn & (1U << 11)) != 0U;
        const uint32_t rb = (insn >> 8U) & 7U;
        const uint8_t rlist = static_cast<uint8_t>(insn & 0xFFU);
        uint32_t addr = cpu.r[rb];
        for (int i = 0; i < 8; ++i) {
            if (rlist & (1U << i)) {
                if (l)
                    cpu.r[i] = mem_r32(cpu, bus, addr & ~3U);
                else
                    mem_w32(cpu, bus, addr & ~3U, cpu.r[i]);
                addr += 4U;
            }
        }
        if (!l || ((rlist & (1U << rb)) == 0U)) {
            cpu.r[rb] = addr;
        }
        return;
    }

    // Format 16 & 17: Conditional branch / SWI (1101)
    if ((insn >> 12U) == 0x0DU) {
        const uint32_t cond = (insn >> 8U) & 0xFU;
        if (cond == 0xFU) {
            cpu_handle_swi(cpu, bus, static_cast<uint8_t>(insn & 0xFFU));
        } else if (cond < 0xEU && nds_cond_passed(&cpu, cond)) {
            const int32_t off = static_cast<int32_t>(static_cast<int8_t>(insn & 0xFFU)) << 1;
            cpu.r[15] = static_cast<uint32_t>(static_cast<int32_t>(pc4) + off);
        }
        return;
    }

    // Format 18: Unconditional branch (11100)
    if (top5 == 0x1CU) {
        int32_t off = static_cast<int32_t>(insn & 0x7FFU);
        if (off & 0x400)
            off |= static_cast<int32_t>(0xFFFFF800U);
        cpu.r[15] = static_cast<uint32_t>(static_cast<int32_t>(pc4) + (off << 1));
        return;
    }

    // Format 19: Long BL / BLX prefix (11110) & suffix (11111 / 11101)
    if (top5 == 0x1EU) {
        int32_t off = static_cast<int32_t>(insn & 0x7FFU);
        if (off & 0x400)
            off |= static_cast<int32_t>(0xFFFFF800U);
        cpu.r[14] = static_cast<uint32_t>(static_cast<int32_t>(pc4) + (off << 12));
        return;
    }
    if (top5 == 0x1FU || (cpu.is_arm9 && top5 == 0x1DU)) {
        const uint32_t target = cpu.r[14] + ((insn & 0x7FFU) << 1U);
        cpu.r[14] = (pc_cur + 2U) | 1U;
        if (top5 == 0x1DU) {
            cpu.thumb = 0U;
            cpu.r[15] = target & ~3U;
        } else {
            cpu.r[15] = target & ~1U;
        }
    }
}

uint16_t swi_crc16(NdsCpuState& cpu, Bus& bus, uint16_t init, uint32_t addr,
                   uint32_t len) noexcept {
    static constexpr uint16_t kVal[8] = {0xC0C1, 0xC181, 0xC301, 0xC601,
                                         0xCC01, 0xD801, 0xF001, 0xA001};
    uint16_t crc = init;
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= mem_r8(cpu, bus, addr + i);
        for (int b = 0; b < 8; ++b) {
            if (crc & 1U)
                crc = static_cast<uint16_t>((crc >> 1U) ^ (kVal[b] << (7 - b)));
            else
                crc >>= 1U;
        }
    }
    return crc;
}

}  // namespace

void cpu_reset_arm9(NdsCpuState& cpu, Bus& bus, uint32_t entry_pc) noexcept {
    std::memset(&cpu, 0, sizeof(cpu));
    cpu.is_arm9 = 1U;
    cpu.bus = &bus;
    cpu.cpsr = NDS_MODE_SYS;
    cpu.r[13] = 0x03002F7CU;
    cpu.r13_usr = 0x03002F7CU;
    cpu.r13_irq = 0x03003F80U;
    cpu.r13_svc = 0x03003FC0U;
    cpu.r[14] = entry_pc;
    cpu.r[15] = entry_pc;
    cpu.cp15_ctrl = 0x00052078U;  // High vectors, ITCM + DTCM enabled
    cpu.dtcm_base = 0x027C0000U;
    cpu.dtcm_size = 0x00004000U;
    cpu.itcm_size = 0x02000000U;
    cpu.cp15_dtcm_reg = 0x027C000AU;
    cpu.cp15_itcm_reg = 0x00000020U;
    cpu.next_event = Bus::kArm9CyclesPerLine;
}

void cpu_reset_arm7(NdsCpuState& cpu, Bus& bus, uint32_t entry_pc) noexcept {
    std::memset(&cpu, 0, sizeof(cpu));
    cpu.is_arm9 = 0U;
    cpu.bus = &bus;
    cpu.cpsr = NDS_MODE_SYS;
    cpu.r[13] = 0x0380FD80U;
    cpu.r13_usr = 0x0380FD80U;
    cpu.r13_irq = 0x0380FF80U;
    cpu.r13_svc = 0x0380FFC0U;
    cpu.r[14] = entry_pc;
    cpu.r[15] = entry_pc;
    cpu.next_event = Bus::kArm7CyclesPerLine;
}

uint32_t cpu_step(NdsCpuState& cpu, Bus& bus) noexcept {
    if (cpu.halted != 0U) {
        cpu.cycles += 4ULL;
        return 4U;
    }

    const uint32_t pc_cur = cpu.r[15];

    // Intercept HLE BIOS IRQ return trampoline (ARM9: 0xFFFF0020, ARM7: 0x00000020)
    if (pc_cur == (cpu.is_arm9 ? 0xFFFF0020U : 0x00000020U)) {
        // Restore {r0-r3, r12, lr} from IRQ stack and return via SUBS PC, LR, #4
        cpu.r[0] = mem_r32(cpu, bus, cpu.r[13]);
        cpu.r[1] = mem_r32(cpu, bus, cpu.r[13] + 4U);
        cpu.r[2] = mem_r32(cpu, bus, cpu.r[13] + 8U);
        cpu.r[3] = mem_r32(cpu, bus, cpu.r[13] + 12U);
        cpu.r[12] = mem_r32(cpu, bus, cpu.r[13] + 16U);
        const uint32_t lr = mem_r32(cpu, bus, cpu.r[13] + 20U);
        cpu.r[13] += 24U;
        const uint32_t s = cpu.spsr;
        nds_switch_mode(&cpu, s & 0x1FU);
        nds_unpack_cpsr(&cpu, s);
        cpu.r[15] = cpu.thumb ? ((lr - 4U) & ~1U) : ((lr - 4U) & ~3U);
        cpu.cycles += 8ULL;
        return 8U;
    }

    if (cpu.thumb) {
        const uint16_t insn = mem_r16(cpu, bus, pc_cur);
        cpu.r[15] = pc_cur + 2U;
        exec_thumb(cpu, bus, insn, pc_cur);
    } else {
        const uint32_t insn = mem_r32(cpu, bus, pc_cur);
        cpu.r[15] = pc_cur + 4U;
        exec_arm(cpu, bus, insn, pc_cur);
    }

    cpu.cycles += 4ULL;
    return 4U;
}

void cpu_dispatch_irq(NdsCpuState& cpu, Bus& bus) noexcept {
    // NDS BIOS IRQ handler reads user handler from:
    //   ARM9: DTCM_base + 0x3FFC
    //   ARM7: 0x0380FFFC
    const uint32_t handler_ptr_addr = cpu.is_arm9 ? (cpu.dtcm_base + 0x3FFCU) : 0x0380FFFCU;
    const uint32_t handler = mem_r32(cpu, bus, handler_ptr_addr);
    if (handler == 0U) {
        return;
    }

    const uint32_t old_cpsr = nds_pack_cpsr(&cpu);
    const uint32_t ret_lr = cpu.r[15] + 4U;

    nds_switch_mode(&cpu, NDS_MODE_IRQ);
    cpu.spsr = old_cpsr;
    cpu.i_disable = 1U;
    cpu.thumb = 0U;

    // Push {r0-r3, r12, lr} onto IRQ stack (exact NDS BIOS convention)
    cpu.r[13] -= 24U;
    mem_w32(cpu, bus, cpu.r[13] + 0U, cpu.r[0]);
    mem_w32(cpu, bus, cpu.r[13] + 4U, cpu.r[1]);
    mem_w32(cpu, bus, cpu.r[13] + 8U, cpu.r[2]);
    mem_w32(cpu, bus, cpu.r[13] + 12U, cpu.r[3]);
    mem_w32(cpu, bus, cpu.r[13] + 16U, cpu.r[12]);
    mem_w32(cpu, bus, cpu.r[13] + 20U, ret_lr);

    cpu.r[0] = 0x04000000U;
    cpu.r[14] = cpu.is_arm9 ? 0xFFFF0020U : 0x00000020U;
    cpu.r[15] = handler & ~3U;
    cpu.halted = 0U;
}

void cpu_handle_swi(NdsCpuState& cpu, Bus& bus, uint8_t swi_num) noexcept {
    switch (swi_num) {
        case 0x03U:  // WaitByLoop
            cpu.cycles += static_cast<uint64_t>(cpu.r[0]) * 4ULL;
            break;
        case 0x04U:    // IntrWait
        case 0x05U: {  // VBlankIntrWait
            const uint32_t flags = (swi_num == 0x05U) ? kIrqVBlank : cpu.r[1];
            const bool discard = (swi_num == 0x05U) ? true : (cpu.r[0] != 0U);
            const uint32_t chk_addr = cpu.is_arm9 ? (cpu.dtcm_base + 0x3FF8U) : 0x0380FFF8U;
            if (discard) {
                mem_w32(cpu, bus, chk_addr, mem_r32(cpu, bus, chk_addr) & ~flags);
            }
            if ((mem_r32(cpu, bus, chk_addr) & flags) == 0U) {
                cpu.halted = 1U;
            } else {
                mem_w32(cpu, bus, chk_addr, mem_r32(cpu, bus, chk_addr) & ~flags);
            }
            break;
        }
        case 0x06U:  // Halt
            cpu.halted = 1U;
            break;
        case 0x09U: {  // Div
            const int32_t num = static_cast<int32_t>(cpu.r[0]);
            const int32_t den = static_cast<int32_t>(cpu.r[1]);
            if (den != 0) {
                const int32_t q = num / den;
                const int32_t r = num % den;
                cpu.r[0] = static_cast<uint32_t>(q);
                cpu.r[1] = static_cast<uint32_t>(r);
                cpu.r[3] = static_cast<uint32_t>(q < 0 ? -q : q);
            }
            break;
        }
        case 0x0BU: {  // CpuSet
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            const uint32_t cnt = cpu.r[2];
            const uint32_t count = cnt & 0x1FFFFFU;
            const bool fixed = (cnt & (1U << 24)) != 0U;
            const bool is_32 = (cnt & (1U << 26)) != 0U;
            for (uint32_t i = 0; i < count; ++i) {
                if (is_32) {
                    mem_w32(cpu, bus, dst, mem_r32(cpu, bus, src));
                    dst += 4U;
                    if (!fixed)
                        src += 4U;
                } else {
                    mem_w16(cpu, bus, dst, mem_r16(cpu, bus, src));
                    dst += 2U;
                    if (!fixed)
                        src += 2U;
                }
            }
            break;
        }
        case 0x0CU: {  // CpuFastSet
            uint32_t src = cpu.r[0] & ~3U;
            uint32_t dst = cpu.r[1] & ~3U;
            const uint32_t cnt = cpu.r[2];
            const uint32_t count = (cnt & 0x1FFFFFU) & ~7U;
            const bool fixed = (cnt & (1U << 24)) != 0U;
            for (uint32_t i = 0; i < count; ++i) {
                mem_w32(cpu, bus, dst, mem_r32(cpu, bus, src));
                dst += 4U;
                if (!fixed)
                    src += 4U;
            }
            break;
        }
        case 0x0DU: {  // Sqrt
            const uint32_t v = cpu.r[0];
            cpu.r[0] = static_cast<uint32_t>(std::sqrt(static_cast<double>(v)));
            break;
        }
        case 0x0EU:  // GetCRC16
            cpu.r[0] = swi_crc16(cpu, bus, static_cast<uint16_t>(cpu.r[0]), cpu.r[1], cpu.r[2]);
            break;
        case 0x11U:    // LZ77UnCompWram
        case 0x12U: {  // LZ77UnCompVram
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            const uint32_t hdr = mem_r32(cpu, bus, src);
            src += 4U;
            uint32_t rem = hdr >> 8U;
            while (rem > 0U) {
                const uint8_t flags = mem_r8(cpu, bus, src++);
                for (int b = 7; b >= 0 && rem > 0U; --b) {
                    if (flags & (1U << b)) {
                        const uint8_t b0 = mem_r8(cpu, bus, src++);
                        const uint8_t b1 = mem_r8(cpu, bus, src++);
                        const uint32_t len = ((b0 >> 4U) & 0x0FU) + 3U;
                        const uint32_t disp = (((b0 & 0x0FU) << 8U) | b1) + 1U;
                        for (uint32_t k = 0; k < len && rem > 0U; ++k) {
                            const uint8_t v = mem_r8(cpu, bus, dst - disp);
                            mem_w8(cpu, bus, dst++, v);
                            --rem;
                        }
                    } else {
                        mem_w8(cpu, bus, dst++, mem_r8(cpu, bus, src++));
                        --rem;
                    }
                }
            }
            break;
        }
        case 0x14U:    // RLUnCompWram
        case 0x15U: {  // RLUnCompVram
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            const uint32_t hdr = mem_r32(cpu, bus, src);
            src += 4U;
            uint32_t rem = hdr >> 8U;
            while (rem > 0U) {
                const uint8_t flag = mem_r8(cpu, bus, src++);
                if (flag & 0x80U) {
                    const uint32_t len = (flag & 0x7FU) + 3U;
                    const uint8_t val = mem_r8(cpu, bus, src++);
                    for (uint32_t k = 0; k < len && rem > 0U; ++k) {
                        mem_w8(cpu, bus, dst++, val);
                        --rem;
                    }
                } else {
                    const uint32_t len = (flag & 0x7FU) + 1U;
                    for (uint32_t k = 0; k < len && rem > 0U; ++k) {
                        mem_w8(cpu, bus, dst++, mem_r8(cpu, bus, src++));
                        --rem;
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

}  // namespace dragon::nds
