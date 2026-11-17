#include "snes/interpreter.h"

#include "snes/opcodes.h"

namespace dragon::snes {

namespace {

constexpr uint64_t kMasterClocksPerCpuCycle = 6ULL;

bool is_dp_mode(AddrMode mode) noexcept {
    switch (mode) {
        case AddrMode::DP:
        case AddrMode::DPX:
        case AddrMode::DPY:
        case AddrMode::DPInd:
        case AddrMode::DPIndLong:
        case AddrMode::DPXInd:
        case AddrMode::DPIndY:
        case AddrMode::DPIndLongY:
            return true;
        default:
            return false;
    }
}

uint16_t dp_addr(const CpuState& cpu, uint8_t dp, uint16_t index = 0U,
                 bool indexed = false) noexcept {
    if (cpu.e && (cpu.d & 0x00FFU) == 0U && indexed) {
        return static_cast<uint16_t>((cpu.d & 0xFF00U) | ((dp + index) & 0x00FFU));
    }
    return static_cast<uint16_t>(cpu.d + dp + index);
}

uint16_t read_dp_ptr16(CpuState& cpu, uint8_t dp, uint16_t index = 0U,
                       bool indexed = false) noexcept {
    if (cpu.e && (cpu.d & 0x00FFU) == 0U) {
        const uint8_t off = static_cast<uint8_t>(dp + (indexed ? (index & 0xFFU) : 0U));
        const uint16_t hi_base = static_cast<uint16_t>(cpu.d & 0xFF00U);
        const uint8_t lo = bus_read(&cpu, static_cast<uint16_t>(hi_base | off));
        const uint8_t hi =
            bus_read(&cpu, static_cast<uint16_t>(hi_base | static_cast<uint8_t>(off + 1U)));
        return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
    }
    const uint16_t ptr = static_cast<uint16_t>(cpu.d + dp + (indexed ? index : 0U));
    return bus_read16_bank0(&cpu, ptr);
}

}  // namespace

void cpu_init(CpuState& cpu, Bus* bus) noexcept {
    cpu = CpuState{};
    cpu.s = 0x01FFU;
    cpu.d = 0x0000U;
    cpu.db = 0x00U;
    cpu.pb = 0x00U;
    cpu.pc = 0x8000U;
    cpu.c = 0U;
    cpu.v = 0U;
    cpu.i = 1U;
    cpu.d_flag = 0U;
    cpu.m = 1U;
    cpu.xf = 1U;
    cpu.e = 1U;
    cpu.n = 0U;
    cpu.z = 1U;
    cpu.master_cycles = 0ULL;
    cpu.next_event = 357368ULL;
    cpu.bus = bus;
    cpu.halted = 0U;
    cpu.waiting = 0U;
    if (bus) {
        bus->attach_cpu(&cpu);
    }
}

void cpu_reset(CpuState& cpu) noexcept {
    cpu.d = 0x0000U;
    cpu.db = 0x00U;
    cpu.pb = 0x00U;
    cpu.s = 0x01FFU;
    cpu.e = 1U;
    cpu.m = 1U;
    cpu.xf = 1U;
    cpu.i = 1U;
    cpu.d_flag = 0U;
    cpu.x &= 0x00FFU;
    cpu.y &= 0x00FFU;
    cpu.halted = 0U;
    cpu.waiting = 0U;
    cpu.pc = bus_read16_bank0(&cpu, 0xFFFCU);
}

void cpu_nmi(CpuState& cpu) noexcept {
    cpu.waiting = 0U;
    if (!cpu.e) {
        cpu_push8(&cpu, cpu.pb);
        cpu_push16(&cpu, cpu.pc);
        cpu_push8(&cpu, cpu_pack_flags(&cpu, 0U));
        cpu.i = 1U;
        cpu.d_flag = 0U;
        cpu.pb = 0x00U;
        cpu.pc = bus_read16_bank0(&cpu, 0xFFEAU);
        cpu.master_cycles += 8ULL * kMasterClocksPerCpuCycle;
    } else {
        cpu_push16(&cpu, cpu.pc);
        cpu_push8(&cpu, cpu_pack_flags(&cpu, 0U));
        cpu.i = 1U;
        cpu.d_flag = 0U;
        cpu.pb = 0x00U;
        cpu.pc = bus_read16_bank0(&cpu, 0xFFFAU);
        cpu.master_cycles += 7ULL * kMasterClocksPerCpuCycle;
    }
}

bool cpu_irq(CpuState& cpu) noexcept {
    if (cpu.i != 0U) {
        return false;
    }
    cpu.waiting = 0U;
    if (!cpu.e) {
        cpu_push8(&cpu, cpu.pb);
        cpu_push16(&cpu, cpu.pc);
        cpu_push8(&cpu, cpu_pack_flags(&cpu, 0U));
        cpu.i = 1U;
        cpu.d_flag = 0U;
        cpu.pb = 0x00U;
        cpu.pc = bus_read16_bank0(&cpu, 0xFFEEU);
        cpu.master_cycles += 8ULL * kMasterClocksPerCpuCycle;
    } else {
        cpu_push16(&cpu, cpu.pc);
        cpu_push8(&cpu, cpu_pack_flags(&cpu, 0U));
        cpu.i = 1U;
        cpu.d_flag = 0U;
        cpu.pb = 0x00U;
        cpu.pc = bus_read16_bank0(&cpu, 0xFFFEU);
        cpu.master_cycles += 7ULL * kMasterClocksPerCpuCycle;
    }
    return true;
}

uint32_t cpu_step(CpuState& cpu) noexcept {
    if (cpu.halted || cpu.waiting) {
        return 0U;
    }

    const uint8_t pb = cpu.pb;
    const uint16_t instr_pc = cpu.pc;
    const bool eff_m = (cpu.e != 0U) || (cpu.m != 0U);
    const bool eff_x = (cpu.e != 0U) || (cpu.xf != 0U);

    const uint8_t op = bus_read(&cpu, snes_addr24(pb, instr_pc));
    const OpcodeInfo& info = get_opcode_info(op);
    const uint8_t len = instruction_length(info.mode, eff_m, eff_x, cpu.e != 0U);

    uint8_t op1 = 0U;
    uint8_t op2 = 0U;
    uint8_t op3 = 0U;
    if (len >= 2U) {
        op1 = bus_read(&cpu, snes_addr24(pb, static_cast<uint16_t>(instr_pc + 1U)));
    }
    if (len >= 3U) {
        op2 = bus_read(&cpu, snes_addr24(pb, static_cast<uint16_t>(instr_pc + 2U)));
    }
    if (len >= 4U) {
        op3 = bus_read(&cpu, snes_addr24(pb, static_cast<uint16_t>(instr_pc + 3U)));
    }

    const uint16_t operand16 =
        static_cast<uint16_t>((static_cast<uint16_t>(op2) << 8U) | static_cast<uint16_t>(op1));
    const uint32_t operand24 = (static_cast<uint32_t>(op3) << 16U) |
                               (static_cast<uint32_t>(op2) << 8U) | static_cast<uint32_t>(op1);
    const uint16_t next_pc = static_cast<uint16_t>(instr_pc + len);
    cpu.pc = next_pc;

    uint32_t cycles = info.base_cycles;
    if ((cpu.d & 0x00FFU) != 0U && is_dp_mode(info.mode)) {
        cycles += 1U;
    }

    // Effective 24-bit data address and page-cross check for memory addressing modes
    uint32_t eff_addr = 0U;
    bool page_crossed = false;

    switch (info.mode) {
        case AddrMode::Implied:
        case AddrMode::ImmM:
        case AddrMode::ImmX:
        case AddrMode::Imm8:
        case AddrMode::Imm16:
        case AddrMode::PCRel8:
        case AddrMode::PCRel16:
        case AddrMode::BlockMove:
        case AddrMode::AbsInd:
        case AddrMode::AbsIndLong:
        case AddrMode::AbsXInd:
            break;
        case AddrMode::DP:
            eff_addr = dp_addr(cpu, op1);
            break;
        case AddrMode::DPX:
            eff_addr = dp_addr(cpu, op1, cpu.x, true);
            break;
        case AddrMode::DPY:
            eff_addr = dp_addr(cpu, op1, cpu.y, true);
            break;
        case AddrMode::DPInd:
            if (info.mnemonic != Mnemonic::PEI) {
                eff_addr = snes_addr24(cpu.db, read_dp_ptr16(cpu, op1));
            }
            break;
        case AddrMode::DPIndLong:
            eff_addr = bus_read24_bank0(&cpu, static_cast<uint16_t>(cpu.d + op1));
            break;
        case AddrMode::DPXInd:
            eff_addr = snes_addr24(cpu.db, read_dp_ptr16(cpu, op1, cpu.x, true));
            break;
        case AddrMode::DPIndY: {
            const uint16_t base16 = read_dp_ptr16(cpu, op1);
            const uint32_t base24 = snes_addr24(cpu.db, base16);
            eff_addr = (base24 + cpu.y) & 0xFFFFFFU;
            page_crossed =
                !eff_x || ((base16 & 0xFF00U) != (static_cast<uint16_t>(base16 + cpu.y) & 0xFF00U));
            break;
        }
        case AddrMode::DPIndLongY: {
            const uint32_t base24 = bus_read24_bank0(&cpu, static_cast<uint16_t>(cpu.d + op1));
            eff_addr = (base24 + cpu.y) & 0xFFFFFFU;
            break;
        }
        case AddrMode::StackRel:
            eff_addr = static_cast<uint16_t>(cpu.s + op1);
            break;
        case AddrMode::StackRelIndY: {
            const uint16_t base16 = bus_read16_bank0(&cpu, static_cast<uint16_t>(cpu.s + op1));
            eff_addr = (snes_addr24(cpu.db, base16) + cpu.y) & 0xFFFFFFU;
            break;
        }
        case AddrMode::Abs:
            eff_addr = snes_addr24(cpu.db, operand16);
            break;
        case AddrMode::AbsX: {
            const uint32_t base24 = snes_addr24(cpu.db, operand16);
            eff_addr = (base24 + cpu.x) & 0xFFFFFFU;
            page_crossed = !eff_x || ((operand16 & 0xFF00U) !=
                                      (static_cast<uint16_t>(operand16 + cpu.x) & 0xFF00U));
            break;
        }
        case AddrMode::AbsY: {
            const uint32_t base24 = snes_addr24(cpu.db, operand16);
            eff_addr = (base24 + cpu.y) & 0xFFFFFFU;
            page_crossed = !eff_x || ((operand16 & 0xFF00U) !=
                                      (static_cast<uint16_t>(operand16 + cpu.y) & 0xFF00U));
            break;
        }
        case AddrMode::AbsLong:
            eff_addr = operand24;
            break;
        case AddrMode::AbsLongX:
            eff_addr = (operand24 + cpu.x) & 0xFFFFFFU;
            break;
    }

    if (info.page_cross_penalty && page_crossed) {
        cycles += 1U;
    }

    auto read_m8 = [&]() -> uint8_t {
        return (info.mode == AddrMode::ImmM) ? op1 : bus_read(&cpu, eff_addr);
    };
    auto read_m16 = [&]() -> uint16_t {
        if (info.mode == AddrMode::ImmM) {
            return operand16;
        }
        if (is_dp_mode(info.mode) && info.mode != AddrMode::DPInd &&
            info.mode != AddrMode::DPIndLong && info.mode != AddrMode::DPXInd &&
            info.mode != AddrMode::DPIndY && info.mode != AddrMode::DPIndLongY) {
            return bus_read16_bank0(&cpu, static_cast<uint16_t>(eff_addr));
        }
        return bus_read16(&cpu, eff_addr);
    };
    auto write_m8 = [&](uint8_t val) { bus_write(&cpu, eff_addr, val); };
    auto write_m16 = [&](uint16_t val) {
        if (info.mode == AddrMode::DP || info.mode == AddrMode::DPX || info.mode == AddrMode::DPY ||
            info.mode == AddrMode::StackRel) {
            bus_write16_bank0(&cpu, static_cast<uint16_t>(eff_addr), val);
        } else {
            bus_write16(&cpu, eff_addr, val);
        }
    };

    auto read_x8 = [&]() -> uint8_t {
        return (info.mode == AddrMode::ImmX) ? op1 : bus_read(&cpu, eff_addr);
    };
    auto read_x16 = [&]() -> uint16_t {
        if (info.mode == AddrMode::ImmX) {
            return operand16;
        }
        if (info.mode == AddrMode::DP || info.mode == AddrMode::DPX || info.mode == AddrMode::DPY) {
            return bus_read16_bank0(&cpu, static_cast<uint16_t>(eff_addr));
        }
        return bus_read16(&cpu, eff_addr);
    };

    switch (info.mnemonic) {
        case Mnemonic::LDA:
            if (eff_m) {
                const uint8_t v = read_m8();
                cpu.a = static_cast<uint16_t>((cpu.a & 0xFF00U) | v);
                cpu_set_nz8(&cpu, v);
            } else {
                cycles += 1U;
                cpu.a = read_m16();
                cpu_set_nz16(&cpu, cpu.a);
            }
            break;

        case Mnemonic::LDX:
            if (eff_x) {
                cpu.x = read_x8();
                cpu_set_nz8(&cpu, static_cast<uint8_t>(cpu.x));
            } else {
                cycles += 1U;
                cpu.x = read_x16();
                cpu_set_nz16(&cpu, cpu.x);
            }
            break;

        case Mnemonic::LDY:
            if (eff_x) {
                cpu.y = read_x8();
                cpu_set_nz8(&cpu, static_cast<uint8_t>(cpu.y));
            } else {
                cycles += 1U;
                cpu.y = read_x16();
                cpu_set_nz16(&cpu, cpu.y);
            }
            break;

        case Mnemonic::STA:
            if (eff_m) {
                write_m8(static_cast<uint8_t>(cpu.a & 0xFFU));
            } else {
                cycles += 1U;
                write_m16(cpu.a);
            }
            break;

        case Mnemonic::STX:
            if (eff_x) {
                write_m8(static_cast<uint8_t>(cpu.x & 0xFFU));
            } else {
                cycles += 1U;
                write_m16(cpu.x);
            }
            break;

        case Mnemonic::STY:
            if (eff_x) {
                write_m8(static_cast<uint8_t>(cpu.y & 0xFFU));
            } else {
                cycles += 1U;
                write_m16(cpu.y);
            }
            break;

        case Mnemonic::STZ:
            if (eff_m) {
                write_m8(0U);
            } else {
                cycles += 1U;
                write_m16(0U);
            }
            break;

        case Mnemonic::ADC:
            if (eff_m) {
                cpu_adc8(&cpu, read_m8());
            } else {
                cycles += 1U;
                cpu_adc16(&cpu, read_m16());
            }
            break;

        case Mnemonic::SBC:
            if (eff_m) {
                cpu_sbc8(&cpu, read_m8());
            } else {
                cycles += 1U;
                cpu_sbc16(&cpu, read_m16());
            }
            break;

        case Mnemonic::AND:
            if (eff_m) {
                const uint8_t res = static_cast<uint8_t>((cpu.a & 0xFFU) & read_m8());
                cpu.a = static_cast<uint16_t>((cpu.a & 0xFF00U) | res);
                cpu_set_nz8(&cpu, res);
            } else {
                cycles += 1U;
                cpu.a = static_cast<uint16_t>(cpu.a & read_m16());
                cpu_set_nz16(&cpu, cpu.a);
            }
            break;

        case Mnemonic::ORA:
            if (eff_m) {
                const uint8_t res = static_cast<uint8_t>((cpu.a & 0xFFU) | read_m8());
                cpu.a = static_cast<uint16_t>((cpu.a & 0xFF00U) | res);
                cpu_set_nz8(&cpu, res);
            } else {
                cycles += 1U;
                cpu.a = static_cast<uint16_t>(cpu.a | read_m16());
                cpu_set_nz16(&cpu, cpu.a);
            }
            break;

        case Mnemonic::EOR:
            if (eff_m) {
                const uint8_t res = static_cast<uint8_t>((cpu.a & 0xFFU) ^ read_m8());
                cpu.a = static_cast<uint16_t>((cpu.a & 0xFF00U) | res);
                cpu_set_nz8(&cpu, res);
            } else {
                cycles += 1U;
                cpu.a = static_cast<uint16_t>(cpu.a ^ read_m16());
                cpu_set_nz16(&cpu, cpu.a);
            }
            break;

        case Mnemonic::CMP:
            if (eff_m) {
                cpu_cmp8(&cpu, static_cast<uint8_t>(cpu.a & 0xFFU), read_m8());
            } else {
                cycles += 1U;
                cpu_cmp16(&cpu, cpu.a, read_m16());
            }
            break;

        case Mnemonic::CPX:
            if (eff_x) {
                cpu_cmp8(&cpu, static_cast<uint8_t>(cpu.x & 0xFFU), read_x8());
            } else {
                cycles += 1U;
                cpu_cmp16(&cpu, cpu.x, read_x16());
            }
            break;

        case Mnemonic::CPY:
            if (eff_x) {
                cpu_cmp8(&cpu, static_cast<uint8_t>(cpu.y & 0xFFU), read_x8());
            } else {
                cycles += 1U;
                cpu_cmp16(&cpu, cpu.y, read_x16());
            }
            break;

        case Mnemonic::BIT:
            if (info.mode == AddrMode::ImmM) {
                if (eff_m) {
                    cpu.z = static_cast<uint16_t>((cpu.a & 0xFFU) & op1);
                } else {
                    cycles += 1U;
                    cpu.z = static_cast<uint16_t>(cpu.a & operand16);
                }
            } else {
                if (eff_m) {
                    cpu_bit8(&cpu, read_m8());
                } else {
                    cycles += 1U;
                    cpu_bit16(&cpu, read_m16());
                }
            }
            break;

        case Mnemonic::TSB:
            if (eff_m) {
                write_m8(cpu_tsb8(&cpu, read_m8()));
            } else {
                cycles += 2U;
                write_m16(cpu_tsb16(&cpu, read_m16()));
            }
            break;

        case Mnemonic::TRB:
            if (eff_m) {
                write_m8(cpu_trb8(&cpu, read_m8()));
            } else {
                cycles += 2U;
                write_m16(cpu_trb16(&cpu, read_m16()));
            }
            break;

        case Mnemonic::ASL:
            if (info.mode == AddrMode::Implied) {
                if (eff_m) {
                    const uint8_t r = cpu_asl8(&cpu, static_cast<uint8_t>(cpu.a & 0xFFU));
                    cpu.a = static_cast<uint16_t>((cpu.a & 0xFF00U) | r);
                } else {
                    cpu.a = cpu_asl16(&cpu, cpu.a);
                }
            } else {
                if (eff_m) {
                    write_m8(cpu_asl8(&cpu, read_m8()));
                } else {
                    cycles += 2U;
                    write_m16(cpu_asl16(&cpu, read_m16()));
                }
            }
            break;

        case Mnemonic::LSR:
            if (info.mode == AddrMode::Implied) {
                if (eff_m) {
                    const uint8_t r = cpu_lsr8(&cpu, static_cast<uint8_t>(cpu.a & 0xFFU));
                    cpu.a = static_cast<uint16_t>((cpu.a & 0xFF00U) | r);
                } else {
                    cpu.a = cpu_lsr16(&cpu, cpu.a);
                }
            } else {
                if (eff_m) {
                    write_m8(cpu_lsr8(&cpu, read_m8()));
                } else {
                    cycles += 2U;
                    write_m16(cpu_lsr16(&cpu, read_m16()));
                }
            }
            break;

        case Mnemonic::ROL:
            if (info.mode == AddrMode::Implied) {
                if (eff_m) {
                    const uint8_t r = cpu_rol8(&cpu, static_cast<uint8_t>(cpu.a & 0xFFU));
                    cpu.a = static_cast<uint16_t>((cpu.a & 0xFF00U) | r);
                } else {
                    cpu.a = cpu_rol16(&cpu, cpu.a);
                }
            } else {
                if (eff_m) {
                    write_m8(cpu_rol8(&cpu, read_m8()));
                } else {
                    cycles += 2U;
                    write_m16(cpu_rol16(&cpu, read_m16()));
                }
            }
            break;

        case Mnemonic::ROR:
            if (info.mode == AddrMode::Implied) {
                if (eff_m) {
                    const uint8_t r = cpu_ror8(&cpu, static_cast<uint8_t>(cpu.a & 0xFFU));
                    cpu.a = static_cast<uint16_t>((cpu.a & 0xFF00U) | r);
                } else {
                    cpu.a = cpu_ror16(&cpu, cpu.a);
                }
            } else {
                if (eff_m) {
                    write_m8(cpu_ror8(&cpu, read_m8()));
                } else {
                    cycles += 2U;
                    write_m16(cpu_ror16(&cpu, read_m16()));
                }
            }
            break;

        case Mnemonic::INC:
            if (info.mode == AddrMode::Implied) {
                if (eff_m) {
                    const uint8_t r = static_cast<uint8_t>((cpu.a & 0xFFU) + 1U);
                    cpu.a = static_cast<uint16_t>((cpu.a & 0xFF00U) | r);
                    cpu_set_nz8(&cpu, r);
                } else {
                    cpu.a = static_cast<uint16_t>(cpu.a + 1U);
                    cpu_set_nz16(&cpu, cpu.a);
                }
            } else {
                if (eff_m) {
                    const uint8_t r = static_cast<uint8_t>(read_m8() + 1U);
                    write_m8(r);
                    cpu_set_nz8(&cpu, r);
                } else {
                    cycles += 2U;
                    const uint16_t r = static_cast<uint16_t>(read_m16() + 1U);
                    write_m16(r);
                    cpu_set_nz16(&cpu, r);
                }
            }
            break;

        case Mnemonic::DEC:
            if (info.mode == AddrMode::Implied) {
                if (eff_m) {
                    const uint8_t r = static_cast<uint8_t>((cpu.a & 0xFFU) - 1U);
                    cpu.a = static_cast<uint16_t>((cpu.a & 0xFF00U) | r);
                    cpu_set_nz8(&cpu, r);
                } else {
                    cpu.a = static_cast<uint16_t>(cpu.a - 1U);
                    cpu_set_nz16(&cpu, cpu.a);
                }
            } else {
                if (eff_m) {
                    const uint8_t r = static_cast<uint8_t>(read_m8() - 1U);
                    write_m8(r);
                    cpu_set_nz8(&cpu, r);
                } else {
                    cycles += 2U;
                    const uint16_t r = static_cast<uint16_t>(read_m16() - 1U);
                    write_m16(r);
                    cpu_set_nz16(&cpu, r);
                }
            }
            break;

        case Mnemonic::INX:
            if (eff_x) {
                cpu.x = static_cast<uint16_t>((cpu.x + 1U) & 0x00FFU);
                cpu_set_nz8(&cpu, static_cast<uint8_t>(cpu.x));
            } else {
                cpu.x = static_cast<uint16_t>(cpu.x + 1U);
                cpu_set_nz16(&cpu, cpu.x);
            }
            break;

        case Mnemonic::DEX:
            if (eff_x) {
                cpu.x = static_cast<uint16_t>((cpu.x - 1U) & 0x00FFU);
                cpu_set_nz8(&cpu, static_cast<uint8_t>(cpu.x));
            } else {
                cpu.x = static_cast<uint16_t>(cpu.x - 1U);
                cpu_set_nz16(&cpu, cpu.x);
            }
            break;

        case Mnemonic::INY:
            if (eff_x) {
                cpu.y = static_cast<uint16_t>((cpu.y + 1U) & 0x00FFU);
                cpu_set_nz8(&cpu, static_cast<uint8_t>(cpu.y));
            } else {
                cpu.y = static_cast<uint16_t>(cpu.y + 1U);
                cpu_set_nz16(&cpu, cpu.y);
            }
            break;

        case Mnemonic::DEY:
            if (eff_x) {
                cpu.y = static_cast<uint16_t>((cpu.y - 1U) & 0x00FFU);
                cpu_set_nz8(&cpu, static_cast<uint8_t>(cpu.y));
            } else {
                cpu.y = static_cast<uint16_t>(cpu.y - 1U);
                cpu_set_nz16(&cpu, cpu.y);
            }
            break;

        case Mnemonic::TAX:
            if (eff_x) {
                cpu.x = static_cast<uint16_t>(cpu.a & 0x00FFU);
                cpu_set_nz8(&cpu, static_cast<uint8_t>(cpu.x));
            } else {
                cpu.x = cpu.a;
                cpu_set_nz16(&cpu, cpu.x);
            }
            break;

        case Mnemonic::TAY:
            if (eff_x) {
                cpu.y = static_cast<uint16_t>(cpu.a & 0x00FFU);
                cpu_set_nz8(&cpu, static_cast<uint8_t>(cpu.y));
            } else {
                cpu.y = cpu.a;
                cpu_set_nz16(&cpu, cpu.y);
            }
            break;

        case Mnemonic::TXA:
            if (eff_m) {
                const uint8_t r = static_cast<uint8_t>(cpu.x & 0x00FFU);
                cpu.a = static_cast<uint16_t>((cpu.a & 0xFF00U) | r);
                cpu_set_nz8(&cpu, r);
            } else {
                cpu.a = cpu.x;
                cpu_set_nz16(&cpu, cpu.a);
            }
            break;

        case Mnemonic::TYA:
            if (eff_m) {
                const uint8_t r = static_cast<uint8_t>(cpu.y & 0x00FFU);
                cpu.a = static_cast<uint16_t>((cpu.a & 0xFF00U) | r);
                cpu_set_nz8(&cpu, r);
            } else {
                cpu.a = cpu.y;
                cpu_set_nz16(&cpu, cpu.a);
            }
            break;

        case Mnemonic::TSX:
            if (eff_x) {
                cpu.x = static_cast<uint16_t>(cpu.s & 0x00FFU);
                cpu_set_nz8(&cpu, static_cast<uint8_t>(cpu.x));
            } else {
                cpu.x = cpu.s;
                cpu_set_nz16(&cpu, cpu.x);
            }
            break;

        case Mnemonic::TXS:
            if (cpu.e) {
                cpu.s = static_cast<uint16_t>(0x0100U | (cpu.x & 0x00FFU));
            } else {
                cpu.s = cpu.x;
            }
            break;

        case Mnemonic::TXY:
            if (eff_x) {
                cpu.y = static_cast<uint16_t>(cpu.x & 0x00FFU);
                cpu_set_nz8(&cpu, static_cast<uint8_t>(cpu.y));
            } else {
                cpu.y = cpu.x;
                cpu_set_nz16(&cpu, cpu.y);
            }
            break;

        case Mnemonic::TYX:
            if (eff_x) {
                cpu.x = static_cast<uint16_t>(cpu.y & 0x00FFU);
                cpu_set_nz8(&cpu, static_cast<uint8_t>(cpu.x));
            } else {
                cpu.x = cpu.y;
                cpu_set_nz16(&cpu, cpu.x);
            }
            break;

        case Mnemonic::TCD:
            cpu.d = cpu.a;
            cpu_set_nz16(&cpu, cpu.d);
            break;

        case Mnemonic::TDC:
            cpu.a = cpu.d;
            cpu_set_nz16(&cpu, cpu.a);
            break;

        case Mnemonic::TCS:
            if (cpu.e) {
                cpu.s = static_cast<uint16_t>(0x0100U | (cpu.a & 0x00FFU));
            } else {
                cpu.s = cpu.a;
            }
            break;

        case Mnemonic::TSC:
            cpu.a = cpu.s;
            cpu_set_nz16(&cpu, cpu.a);
            break;

        case Mnemonic::XBA: {
            const uint8_t lo = static_cast<uint8_t>(cpu.a & 0x00FFU);
            const uint8_t hi = static_cast<uint8_t>(cpu.a >> 8U);
            cpu.a = static_cast<uint16_t>((static_cast<uint16_t>(lo) << 8U) | hi);
            cpu_set_nz8(&cpu, hi);
            break;
        }

        case Mnemonic::XCE:
            cpu_xce(&cpu);
            break;

        case Mnemonic::SEP:
            cpu_sep(&cpu, op1);
            break;

        case Mnemonic::REP:
            cpu_rep(&cpu, op1);
            break;

        case Mnemonic::CLC:
            cpu.c = 0U;
            break;
        case Mnemonic::SEC:
            cpu.c = 1U;
            break;
        case Mnemonic::CLI:
            cpu.i = 0U;
            break;
        case Mnemonic::SEI:
            cpu.i = 1U;
            break;
        case Mnemonic::CLD:
            cpu.d_flag = 0U;
            break;
        case Mnemonic::SED:
            cpu.d_flag = 1U;
            break;
        case Mnemonic::CLV:
            cpu.v = 0U;
            break;

        case Mnemonic::PHA:
            if (eff_m) {
                cpu_push8(&cpu, static_cast<uint8_t>(cpu.a & 0x00FFU));
            } else {
                cycles += 1U;
                cpu_push16(&cpu, cpu.a);
            }
            break;

        case Mnemonic::PLA:
            if (eff_m) {
                const uint8_t r = cpu_pop8(&cpu);
                cpu.a = static_cast<uint16_t>((cpu.a & 0xFF00U) | r);
                cpu_set_nz8(&cpu, r);
            } else {
                cycles += 1U;
                cpu.a = cpu_pop16(&cpu);
                cpu_set_nz16(&cpu, cpu.a);
            }
            break;

        case Mnemonic::PHX:
            if (eff_x) {
                cpu_push8(&cpu, static_cast<uint8_t>(cpu.x & 0x00FFU));
            } else {
                cycles += 1U;
                cpu_push16(&cpu, cpu.x);
            }
            break;

        case Mnemonic::PLX:
            if (eff_x) {
                cpu.x = cpu_pop8(&cpu);
                cpu_set_nz8(&cpu, static_cast<uint8_t>(cpu.x));
            } else {
                cycles += 1U;
                cpu.x = cpu_pop16(&cpu);
                cpu_set_nz16(&cpu, cpu.x);
            }
            break;

        case Mnemonic::PHY:
            if (eff_x) {
                cpu_push8(&cpu, static_cast<uint8_t>(cpu.y & 0x00FFU));
            } else {
                cycles += 1U;
                cpu_push16(&cpu, cpu.y);
            }
            break;

        case Mnemonic::PLY:
            if (eff_x) {
                cpu.y = cpu_pop8(&cpu);
                cpu_set_nz8(&cpu, static_cast<uint8_t>(cpu.y));
            } else {
                cycles += 1U;
                cpu.y = cpu_pop16(&cpu);
                cpu_set_nz16(&cpu, cpu.y);
            }
            break;

        case Mnemonic::PHP:
            cpu_push8(&cpu, cpu_pack_flags(&cpu, 1U));
            break;

        case Mnemonic::PLP:
            cpu_unpack_flags(&cpu, cpu_pop8(&cpu));
            break;

        case Mnemonic::PHB:
            cpu_push8(&cpu, cpu.db);
            break;

        case Mnemonic::PLB:
            cpu.db = cpu_pop8(&cpu);
            cpu_set_nz8(&cpu, cpu.db);
            break;

        case Mnemonic::PHK:
            cpu_push8(&cpu, cpu.pb);
            break;

        case Mnemonic::PHD:
            cpu_push16(&cpu, cpu.d);
            break;

        case Mnemonic::PLD:
            cpu.d = cpu_pop16(&cpu);
            cpu_set_nz16(&cpu, cpu.d);
            break;

        case Mnemonic::PEA:
            cpu_push16(&cpu, operand16);
            break;

        case Mnemonic::PEI: {
            const uint16_t val = bus_read16_bank0(&cpu, static_cast<uint16_t>(cpu.d + op1));
            cpu_push16(&cpu, val);
            break;
        }

        case Mnemonic::PER:
            cpu_push16(&cpu, static_cast<uint16_t>(next_pc + operand16));
            break;

        case Mnemonic::BCC:
        case Mnemonic::BCS:
        case Mnemonic::BEQ:
        case Mnemonic::BMI:
        case Mnemonic::BNE:
        case Mnemonic::BPL:
        case Mnemonic::BVC:
        case Mnemonic::BVS: {
            bool cond = false;
            switch (info.mnemonic) {
                case Mnemonic::BCC:
                    cond = (cpu.c == 0U);
                    break;
                case Mnemonic::BCS:
                    cond = (cpu.c != 0U);
                    break;
                case Mnemonic::BEQ:
                    cond = (cpu.z == 0U);
                    break;
                case Mnemonic::BNE:
                    cond = (cpu.z != 0U);
                    break;
                case Mnemonic::BMI:
                    cond = (cpu.n != 0U);
                    break;
                case Mnemonic::BPL:
                    cond = (cpu.n == 0U);
                    break;
                case Mnemonic::BVC:
                    cond = (cpu.v == 0U);
                    break;
                case Mnemonic::BVS:
                    cond = (cpu.v != 0U);
                    break;
                default:
                    break;
            }
            if (cond) {
                const uint16_t target =
                    static_cast<uint16_t>(static_cast<int32_t>(next_pc) + static_cast<int8_t>(op1));
                cycles += 1U;
                if (cpu.e && ((next_pc & 0xFF00U) != (target & 0xFF00U))) {
                    cycles += 1U;
                }
                cpu.pc = target;
            }
            break;
        }

        case Mnemonic::BRA:
            cpu.pc =
                static_cast<uint16_t>(static_cast<int32_t>(next_pc) + static_cast<int8_t>(op1));
            break;

        case Mnemonic::BRL:
            cpu.pc = static_cast<uint16_t>(static_cast<int32_t>(next_pc) +
                                           static_cast<int16_t>(operand16));
            break;

        case Mnemonic::JMP:
            if (info.mode == AddrMode::Abs) {
                cpu.pc = operand16;
            } else if (info.mode == AddrMode::AbsInd) {
                cpu.pc = bus_read16_bank0(&cpu, operand16);
            } else if (info.mode == AddrMode::AbsXInd) {
                cpu.pc = bus_read16_bank_wrap(&cpu, pb, static_cast<uint16_t>(operand16 + cpu.x));
            }
            break;

        case Mnemonic::JML:
            if (info.mode == AddrMode::AbsLong) {
                cpu.pb = op3;
                cpu.pc = operand16;
            } else if (info.mode == AddrMode::AbsIndLong) {
                const uint32_t target24 = bus_read24_bank0(&cpu, operand16);
                cpu.pb = static_cast<uint8_t>((target24 >> 16U) & 0xFFU);
                cpu.pc = static_cast<uint16_t>(target24 & 0xFFFFU);
            }
            break;

        case Mnemonic::JSR:
            if (info.mode == AddrMode::Abs) {
                cpu_push16(&cpu, static_cast<uint16_t>(instr_pc + 2U));
                cpu.pc = operand16;
            } else if (info.mode == AddrMode::AbsXInd) {
                cpu_push16(&cpu, static_cast<uint16_t>(instr_pc + 2U));
                cpu.pc = bus_read16_bank_wrap(&cpu, pb, static_cast<uint16_t>(operand16 + cpu.x));
            }
            break;

        case Mnemonic::JSL:
            cpu_push8(&cpu, pb);
            cpu_push16(&cpu, static_cast<uint16_t>(instr_pc + 3U));
            cpu.pb = op3;
            cpu.pc = operand16;
            break;

        case Mnemonic::RTS:
            cpu.pc = static_cast<uint16_t>(cpu_pop16(&cpu) + 1U);
            break;

        case Mnemonic::RTL:
            cpu.pc = static_cast<uint16_t>(cpu_pop16(&cpu) + 1U);
            cpu.pb = cpu_pop8(&cpu);
            break;

        case Mnemonic::RTI:
            cpu_unpack_flags(&cpu, cpu_pop8(&cpu));
            cpu.pc = cpu_pop16(&cpu);
            if (!cpu.e) {
                cycles += 1U;
                cpu.pb = cpu_pop8(&cpu);
            }
            break;

        case Mnemonic::BRK:
            if (!cpu.e) {
                cycles += 1U;
                cpu_push8(&cpu, pb);
                cpu_push16(&cpu, static_cast<uint16_t>(instr_pc + 2U));
                cpu_push8(&cpu, cpu_pack_flags(&cpu, 0U));
                cpu.i = 1U;
                cpu.d_flag = 0U;
                cpu.pb = 0x00U;
                cpu.pc = bus_read16_bank0(&cpu, 0xFFE6U);
            } else {
                cpu_push16(&cpu, static_cast<uint16_t>(instr_pc + 2U));
                cpu_push8(&cpu, cpu_pack_flags(&cpu, 1U));
                cpu.i = 1U;
                cpu.d_flag = 0U;
                cpu.pb = 0x00U;
                cpu.pc = bus_read16_bank0(&cpu, 0xFFFEU);
            }
            break;

        case Mnemonic::COP:
            if (!cpu.e) {
                cycles += 1U;
                cpu_push8(&cpu, pb);
                cpu_push16(&cpu, static_cast<uint16_t>(instr_pc + 2U));
                cpu_push8(&cpu, cpu_pack_flags(&cpu, 0U));
                cpu.i = 1U;
                cpu.d_flag = 0U;
                cpu.pb = 0x00U;
                cpu.pc = bus_read16_bank0(&cpu, 0xFFE4U);
            } else {
                cpu_push16(&cpu, static_cast<uint16_t>(instr_pc + 2U));
                cpu_push8(&cpu, cpu_pack_flags(&cpu, 1U));
                cpu.i = 1U;
                cpu.d_flag = 0U;
                cpu.pb = 0x00U;
                cpu.pc = bus_read16_bank0(&cpu, 0xFFF4U);
            }
            break;

        case Mnemonic::MVN: {
            const uint32_t count = static_cast<uint32_t>(cpu.a) + 1U;
            cpu_mvn(&cpu, op1, op2);
            return 7U * count;
        }

        case Mnemonic::MVP: {
            const uint32_t count = static_cast<uint32_t>(cpu.a) + 1U;
            cpu_mvp(&cpu, op1, op2);
            return 7U * count;
        }

        case Mnemonic::WAI:
            cpu.waiting = 1U;
            break;

        case Mnemonic::STP:
            cpu.halted = 1U;
            break;

        case Mnemonic::NOP:
        case Mnemonic::WDM:
            break;
    }

    cpu.master_cycles += static_cast<uint64_t>(cycles) * kMasterClocksPerCpuCycle;
    return cycles;
}

void cpu_run_until(CpuState& cpu, uint64_t max_master_cycles) noexcept {
    while (!cpu.halted && !cpu.waiting && cpu.master_cycles < max_master_cycles) {
        cpu_step(cpu);
    }
}

}  // namespace dragon::snes
