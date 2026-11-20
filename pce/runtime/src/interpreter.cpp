#include "pce/interpreter.h"

#include "pce/opcodes.h"

namespace dragon::pce {

namespace {

inline uint8_t fetch8(PceCpuState& cpu, Bus& bus) noexcept {
    const uint8_t v = bus.read(cpu, cpu.pc);
    cpu.pc = static_cast<uint16_t>(cpu.pc + 1U);
    return v;
}

inline uint16_t fetch16(PceCpuState& cpu, Bus& bus) noexcept {
    const uint8_t lo = fetch8(cpu, bus);
    const uint8_t hi = fetch8(cpu, bus);
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8U));
}

inline uint8_t zp_read(const PceCpuState& cpu, Bus& bus, uint8_t zp) noexcept {
    return bus.read(cpu, static_cast<uint16_t>(0x2000U | zp));
}

inline void zp_write(PceCpuState& cpu, Bus& bus, uint8_t zp, uint8_t v) noexcept {
    bus.write(cpu, static_cast<uint16_t>(0x2000U | zp), v);
}

inline uint16_t zp_read16(const PceCpuState& cpu, Bus& bus, uint8_t zp) noexcept {
    const uint8_t lo = zp_read(cpu, bus, zp);
    const uint8_t hi = zp_read(cpu, bus, static_cast<uint8_t>(zp + 1U));
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8U));
}

inline void push8(PceCpuState& cpu, Bus& bus, uint8_t v) noexcept {
    bus.write(cpu, static_cast<uint16_t>(0x2100U | cpu.s), v);
    cpu.s = static_cast<uint8_t>(cpu.s - 1U);
}

inline uint8_t pop8(PceCpuState& cpu, Bus& bus) noexcept {
    cpu.s = static_cast<uint8_t>(cpu.s + 1U);
    return bus.read(cpu, static_cast<uint16_t>(0x2100U | cpu.s));
}

inline void push16(PceCpuState& cpu, Bus& bus, uint16_t v) noexcept {
    push8(cpu, bus, static_cast<uint8_t>((v >> 8U) & 0xFFU));
    push8(cpu, bus, static_cast<uint8_t>(v & 0xFFU));
}

inline uint16_t pop16(PceCpuState& cpu, Bus& bus) noexcept {
    const uint8_t lo = pop8(cpu, bus);
    const uint8_t hi = pop8(cpu, bus);
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8U));
}

inline uint16_t addr_zp(PceCpuState& cpu, Bus& bus) noexcept {
    return static_cast<uint16_t>(0x2000U | fetch8(cpu, bus));
}
inline uint16_t addr_zpx(PceCpuState& cpu, Bus& bus) noexcept {
    return static_cast<uint16_t>(0x2000U | static_cast<uint8_t>(fetch8(cpu, bus) + cpu.x));
}
inline uint16_t addr_zpy(PceCpuState& cpu, Bus& bus) noexcept {
    return static_cast<uint16_t>(0x2000U | static_cast<uint8_t>(fetch8(cpu, bus) + cpu.y));
}
inline uint16_t addr_abs(PceCpuState& cpu, Bus& bus) noexcept { return fetch16(cpu, bus); }
inline uint16_t addr_absx(PceCpuState& cpu, Bus& bus) noexcept {
    return static_cast<uint16_t>(fetch16(cpu, bus) + cpu.x);
}
inline uint16_t addr_absy(PceCpuState& cpu, Bus& bus) noexcept {
    return static_cast<uint16_t>(fetch16(cpu, bus) + cpu.y);
}
inline uint16_t addr_zpind(PceCpuState& cpu, Bus& bus) noexcept {
    return zp_read16(cpu, bus, fetch8(cpu, bus));
}
inline uint16_t addr_zpindx(PceCpuState& cpu, Bus& bus) noexcept {
    return zp_read16(cpu, bus, static_cast<uint8_t>(fetch8(cpu, bus) + cpu.x));
}
inline uint16_t addr_zpindy(PceCpuState& cpu, Bus& bus) noexcept {
    return static_cast<uint16_t>(zp_read16(cpu, bus, fetch8(cpu, bus)) + cpu.y);
}

inline uint32_t branch_if(PceCpuState& cpu, Bus& bus, bool cond) noexcept {
    const int8_t rel = static_cast<int8_t>(fetch8(cpu, bus));
    if (cond) {
        cpu.pc = static_cast<uint16_t>(cpu.pc + rel);
        return 4U;
    }
    return 2U;
}

uint32_t exec_block_transfer(PceCpuState& cpu, Bus& bus, uint8_t op) noexcept {
    uint16_t src = fetch16(cpu, bus);
    uint16_t dst = fetch16(cpu, bus);
    uint16_t len = fetch16(cpu, bus);
    const uint32_t count = (len == 0U) ? 65536U : static_cast<uint32_t>(len);

    // HuC6280 preserves Y, A, X on stack internally (transparent to software), runs transfer
    bool alt = false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t v = bus.read(cpu, src);
        bus.write(cpu, dst, v);
        switch (op) {
            case 0x73U:  // TII
                src = static_cast<uint16_t>(src + 1U);
                dst = static_cast<uint16_t>(dst + 1U);
                break;
            case 0xC3U:  // TDD
                src = static_cast<uint16_t>(src - 1U);
                dst = static_cast<uint16_t>(dst - 1U);
                break;
            case 0xD3U:  // TIN
                src = static_cast<uint16_t>(src + 1U);
                break;
            case 0xE3U:  // TIA
                src = static_cast<uint16_t>(src + 1U);
                dst = static_cast<uint16_t>(alt ? (dst - 1U) : (dst + 1U));
                alt = !alt;
                break;
            case 0xF3U:  // TAI
                src = static_cast<uint16_t>(alt ? (src - 1U) : (src + 1U));
                dst = static_cast<uint16_t>(dst + 1U);
                alt = !alt;
                break;
            default:
                break;
        }
    }
    return 17U + 6U * count;
}

uint32_t service_vector(PceCpuState& cpu, Bus& bus, uint16_t vec, bool is_brk) noexcept {
    push16(cpu, bus, cpu.pc);
    uint8_t pushed_p = static_cast<uint8_t>(cpu.p & static_cast<uint8_t>(~PCE_FLAG_T));
    if (is_brk) {
        pushed_p |= PCE_FLAG_B;
    } else {
        pushed_p &= static_cast<uint8_t>(~PCE_FLAG_B);
    }
    push8(cpu, bus, pushed_p);
    cpu.p = static_cast<uint8_t>((cpu.p & static_cast<uint8_t>(~(PCE_FLAG_D | PCE_FLAG_T))) |
                                 PCE_FLAG_I);
    const uint8_t lo = bus.read(cpu, vec);
    const uint8_t hi = bus.read(cpu, static_cast<uint16_t>(vec + 1U));
    cpu.pc = static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8U));
    const uint32_t raw_cyc = 8U;
    pce_add_cycles(&cpu, raw_cyc);
    return cpu.speed_high ? raw_cyc : (raw_cyc << 2U);
}

}  // namespace

uint32_t check_interrupts(PceCpuState& cpu, Bus& bus) noexcept {
    if (cpu.nmi_pending != 0U) {
        cpu.nmi_pending = 0U;
        return service_vector(cpu, bus, PCE_VEC_NMI, false);
    }

    if ((cpu.p & PCE_FLAG_I) != 0U) {
        return 0U;
    }

    const uint8_t pending = bus.pending_irqs();
    if (pending == 0U) {
        return 0U;
    }

    // Priority order on HuC6280: TIQ > IRQ1 (VDC) > IRQ2 (External)
    if (pending & Bus::kTiqBit) {
        return service_vector(cpu, bus, PCE_VEC_TIQ, false);
    }
    if (pending & Bus::kIrq1Bit) {
        return service_vector(cpu, bus, PCE_VEC_IRQ1, false);
    }
    if (pending & Bus::kIrq2Bit) {
        return service_vector(cpu, bus, PCE_VEC_IRQ2, false);
    }
    return 0U;
}

uint32_t step_instruction(PceCpuState& cpu, Bus& bus) noexcept {
    const uint32_t irq_cyc = check_interrupts(cpu, bus);
    if (irq_cyc > 0U) {
        bus.sync_hardware(cpu);
        return irq_cyc;
    }

    const uint8_t op = fetch8(cpu, bus);
    const OpcodeInfo& info = opcode_info(op);
    uint32_t cyc = info.cycles;

    // T-flag only persists across SET ($F4) into the single immediately following instruction.
    const bool had_t = (cpu.p & PCE_FLAG_T) != 0U;
    if (op != 0xF4U) {
        cpu.p &= static_cast<uint8_t>(~PCE_FLAG_T);
    }

    auto do_ora = [&](uint8_t v) noexcept {
        if (had_t) {
            const uint8_t r = static_cast<uint8_t>(zp_read(cpu, bus, cpu.x) | v);
            zp_write(cpu, bus, cpu.x, r);
            pce_set_nz(&cpu, r);
            cyc += 3U;
        } else {
            cpu.a |= v;
            pce_set_nz(&cpu, cpu.a);
        }
    };
    auto do_and = [&](uint8_t v) noexcept {
        if (had_t) {
            const uint8_t r = static_cast<uint8_t>(zp_read(cpu, bus, cpu.x) & v);
            zp_write(cpu, bus, cpu.x, r);
            pce_set_nz(&cpu, r);
            cyc += 3U;
        } else {
            cpu.a &= v;
            pce_set_nz(&cpu, cpu.a);
        }
    };
    auto do_eor = [&](uint8_t v) noexcept {
        if (had_t) {
            const uint8_t r = static_cast<uint8_t>(zp_read(cpu, bus, cpu.x) ^ v);
            zp_write(cpu, bus, cpu.x, r);
            pce_set_nz(&cpu, r);
            cyc += 3U;
        } else {
            cpu.a ^= v;
            pce_set_nz(&cpu, cpu.a);
        }
    };
    auto do_adc = [&](uint8_t v) noexcept {
        if (had_t) {
            const uint8_t r = pce_adc_val(&cpu, zp_read(cpu, bus, cpu.x), v);
            zp_write(cpu, bus, cpu.x, r);
            cyc += 3U;
        } else {
            cpu.a = pce_adc_val(&cpu, cpu.a, v);
        }
    };

    switch (op) {
        // --- Control / Interrupts / Jumps / Subroutines ---
        case 0x00U: {  // BRK
            (void)fetch8(cpu, bus);
            push16(cpu, bus, cpu.pc);
            push8(cpu, bus, static_cast<uint8_t>((cpu.p & ~PCE_FLAG_T) | PCE_FLAG_B));
            cpu.p = static_cast<uint8_t>((cpu.p & ~(PCE_FLAG_D | PCE_FLAG_T)) | PCE_FLAG_I);
            const uint8_t lo = bus.read(cpu, PCE_VEC_IRQ2);
            const uint8_t hi = bus.read(cpu, static_cast<uint16_t>(PCE_VEC_IRQ2 + 1U));
            cpu.pc = static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8U));
            break;
        }
        case 0x20U: {  // JSR abs
            const uint16_t tgt = fetch16(cpu, bus);
            push16(cpu, bus, static_cast<uint16_t>(cpu.pc - 1U));
            cpu.pc = tgt;
            break;
        }
        case 0x44U: {  // BSR rel
            const int8_t rel = static_cast<int8_t>(fetch8(cpu, bus));
            push16(cpu, bus, static_cast<uint16_t>(cpu.pc - 1U));
            cpu.pc = static_cast<uint16_t>(cpu.pc + rel);
            break;
        }
        case 0x40U:  // RTI
            cpu.p = static_cast<uint8_t>(pop8(cpu, bus) & static_cast<uint8_t>(~(PCE_FLAG_B | PCE_FLAG_T)));
            cpu.pc = pop16(cpu, bus);
            break;
        case 0x60U:  // RTS
            cpu.pc = static_cast<uint16_t>(pop16(cpu, bus) + 1U);
            break;
        case 0x4CU:  // JMP abs
            cpu.pc = fetch16(cpu, bus);
            break;
        case 0x6CU: {  // JMP (abs)
            const uint16_t a = fetch16(cpu, bus);
            const uint8_t lo = bus.read(cpu, a);
            const uint8_t hi = bus.read(cpu, static_cast<uint16_t>(a + 1U));
            cpu.pc = static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8U));
            break;
        }
        case 0x7CU: {  // JMP (abs,X)
            const uint16_t a = static_cast<uint16_t>(fetch16(cpu, bus) + cpu.x);
            const uint8_t lo = bus.read(cpu, a);
            const uint8_t hi = bus.read(cpu, static_cast<uint16_t>(a + 1U));
            cpu.pc = static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8U));
            break;
        }

        // --- Branches ---
        case 0x10U: cyc = branch_if(cpu, bus, (cpu.p & PCE_FLAG_N) == 0U); break;  // BPL
        case 0x30U: cyc = branch_if(cpu, bus, (cpu.p & PCE_FLAG_N) != 0U); break;  // BMI
        case 0x50U: cyc = branch_if(cpu, bus, (cpu.p & PCE_FLAG_V) == 0U); break;  // BVC
        case 0x70U: cyc = branch_if(cpu, bus, (cpu.p & PCE_FLAG_V) != 0U); break;  // BVS
        case 0x80U: cyc = branch_if(cpu, bus, true); break;                        // BRA
        case 0x90U: cyc = branch_if(cpu, bus, (cpu.p & PCE_FLAG_C) == 0U); break;  // BCC
        case 0xB0U: cyc = branch_if(cpu, bus, (cpu.p & PCE_FLAG_C) != 0U); break;  // BCS
        case 0xD0U: cyc = branch_if(cpu, bus, (cpu.p & PCE_FLAG_Z) == 0U); break;  // BNE
        case 0xF0U: cyc = branch_if(cpu, bus, (cpu.p & PCE_FLAG_Z) != 0U); break;  // BEQ

        // --- HuC6280 BBR0..7 / BBS0..7 ---
        case 0x0FU: case 0x1FU: case 0x2FU: case 0x3FU:
        case 0x4FU: case 0x5FU: case 0x6FU: case 0x7FU: {
            const uint8_t bit = static_cast<uint8_t>((op >> 4U) & 7U);
            const uint8_t v = zp_read(cpu, bus, fetch8(cpu, bus));
            const int8_t rel = static_cast<int8_t>(fetch8(cpu, bus));
            if ((v & (1U << bit)) == 0U) {
                cpu.pc = static_cast<uint16_t>(cpu.pc + rel);
                cyc = 8U;
            }
            break;
        }
        case 0x8FU: case 0x9FU: case 0xAFU: case 0xBFU:
        case 0xCFU: case 0xDFU: case 0xEFU: case 0xFFU: {
            const uint8_t bit = static_cast<uint8_t>((op >> 4U) & 7U);
            const uint8_t v = zp_read(cpu, bus, fetch8(cpu, bus));
            const int8_t rel = static_cast<int8_t>(fetch8(cpu, bus));
            if ((v & (1U << bit)) != 0U) {
                cpu.pc = static_cast<uint16_t>(cpu.pc + rel);
                cyc = 8U;
            }
            break;
        }

        // --- HuC6280 RMB0..7 / SMB0..7 ---
        case 0x07U: case 0x17U: case 0x27U: case 0x37U:
        case 0x47U: case 0x57U: case 0x67U: case 0x77U: {
            const uint8_t bit = static_cast<uint8_t>((op >> 4U) & 7U);
            const uint8_t z = fetch8(cpu, bus);
            zp_write(cpu, bus, z, static_cast<uint8_t>(zp_read(cpu, bus, z) & ~(1U << bit)));
            break;
        }
        case 0x87U: case 0x97U: case 0xA7U: case 0xB7U:
        case 0xC7U: case 0xD7U: case 0xE7U: case 0xF7U: {
            const uint8_t bit = static_cast<uint8_t>((op >> 4U) & 7U);
            const uint8_t z = fetch8(cpu, bus);
            zp_write(cpu, bus, z, static_cast<uint8_t>(zp_read(cpu, bus, z) | (1U << bit)));
            break;
        }

        // --- HuC6280 VDC Direct Ports, MMU, Speed, Swaps, Clears, SET, Block Transfers ---
        case 0x03U: bus.write_vdc_direct(0U, fetch8(cpu, bus)); break;  // ST0
        case 0x13U: bus.write_vdc_direct(2U, fetch8(cpu, bus)); break;  // ST1
        case 0x23U: bus.write_vdc_direct(3U, fetch8(cpu, bus)); break;  // ST2
        case 0x43U: pce_tma(&cpu, fetch8(cpu, bus)); break;             // TMA
        case 0x53U: pce_tam(&cpu, fetch8(cpu, bus)); break;             // TAM
        case 0x54U: cpu.speed_high = 0U; break;                         // CSL
        case 0xD4U: cpu.speed_high = 1U; break;                         // CSH
        case 0xF4U: cpu.p |= PCE_FLAG_T; break;                         // SET
        case 0x02U: { const uint8_t t = cpu.x; cpu.x = cpu.y; cpu.y = t; break; }  // SXY
        case 0x22U: { const uint8_t t = cpu.a; cpu.a = cpu.x; cpu.x = t; break; }  // SAX
        case 0x42U: { const uint8_t t = cpu.a; cpu.a = cpu.y; cpu.y = t; break; }  // SAY
        case 0x62U: cpu.a = 0U; break;  // CLA
        case 0x82U: cpu.x = 0U; break;  // CLX
        case 0xC2U: cpu.y = 0U; break;  // CLY
        case 0x73U: case 0xC3U: case 0xD3U: case 0xE3U: case 0xF3U:
            cyc = exec_block_transfer(cpu, bus, op);
            break;

        // --- HuC6280 TST ---
        case 0x83U: { const uint8_t i = fetch8(cpu, bus); pce_tst_val(&cpu, i, bus.read(cpu, addr_zp(cpu, bus))); break; }
        case 0x93U: { const uint8_t i = fetch8(cpu, bus); pce_tst_val(&cpu, i, bus.read(cpu, addr_abs(cpu, bus))); break; }
        case 0xA3U: { const uint8_t i = fetch8(cpu, bus); pce_tst_val(&cpu, i, bus.read(cpu, addr_zpx(cpu, bus))); break; }
        case 0xB3U: { const uint8_t i = fetch8(cpu, bus); pce_tst_val(&cpu, i, bus.read(cpu, addr_absx(cpu, bus))); break; }

        // --- Flags & Stack ---
        case 0x18U: cpu.p &= static_cast<uint8_t>(~PCE_FLAG_C); break;  // CLC
        case 0x38U: cpu.p |= PCE_FLAG_C; break;                         // SEC
        case 0x58U: cpu.p &= static_cast<uint8_t>(~PCE_FLAG_I); break;  // CLI
        case 0x78U: cpu.p |= PCE_FLAG_I; break;                         // SEI
        case 0xB8U: cpu.p &= static_cast<uint8_t>(~PCE_FLAG_V); break;  // CLV
        case 0xD8U: cpu.p &= static_cast<uint8_t>(~PCE_FLAG_D); break;  // CLD
        case 0xF8U: cpu.p |= PCE_FLAG_D; break;                         // SED
        case 0x08U: push8(cpu, bus, static_cast<uint8_t>((cpu.p & ~PCE_FLAG_T) | PCE_FLAG_B)); break;  // PHP
        case 0x28U: cpu.p = static_cast<uint8_t>(pop8(cpu, bus) & ~(PCE_FLAG_B | PCE_FLAG_T)); break;  // PLP
        case 0x48U: push8(cpu, bus, cpu.a); break;                                                     // PHA
        case 0x68U: cpu.a = pop8(cpu, bus); pce_set_nz(&cpu, cpu.a); break;                            // PLA
        case 0xDAU: push8(cpu, bus, cpu.x); break;                                                     // PHX
        case 0xFAU: cpu.x = pop8(cpu, bus); pce_set_nz(&cpu, cpu.x); break;                            // PLX
        case 0x5AU: push8(cpu, bus, cpu.y); break;                                                     // PHY
        case 0x7AU: cpu.y = pop8(cpu, bus); pce_set_nz(&cpu, cpu.y); break;                            // PLY

        // --- Transfers & Register Inc/Dec ---
        case 0xAAU: cpu.x = cpu.a; pce_set_nz(&cpu, cpu.x); break;  // TAX
        case 0x8AU: cpu.a = cpu.x; pce_set_nz(&cpu, cpu.a); break;  // TXA
        case 0xA8U: cpu.y = cpu.a; pce_set_nz(&cpu, cpu.y); break;  // TAY
        case 0x98U: cpu.a = cpu.y; pce_set_nz(&cpu, cpu.a); break;  // TYA
        case 0xBAU: cpu.x = cpu.s; pce_set_nz(&cpu, cpu.x); break;  // TSX
        case 0x9AU: cpu.s = cpu.x; break;                           // TXS
        case 0xE8U: cpu.x = static_cast<uint8_t>(cpu.x + 1U); pce_set_nz(&cpu, cpu.x); break;  // INX
        case 0xCAU: cpu.x = static_cast<uint8_t>(cpu.x - 1U); pce_set_nz(&cpu, cpu.x); break;  // DEX
        case 0xC8U: cpu.y = static_cast<uint8_t>(cpu.y + 1U); pce_set_nz(&cpu, cpu.y); break;  // INY
        case 0x88U: cpu.y = static_cast<uint8_t>(cpu.y - 1U); pce_set_nz(&cpu, cpu.y); break;  // DEY
        case 0x1AU: cpu.a = static_cast<uint8_t>(cpu.a + 1U); pce_set_nz(&cpu, cpu.a); break;  // INC A
        case 0x3AU: cpu.a = static_cast<uint8_t>(cpu.a - 1U); pce_set_nz(&cpu, cpu.a); break;  // DEC A

        // --- Loads: LDA / LDX / LDY ---
        case 0xA9U: cpu.a = fetch8(cpu, bus); pce_set_nz(&cpu, cpu.a); break;
        case 0xA5U: cpu.a = bus.read(cpu, addr_zp(cpu, bus)); pce_set_nz(&cpu, cpu.a); break;
        case 0xB5U: cpu.a = bus.read(cpu, addr_zpx(cpu, bus)); pce_set_nz(&cpu, cpu.a); break;
        case 0xADU: cpu.a = bus.read(cpu, addr_abs(cpu, bus)); pce_set_nz(&cpu, cpu.a); break;
        case 0xBDU: cpu.a = bus.read(cpu, addr_absx(cpu, bus)); pce_set_nz(&cpu, cpu.a); break;
        case 0xB9U: cpu.a = bus.read(cpu, addr_absy(cpu, bus)); pce_set_nz(&cpu, cpu.a); break;
        case 0xA1U: cpu.a = bus.read(cpu, addr_zpindx(cpu, bus)); pce_set_nz(&cpu, cpu.a); break;
        case 0xB1U: cpu.a = bus.read(cpu, addr_zpindy(cpu, bus)); pce_set_nz(&cpu, cpu.a); break;
        case 0xB2U: cpu.a = bus.read(cpu, addr_zpind(cpu, bus)); pce_set_nz(&cpu, cpu.a); break;

        case 0xA2U: cpu.x = fetch8(cpu, bus); pce_set_nz(&cpu, cpu.x); break;
        case 0xA6U: cpu.x = bus.read(cpu, addr_zp(cpu, bus)); pce_set_nz(&cpu, cpu.x); break;
        case 0xB6U: cpu.x = bus.read(cpu, addr_zpy(cpu, bus)); pce_set_nz(&cpu, cpu.x); break;
        case 0xAEU: cpu.x = bus.read(cpu, addr_abs(cpu, bus)); pce_set_nz(&cpu, cpu.x); break;
        case 0xBEU: cpu.x = bus.read(cpu, addr_absy(cpu, bus)); pce_set_nz(&cpu, cpu.x); break;

        case 0xA0U: cpu.y = fetch8(cpu, bus); pce_set_nz(&cpu, cpu.y); break;
        case 0xA4U: cpu.y = bus.read(cpu, addr_zp(cpu, bus)); pce_set_nz(&cpu, cpu.y); break;
        case 0xB4U: cpu.y = bus.read(cpu, addr_zpx(cpu, bus)); pce_set_nz(&cpu, cpu.y); break;
        case 0xACU: cpu.y = bus.read(cpu, addr_abs(cpu, bus)); pce_set_nz(&cpu, cpu.y); break;
        case 0xBCU: cpu.y = bus.read(cpu, addr_absx(cpu, bus)); pce_set_nz(&cpu, cpu.y); break;

        // --- Stores: STA / STX / STY / STZ ---
        case 0x85U: bus.write(cpu, addr_zp(cpu, bus), cpu.a); break;
        case 0x95U: bus.write(cpu, addr_zpx(cpu, bus), cpu.a); break;
        case 0x8DU: bus.write(cpu, addr_abs(cpu, bus), cpu.a); break;
        case 0x9DU: bus.write(cpu, addr_absx(cpu, bus), cpu.a); break;
        case 0x99U: bus.write(cpu, addr_absy(cpu, bus), cpu.a); break;
        case 0x81U: bus.write(cpu, addr_zpindx(cpu, bus), cpu.a); break;
        case 0x91U: bus.write(cpu, addr_zpindy(cpu, bus), cpu.a); break;
        case 0x92U: bus.write(cpu, addr_zpind(cpu, bus), cpu.a); break;

        case 0x86U: bus.write(cpu, addr_zp(cpu, bus), cpu.x); break;
        case 0x96U: bus.write(cpu, addr_zpy(cpu, bus), cpu.x); break;
        case 0x8EU: bus.write(cpu, addr_abs(cpu, bus), cpu.x); break;

        case 0x84U: bus.write(cpu, addr_zp(cpu, bus), cpu.y); break;
        case 0x94U: bus.write(cpu, addr_zpx(cpu, bus), cpu.y); break;
        case 0x8CU: bus.write(cpu, addr_abs(cpu, bus), cpu.y); break;

        case 0x64U: bus.write(cpu, addr_zp(cpu, bus), 0U); break;
        case 0x74U: bus.write(cpu, addr_zpx(cpu, bus), 0U); break;
        case 0x9CU: bus.write(cpu, addr_abs(cpu, bus), 0U); break;
        case 0x9EU: bus.write(cpu, addr_absx(cpu, bus), 0U); break;

        // --- ALU: ORA / AND / EOR / ADC / SBC / CMP / CPX / CPY ---
        case 0x09U: do_ora(fetch8(cpu, bus)); break;
        case 0x05U: do_ora(bus.read(cpu, addr_zp(cpu, bus))); break;
        case 0x15U: do_ora(bus.read(cpu, addr_zpx(cpu, bus))); break;
        case 0x0DU: do_ora(bus.read(cpu, addr_abs(cpu, bus))); break;
        case 0x1DU: do_ora(bus.read(cpu, addr_absx(cpu, bus))); break;
        case 0x19U: do_ora(bus.read(cpu, addr_absy(cpu, bus))); break;
        case 0x01U: do_ora(bus.read(cpu, addr_zpindx(cpu, bus))); break;
        case 0x11U: do_ora(bus.read(cpu, addr_zpindy(cpu, bus))); break;
        case 0x12U: do_ora(bus.read(cpu, addr_zpind(cpu, bus))); break;

        case 0x29U: do_and(fetch8(cpu, bus)); break;
        case 0x25U: do_and(bus.read(cpu, addr_zp(cpu, bus))); break;
        case 0x35U: do_and(bus.read(cpu, addr_zpx(cpu, bus))); break;
        case 0x2DU: do_and(bus.read(cpu, addr_abs(cpu, bus))); break;
        case 0x3DU: do_and(bus.read(cpu, addr_absx(cpu, bus))); break;
        case 0x39U: do_and(bus.read(cpu, addr_absy(cpu, bus))); break;
        case 0x21U: do_and(bus.read(cpu, addr_zpindx(cpu, bus))); break;
        case 0x31U: do_and(bus.read(cpu, addr_zpindy(cpu, bus))); break;
        case 0x32U: do_and(bus.read(cpu, addr_zpind(cpu, bus))); break;

        case 0x49U: do_eor(fetch8(cpu, bus)); break;
        case 0x45U: do_eor(bus.read(cpu, addr_zp(cpu, bus))); break;
        case 0x55U: do_eor(bus.read(cpu, addr_zpx(cpu, bus))); break;
        case 0x4DU: do_eor(bus.read(cpu, addr_abs(cpu, bus))); break;
        case 0x5DU: do_eor(bus.read(cpu, addr_absx(cpu, bus))); break;
        case 0x59U: do_eor(bus.read(cpu, addr_absy(cpu, bus))); break;
        case 0x41U: do_eor(bus.read(cpu, addr_zpindx(cpu, bus))); break;
        case 0x51U: do_eor(bus.read(cpu, addr_zpindy(cpu, bus))); break;
        case 0x52U: do_eor(bus.read(cpu, addr_zpind(cpu, bus))); break;

        case 0x69U: do_adc(fetch8(cpu, bus)); break;
        case 0x65U: do_adc(bus.read(cpu, addr_zp(cpu, bus))); break;
        case 0x75U: do_adc(bus.read(cpu, addr_zpx(cpu, bus))); break;
        case 0x6DU: do_adc(bus.read(cpu, addr_abs(cpu, bus))); break;
        case 0x7DU: do_adc(bus.read(cpu, addr_absx(cpu, bus))); break;
        case 0x79U: do_adc(bus.read(cpu, addr_absy(cpu, bus))); break;
        case 0x61U: do_adc(bus.read(cpu, addr_zpindx(cpu, bus))); break;
        case 0x71U: do_adc(bus.read(cpu, addr_zpindy(cpu, bus))); break;
        case 0x72U: do_adc(bus.read(cpu, addr_zpind(cpu, bus))); break;

        case 0xE9U: cpu.a = pce_sbc_val(&cpu, cpu.a, fetch8(cpu, bus)); break;
        case 0xE5U: cpu.a = pce_sbc_val(&cpu, cpu.a, bus.read(cpu, addr_zp(cpu, bus))); break;
        case 0xF5U: cpu.a = pce_sbc_val(&cpu, cpu.a, bus.read(cpu, addr_zpx(cpu, bus))); break;
        case 0xEDU: cpu.a = pce_sbc_val(&cpu, cpu.a, bus.read(cpu, addr_abs(cpu, bus))); break;
        case 0xFDU: cpu.a = pce_sbc_val(&cpu, cpu.a, bus.read(cpu, addr_absx(cpu, bus))); break;
        case 0xF9U: cpu.a = pce_sbc_val(&cpu, cpu.a, bus.read(cpu, addr_absy(cpu, bus))); break;
        case 0xE1U: cpu.a = pce_sbc_val(&cpu, cpu.a, bus.read(cpu, addr_zpindx(cpu, bus))); break;
        case 0xF1U: cpu.a = pce_sbc_val(&cpu, cpu.a, bus.read(cpu, addr_zpindy(cpu, bus))); break;
        case 0xF2U: cpu.a = pce_sbc_val(&cpu, cpu.a, bus.read(cpu, addr_zpind(cpu, bus))); break;

        case 0xC9U: pce_cmp(&cpu, cpu.a, fetch8(cpu, bus)); break;
        case 0xC5U: pce_cmp(&cpu, cpu.a, bus.read(cpu, addr_zp(cpu, bus))); break;
        case 0xD5U: pce_cmp(&cpu, cpu.a, bus.read(cpu, addr_zpx(cpu, bus))); break;
        case 0xCDU: pce_cmp(&cpu, cpu.a, bus.read(cpu, addr_abs(cpu, bus))); break;
        case 0xDDU: pce_cmp(&cpu, cpu.a, bus.read(cpu, addr_absx(cpu, bus))); break;
        case 0xD9U: pce_cmp(&cpu, cpu.a, bus.read(cpu, addr_absy(cpu, bus))); break;
        case 0xC1U: pce_cmp(&cpu, cpu.a, bus.read(cpu, addr_zpindx(cpu, bus))); break;
        case 0xD1U: pce_cmp(&cpu, cpu.a, bus.read(cpu, addr_zpindy(cpu, bus))); break;
        case 0xD2U: pce_cmp(&cpu, cpu.a, bus.read(cpu, addr_zpind(cpu, bus))); break;

        case 0xE0U: pce_cmp(&cpu, cpu.x, fetch8(cpu, bus)); break;
        case 0xE4U: pce_cmp(&cpu, cpu.x, bus.read(cpu, addr_zp(cpu, bus))); break;
        case 0xECU: pce_cmp(&cpu, cpu.x, bus.read(cpu, addr_abs(cpu, bus))); break;

        case 0xC0U: pce_cmp(&cpu, cpu.y, fetch8(cpu, bus)); break;
        case 0xC4U: pce_cmp(&cpu, cpu.y, bus.read(cpu, addr_zp(cpu, bus))); break;
        case 0xCCU: pce_cmp(&cpu, cpu.y, bus.read(cpu, addr_abs(cpu, bus))); break;

        // --- BIT / TSB / TRB ---
        case 0x89U: {  // BIT #imm (65C02: only modifies Z, not N/V)
            const uint8_t v = fetch8(cpu, bus);
            cpu.p = static_cast<uint8_t>((cpu.p & ~PCE_FLAG_Z) | ((cpu.a & v) == 0U ? PCE_FLAG_Z : 0U));
            break;
        }
        case 0x24U: pce_tst_val(&cpu, cpu.a, bus.read(cpu, addr_zp(cpu, bus))); break;
        case 0x34U: pce_tst_val(&cpu, cpu.a, bus.read(cpu, addr_zpx(cpu, bus))); break;
        case 0x2CU: pce_tst_val(&cpu, cpu.a, bus.read(cpu, addr_abs(cpu, bus))); break;
        case 0x3CU: pce_tst_val(&cpu, cpu.a, bus.read(cpu, addr_absx(cpu, bus))); break;

        case 0x04U: { const uint16_t a = addr_zp(cpu, bus); const uint8_t m = bus.read(cpu, a); pce_tst_val(&cpu, cpu.a, m); bus.write(cpu, a, static_cast<uint8_t>(m | cpu.a)); break; }
        case 0x0CU: { const uint16_t a = addr_abs(cpu, bus); const uint8_t m = bus.read(cpu, a); pce_tst_val(&cpu, cpu.a, m); bus.write(cpu, a, static_cast<uint8_t>(m | cpu.a)); break; }
        case 0x14U: { const uint16_t a = addr_zp(cpu, bus); const uint8_t m = bus.read(cpu, a); pce_tst_val(&cpu, cpu.a, m); bus.write(cpu, a, static_cast<uint8_t>(m & ~cpu.a)); break; }
        case 0x1CU: { const uint16_t a = addr_abs(cpu, bus); const uint8_t m = bus.read(cpu, a); pce_tst_val(&cpu, cpu.a, m); bus.write(cpu, a, static_cast<uint8_t>(m & ~cpu.a)); break; }

        // --- Shifts, Rotates, Memory Inc/Dec ---
        case 0x0AU: cpu.a = pce_asl(&cpu, cpu.a); break;
        case 0x06U: { const uint16_t a = addr_zp(cpu, bus); bus.write(cpu, a, pce_asl(&cpu, bus.read(cpu, a))); break; }
        case 0x16U: { const uint16_t a = addr_zpx(cpu, bus); bus.write(cpu, a, pce_asl(&cpu, bus.read(cpu, a))); break; }
        case 0x0EU: { const uint16_t a = addr_abs(cpu, bus); bus.write(cpu, a, pce_asl(&cpu, bus.read(cpu, a))); break; }
        case 0x1EU: { const uint16_t a = addr_absx(cpu, bus); bus.write(cpu, a, pce_asl(&cpu, bus.read(cpu, a))); break; }

        case 0x4AU: cpu.a = pce_lsr(&cpu, cpu.a); break;
        case 0x46U: { const uint16_t a = addr_zp(cpu, bus); bus.write(cpu, a, pce_lsr(&cpu, bus.read(cpu, a))); break; }
        case 0x56U: { const uint16_t a = addr_zpx(cpu, bus); bus.write(cpu, a, pce_lsr(&cpu, bus.read(cpu, a))); break; }
        case 0x4EU: { const uint16_t a = addr_abs(cpu, bus); bus.write(cpu, a, pce_lsr(&cpu, bus.read(cpu, a))); break; }
        case 0x5EU: { const uint16_t a = addr_absx(cpu, bus); bus.write(cpu, a, pce_lsr(&cpu, bus.read(cpu, a))); break; }

        case 0x2AU: cpu.a = pce_rol(&cpu, cpu.a); break;
        case 0x26U: { const uint16_t a = addr_zp(cpu, bus); bus.write(cpu, a, pce_rol(&cpu, bus.read(cpu, a))); break; }
        case 0x36U: { const uint16_t a = addr_zpx(cpu, bus); bus.write(cpu, a, pce_rol(&cpu, bus.read(cpu, a))); break; }
        case 0x2EU: { const uint16_t a = addr_abs(cpu, bus); bus.write(cpu, a, pce_rol(&cpu, bus.read(cpu, a))); break; }
        case 0x3EU: { const uint16_t a = addr_absx(cpu, bus); bus.write(cpu, a, pce_rol(&cpu, bus.read(cpu, a))); break; }

        case 0x6AU: cpu.a = pce_ror(&cpu, cpu.a); break;
        case 0x66U: { const uint16_t a = addr_zp(cpu, bus); bus.write(cpu, a, pce_ror(&cpu, bus.read(cpu, a))); break; }
        case 0x76U: { const uint16_t a = addr_zpx(cpu, bus); bus.write(cpu, a, pce_ror(&cpu, bus.read(cpu, a))); break; }
        case 0x6EU: { const uint16_t a = addr_abs(cpu, bus); bus.write(cpu, a, pce_ror(&cpu, bus.read(cpu, a))); break; }
        case 0x7EU: { const uint16_t a = addr_absx(cpu, bus); bus.write(cpu, a, pce_ror(&cpu, bus.read(cpu, a))); break; }

        case 0xE6U: { const uint16_t a = addr_zp(cpu, bus); const uint8_t v = static_cast<uint8_t>(bus.read(cpu, a) + 1U); bus.write(cpu, a, v); pce_set_nz(&cpu, v); break; }
        case 0xF6U: { const uint16_t a = addr_zpx(cpu, bus); const uint8_t v = static_cast<uint8_t>(bus.read(cpu, a) + 1U); bus.write(cpu, a, v); pce_set_nz(&cpu, v); break; }
        case 0xEEU: { const uint16_t a = addr_abs(cpu, bus); const uint8_t v = static_cast<uint8_t>(bus.read(cpu, a) + 1U); bus.write(cpu, a, v); pce_set_nz(&cpu, v); break; }
        case 0xFEU: { const uint16_t a = addr_absx(cpu, bus); const uint8_t v = static_cast<uint8_t>(bus.read(cpu, a) + 1U); bus.write(cpu, a, v); pce_set_nz(&cpu, v); break; }

        case 0xC6U: { const uint16_t a = addr_zp(cpu, bus); const uint8_t v = static_cast<uint8_t>(bus.read(cpu, a) - 1U); bus.write(cpu, a, v); pce_set_nz(&cpu, v); break; }
        case 0xD6U: { const uint16_t a = addr_zpx(cpu, bus); const uint8_t v = static_cast<uint8_t>(bus.read(cpu, a) - 1U); bus.write(cpu, a, v); pce_set_nz(&cpu, v); break; }
        case 0xCEU: { const uint16_t a = addr_abs(cpu, bus); const uint8_t v = static_cast<uint8_t>(bus.read(cpu, a) - 1U); bus.write(cpu, a, v); pce_set_nz(&cpu, v); break; }
        case 0xDEU: { const uint16_t a = addr_absx(cpu, bus); const uint8_t v = static_cast<uint8_t>(bus.read(cpu, a) - 1U); bus.write(cpu, a, v); pce_set_nz(&cpu, v); break; }

        // NOPs & undefined opcodes (1-byte 2-cycle NOPs on HuC6280)
        default:
            break;
    }

    pce_add_cycles(&cpu, cyc);
    bus.sync_hardware(cpu);
    return cpu.speed_high ? cyc : (cyc << 2U);
}

uint32_t run_frame_interpreter(PceCpuState& cpu, Bus& bus, uint32_t max_cycles) noexcept {
    uint32_t total = 0U;
    while (total < max_cycles && !bus.stop_requested()) {
        total += step_instruction(cpu, bus);
        if (bus.vdc().consume_frame_ready()) {
            break;
        }
    }
    return total;
}

}  // namespace dragon::pce
