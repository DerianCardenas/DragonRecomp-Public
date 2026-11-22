#include "ps1/interpreter.h"

#include "dragon/mips.h"
#include "ps1/gte.h"
#include "ps1/runtime.h"

namespace dragon::ps1 {

namespace {

inline bool is_bios_vector(uint32_t pc) noexcept {
    const uint32_t p = pc & 0x1FFFFFFFU;
    return (p == 0x000000A0U || p == 0x000000B0U || p == 0x000000C0U);
}

void exec_non_branch(Ps1Runtime& rt, Ps1CpuState& cpu,
                     const dragon::mips::DecodedMips& d) noexcept {
    using dragon::mips::MipsOp;
    Ps1Bus& bus = rt.bus();
    auto set_gpr = [&](uint8_t r, uint32_t v) noexcept {
        if (r != 0)
            cpu.gpr[r] = v;
    };

    switch (d.op) {
        case MipsOp::Nop:
        case MipsOp::Sync:
        case MipsOp::Cache:
        case MipsOp::Break:
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

        // Mul / Div
        case MipsOp::Mult: {
            const int64_t p = static_cast<int64_t>(static_cast<int32_t>(cpu.gpr[d.rs])) *
                              static_cast<int64_t>(static_cast<int32_t>(cpu.gpr[d.rt]));
            cpu.lo = static_cast<uint32_t>(p & 0xFFFFFFFFLL);
            cpu.hi = static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32U);
            break;
        }
        case MipsOp::Multu: {
            const uint64_t p =
                static_cast<uint64_t>(cpu.gpr[d.rs]) * static_cast<uint64_t>(cpu.gpr[d.rt]);
            cpu.lo = static_cast<uint32_t>(p & 0xFFFFFFFFULL);
            cpu.hi = static_cast<uint32_t>(p >> 32U);
            break;
        }
        case MipsOp::Div: {
            const int32_t a = static_cast<int32_t>(cpu.gpr[d.rs]);
            const int32_t b = static_cast<int32_t>(cpu.gpr[d.rt]);
            if (b == 0) {
                cpu.lo = (a >= 0) ? 0xFFFFFFFFU : 1U;
                cpu.hi = static_cast<uint32_t>(a);
            } else if (a == INT32_MIN && b == -1) {
                cpu.lo = static_cast<uint32_t>(INT32_MIN);
                cpu.hi = 0U;
            } else {
                cpu.lo = static_cast<uint32_t>(a / b);
                cpu.hi = static_cast<uint32_t>(a % b);
            }
            break;
        }
        case MipsOp::Divu: {
            const uint32_t a = cpu.gpr[d.rs];
            const uint32_t b = cpu.gpr[d.rt];
            if (b == 0U) {
                cpu.lo = 0xFFFFFFFFU;
                cpu.hi = a;
            } else {
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

        // Loads / Stores (skip RAM writes if COP0 SR bit 16 IsC = Isolate Cache is set)
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
        case MipsOp::Lw: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            set_gpr(d.rt, bus.read32(addr));
            break;
        }
        case MipsOp::Lwl: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            set_gpr(d.rt, ps1_lwl(&cpu, addr, cpu.gpr[d.rt]));
            break;
        }
        case MipsOp::Lwr: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            set_gpr(d.rt, ps1_lwr(&cpu, addr, cpu.gpr[d.rt]));
            break;
        }
        case MipsOp::Sb:
            if ((cpu.cop0[12] & 0x00010000U) == 0U) {
                bus.write8(cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm)),
                           static_cast<uint8_t>(cpu.gpr[d.rt] & 0xFFU));
            }
            break;
        case MipsOp::Sh:
            if ((cpu.cop0[12] & 0x00010000U) == 0U) {
                bus.write16(cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm)),
                            static_cast<uint16_t>(cpu.gpr[d.rt] & 0xFFFFU));
            }
            break;
        case MipsOp::Sw:
            if ((cpu.cop0[12] & 0x00010000U) == 0U) {
                bus.write32(cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm)),
                            cpu.gpr[d.rt]);
            }
            break;
        case MipsOp::Swl:
            if ((cpu.cop0[12] & 0x00010000U) == 0U) {
                ps1_swl(&cpu, cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm)),
                        cpu.gpr[d.rt]);
            }
            break;
        case MipsOp::Swr:
            if ((cpu.cop0[12] & 0x00010000U) == 0U) {
                ps1_swr(&cpu, cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm)),
                        cpu.gpr[d.rt]);
            }
            break;

        // COP0
        case MipsOp::Mfc0:
            set_gpr(d.rt, ps1_cop0_read(&cpu, d.rd));
            break;
        case MipsOp::Mtc0:
            ps1_cop0_write(&cpu, d.rd, cpu.gpr[d.rt]);
            break;
        case MipsOp::Rfe:
            ps1_rfe(&cpu);
            break;

        // COP2 (GTE)
        case MipsOp::Lwc2: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            Ps1Gte::write_data(cpu, d.rt, bus.read32(addr));
            break;
        }
        case MipsOp::Swc2: {
            const uint32_t addr =
                cpu.gpr[d.rs] + static_cast<uint32_t>(static_cast<int32_t>(d.simm));
            bus.write32(addr, Ps1Gte::read_data(cpu, d.rt));
            break;
        }
        case MipsOp::Cop2: {
            if ((d.raw & 0x02000000U) != 0U) {
                Ps1Gte::execute_command(cpu, d.raw);
            } else {
                switch (d.rs) {
                    case 0x00:
                        set_gpr(d.rt, Ps1Gte::read_data(cpu, d.rd));
                        break;  // mfc2
                    case 0x02:
                        set_gpr(d.rt, Ps1Gte::read_ctrl(cpu, d.rd));
                        break;  // cfc2
                    case 0x04:
                        Ps1Gte::write_data(cpu, d.rd, cpu.gpr[d.rt]);
                        break;  // mtc2
                    case 0x06:
                        Ps1Gte::write_ctrl(cpu, d.rd, cpu.gpr[d.rt]);
                        break;  // ctc2
                    default:
                        break;
                }
            }
            break;
        }

        // SYSCALL -> HLE
        case MipsOp::Syscall:
            rt.bios().handle_syscall(rt, cpu, d.syscall_code);
            break;

        default:
            break;
    }
    cpu.gpr[0] = 0U;
}

}  // namespace

void Ps1Interpreter::step(Ps1Runtime& rt, Ps1CpuState& cpu) noexcept {
    using dragon::mips::MipsOp;

    const uint32_t pc = cpu.pc;
    if (is_bios_vector(pc)) {
        rt.bios().handle_vector(rt, cpu, pc);
        cpu.pc = cpu.gpr[31];
        cpu.npc = cpu.pc + 4U;
        cpu.gpr[0] = 0U;
        cpu.cycles += 12ULL;
        return;
    }

    const uint32_t raw = rt.bus().read32(pc);
    const auto d = dragon::mips::decode_mips(raw, pc, dragon::mips::MipsProfile::R3000A);

    const uint32_t cur_npc = cpu.npc;
    if (!d.has_delay_slot) {
        exec_non_branch(rt, cpu, d);
        cpu.pc = cur_npc;
        cpu.npc = cur_npc + 4U;
        cpu.gpr[0] = 0U;
        ++cpu.cycles;
        return;
    }

    bool take = false;
    uint32_t target = d.target;
    const uint32_t rs_v = cpu.gpr[d.rs];
    const uint32_t rt_v = cpu.gpr[d.rt];
    const int32_t rs_s = static_cast<int32_t>(rs_v);

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
            take = (rs_v == rt_v);
            break;
        case MipsOp::Bne:
            take = (rs_v != rt_v);
            break;
        case MipsOp::Blez:
            take = (rs_s <= 0);
            break;
        case MipsOp::Bgtz:
            take = (rs_s > 0);
            break;
        case MipsOp::Bltz:
            take = (rs_s < 0);
            break;
        case MipsOp::Bgez:
            take = (rs_s >= 0);
            break;
        case MipsOp::Bltzal:
            cpu.gpr[31] = pc + 8U;
            take = (rs_s < 0);
            break;
        case MipsOp::Bgezal:
            cpu.gpr[31] = pc + 8U;
            take = (rs_s >= 0);
            break;
        default:
            break;
    }

    cpu.pc = cur_npc;
    cpu.npc = take ? target : (cur_npc + 4U);
    cpu.gpr[0] = 0U;
    ++cpu.cycles;
}

uint64_t Ps1Interpreter::run_slice(Ps1Runtime& rt, Ps1CpuState& cpu,
                                   uint64_t max_instructions) noexcept {
    uint64_t executed = 0;
    while (executed < max_instructions && !cpu.halted && !cpu.vblank_yield) {
        if (cpu.pc == 0xBFC00000U || cpu.pc == 0U) {
            cpu.halted = 1U;
            break;
        }
        step(rt, cpu);
        ++executed;
        if ((executed & 255ULL) == 0ULL) {
            rt.bus().step_timers(256U);
        }
    }
    return executed;
}

void Ps1Interpreter::run_until_return(Ps1Runtime& rt, Ps1CpuState& cpu, uint32_t target_pc,
                                      uint32_t return_pc, uint64_t max_instructions) noexcept {
    cpu.pc = target_pc;
    cpu.npc = target_pc + 4U;
    uint64_t executed = 0;
    while (executed < max_instructions && !cpu.halted && !cpu.vblank_yield) {
        if (cpu.pc == return_pc || cpu.pc == 0xBFC00000U || cpu.pc == 0U)
            break;
        step(rt, cpu);
        ++executed;
    }
}

}  // namespace dragon::ps1

// =========================================================================
// extern "C" Runtime Helpers for C99 Static Recompiled Code
// =========================================================================
extern "C" {

static inline dragon::ps1::Ps1Runtime* get_rt(Ps1CpuState* cpu) noexcept {
    return static_cast<dragon::ps1::Ps1Runtime*>(cpu->runtime);
}

uint8_t ps1_read8(Ps1CpuState* cpu, uint32_t addr) {
    return get_rt(cpu)->bus().read8(addr);
}
uint16_t ps1_read16(Ps1CpuState* cpu, uint32_t addr) {
    return get_rt(cpu)->bus().read16(addr);
}
uint32_t ps1_read32(Ps1CpuState* cpu, uint32_t addr) {
    return get_rt(cpu)->bus().read32(addr);
}

void ps1_write8(Ps1CpuState* cpu, uint32_t addr, uint8_t val) {
    if ((cpu->cop0[12] & 0x00010000U) == 0U)
        get_rt(cpu)->bus().write8(addr, val);
}
void ps1_write16(Ps1CpuState* cpu, uint32_t addr, uint16_t val) {
    if ((cpu->cop0[12] & 0x00010000U) == 0U)
        get_rt(cpu)->bus().write16(addr, val);
}
void ps1_write32(Ps1CpuState* cpu, uint32_t addr, uint32_t val) {
    if ((cpu->cop0[12] & 0x00010000U) == 0U)
        get_rt(cpu)->bus().write32(addr, val);
}

uint32_t ps1_lwl(Ps1CpuState* cpu, uint32_t addr, uint32_t rt_val) {
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

uint32_t ps1_lwr(Ps1CpuState* cpu, uint32_t addr, uint32_t rt_val) {
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

void ps1_swl(Ps1CpuState* cpu, uint32_t addr, uint32_t rt_val) {
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

void ps1_swr(Ps1CpuState* cpu, uint32_t addr, uint32_t rt_val) {
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

uint32_t ps1_cop0_read(Ps1CpuState* cpu, uint8_t reg) {
    reg &= 31U;
    if (reg == 15U)
        return 0x00000002U;  // R3000A PRID
    return cpu->cop0[reg];
}

void ps1_cop0_write(Ps1CpuState* cpu, uint8_t reg, uint32_t val) {
    cpu->cop0[reg & 31U] = val;
}

void ps1_rfe(Ps1CpuState* cpu) {
    cpu->cop0[12] = (cpu->cop0[12] & ~0x0FU) | ((cpu->cop0[12] >> 2U) & 0x0FU);
}

uint32_t ps1_gte_read_data(Ps1CpuState* cpu, uint8_t reg) {
    return dragon::ps1::Ps1Gte::read_data(*cpu, reg);
}

void ps1_gte_write_data(Ps1CpuState* cpu, uint8_t reg, uint32_t val) {
    dragon::ps1::Ps1Gte::write_data(*cpu, reg, val);
}

uint32_t ps1_gte_read_ctrl(Ps1CpuState* cpu, uint8_t reg) {
    return dragon::ps1::Ps1Gte::read_ctrl(*cpu, reg);
}

void ps1_gte_write_ctrl(Ps1CpuState* cpu, uint8_t reg, uint32_t val) {
    dragon::ps1::Ps1Gte::write_ctrl(*cpu, reg, val);
}

void ps1_gte_cmd(Ps1CpuState* cpu, uint32_t raw_insn) {
    dragon::ps1::Ps1Gte::execute_command(*cpu, raw_insn);
}

void ps1_syscall(Ps1CpuState* cpu, uint32_t code) {
    auto* rt = get_rt(cpu);
    rt->bios().handle_syscall(*rt, *cpu, code);
}

void ps1_bios_call(Ps1CpuState* cpu, uint32_t vector_addr) {
    auto* rt = get_rt(cpu);
    rt->bios().handle_vector(*rt, *cpu, vector_addr);
}

void ps1_runtime_dispatch(Ps1CpuState* cpu, uint32_t target_pc) {
    auto* rt = get_rt(cpu);
    const uint32_t p = target_pc & 0x1FFFFFFFU;
    if (p == 0x000000A0U || p == 0x000000B0U || p == 0x000000C0U) {
        rt->bios().handle_vector(*rt, *cpu, target_pc);
        return;
    }
    if (target_pc == 0xBFC00000U || target_pc == 0U) {
        cpu->halted = 1U;
        return;
    }
    if (auto fn = rt->lookup_recomp(target_pc)) {
        rt->note_recomp_call();
        fn(cpu);
        return;
    }
    rt->note_interp_fallback();
    const uint32_t ret_pc = cpu->gpr[31];
    dragon::ps1::Ps1Interpreter::run_until_return(*rt, *cpu, target_pc, ret_pc);
}

}  // extern "C"
