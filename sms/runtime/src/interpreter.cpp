#include "sms/interpreter.h"

#include "sms/opcodes.h"

namespace dragon::sms {

namespace {

inline void inc_r(SmsCpuState& cpu) noexcept {
    cpu.r = static_cast<uint8_t>((cpu.r & 0x80U) | ((cpu.r + 1U) & 0x7FU));
}

inline uint8_t fetch8(SmsCpuState& cpu, Bus& bus) noexcept {
    const uint8_t v = bus.read(cpu.pc);
    cpu.pc = static_cast<uint16_t>(cpu.pc + 1U);
    return v;
}

inline uint16_t fetch16(SmsCpuState& cpu, Bus& bus) noexcept {
    const uint8_t lo = fetch8(cpu, bus);
    const uint8_t hi = fetch8(cpu, bus);
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8U));
}

inline uint8_t read_r8(const SmsCpuState& cpu, const Bus& bus, uint8_t idx) noexcept {
    switch (idx & 7U) {
        case 0: return cpu.b;
        case 1: return cpu.c;
        case 2: return cpu.d;
        case 3: return cpu.e;
        case 4: return cpu.h;
        case 5: return cpu.l;
        case 6: return bus.read(sms_hl(&cpu));
        case 7: return cpu.a;
        default: return 0;
    }
}

inline void write_r8(SmsCpuState& cpu, Bus& bus, uint8_t idx, uint8_t v) noexcept {
    switch (idx & 7U) {
        case 0: cpu.b = v; break;
        case 1: cpu.c = v; break;
        case 2: cpu.d = v; break;
        case 3: cpu.e = v; break;
        case 4: cpu.h = v; break;
        case 5: cpu.l = v; break;
        case 6: bus.write(sms_hl(&cpu), v); break;
        case 7: cpu.a = v; break;
        default: break;
    }
}

inline uint16_t read_rp(const SmsCpuState& cpu, uint8_t idx) noexcept {
    switch (idx & 3U) {
        case 0: return sms_bc(&cpu);
        case 1: return sms_de(&cpu);
        case 2: return sms_hl(&cpu);
        case 3: return cpu.sp;
        default: return 0;
    }
}

inline void write_rp(SmsCpuState& cpu, uint8_t idx, uint16_t v) noexcept {
    switch (idx & 3U) {
        case 0: sms_set_bc(&cpu, v); break;
        case 1: sms_set_de(&cpu, v); break;
        case 2: sms_set_hl(&cpu, v); break;
        case 3: cpu.sp = v; break;
        default: break;
    }
}

inline uint16_t read_rp2(const SmsCpuState& cpu, uint8_t idx) noexcept {
    return ((idx & 3U) == 3U) ? sms_af(&cpu) : read_rp(cpu, idx);
}

inline void write_rp2(SmsCpuState& cpu, uint8_t idx, uint16_t v) noexcept {
    if ((idx & 3U) == 3U) {
        sms_set_af(&cpu, v);
    } else {
        write_rp(cpu, idx, v);
    }
}

inline bool eval_cc(const SmsCpuState& cpu, uint8_t cc) noexcept {
    switch (cc & 7U) {
        case 0: return (cpu.f & SMS_FLAG_Z) == 0U;   // NZ
        case 1: return (cpu.f & SMS_FLAG_Z) != 0U;   // Z
        case 2: return (cpu.f & SMS_FLAG_C) == 0U;   // NC
        case 3: return (cpu.f & SMS_FLAG_C) != 0U;   // C
        case 4: return (cpu.f & SMS_FLAG_PV) == 0U;  // PO
        case 5: return (cpu.f & SMS_FLAG_PV) != 0U;  // PE
        case 6: return (cpu.f & SMS_FLAG_S) == 0U;   // P
        case 7: return (cpu.f & SMS_FLAG_S) != 0U;   // M
        default: return false;
    }
}

inline void exec_alu(SmsCpuState& cpu, uint8_t op_idx, uint8_t val) noexcept {
    switch (op_idx & 7U) {
        case 0: sms_add8(&cpu, val, 0U); break;
        case 1: sms_add8(&cpu, val, (cpu.f & SMS_FLAG_C) ? 1U : 0U); break;
        case 2: sms_sub8(&cpu, val, 0U); break;
        case 3: sms_sub8(&cpu, val, (cpu.f & SMS_FLAG_C) ? 1U : 0U); break;
        case 4: sms_and8(&cpu, val); break;
        case 5: sms_xor8(&cpu, val); break;
        case 6: sms_or8(&cpu, val); break;
        case 7: sms_cp8(&cpu, val); break;
        default: break;
    }
}

inline uint8_t exec_rot_shift(SmsCpuState& cpu, uint8_t op_idx, uint8_t val) noexcept {
    switch (op_idx & 7U) {
        case 0: return sms_rlc(&cpu, val);
        case 1: return sms_rrc(&cpu, val);
        case 2: return sms_rl(&cpu, val);
        case 3: return sms_rr(&cpu, val);
        case 4: return sms_sla(&cpu, val);
        case 5: return sms_sra(&cpu, val);
        case 6: return sms_sll(&cpu, val);
        case 7: return sms_srl(&cpu, val);
        default: return val;
    }
}

uint32_t exec_cb(SmsCpuState& cpu, Bus& bus) noexcept {
    inc_r(cpu);
    const uint8_t op = fetch8(cpu, bus);
    const uint8_t r = op & 7U;
    const uint8_t y = (op >> 3U) & 7U;
    const uint8_t x = (op >> 6U) & 3U;

    const uint8_t val = read_r8(cpu, bus, r);
    if (x == 0U) {
        write_r8(cpu, bus, r, exec_rot_shift(cpu, y, val));
    } else if (x == 1U) {
        sms_bit(&cpu, y, val);
    } else if (x == 2U) {
        write_r8(cpu, bus, r, static_cast<uint8_t>(val & ~(1U << y)));
    } else {
        write_r8(cpu, bus, r, static_cast<uint8_t>(val | (1U << y)));
    }
    return opcode_info_cb(op).cycles;
}

uint32_t exec_ed(SmsCpuState& cpu, Bus& bus) noexcept {
    inc_r(cpu);
    const uint8_t op = fetch8(cpu, bus);
    const OpcodeInfo& info = opcode_info_ed(op);

    // 0x40..0x7F
    if (op >= 0x40U && op <= 0x7FU) {
        const uint8_t y = (op >> 3U) & 7U;
        const uint8_t z = op & 7U;
        switch (z) {
            case 0: {  // IN r,(C) (y == 6 reads & sets flags only)
                const uint8_t v = bus.io_read(cpu.c);
                if (y != 6U) {
                    write_r8(cpu, bus, y, v);
                }
                cpu.f = static_cast<uint8_t>((cpu.f & SMS_FLAG_C) | sms_szxyp(v));
                return info.cycles;
            }
            case 1: {  // OUT (C),r (y == 6 outputs 0)
                const uint8_t v = (y == 6U) ? 0U : read_r8(cpu, bus, y);
                bus.io_write(cpu.c, v);
                return info.cycles;
            }
            case 2: {  // SBC HL,rr / ADC HL,rr
                const uint16_t rp = read_rp(cpu, y >> 1U);
                if ((y & 1U) == 0U) {
                    sms_sbc_hl(&cpu, rp);
                } else {
                    sms_adc_hl(&cpu, rp);
                }
                return info.cycles;
            }
            case 3: {  // LD (nn),rr / LD rr,(nn)
                const uint16_t addr = fetch16(cpu, bus);
                const uint8_t p = y >> 1U;
                if ((y & 1U) == 0U) {
                    const uint16_t v = read_rp(cpu, p);
                    bus.write(addr, static_cast<uint8_t>(v & 0xFFU));
                    bus.write(static_cast<uint16_t>(addr + 1U),
                              static_cast<uint8_t>((v >> 8U) & 0xFFU));
                } else {
                    const uint16_t v = static_cast<uint16_t>(
                        bus.read(addr) |
                        (static_cast<uint16_t>(bus.read(static_cast<uint16_t>(addr + 1U))) << 8U));
                    write_rp(cpu, p, v);
                }
                return info.cycles;
            }
            case 4: {  // NEG
                const uint8_t old_a = cpu.a;
                cpu.a = 0U;
                sms_sub8(&cpu, old_a, 0U);
                return info.cycles;
            }
            case 5: {  // RETN / RETI
                cpu.pc = sms_pop(&cpu);
                cpu.iff1 = cpu.iff2;
                return info.cycles;
            }
            case 6: {  // IM 0/1/2
                const uint8_t mode_sel = y & 3U;
                cpu.im = (mode_sel == 2U) ? 1U : (mode_sel == 3U ? 2U : 0U);
                return info.cycles;
            }
            case 7: {
                switch (y) {
                    case 0: cpu.i = cpu.a; break;
                    case 1: cpu.r = cpu.a; break;
                    case 2:  // LD A,I
                        cpu.a = cpu.i;
                        cpu.f = static_cast<uint8_t>((cpu.f & SMS_FLAG_C) | sms_szxy(cpu.a) |
                                                     (cpu.iff2 ? SMS_FLAG_PV : 0U));
                        break;
                    case 3:  // LD A,R
                        cpu.a = cpu.r;
                        cpu.f = static_cast<uint8_t>((cpu.f & SMS_FLAG_C) | sms_szxy(cpu.a) |
                                                     (cpu.iff2 ? SMS_FLAG_PV : 0U));
                        break;
                    case 4: {  // RRD
                        const uint16_t hl = sms_hl(&cpu);
                        const uint8_t m = bus.read(hl);
                        const uint8_t a_lo = cpu.a & 0x0FU;
                        cpu.a = static_cast<uint8_t>((cpu.a & 0xF0U) | (m & 0x0FU));
                        bus.write(hl, static_cast<uint8_t>((a_lo << 4U) | (m >> 4U)));
                        cpu.f = static_cast<uint8_t>((cpu.f & SMS_FLAG_C) | sms_szxyp(cpu.a));
                        break;
                    }
                    case 5: {  // RLD
                        const uint16_t hl = sms_hl(&cpu);
                        const uint8_t m = bus.read(hl);
                        const uint8_t a_lo = cpu.a & 0x0FU;
                        cpu.a = static_cast<uint8_t>((cpu.a & 0xF0U) | (m >> 4U));
                        bus.write(hl, static_cast<uint8_t>((m << 4U) | a_lo));
                        cpu.f = static_cast<uint8_t>((cpu.f & SMS_FLAG_C) | sms_szxyp(cpu.a));
                        break;
                    }
                    default:
                        break;
                }
                return info.cycles;
            }
            default:
                return info.cycles;
        }
    }

    // Block transfer / compare / I/O instructions (0xA0..0xBB)
    const int delta = (op & 0x08U) ? -1 : 1;
    const bool repeat = (op & 0x10U) != 0U;

    switch (op & 0xE7U) {
        case 0xA0U: {  // LDI / LDD / LDIR / LDDR
            const uint16_t hl = sms_hl(&cpu);
            const uint16_t de = sms_de(&cpu);
            const uint8_t v = bus.read(hl);
            bus.write(de, v);
            sms_set_hl(&cpu, static_cast<uint16_t>(hl + delta));
            sms_set_de(&cpu, static_cast<uint16_t>(de + delta));
            const uint16_t bc = static_cast<uint16_t>(sms_bc(&cpu) - 1U);
            sms_set_bc(&cpu, bc);
            const uint8_t n = static_cast<uint8_t>(cpu.a + v);
            uint8_t f = static_cast<uint8_t>(cpu.f & (SMS_FLAG_S | SMS_FLAG_Z | SMS_FLAG_C));
            if (bc != 0U) f |= SMS_FLAG_PV;
            if (n & 0x08U) f |= SMS_FLAG_X;
            if (n & 0x02U) f |= SMS_FLAG_Y;
            cpu.f = f;
            if (repeat && bc != 0U) {
                cpu.pc = static_cast<uint16_t>(cpu.pc - 2U);
                return info.cycles_taken;
            }
            return info.cycles;
        }
        case 0xA1U: {  // CPI / CPD / CPIR / CPDR
            const uint16_t hl = sms_hl(&cpu);
            const uint8_t v = bus.read(hl);
            sms_set_hl(&cpu, static_cast<uint16_t>(hl + delta));
            const uint16_t bc = static_cast<uint16_t>(sms_bc(&cpu) - 1U);
            sms_set_bc(&cpu, bc);
            const uint8_t res = static_cast<uint8_t>(cpu.a - v);
            const bool half = static_cast<int>(cpu.a & 0x0FU) - static_cast<int>(v & 0x0FU) < 0;
            const uint8_t n = static_cast<uint8_t>(res - (half ? 1U : 0U));
            uint8_t f = static_cast<uint8_t>((cpu.f & SMS_FLAG_C) | SMS_FLAG_N |
                                             (res & SMS_FLAG_S) | (res == 0U ? SMS_FLAG_Z : 0U));
            if (half) f |= SMS_FLAG_H;
            if (bc != 0U) f |= SMS_FLAG_PV;
            if (n & 0x08U) f |= SMS_FLAG_X;
            if (n & 0x02U) f |= SMS_FLAG_Y;
            cpu.f = f;
            if (repeat && bc != 0U && res != 0U) {
                cpu.pc = static_cast<uint16_t>(cpu.pc - 2U);
                return info.cycles_taken;
            }
            return info.cycles;
        }
        case 0xA2U: {  // INI / IND / INIR / INDR
            const uint8_t v = bus.io_read(cpu.c);
            const uint16_t hl = sms_hl(&cpu);
            bus.write(hl, v);
            sms_set_hl(&cpu, static_cast<uint16_t>(hl + delta));
            cpu.b = static_cast<uint8_t>(cpu.b - 1U);
            cpu.f = static_cast<uint8_t>((cpu.f & SMS_FLAG_C) | sms_szxy(cpu.b) | SMS_FLAG_N);
            if (repeat && cpu.b != 0U) {
                cpu.pc = static_cast<uint16_t>(cpu.pc - 2U);
                return info.cycles_taken;
            }
            return info.cycles;
        }
        case 0xA3U: {  // OUTI / OUTD / OTIR / OTDR
            const uint16_t hl = sms_hl(&cpu);
            const uint8_t v = bus.read(hl);
            cpu.b = static_cast<uint8_t>(cpu.b - 1U);
            bus.io_write(cpu.c, v);
            sms_set_hl(&cpu, static_cast<uint16_t>(hl + delta));
            cpu.f = static_cast<uint8_t>((cpu.f & SMS_FLAG_C) | sms_szxy(cpu.b) | SMS_FLAG_N);
            if (repeat && cpu.b != 0U) {
                cpu.pc = static_cast<uint16_t>(cpu.pc - 2U);
                return info.cycles_taken;
            }
            return info.cycles;
        }
        default:
            return info.cycles;
    }
}

uint32_t exec_base(SmsCpuState& cpu, Bus& bus, uint8_t op) noexcept;

uint32_t exec_index(SmsCpuState& cpu, Bus& bus, uint16_t& idx_reg) noexcept {
    inc_r(cpu);
    const uint8_t op = fetch8(cpu, bus);

    // Chained DD/FD/ED prefixes simply override the current prefix (re-dispatch)
    if (op == 0xDDU) return 4U + exec_index(cpu, bus, cpu.ix);
    if (op == 0xFDU) return 4U + exec_index(cpu, bus, cpu.iy);
    if (op == 0xEDU) return 4U + exec_ed(cpu, bus);

    if (op == 0xCBU) {
        // DDCB / FDCB <d> <cb_op>
        const int8_t d = static_cast<int8_t>(fetch8(cpu, bus));
        const uint8_t cb_op = fetch8(cpu, bus);
        const uint16_t ea = static_cast<uint16_t>(idx_reg + d);
        const uint8_t x = (cb_op >> 6U) & 3U;
        const uint8_t y = (cb_op >> 3U) & 7U;
        const uint8_t r = cb_op & 7U;
        const uint8_t val = bus.read(ea);
        if (x == 1U) {
            sms_bit(&cpu, y, val);
            return 20U;
        }
        uint8_t res = val;
        if (x == 0U) {
            res = exec_rot_shift(cpu, y, val);
        } else if (x == 2U) {
            res = static_cast<uint8_t>(val & ~(1U << y));
        } else {
            res = static_cast<uint8_t>(val | (1U << y));
        }
        bus.write(ea, res);
        if (r != 6U) {
            write_r8(cpu, bus, r, res);  // Undocumented copy-back to register
        }
        return 23U;
    }

    auto idx_hi = [&]() -> uint8_t { return static_cast<uint8_t>((idx_reg >> 8U) & 0xFFU); };
    auto idx_lo = [&]() -> uint8_t { return static_cast<uint8_t>(idx_reg & 0xFFU); };
    auto set_idx_hi = [&](uint8_t v) {
        idx_reg = static_cast<uint16_t>((idx_reg & 0x00FFU) | (static_cast<uint16_t>(v) << 8U));
    };
    auto set_idx_lo = [&](uint8_t v) {
        idx_reg = static_cast<uint16_t>((idx_reg & 0xFF00U) | v);
    };

    switch (op) {
        case 0x09U: idx_reg = sms_add16(&cpu, idx_reg, sms_bc(&cpu)); return 15U;
        case 0x19U: idx_reg = sms_add16(&cpu, idx_reg, sms_de(&cpu)); return 15U;
        case 0x29U: idx_reg = sms_add16(&cpu, idx_reg, idx_reg); return 15U;
        case 0x39U: idx_reg = sms_add16(&cpu, idx_reg, cpu.sp); return 15U;
        case 0x21U: idx_reg = fetch16(cpu, bus); return 14U;
        case 0x22U: {
            const uint16_t addr = fetch16(cpu, bus);
            bus.write(addr, idx_lo());
            bus.write(static_cast<uint16_t>(addr + 1U), idx_hi());
            return 20U;
        }
        case 0x2AU: {
            const uint16_t addr = fetch16(cpu, bus);
            idx_reg = static_cast<uint16_t>(
                bus.read(addr) |
                (static_cast<uint16_t>(bus.read(static_cast<uint16_t>(addr + 1U))) << 8U));
            return 20U;
        }
        case 0x23U: idx_reg = static_cast<uint16_t>(idx_reg + 1U); return 10U;
        case 0x2BU: idx_reg = static_cast<uint16_t>(idx_reg - 1U); return 10U;
        case 0x24U: set_idx_hi(sms_inc8(&cpu, idx_hi())); return 8U;
        case 0x25U: set_idx_hi(sms_dec8(&cpu, idx_hi())); return 8U;
        case 0x26U: set_idx_hi(fetch8(cpu, bus)); return 11U;
        case 0x2CU: set_idx_lo(sms_inc8(&cpu, idx_lo())); return 8U;
        case 0x2DU: set_idx_lo(sms_dec8(&cpu, idx_lo())); return 8U;
        case 0x2EU: set_idx_lo(fetch8(cpu, bus)); return 11U;
        case 0x34U: {
            const uint16_t ea = static_cast<uint16_t>(idx_reg + static_cast<int8_t>(fetch8(cpu, bus)));
            bus.write(ea, sms_inc8(&cpu, bus.read(ea)));
            return 23U;
        }
        case 0x35U: {
            const uint16_t ea = static_cast<uint16_t>(idx_reg + static_cast<int8_t>(fetch8(cpu, bus)));
            bus.write(ea, sms_dec8(&cpu, bus.read(ea)));
            return 23U;
        }
        case 0x36U: {
            const int8_t d = static_cast<int8_t>(fetch8(cpu, bus));
            const uint8_t n = fetch8(cpu, bus);
            bus.write(static_cast<uint16_t>(idx_reg + d), n);
            return 19U;
        }
        case 0xE1U: idx_reg = sms_pop(&cpu); return 14U;
        case 0xE3U: {  // EX (SP), IX/IY
            const uint16_t v = static_cast<uint16_t>(
                bus.read(cpu.sp) |
                (static_cast<uint16_t>(bus.read(static_cast<uint16_t>(cpu.sp + 1U))) << 8U));
            bus.write(cpu.sp, idx_lo());
            bus.write(static_cast<uint16_t>(cpu.sp + 1U), idx_hi());
            idx_reg = v;
            return 23U;
        }
        case 0xE5U: sms_push(&cpu, idx_reg); return 15U;
        case 0xE9U: cpu.pc = idx_reg; return 8U;
        case 0xF9U: cpu.sp = idx_reg; return 10U;
        default: break;
    }

    // 8-bit LD r, r' block (0x40..0x7F, excluding HALT 0x76)
    if (op >= 0x40U && op <= 0x7FU && op != 0x76U) {
        const uint8_t dst = (op >> 3U) & 7U;
        const uint8_t src = op & 7U;
        if (dst == 6U) {
            const uint16_t ea = static_cast<uint16_t>(idx_reg + static_cast<int8_t>(fetch8(cpu, bus)));
            bus.write(ea, read_r8(cpu, bus, src));  // src=4/5 uses H/L when dst is (IX+d)!
            return 19U;
        }
        if (src == 6U) {
            const uint16_t ea = static_cast<uint16_t>(idx_reg + static_cast<int8_t>(fetch8(cpu, bus)));
            write_r8(cpu, bus, dst, bus.read(ea));
            return 19U;
        }
        // Undocumented half-index register loads (IXH/IXL)
        uint8_t val = (src == 4U) ? idx_hi() : (src == 5U ? idx_lo() : read_r8(cpu, bus, src));
        if (dst == 4U) set_idx_hi(val);
        else if (dst == 5U) set_idx_lo(val);
        else write_r8(cpu, bus, dst, val);
        return 8U;
    }

    // 8-bit ALU block (0x80..0xBF)
    if (op >= 0x80U && op <= 0xBFU) {
        const uint8_t alu_op = (op >> 3U) & 7U;
        const uint8_t src = op & 7U;
        if (src == 6U) {
            const uint16_t ea = static_cast<uint16_t>(idx_reg + static_cast<int8_t>(fetch8(cpu, bus)));
            exec_alu(cpu, alu_op, bus.read(ea));
            return 19U;
        }
        uint8_t val = (src == 4U) ? idx_hi() : (src == 5U ? idx_lo() : read_r8(cpu, bus, src));
        exec_alu(cpu, alu_op, val);
        return 8U;
    }

    // Any unaffected opcode executes as normal base opcode + 4 prefix T-cycles
    return 4U + exec_base(cpu, bus, op);
}

uint32_t exec_base(SmsCpuState& cpu, Bus& bus, uint8_t op) noexcept {
    const OpcodeInfo& info = opcode_info_base(op);

    // 0x40..0x7F: 8-bit register-to-register loads + HALT
    if (op >= 0x40U && op <= 0x7FU) {
        if (op == 0x76U) {
            cpu.halted = 1U;
            return 4U;
        }
        write_r8(cpu, bus, (op >> 3U) & 7U, read_r8(cpu, bus, op & 7U));
        return info.cycles;
    }

    // 0x80..0xBF: 8-bit ALU register ops
    if (op >= 0x80U && op <= 0xBFU) {
        exec_alu(cpu, (op >> 3U) & 7U, read_r8(cpu, bus, op & 7U));
        return info.cycles;
    }

    switch (op) {
        case 0x00U: return 4U;  // NOP
        case 0x01U: case 0x11U: case 0x21U: case 0x31U:
            write_rp(cpu, (op >> 4U) & 3U, fetch16(cpu, bus));
            return 10U;
        case 0x02U: bus.write(sms_bc(&cpu), cpu.a); return 7U;
        case 0x12U: bus.write(sms_de(&cpu), cpu.a); return 7U;
        case 0x22U: {
            const uint16_t a = fetch16(cpu, bus);
            bus.write(a, cpu.l);
            bus.write(static_cast<uint16_t>(a + 1U), cpu.h);
            return 16U;
        }
        case 0x32U: bus.write(fetch16(cpu, bus), cpu.a); return 13U;
        case 0x0AU: cpu.a = bus.read(sms_bc(&cpu)); return 7U;
        case 0x1AU: cpu.a = bus.read(sms_de(&cpu)); return 7U;
        case 0x2AU: {
            const uint16_t a = fetch16(cpu, bus);
            cpu.l = bus.read(a);
            cpu.h = bus.read(static_cast<uint16_t>(a + 1U));
            return 16U;
        }
        case 0x3AU: cpu.a = bus.read(fetch16(cpu, bus)); return 13U;

        case 0x03U: case 0x13U: case 0x23U: case 0x33U: {
            const uint8_t p = (op >> 4U) & 3U;
            write_rp(cpu, p, static_cast<uint16_t>(read_rp(cpu, p) + 1U));
            return 6U;
        }
        case 0x0BU: case 0x1BU: case 0x2BU: case 0x3BU: {
            const uint8_t p = (op >> 4U) & 3U;
            write_rp(cpu, p, static_cast<uint16_t>(read_rp(cpu, p) - 1U));
            return 6U;
        }
        case 0x04U: case 0x0CU: case 0x14U: case 0x1CU:
        case 0x24U: case 0x2CU: case 0x34U: case 0x3CU: {
            const uint8_t r = (op >> 3U) & 7U;
            write_r8(cpu, bus, r, sms_inc8(&cpu, read_r8(cpu, bus, r)));
            return info.cycles;
        }
        case 0x05U: case 0x0DU: case 0x15U: case 0x1DU:
        case 0x25U: case 0x2DU: case 0x35U: case 0x3DU: {
            const uint8_t r = (op >> 3U) & 7U;
            write_r8(cpu, bus, r, sms_dec8(&cpu, read_r8(cpu, bus, r)));
            return info.cycles;
        }
        case 0x06U: case 0x0EU: case 0x16U: case 0x1EU:
        case 0x26U: case 0x2EU: case 0x36U: case 0x3EU: {
            const uint8_t r = (op >> 3U) & 7U;
            write_r8(cpu, bus, r, fetch8(cpu, bus));
            return info.cycles;
        }
        case 0x09U: case 0x19U: case 0x29U: case 0x39U:
            sms_set_hl(&cpu, sms_add16(&cpu, sms_hl(&cpu), read_rp(cpu, (op >> 4U) & 3U)));
            return 11U;

        // Accumulator rotates & flag ops (preserve S, Z, P/V)
        case 0x07U: {  // RLCA
            const uint8_t cy = cpu.a >> 7U;
            cpu.a = static_cast<uint8_t>((cpu.a << 1U) | cy);
            cpu.f = static_cast<uint8_t>((cpu.f & (SMS_FLAG_S | SMS_FLAG_Z | SMS_FLAG_PV)) |
                                         (cpu.a & (SMS_FLAG_Y | SMS_FLAG_X)) | cy);
            return 4U;
        }
        case 0x0FU: {  // RRCA
            const uint8_t cy = cpu.a & 1U;
            cpu.a = static_cast<uint8_t>((cpu.a >> 1U) | (cy << 7U));
            cpu.f = static_cast<uint8_t>((cpu.f & (SMS_FLAG_S | SMS_FLAG_Z | SMS_FLAG_PV)) |
                                         (cpu.a & (SMS_FLAG_Y | SMS_FLAG_X)) | cy);
            return 4U;
        }
        case 0x17U: {  // RLA
            const uint8_t old_c = (cpu.f & SMS_FLAG_C) ? 1U : 0U;
            const uint8_t cy = cpu.a >> 7U;
            cpu.a = static_cast<uint8_t>((cpu.a << 1U) | old_c);
            cpu.f = static_cast<uint8_t>((cpu.f & (SMS_FLAG_S | SMS_FLAG_Z | SMS_FLAG_PV)) |
                                         (cpu.a & (SMS_FLAG_Y | SMS_FLAG_X)) | cy);
            return 4U;
        }
        case 0x1FU: {  // RRA
            const uint8_t old_c = (cpu.f & SMS_FLAG_C) ? 1U : 0U;
            const uint8_t cy = cpu.a & 1U;
            cpu.a = static_cast<uint8_t>((cpu.a >> 1U) | (old_c << 7U));
            cpu.f = static_cast<uint8_t>((cpu.f & (SMS_FLAG_S | SMS_FLAG_Z | SMS_FLAG_PV)) |
                                         (cpu.a & (SMS_FLAG_Y | SMS_FLAG_X)) | cy);
            return 4U;
        }
        case 0x27U: sms_daa(&cpu); return 4U;
        case 0x2FU:  // CPL
            cpu.a = static_cast<uint8_t>(~cpu.a);
            cpu.f = static_cast<uint8_t>((cpu.f & (SMS_FLAG_S | SMS_FLAG_Z | SMS_FLAG_PV | SMS_FLAG_C)) |
                                         (cpu.a & (SMS_FLAG_Y | SMS_FLAG_X)) | SMS_FLAG_H | SMS_FLAG_N);
            return 4U;
        case 0x37U:  // SCF
            cpu.f = static_cast<uint8_t>((cpu.f & (SMS_FLAG_S | SMS_FLAG_Z | SMS_FLAG_PV)) |
                                         (cpu.a & (SMS_FLAG_Y | SMS_FLAG_X)) | SMS_FLAG_C);
            return 4U;
        case 0x3FU: {  // CCF
            const uint8_t old_c = cpu.f & SMS_FLAG_C;
            cpu.f = static_cast<uint8_t>((cpu.f & (SMS_FLAG_S | SMS_FLAG_Z | SMS_FLAG_PV)) |
                                         (cpu.a & (SMS_FLAG_Y | SMS_FLAG_X)) |
                                         (old_c ? SMS_FLAG_H : SMS_FLAG_C));
            return 4U;
        }

        case 0x08U: sms_ex_af(&cpu); return 4U;
        case 0xD9U: sms_exx(&cpu); return 4U;
        case 0xEBU: sms_ex_de_hl(&cpu); return 4U;
        case 0xE3U: {  // EX (SP), HL
            const uint16_t v = static_cast<uint16_t>(
                bus.read(cpu.sp) |
                (static_cast<uint16_t>(bus.read(static_cast<uint16_t>(cpu.sp + 1U))) << 8U));
            bus.write(cpu.sp, cpu.l);
            bus.write(static_cast<uint16_t>(cpu.sp + 1U), cpu.h);
            sms_set_hl(&cpu, v);
            return 19U;
        }

        // Relative branches
        case 0x10U: {  // DJNZ d
            const int8_t d = static_cast<int8_t>(fetch8(cpu, bus));
            cpu.b = static_cast<uint8_t>(cpu.b - 1U);
            if (cpu.b != 0U) {
                cpu.pc = static_cast<uint16_t>(cpu.pc + d);
                return 13U;
            }
            return 8U;
        }
        case 0x18U: {  // JR d
            const int8_t d = static_cast<int8_t>(fetch8(cpu, bus));
            cpu.pc = static_cast<uint16_t>(cpu.pc + d);
            return 12U;
        }
        case 0x20U: case 0x28U: case 0x30U: case 0x38U: {  // JR cc, d
            const int8_t d = static_cast<int8_t>(fetch8(cpu, bus));
            if (eval_cc(cpu, ((op >> 3U) & 3U))) {
                cpu.pc = static_cast<uint16_t>(cpu.pc + d);
                return 12U;
            }
            return 7U;
        }

        // Returns
        case 0xC0U: case 0xC8U: case 0xD0U: case 0xD8U:
        case 0xE0U: case 0xE8U: case 0xF0U: case 0xF8U:
            if (eval_cc(cpu, (op >> 3U) & 7U)) {
                cpu.pc = sms_pop(&cpu);
                return 11U;
            }
            return 5U;
        case 0xC9U: cpu.pc = sms_pop(&cpu); return 10U;

        // Jumps
        case 0xC2U: case 0xCAU: case 0xD2U: case 0xDAU:
        case 0xE2U: case 0xEAU: case 0xF2U: case 0xFAU: {
            const uint16_t addr = fetch16(cpu, bus);
            if (eval_cc(cpu, (op >> 3U) & 7U)) {
                cpu.pc = addr;
            }
            return 10U;
        }
        case 0xC3U: cpu.pc = fetch16(cpu, bus); return 10U;
        case 0xE9U: cpu.pc = sms_hl(&cpu); return 4U;

        // Calls
        case 0xC4U: case 0xCCU: case 0xD4U: case 0xDCU:
        case 0xE4U: case 0xECU: case 0xF4U: case 0xFCU: {
            const uint16_t addr = fetch16(cpu, bus);
            if (eval_cc(cpu, (op >> 3U) & 7U)) {
                sms_push(&cpu, cpu.pc);
                cpu.pc = addr;
                return 17U;
            }
            return 10U;
        }
        case 0xCDU: {
            const uint16_t addr = fetch16(cpu, bus);
            sms_push(&cpu, cpu.pc);
            cpu.pc = addr;
            return 17U;
        }

        // Stack & SP
        case 0xC1U: case 0xD1U: case 0xE1U: case 0xF1U:
            write_rp2(cpu, (op >> 4U) & 3U, sms_pop(&cpu));
            return 10U;
        case 0xC5U: case 0xD5U: case 0xE5U: case 0xF5U:
            sms_push(&cpu, read_rp2(cpu, (op >> 4U) & 3U));
            return 11U;
        case 0xF9U: cpu.sp = sms_hl(&cpu); return 6U;

        // ALU immediate
        case 0xC6U: case 0xCEU: case 0xD6U: case 0xDEU:
        case 0xE6U: case 0xEEU: case 0xF6U: case 0xFEU:
            exec_alu(cpu, (op >> 3U) & 7U, fetch8(cpu, bus));
            return 7U;

        // RST p
        case 0xC7U: case 0xCFU: case 0xD7U: case 0xDFU:
        case 0xE7U: case 0xEFU: case 0xF7U: case 0xFFU:
            sms_push(&cpu, cpu.pc);
            cpu.pc = static_cast<uint16_t>(op & 0x38U);
            return 11U;

        // I/O immediate
        case 0xD3U: bus.io_write(fetch8(cpu, bus), cpu.a); return 11U;
        case 0xDBU: cpu.a = bus.io_read(fetch8(cpu, bus)); return 11U;

        // Interrupt control
        case 0xF3U:  // DI
            cpu.iff1 = 0U;
            cpu.iff2 = 0U;
            cpu.ei_delay = 0U;
            return 4U;
        case 0xFBU:  // EI (takes effect AFTER the next instruction)
            cpu.ei_delay = 1U;
            return 4U;

        // Prefixes
        case 0xCBU: return exec_cb(cpu, bus);
        case 0xDDU: return exec_index(cpu, bus, cpu.ix);
        case 0xEDU: return exec_ed(cpu, bus);
        case 0xFDU: return exec_index(cpu, bus, cpu.iy);

        default:
            return 4U;
    }
}

}  // namespace

uint32_t check_interrupts(SmsCpuState& cpu, Bus& bus) noexcept {
    // 1. NMI has priority over maskable IRQ and cannot be disabled by IFF1
    if (cpu.nmi_pending != 0U) {
        cpu.nmi_pending = 0U;
        cpu.halted = 0U;
        cpu.iff2 = cpu.iff1;
        cpu.iff1 = 0U;
        sms_push(&cpu, cpu.pc);
        cpu.pc = 0x0066U;
        cpu.cycles += 11U;
        return 11U;
    }

    // 2. Maskable VDP IRQ (level-triggered)
    if (bus.vdp().irq_asserted()) {
        if (cpu.halted != 0U) {
            cpu.halted = 0U;
        }
        if (cpu.iff1 != 0U && cpu.ei_delay == 0U) {
            cpu.iff1 = 0U;
            cpu.iff2 = 0U;
            sms_push(&cpu, cpu.pc);
            if (cpu.im == 2U) {
                const uint16_t vec_addr = static_cast<uint16_t>((static_cast<uint16_t>(cpu.i) << 8U) | 0xFFU);
                cpu.pc = static_cast<uint16_t>(
                    bus.read(vec_addr) |
                    (static_cast<uint16_t>(bus.read(static_cast<uint16_t>(vec_addr + 1U))) << 8U));
                cpu.cycles += 19U;
                return 19U;
            }
            // IM 1 (SMS standard) & IM 0 (open bus 0xFF -> RST 38h)
            cpu.pc = 0x0038U;
            cpu.cycles += 13U;
            return 13U;
        }
    }

    return 0U;
}

uint32_t step_instruction(SmsCpuState& cpu, Bus& bus) noexcept {
    uint32_t cycles = check_interrupts(cpu, bus);
    if (cycles > 0U) {
        bus.sync_hardware(cpu);
        return cycles;
    }

    const bool had_ei_delay = (cpu.ei_delay != 0U);

    if (cpu.halted != 0U) {
        cycles = 4U;
    } else {
        inc_r(cpu);
        const uint8_t op = fetch8(cpu, bus);
        cycles = exec_base(cpu, bus, op);
    }

    // EI activates IFF1/IFF2 immediately after the instruction following EI
    if (had_ei_delay && cpu.ei_delay != 0U) {
        cpu.ei_delay = 0U;
        cpu.iff1 = 1U;
        cpu.iff2 = 1U;
    }

    cpu.cycles += cycles;
    bus.sync_hardware(cpu);
    return cycles;
}

uint32_t run_frame_interpreter(SmsCpuState& cpu, Bus& bus, uint32_t max_cycles) noexcept {
    uint32_t total = 0U;
    while (total < max_cycles && !bus.stop_requested()) {
        total += step_instruction(cpu, bus);
        if (bus.vdp().consume_frame_ready()) {
            break;
        }
    }
    return total;
}

}  // namespace dragon::sms
