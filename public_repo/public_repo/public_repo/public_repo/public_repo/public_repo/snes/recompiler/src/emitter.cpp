#include "snesrecomp/emitter.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <vector>

#include "dragon/file_io.h"

namespace dragon::snesrecomp {

namespace {

constexpr uint32_t kMasterClocksPerCpuCycle = 6U;

bool is_dp_mode(snes::AddrMode mode) noexcept {
    switch (mode) {
        case snes::AddrMode::DP:
        case snes::AddrMode::DPX:
        case snes::AddrMode::DPY:
        case snes::AddrMode::DPInd:
        case snes::AddrMode::DPIndLong:
        case snes::AddrMode::DPXInd:
        case snes::AddrMode::DPIndY:
        case snes::AddrMode::DPIndLongY:
            return true;
        default:
            return false;
    }
}

bool has_m16_extra_cycle(snes::Mnemonic m, snes::AddrMode mode) noexcept {
    switch (m) {
        case snes::Mnemonic::LDA:
        case snes::Mnemonic::STA:
        case snes::Mnemonic::STZ:
        case snes::Mnemonic::ADC:
        case snes::Mnemonic::SBC:
        case snes::Mnemonic::AND:
        case snes::Mnemonic::ORA:
        case snes::Mnemonic::EOR:
        case snes::Mnemonic::CMP:
        case snes::Mnemonic::BIT:
        case snes::Mnemonic::PHA:
        case snes::Mnemonic::PLA:
            return true;
        default:
            (void)mode;
            return false;
    }
}

bool has_m16_rmw_extra_cycles(snes::Mnemonic m, snes::AddrMode mode) noexcept {
    if (mode == snes::AddrMode::Implied) {
        return false;
    }
    switch (m) {
        case snes::Mnemonic::ASL:
        case snes::Mnemonic::LSR:
        case snes::Mnemonic::ROL:
        case snes::Mnemonic::ROR:
        case snes::Mnemonic::INC:
        case snes::Mnemonic::DEC:
        case snes::Mnemonic::TSB:
        case snes::Mnemonic::TRB:
            return true;
        default:
            return false;
    }
}

bool has_x16_extra_cycle(snes::Mnemonic m) noexcept {
    switch (m) {
        case snes::Mnemonic::LDX:
        case snes::Mnemonic::LDY:
        case snes::Mnemonic::STX:
        case snes::Mnemonic::STY:
        case snes::Mnemonic::CPX:
        case snes::Mnemonic::CPY:
        case snes::Mnemonic::PHX:
        case snes::Mnemonic::PLX:
        case snes::Mnemonic::PHY:
        case snes::Mnemonic::PLY:
            return true;
        default:
            return false;
    }
}

void emit_addr_calculation(std::ostream& out, const Instruction& instr) {
    char buf[256];
    const bool eff_x = instr.state.eff_x();
    const bool emu = (instr.state.e != 0U);

    switch (instr.mode) {
        case snes::AddrMode::Implied:
        case snes::AddrMode::ImmM:
        case snes::AddrMode::ImmX:
        case snes::AddrMode::Imm8:
        case snes::AddrMode::Imm16:
        case snes::AddrMode::PCRel8:
        case snes::AddrMode::PCRel16:
        case snes::AddrMode::BlockMove:
        case snes::AddrMode::AbsInd:
        case snes::AddrMode::AbsIndLong:
        case snes::AddrMode::AbsXInd:
            break;

        case snes::AddrMode::DP:
            std::snprintf(buf, sizeof(buf), "    addr = (uint16_t)(cpu->d + 0x%02Xu);\n",
                          instr.op1);
            out << buf;
            break;

        case snes::AddrMode::DPX:
            if (emu) {
                std::snprintf(buf, sizeof(buf),
                              "    if ((cpu->d & 0x00FFu) == 0u) {\n"
                              "        addr = (uint16_t)((cpu->d & 0xFF00u) | ((0x%02Xu + cpu->x) "
                              "& 0x00FFu));\n"
                              "    } else {\n"
                              "        addr = (uint16_t)(cpu->d + 0x%02Xu + cpu->x);\n"
                              "    }\n",
                              instr.op1, instr.op1);
            } else {
                std::snprintf(buf, sizeof(buf),
                              "    addr = (uint16_t)(cpu->d + 0x%02Xu + cpu->x);\n", instr.op1);
            }
            out << buf;
            break;

        case snes::AddrMode::DPY:
            if (emu) {
                std::snprintf(buf, sizeof(buf),
                              "    if ((cpu->d & 0x00FFu) == 0u) {\n"
                              "        addr = (uint16_t)((cpu->d & 0xFF00u) | ((0x%02Xu + cpu->y) "
                              "& 0x00FFu));\n"
                              "    } else {\n"
                              "        addr = (uint16_t)(cpu->d + 0x%02Xu + cpu->y);\n"
                              "    }\n",
                              instr.op1, instr.op1);
            } else {
                std::snprintf(buf, sizeof(buf),
                              "    addr = (uint16_t)(cpu->d + 0x%02Xu + cpu->y);\n", instr.op1);
            }
            out << buf;
            break;

        case snes::AddrMode::DPInd:
            if (instr.mnemonic != snes::Mnemonic::PEI) {
                std::snprintf(buf, sizeof(buf),
                              "    base16 = bus_read16_bank0(cpu, (uint16_t)(cpu->d + 0x%02Xu));\n"
                              "    addr = snes_addr24(cpu->db, base16);\n",
                              instr.op1);
                out << buf;
            }
            break;

        case snes::AddrMode::DPIndLong:
            std::snprintf(buf, sizeof(buf),
                          "    addr = bus_read24_bank0(cpu, (uint16_t)(cpu->d + 0x%02Xu));\n",
                          instr.op1);
            out << buf;
            break;

        case snes::AddrMode::DPXInd:
            std::snprintf(
                buf, sizeof(buf),
                "    base16 = bus_read16_bank0(cpu, (uint16_t)(cpu->d + 0x%02Xu + cpu->x));\n"
                "    addr = snes_addr24(cpu->db, base16);\n",
                instr.op1);
            out << buf;
            break;

        case snes::AddrMode::DPIndY:
            std::snprintf(buf, sizeof(buf),
                          "    base16 = bus_read16_bank0(cpu, (uint16_t)(cpu->d + 0x%02Xu));\n"
                          "    addr = (snes_addr24(cpu->db, base16) + cpu->y) & 0xFFFFFFu;\n",
                          instr.op1);
            out << buf;
            if (instr.page_cross_penalty) {
                if (!eff_x) {
                    out << "    cpu->master_cycles += 6u;\n";
                } else {
                    out << "    if ((base16 & 0xFF00u) != ((uint16_t)(base16 + cpu->y) & 0xFF00u)) "
                           "{\n"
                        << "        cpu->master_cycles += 6u;\n"
                        << "    }\n";
                }
            }
            break;

        case snes::AddrMode::DPIndLongY:
            std::snprintf(buf, sizeof(buf),
                          "    addr = (bus_read24_bank0(cpu, (uint16_t)(cpu->d + 0x%02Xu)) + "
                          "cpu->y) & 0xFFFFFFu;\n",
                          instr.op1);
            out << buf;
            break;

        case snes::AddrMode::StackRel:
            std::snprintf(buf, sizeof(buf), "    addr = (uint16_t)(cpu->s + 0x%02Xu);\n",
                          instr.op1);
            out << buf;
            break;

        case snes::AddrMode::StackRelIndY:
            std::snprintf(buf, sizeof(buf),
                          "    base16 = bus_read16_bank0(cpu, (uint16_t)(cpu->s + 0x%02Xu));\n"
                          "    addr = (snes_addr24(cpu->db, base16) + cpu->y) & 0xFFFFFFu;\n",
                          instr.op1);
            out << buf;
            break;

        case snes::AddrMode::Abs:
            std::snprintf(buf, sizeof(buf), "    addr = snes_addr24(cpu->db, 0x%04Xu);\n",
                          instr.operand_u16());
            out << buf;
            break;

        case snes::AddrMode::AbsX:
            std::snprintf(buf, sizeof(buf),
                          "    base16 = 0x%04Xu;\n"
                          "    addr = (snes_addr24(cpu->db, base16) + cpu->x) & 0xFFFFFFu;\n",
                          instr.operand_u16());
            out << buf;
            if (instr.page_cross_penalty) {
                if (!eff_x) {
                    out << "    cpu->master_cycles += 6u;\n";
                } else {
                    out << "    if ((base16 & 0xFF00u) != ((uint16_t)(base16 + cpu->x) & 0xFF00u)) "
                           "{\n"
                        << "        cpu->master_cycles += 6u;\n"
                        << "    }\n";
                }
            }
            break;

        case snes::AddrMode::AbsY:
            std::snprintf(buf, sizeof(buf),
                          "    base16 = 0x%04Xu;\n"
                          "    addr = (snes_addr24(cpu->db, base16) + cpu->y) & 0xFFFFFFu;\n",
                          instr.operand_u16());
            out << buf;
            if (instr.page_cross_penalty) {
                if (!eff_x) {
                    out << "    cpu->master_cycles += 6u;\n";
                } else {
                    out << "    if ((base16 & 0xFF00u) != ((uint16_t)(base16 + cpu->y) & 0xFF00u)) "
                           "{\n"
                        << "        cpu->master_cycles += 6u;\n"
                        << "    }\n";
                }
            }
            break;

        case snes::AddrMode::AbsLong:
            std::snprintf(buf, sizeof(buf), "    addr = 0x%06Xu;\n", instr.operand_u24());
            out << buf;
            break;

        case snes::AddrMode::AbsLongX:
            std::snprintf(buf, sizeof(buf), "    addr = (0x%06Xu + cpu->x) & 0xFFFFFFu;\n",
                          instr.operand_u24());
            out << buf;
            break;
    }
}

std::string read_m8_expr(const Instruction& instr) {
    if (instr.mode == snes::AddrMode::ImmM) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%02Xu", instr.op1);
        return buf;
    }
    return "bus_read(cpu, addr)";
}

std::string read_m16_expr(const Instruction& instr) {
    if (instr.mode == snes::AddrMode::ImmM) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%04Xu", instr.operand_u16());
        return buf;
    }
    if (instr.mode == snes::AddrMode::DP || instr.mode == snes::AddrMode::DPX ||
        instr.mode == snes::AddrMode::DPY || instr.mode == snes::AddrMode::StackRel) {
        return "bus_read16_bank0(cpu, (uint16_t)addr)";
    }
    return "bus_read16(cpu, addr)";
}

std::string read_x8_expr(const Instruction& instr) {
    if (instr.mode == snes::AddrMode::ImmX) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%02Xu", instr.op1);
        return buf;
    }
    return "bus_read(cpu, addr)";
}

std::string read_x16_expr(const Instruction& instr) {
    if (instr.mode == snes::AddrMode::ImmX) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%04Xu", instr.operand_u16());
        return buf;
    }
    if (instr.mode == snes::AddrMode::DP || instr.mode == snes::AddrMode::DPX ||
        instr.mode == snes::AddrMode::DPY) {
        return "bus_read16_bank0(cpu, (uint16_t)addr)";
    }
    return "bus_read16(cpu, addr)";
}

void emit_write_m8(std::ostream& out, const char* expr) {
    out << "    bus_write(cpu, addr, " << expr << ");\n";
}

void emit_write_m16(std::ostream& out, const Instruction& instr, const char* expr) {
    if (instr.mode == snes::AddrMode::DP || instr.mode == snes::AddrMode::DPX ||
        instr.mode == snes::AddrMode::DPY || instr.mode == snes::AddrMode::StackRel) {
        out << "    bus_write16_bank0(cpu, (uint16_t)addr, " << expr << ");\n";
    } else {
        out << "    bus_write16(cpu, addr, " << expr << ");\n";
    }
}

void emit_target_jump(std::ostream& out, const BlockKey& target_bkey, uint8_t bank,
                      uint16_t from_pc, const ProgramCFG& cfg, const Function& current_func,
                      const char* indent) {
    char buf[256];
    const uint32_t from_addr24 = (static_cast<uint32_t>(bank) << 16U) | from_pc;

    if (from_pc >= target_bkey.addr) {
        std::snprintf(buf, sizeof(buf),
                      "%scpu->pc = 0x%04Xu;\n"
                      "%sif (cpu->master_cycles >= cpu->next_event) {\n"
                      "%s    runtime_poll(cpu);\n"
                      "%s    if (cpu->halted) {\n"
                      "%s        return;\n"
                      "%s    }\n"
                      "%s}\n",
                      indent, target_bkey.addr, indent, indent, indent, indent, indent, indent);
        out << buf;
    }

    auto blk_it = current_func.blocks.find(target_bkey);
    if (blk_it != current_func.blocks.end()) {
        std::snprintf(buf, sizeof(buf), "%sgoto %s;\n", indent,
                      blk_it->second.label_name().c_str());
        out << buf;
        return;
    }

    const FunctionKey fkey{bank, target_bkey.addr, target_bkey.mode};
    auto fn_it = cfg.functions.find(fkey);
    if (fn_it != cfg.functions.end()) {
        std::snprintf(buf, sizeof(buf),
                      "%scpu->pc = 0x%04Xu;\n"
                      "%s%s(cpu);\n"
                      "%sreturn;\n",
                      indent, target_bkey.addr, indent, fn_it->second.c_symbol().c_str(), indent);
        out << buf;
        return;
    }

    std::snprintf(buf, sizeof(buf),
                  "%sruntime_dispatch(cpu, 0x%02Xu, 0x%04Xu, 0x%06Xu);\n"
                  "%sreturn;\n",
                  indent, bank, target_bkey.addr, from_addr24, indent);
    out << buf;
}

void emit_instruction(std::ostream& out, const Instruction& instr, const ProgramCFG& cfg,
                      const Function& current_func) {
    const bool eff_m = instr.state.eff_m();
    const bool eff_x = instr.state.eff_x();
    const uint8_t bank = instr.bank;
    const uint16_t next_pc = static_cast<uint16_t>(instr.addr + instr.length);
    const uint32_t instr_addr24 = (static_cast<uint32_t>(bank) << 16U) | instr.addr;
    char buf[1024];

    if (instr.mnemonic != snes::Mnemonic::MVN && instr.mnemonic != snes::Mnemonic::MVP) {
        uint32_t cyc = instr.base_cycles;
        if (!eff_m && has_m16_extra_cycle(instr.mnemonic, instr.mode)) {
            cyc += 1U;
        }
        if (!eff_m && has_m16_rmw_extra_cycles(instr.mnemonic, instr.mode)) {
            cyc += 2U;
        }
        if (!eff_x && has_x16_extra_cycle(instr.mnemonic)) {
            cyc += 1U;
        }
        if (!instr.state.e &&
            (instr.mnemonic == snes::Mnemonic::BRK || instr.mnemonic == snes::Mnemonic::COP ||
             instr.mnemonic == snes::Mnemonic::RTI)) {
            cyc += 1U;
        }
        out << "    cpu->master_cycles += " << (cyc * kMasterClocksPerCpuCycle) << "u;\n";
        if (is_dp_mode(instr.mode)) {
            out << "    if ((cpu->d & 0x00FFu) != 0u) {\n"
                << "        cpu->master_cycles += 6u;\n"
                << "    }\n";
        }
    }

    emit_addr_calculation(out, instr);

    switch (instr.mnemonic) {
        case snes::Mnemonic::LDA:
            if (eff_m) {
                out << "    val8 = " << read_m8_expr(instr) << ";\n"
                    << "    cpu->a = (uint16_t)((cpu->a & 0xFF00u) | val8);\n"
                    << "    cpu_set_nz8(cpu, val8);\n";
            } else {
                out << "    cpu->a = " << read_m16_expr(instr) << ";\n"
                    << "    cpu_set_nz16(cpu, cpu->a);\n";
            }
            break;

        case snes::Mnemonic::LDX:
            if (eff_x) {
                out << "    cpu->x = " << read_x8_expr(instr) << ";\n"
                    << "    cpu_set_nz8(cpu, (uint8_t)cpu->x);\n";
            } else {
                out << "    cpu->x = " << read_x16_expr(instr) << ";\n"
                    << "    cpu_set_nz16(cpu, cpu->x);\n";
            }
            break;

        case snes::Mnemonic::LDY:
            if (eff_x) {
                out << "    cpu->y = " << read_x8_expr(instr) << ";\n"
                    << "    cpu_set_nz8(cpu, (uint8_t)cpu->y);\n";
            } else {
                out << "    cpu->y = " << read_x16_expr(instr) << ";\n"
                    << "    cpu_set_nz16(cpu, cpu->y);\n";
            }
            break;

        case snes::Mnemonic::STA:
            if (eff_m) {
                emit_write_m8(out, "(uint8_t)(cpu->a & 0x00FFu)");
            } else {
                emit_write_m16(out, instr, "cpu->a");
            }
            break;

        case snes::Mnemonic::STX:
            if (eff_x) {
                emit_write_m8(out, "(uint8_t)(cpu->x & 0x00FFu)");
            } else {
                emit_write_m16(out, instr, "cpu->x");
            }
            break;

        case snes::Mnemonic::STY:
            if (eff_x) {
                emit_write_m8(out, "(uint8_t)(cpu->y & 0x00FFu)");
            } else {
                emit_write_m16(out, instr, "cpu->y");
            }
            break;

        case snes::Mnemonic::STZ:
            if (eff_m) {
                emit_write_m8(out, "0u");
            } else {
                emit_write_m16(out, instr, "0u");
            }
            break;

        case snes::Mnemonic::ADC:
            if (eff_m) {
                out << "    cpu_adc8(cpu, " << read_m8_expr(instr) << ");\n";
            } else {
                out << "    cpu_adc16(cpu, " << read_m16_expr(instr) << ");\n";
            }
            break;

        case snes::Mnemonic::SBC:
            if (eff_m) {
                out << "    cpu_sbc8(cpu, " << read_m8_expr(instr) << ");\n";
            } else {
                out << "    cpu_sbc16(cpu, " << read_m16_expr(instr) << ");\n";
            }
            break;

        case snes::Mnemonic::AND:
            if (eff_m) {
                out << "    val8 = (uint8_t)((cpu->a & 0x00FFu) & " << read_m8_expr(instr) << ");\n"
                    << "    cpu->a = (uint16_t)((cpu->a & 0xFF00u) | val8);\n"
                    << "    cpu_set_nz8(cpu, val8);\n";
            } else {
                out << "    cpu->a = (uint16_t)(cpu->a & " << read_m16_expr(instr) << ");\n"
                    << "    cpu_set_nz16(cpu, cpu->a);\n";
            }
            break;

        case snes::Mnemonic::ORA:
            if (eff_m) {
                out << "    val8 = (uint8_t)((cpu->a & 0x00FFu) | " << read_m8_expr(instr) << ");\n"
                    << "    cpu->a = (uint16_t)((cpu->a & 0xFF00u) | val8);\n"
                    << "    cpu_set_nz8(cpu, val8);\n";
            } else {
                out << "    cpu->a = (uint16_t)(cpu->a | " << read_m16_expr(instr) << ");\n"
                    << "    cpu_set_nz16(cpu, cpu->a);\n";
            }
            break;

        case snes::Mnemonic::EOR:
            if (eff_m) {
                out << "    val8 = (uint8_t)((cpu->a & 0x00FFu) ^ " << read_m8_expr(instr) << ");\n"
                    << "    cpu->a = (uint16_t)((cpu->a & 0xFF00u) | val8);\n"
                    << "    cpu_set_nz8(cpu, val8);\n";
            } else {
                out << "    cpu->a = (uint16_t)(cpu->a ^ " << read_m16_expr(instr) << ");\n"
                    << "    cpu_set_nz16(cpu, cpu->a);\n";
            }
            break;

        case snes::Mnemonic::CMP:
            if (eff_m) {
                out << "    cpu_cmp8(cpu, (uint8_t)(cpu->a & 0x00FFu), " << read_m8_expr(instr)
                    << ");\n";
            } else {
                out << "    cpu_cmp16(cpu, cpu->a, " << read_m16_expr(instr) << ");\n";
            }
            break;

        case snes::Mnemonic::CPX:
            if (eff_x) {
                out << "    cpu_cmp8(cpu, (uint8_t)(cpu->x & 0x00FFu), " << read_x8_expr(instr)
                    << ");\n";
            } else {
                out << "    cpu_cmp16(cpu, cpu->x, " << read_x16_expr(instr) << ");\n";
            }
            break;

        case snes::Mnemonic::CPY:
            if (eff_x) {
                out << "    cpu_cmp8(cpu, (uint8_t)(cpu->y & 0x00FFu), " << read_x8_expr(instr)
                    << ");\n";
            } else {
                out << "    cpu_cmp16(cpu, cpu->y, " << read_x16_expr(instr) << ");\n";
            }
            break;

        case snes::Mnemonic::BIT:
            if (instr.mode == snes::AddrMode::ImmM) {
                if (eff_m) {
                    std::snprintf(buf, sizeof(buf),
                                  "    cpu->z = (uint16_t)((cpu->a & 0x00FFu) & 0x%02Xu);\n",
                                  instr.op1);
                } else {
                    std::snprintf(buf, sizeof(buf), "    cpu->z = (uint16_t)(cpu->a & 0x%04Xu);\n",
                                  instr.operand_u16());
                }
                out << buf;
            } else {
                if (eff_m) {
                    out << "    cpu_bit8(cpu, " << read_m8_expr(instr) << ");\n";
                } else {
                    out << "    cpu_bit16(cpu, " << read_m16_expr(instr) << ");\n";
                }
            }
            break;

        case snes::Mnemonic::TSB:
            if (eff_m) {
                out << "    val8 = cpu_tsb8(cpu, " << read_m8_expr(instr) << ");\n";
                emit_write_m8(out, "val8");
            } else {
                out << "    val16 = cpu_tsb16(cpu, " << read_m16_expr(instr) << ");\n";
                emit_write_m16(out, instr, "val16");
            }
            break;

        case snes::Mnemonic::TRB:
            if (eff_m) {
                out << "    val8 = cpu_trb8(cpu, " << read_m8_expr(instr) << ");\n";
                emit_write_m8(out, "val8");
            } else {
                out << "    val16 = cpu_trb16(cpu, " << read_m16_expr(instr) << ");\n";
                emit_write_m16(out, instr, "val16");
            }
            break;

        case snes::Mnemonic::ASL:
            if (instr.mode == snes::AddrMode::Implied) {
                if (eff_m) {
                    out << "    val8 = cpu_asl8(cpu, (uint8_t)(cpu->a & 0x00FFu));\n"
                        << "    cpu->a = (uint16_t)((cpu->a & 0xFF00u) | val8);\n";
                } else {
                    out << "    cpu->a = cpu_asl16(cpu, cpu->a);\n";
                }
            } else {
                if (eff_m) {
                    out << "    val8 = cpu_asl8(cpu, " << read_m8_expr(instr) << ");\n";
                    emit_write_m8(out, "val8");
                } else {
                    out << "    val16 = cpu_asl16(cpu, " << read_m16_expr(instr) << ");\n";
                    emit_write_m16(out, instr, "val16");
                }
            }
            break;

        case snes::Mnemonic::LSR:
            if (instr.mode == snes::AddrMode::Implied) {
                if (eff_m) {
                    out << "    val8 = cpu_lsr8(cpu, (uint8_t)(cpu->a & 0x00FFu));\n"
                        << "    cpu->a = (uint16_t)((cpu->a & 0xFF00u) | val8);\n";
                } else {
                    out << "    cpu->a = cpu_lsr16(cpu, cpu->a);\n";
                }
            } else {
                if (eff_m) {
                    out << "    val8 = cpu_lsr8(cpu, " << read_m8_expr(instr) << ");\n";
                    emit_write_m8(out, "val8");
                } else {
                    out << "    val16 = cpu_lsr16(cpu, " << read_m16_expr(instr) << ");\n";
                    emit_write_m16(out, instr, "val16");
                }
            }
            break;

        case snes::Mnemonic::ROL:
            if (instr.mode == snes::AddrMode::Implied) {
                if (eff_m) {
                    out << "    val8 = cpu_rol8(cpu, (uint8_t)(cpu->a & 0x00FFu));\n"
                        << "    cpu->a = (uint16_t)((cpu->a & 0xFF00u) | val8);\n";
                } else {
                    out << "    cpu->a = cpu_rol16(cpu, cpu->a);\n";
                }
            } else {
                if (eff_m) {
                    out << "    val8 = cpu_rol8(cpu, " << read_m8_expr(instr) << ");\n";
                    emit_write_m8(out, "val8");
                } else {
                    out << "    val16 = cpu_rol16(cpu, " << read_m16_expr(instr) << ");\n";
                    emit_write_m16(out, instr, "val16");
                }
            }
            break;

        case snes::Mnemonic::ROR:
            if (instr.mode == snes::AddrMode::Implied) {
                if (eff_m) {
                    out << "    val8 = cpu_ror8(cpu, (uint8_t)(cpu->a & 0x00FFu));\n"
                        << "    cpu->a = (uint16_t)((cpu->a & 0xFF00u) | val8);\n";
                } else {
                    out << "    cpu->a = cpu_ror16(cpu, cpu->a);\n";
                }
            } else {
                if (eff_m) {
                    out << "    val8 = cpu_ror8(cpu, " << read_m8_expr(instr) << ");\n";
                    emit_write_m8(out, "val8");
                } else {
                    out << "    val16 = cpu_ror16(cpu, " << read_m16_expr(instr) << ");\n";
                    emit_write_m16(out, instr, "val16");
                }
            }
            break;

        case snes::Mnemonic::INC:
            if (instr.mode == snes::AddrMode::Implied) {
                if (eff_m) {
                    out << "    val8 = (uint8_t)((cpu->a & 0x00FFu) + 1u);\n"
                        << "    cpu->a = (uint16_t)((cpu->a & 0xFF00u) | val8);\n"
                        << "    cpu_set_nz8(cpu, val8);\n";
                } else {
                    out << "    cpu->a = (uint16_t)(cpu->a + 1u);\n"
                        << "    cpu_set_nz16(cpu, cpu->a);\n";
                }
            } else {
                if (eff_m) {
                    out << "    val8 = (uint8_t)(" << read_m8_expr(instr) << " + 1u);\n";
                    emit_write_m8(out, "val8");
                    out << "    cpu_set_nz8(cpu, val8);\n";
                } else {
                    out << "    val16 = (uint16_t)(" << read_m16_expr(instr) << " + 1u);\n";
                    emit_write_m16(out, instr, "val16");
                    out << "    cpu_set_nz16(cpu, val16);\n";
                }
            }
            break;

        case snes::Mnemonic::DEC:
            if (instr.mode == snes::AddrMode::Implied) {
                if (eff_m) {
                    out << "    val8 = (uint8_t)((cpu->a & 0x00FFu) - 1u);\n"
                        << "    cpu->a = (uint16_t)((cpu->a & 0xFF00u) | val8);\n"
                        << "    cpu_set_nz8(cpu, val8);\n";
                } else {
                    out << "    cpu->a = (uint16_t)(cpu->a - 1u);\n"
                        << "    cpu_set_nz16(cpu, cpu->a);\n";
                }
            } else {
                if (eff_m) {
                    out << "    val8 = (uint8_t)(" << read_m8_expr(instr) << " - 1u);\n";
                    emit_write_m8(out, "val8");
                    out << "    cpu_set_nz8(cpu, val8);\n";
                } else {
                    out << "    val16 = (uint16_t)(" << read_m16_expr(instr) << " - 1u);\n";
                    emit_write_m16(out, instr, "val16");
                    out << "    cpu_set_nz16(cpu, val16);\n";
                }
            }
            break;

        case snes::Mnemonic::INX:
            if (eff_x) {
                out << "    cpu->x = (uint16_t)((cpu->x + 1u) & 0x00FFu);\n"
                    << "    cpu_set_nz8(cpu, (uint8_t)cpu->x);\n";
            } else {
                out << "    cpu->x = (uint16_t)(cpu->x + 1u);\n"
                    << "    cpu_set_nz16(cpu, cpu->x);\n";
            }
            break;

        case snes::Mnemonic::DEX:
            if (eff_x) {
                out << "    cpu->x = (uint16_t)((cpu->x - 1u) & 0x00FFu);\n"
                    << "    cpu_set_nz8(cpu, (uint8_t)cpu->x);\n";
            } else {
                out << "    cpu->x = (uint16_t)(cpu->x - 1u);\n"
                    << "    cpu_set_nz16(cpu, cpu->x);\n";
            }
            break;

        case snes::Mnemonic::INY:
            if (eff_x) {
                out << "    cpu->y = (uint16_t)((cpu->y + 1u) & 0x00FFu);\n"
                    << "    cpu_set_nz8(cpu, (uint8_t)cpu->y);\n";
            } else {
                out << "    cpu->y = (uint16_t)(cpu->y + 1u);\n"
                    << "    cpu_set_nz16(cpu, cpu->y);\n";
            }
            break;

        case snes::Mnemonic::DEY:
            if (eff_x) {
                out << "    cpu->y = (uint16_t)((cpu->y - 1u) & 0x00FFu);\n"
                    << "    cpu_set_nz8(cpu, (uint8_t)cpu->y);\n";
            } else {
                out << "    cpu->y = (uint16_t)(cpu->y - 1u);\n"
                    << "    cpu_set_nz16(cpu, cpu->y);\n";
            }
            break;

        case snes::Mnemonic::TAX:
            if (eff_x) {
                out << "    cpu->x = (uint16_t)(cpu->a & 0x00FFu);\n"
                    << "    cpu_set_nz8(cpu, (uint8_t)cpu->x);\n";
            } else {
                out << "    cpu->x = cpu->a;\n"
                    << "    cpu_set_nz16(cpu, cpu->x);\n";
            }
            break;

        case snes::Mnemonic::TAY:
            if (eff_x) {
                out << "    cpu->y = (uint16_t)(cpu->a & 0x00FFu);\n"
                    << "    cpu_set_nz8(cpu, (uint8_t)cpu->y);\n";
            } else {
                out << "    cpu->y = cpu->a;\n"
                    << "    cpu_set_nz16(cpu, cpu->y);\n";
            }
            break;

        case snes::Mnemonic::TXA:
            if (eff_m) {
                out << "    val8 = (uint8_t)(cpu->x & 0x00FFu);\n"
                    << "    cpu->a = (uint16_t)((cpu->a & 0xFF00u) | val8);\n"
                    << "    cpu_set_nz8(cpu, val8);\n";
            } else {
                out << "    cpu->a = cpu->x;\n"
                    << "    cpu_set_nz16(cpu, cpu->a);\n";
            }
            break;

        case snes::Mnemonic::TYA:
            if (eff_m) {
                out << "    val8 = (uint8_t)(cpu->y & 0x00FFu);\n"
                    << "    cpu->a = (uint16_t)((cpu->a & 0xFF00u) | val8);\n"
                    << "    cpu_set_nz8(cpu, val8);\n";
            } else {
                out << "    cpu->a = cpu->y;\n"
                    << "    cpu_set_nz16(cpu, cpu->a);\n";
            }
            break;

        case snes::Mnemonic::TSX:
            if (eff_x) {
                out << "    cpu->x = (uint16_t)(cpu->s & 0x00FFu);\n"
                    << "    cpu_set_nz8(cpu, (uint8_t)cpu->x);\n";
            } else {
                out << "    cpu->x = cpu->s;\n"
                    << "    cpu_set_nz16(cpu, cpu->x);\n";
            }
            break;

        case snes::Mnemonic::TXS:
            if (instr.state.e != 0U) {
                out << "    cpu->s = (uint16_t)(0x0100u | (cpu->x & 0x00FFu));\n";
            } else {
                out << "    cpu->s = cpu->x;\n";
            }
            out << "    entry_s = cpu->s;\n";
            break;

        case snes::Mnemonic::TXY:
            if (eff_x) {
                out << "    cpu->y = (uint16_t)(cpu->x & 0x00FFu);\n"
                    << "    cpu_set_nz8(cpu, (uint8_t)cpu->y);\n";
            } else {
                out << "    cpu->y = cpu->x;\n"
                    << "    cpu_set_nz16(cpu, cpu->y);\n";
            }
            break;

        case snes::Mnemonic::TYX:
            if (eff_x) {
                out << "    cpu->x = (uint16_t)(cpu->y & 0x00FFu);\n"
                    << "    cpu_set_nz8(cpu, (uint8_t)cpu->x);\n";
            } else {
                out << "    cpu->x = cpu->y;\n"
                    << "    cpu_set_nz16(cpu, cpu->x);\n";
            }
            break;

        case snes::Mnemonic::TCD:
            out << "    cpu->d = cpu->a;\n"
                << "    cpu_set_nz16(cpu, cpu->d);\n";
            break;

        case snes::Mnemonic::TDC:
            out << "    cpu->a = cpu->d;\n"
                << "    cpu_set_nz16(cpu, cpu->a);\n";
            break;

        case snes::Mnemonic::TCS:
            if (instr.state.e != 0U) {
                out << "    cpu->s = (uint16_t)(0x0100u | (cpu->a & 0x00FFu));\n";
            } else {
                out << "    cpu->s = cpu->a;\n";
            }
            out << "    entry_s = cpu->s;\n";
            break;

        case snes::Mnemonic::TSC:
            out << "    cpu->a = cpu->s;\n"
                << "    cpu_set_nz16(cpu, cpu->a);\n";
            break;

        case snes::Mnemonic::XBA:
            out << "    val8 = (uint8_t)(cpu->a >> 8);\n"
                << "    cpu->a = (uint16_t)(((cpu->a & 0x00FFu) << 8) | val8);\n"
                << "    cpu_set_nz8(cpu, val8);\n";
            break;

        case snes::Mnemonic::XCE:
            out << "    cpu_xce(cpu);\n";
            break;

        case snes::Mnemonic::SEP:
            std::snprintf(buf, sizeof(buf), "    cpu_sep(cpu, 0x%02Xu);\n", instr.op1);
            out << buf;
            break;

        case snes::Mnemonic::REP:
            std::snprintf(buf, sizeof(buf), "    cpu_rep(cpu, 0x%02Xu);\n", instr.op1);
            out << buf;
            break;

        case snes::Mnemonic::CLC:
            out << "    cpu->c = 0u;\n";
            break;
        case snes::Mnemonic::SEC:
            out << "    cpu->c = 1u;\n";
            break;
        case snes::Mnemonic::CLI:
            out << "    cpu->i = 0u;\n";
            break;
        case snes::Mnemonic::SEI:
            out << "    cpu->i = 1u;\n";
            break;
        case snes::Mnemonic::CLD:
            out << "    cpu->d_flag = 0u;\n";
            break;
        case snes::Mnemonic::SED:
            out << "    cpu->d_flag = 1u;\n";
            break;
        case snes::Mnemonic::CLV:
            out << "    cpu->v = 0u;\n";
            break;

        case snes::Mnemonic::PHA:
            if (eff_m) {
                out << "    cpu_push8(cpu, (uint8_t)(cpu->a & 0x00FFu));\n";
            } else {
                out << "    cpu_push16(cpu, cpu->a);\n";
            }
            break;

        case snes::Mnemonic::PLA:
            if (eff_m) {
                out << "    val8 = cpu_pop8(cpu);\n"
                    << "    cpu->a = (uint16_t)((cpu->a & 0xFF00u) | val8);\n"
                    << "    cpu_set_nz8(cpu, val8);\n";
            } else {
                out << "    cpu->a = cpu_pop16(cpu);\n"
                    << "    cpu_set_nz16(cpu, cpu->a);\n";
            }
            break;

        case snes::Mnemonic::PHX:
            if (eff_x) {
                out << "    cpu_push8(cpu, (uint8_t)(cpu->x & 0x00FFu));\n";
            } else {
                out << "    cpu_push16(cpu, cpu->x);\n";
            }
            break;

        case snes::Mnemonic::PLX:
            if (eff_x) {
                out << "    cpu->x = cpu_pop8(cpu);\n"
                    << "    cpu_set_nz8(cpu, (uint8_t)cpu->x);\n";
            } else {
                out << "    cpu->x = cpu_pop16(cpu);\n"
                    << "    cpu_set_nz16(cpu, cpu->x);\n";
            }
            break;

        case snes::Mnemonic::PHY:
            if (eff_x) {
                out << "    cpu_push8(cpu, (uint8_t)(cpu->y & 0x00FFu));\n";
            } else {
                out << "    cpu_push16(cpu, cpu->y);\n";
            }
            break;

        case snes::Mnemonic::PLY:
            if (eff_x) {
                out << "    cpu->y = cpu_pop8(cpu);\n"
                    << "    cpu_set_nz8(cpu, (uint8_t)cpu->y);\n";
            } else {
                out << "    cpu->y = cpu_pop16(cpu);\n"
                    << "    cpu_set_nz16(cpu, cpu->y);\n";
            }
            break;

        case snes::Mnemonic::PHP:
            out << "    cpu_push8(cpu, cpu_pack_flags(cpu, 1u));\n";
            break;

        case snes::Mnemonic::PLP:
            out << "    cpu_unpack_flags(cpu, cpu_pop8(cpu));\n";
            break;

        case snes::Mnemonic::PHB:
            out << "    cpu_push8(cpu, cpu->db);\n";
            break;

        case snes::Mnemonic::PLB:
            out << "    cpu->db = cpu_pop8(cpu);\n"
                << "    cpu_set_nz8(cpu, cpu->db);\n";
            break;

        case snes::Mnemonic::PHK:
            std::snprintf(buf, sizeof(buf), "    cpu_push8(cpu, 0x%02Xu);\n", bank);
            out << buf;
            break;

        case snes::Mnemonic::PHD:
            out << "    cpu_push16(cpu, cpu->d);\n";
            break;

        case snes::Mnemonic::PLD:
            out << "    cpu->d = cpu_pop16(cpu);\n"
                << "    cpu_set_nz16(cpu, cpu->d);\n";
            break;

        case snes::Mnemonic::PEA:
            std::snprintf(buf, sizeof(buf), "    cpu_push16(cpu, 0x%04Xu);\n", instr.operand_u16());
            out << buf;
            break;

        case snes::Mnemonic::PEI:
            std::snprintf(buf, sizeof(buf),
                          "    val16 = bus_read16_bank0(cpu, (uint16_t)(cpu->d + 0x%02Xu));\n"
                          "    cpu_push16(cpu, val16);\n",
                          instr.op1);
            out << buf;
            break;

        case snes::Mnemonic::PER:
            std::snprintf(buf, sizeof(buf), "    cpu_push16(cpu, 0x%04Xu);\n", instr.operand_u16());
            out << buf;
            break;

        case snes::Mnemonic::BCC:
        case snes::Mnemonic::BCS:
        case snes::Mnemonic::BEQ:
        case snes::Mnemonic::BMI:
        case snes::Mnemonic::BNE:
        case snes::Mnemonic::BPL:
        case snes::Mnemonic::BVC:
        case snes::Mnemonic::BVS: {
            const char* cond = "0";
            switch (instr.mnemonic) {
                case snes::Mnemonic::BCC:
                    cond = "cpu->c == 0u";
                    break;
                case snes::Mnemonic::BCS:
                    cond = "cpu->c != 0u";
                    break;
                case snes::Mnemonic::BEQ:
                    cond = "cpu->z == 0u";
                    break;
                case snes::Mnemonic::BNE:
                    cond = "cpu->z != 0u";
                    break;
                case snes::Mnemonic::BMI:
                    cond = "cpu->n != 0u";
                    break;
                case snes::Mnemonic::BPL:
                    cond = "cpu->n == 0u";
                    break;
                case snes::Mnemonic::BVC:
                    cond = "cpu->v == 0u";
                    break;
                case snes::Mnemonic::BVS:
                    cond = "cpu->v != 0u";
                    break;
                default:
                    break;
            }
            const uint16_t target = instr.operand_u16();
            const uint32_t taken_extra =
                (instr.state.e != 0U && ((next_pc & 0xFF00U) != (target & 0xFF00U))) ? 12U : 6U;
            out << "    if (" << cond << ") {\n"
                << "        cpu->master_cycles += " << taken_extra << "u;\n";
            emit_target_jump(out, BlockKey{target, instr.next_state}, bank, instr.addr, cfg,
                             current_func, "        ");
            out << "    }\n";
            emit_target_jump(out, BlockKey{next_pc, instr.next_state}, bank, instr.addr, cfg,
                             current_func, "    ");
            break;
        }

        case snes::Mnemonic::BRA:
        case snes::Mnemonic::BRL:
            emit_target_jump(out, BlockKey{instr.operand_u16(), instr.next_state}, bank, instr.addr,
                             cfg, current_func, "    ");
            break;

        case snes::Mnemonic::JMP:
            if (instr.mode == snes::AddrMode::Abs) {
                emit_target_jump(out, BlockKey{instr.operand_u16(), instr.next_state}, bank,
                                 instr.addr, cfg, current_func, "    ");
            } else if (instr.mode == snes::AddrMode::AbsInd) {
                std::snprintf(buf, sizeof(buf),
                              "    val16 = bus_read16_bank0(cpu, 0x%04Xu);\n"
                              "    runtime_dispatch(cpu, 0x%02Xu, val16, 0x%06Xu);\n"
                              "    return;\n",
                              instr.operand_u16(), bank, instr_addr24);
                out << buf;
            } else if (instr.mode == snes::AddrMode::AbsXInd) {
                std::snprintf(buf, sizeof(buf),
                              "    val16 = bus_read16_bank_wrap(cpu, 0x%02Xu, (uint16_t)(0x%04Xu + "
                              "cpu->x));\n"
                              "    runtime_dispatch(cpu, 0x%02Xu, val16, 0x%06Xu);\n"
                              "    return;\n",
                              bank, instr.operand_u16(), bank, instr_addr24);
                out << buf;
            }
            break;

        case snes::Mnemonic::JML:
            if (instr.mode == snes::AddrMode::AbsLong) {
                const uint8_t target_bank = instr.op3;
                const uint16_t target_addr = instr.operand_u16();
                const FunctionKey fkey{target_bank, target_addr, instr.next_state};
                auto fn_it = cfg.functions.find(fkey);
                if (fn_it != cfg.functions.end()) {
                    std::snprintf(buf, sizeof(buf),
                                  "    cpu->pb = 0x%02Xu;\n"
                                  "    cpu->pc = 0x%04Xu;\n"
                                  "    %s(cpu);\n"
                                  "    return;\n",
                                  target_bank, target_addr, fn_it->second.c_symbol().c_str());
                } else {
                    std::snprintf(buf, sizeof(buf),
                                  "    runtime_dispatch(cpu, 0x%02Xu, 0x%04Xu, 0x%06Xu);\n"
                                  "    return;\n",
                                  target_bank, target_addr, instr_addr24);
                }
                out << buf;
            } else if (instr.mode == snes::AddrMode::AbsIndLong) {
                std::snprintf(
                    buf, sizeof(buf),
                    "    addr = bus_read24_bank0(cpu, 0x%04Xu);\n"
                    "    runtime_dispatch(cpu, (uint8_t)(addr >> 16), (uint16_t)addr, 0x%06Xu);\n"
                    "    return;\n",
                    instr.operand_u16(), instr_addr24);
                out << buf;
            }
            break;

        case snes::Mnemonic::JSR:
            if (instr.mode == snes::AddrMode::Abs) {
                const uint16_t target = instr.operand_u16();
                const uint16_t push_addr = static_cast<uint16_t>(instr.addr + 2U);
                const FunctionKey fkey{bank, target, instr.state};
                auto fn_it = cfg.functions.find(fkey);
                if (fn_it == cfg.functions.end()) {
                    fn_it = cfg.functions.find(FunctionKey{bank, target, instr.next_state});
                }

                if (cfg.is_jump_engine(bank, target)) {
                    std::snprintf(buf, sizeof(buf), "    cpu_push16(cpu, 0x%04Xu);\n", push_addr);
                    out << buf;
                    if (fn_it != cfg.functions.end()) {
                        std::snprintf(buf, sizeof(buf),
                                      "    cpu->pc = 0x%04Xu;\n"
                                      "    %s(cpu);\n"
                                      "    return;\n",
                                      target, fn_it->second.c_symbol().c_str());
                    } else {
                        std::snprintf(buf, sizeof(buf),
                                      "    runtime_dispatch(cpu, 0x%02Xu, 0x%04Xu, 0x%06Xu);\n"
                                      "    return;\n",
                                      bank, target, instr_addr24);
                    }
                    out << buf;
                    break;
                }

                std::snprintf(buf, sizeof(buf), "    cpu_push16(cpu, 0x%04Xu);\n", push_addr);
                out << buf;
                if (fn_it != cfg.functions.end()) {
                    std::snprintf(buf, sizeof(buf),
                                  "    cpu->pc = 0x%04Xu;\n"
                                  "    %s(cpu);\n",
                                  target, fn_it->second.c_symbol().c_str());
                } else {
                    std::snprintf(buf, sizeof(buf),
                                  "    runtime_dispatch(cpu, 0x%02Xu, 0x%04Xu, 0x%06Xu);\n", bank,
                                  target, instr_addr24);
                }
                out << buf;
                std::snprintf(buf, sizeof(buf),
                              "    cpu->pb = 0x%02Xu;\n"
                              "    if (cpu->halted || cpu->waiting) {\n"
                              "        return;\n"
                              "    }\n"
                              "    if (cpu->pc != 0x%04Xu) {\n"
                              "        if (cpu->s <= entry_s) {\n"
                              "            runtime_dispatch(cpu, 0x%02Xu, cpu->pc, 0x%06Xu);\n"
                              "        }\n"
                              "        return;\n"
                              "    }\n"
                              "    if (cpu_mode_flags(cpu) != 0x%02Xu) {\n"
                              "        runtime_dispatch(cpu, 0x%02Xu, 0x%04Xu, 0x%06Xu);\n"
                              "        return;\n"
                              "    }\n",
                              bank, next_pc, bank, instr_addr24, instr.next_state.mode_flags(),
                              bank, next_pc, instr_addr24);
                out << buf;
            } else if (instr.mode == snes::AddrMode::AbsXInd) {
                const uint16_t push_addr = static_cast<uint16_t>(instr.addr + 2U);
                std::snprintf(buf, sizeof(buf),
                              "    cpu_push16(cpu, 0x%04Xu);\n"
                              "    val16 = bus_read16_bank_wrap(cpu, 0x%02Xu, (uint16_t)(0x%04Xu + "
                              "cpu->x));\n"
                              "    runtime_dispatch(cpu, 0x%02Xu, val16, 0x%06Xu);\n"
                              "    cpu->pb = 0x%02Xu;\n"
                              "    if (cpu->halted || cpu->waiting) {\n"
                              "        return;\n"
                              "    }\n"
                              "    if (cpu->pc != 0x%04Xu) {\n"
                              "        if (cpu->s <= entry_s) {\n"
                              "            runtime_dispatch(cpu, 0x%02Xu, cpu->pc, 0x%06Xu);\n"
                              "        }\n"
                              "        return;\n"
                              "    }\n",
                              push_addr, bank, instr.operand_u16(), bank, instr_addr24, bank,
                              next_pc, bank, instr_addr24);
                out << buf;
            }
            break;

        case snes::Mnemonic::JSL: {
            const uint8_t target_bank = instr.op3;
            const uint16_t target_addr = instr.operand_u16();
            const uint16_t push_addr = static_cast<uint16_t>(instr.addr + 3U);
            const FunctionKey fkey{target_bank, target_addr, instr.state};
            auto fn_it = cfg.functions.find(fkey);
            if (fn_it == cfg.functions.end()) {
                fn_it = cfg.functions.find(FunctionKey{target_bank, target_addr, instr.next_state});
            }

            if (cfg.is_jump_engine(target_bank, target_addr)) {
                std::snprintf(buf, sizeof(buf),
                              "    cpu_push8(cpu, 0x%02Xu);\n"
                              "    cpu_push16(cpu, 0x%04Xu);\n",
                              bank, push_addr);
                out << buf;
                if (fn_it != cfg.functions.end()) {
                    std::snprintf(buf, sizeof(buf),
                                  "    cpu->pb = 0x%02Xu;\n"
                                  "    cpu->pc = 0x%04Xu;\n"
                                  "    %s(cpu);\n"
                                  "    return;\n",
                                  target_bank, target_addr, fn_it->second.c_symbol().c_str());
                } else {
                    std::snprintf(buf, sizeof(buf),
                                  "    runtime_dispatch(cpu, 0x%02Xu, 0x%04Xu, 0x%06Xu);\n"
                                  "    return;\n",
                                  target_bank, target_addr, instr_addr24);
                }
                out << buf;
                break;
            }

            std::snprintf(buf, sizeof(buf),
                          "    cpu_push8(cpu, 0x%02Xu);\n"
                          "    cpu_push16(cpu, 0x%04Xu);\n",
                          bank, push_addr);
            out << buf;
            if (fn_it != cfg.functions.end()) {
                std::snprintf(buf, sizeof(buf),
                              "    cpu->pb = 0x%02Xu;\n"
                              "    cpu->pc = 0x%04Xu;\n"
                              "    %s(cpu);\n",
                              target_bank, target_addr, fn_it->second.c_symbol().c_str());
            } else {
                std::snprintf(buf, sizeof(buf),
                              "    runtime_dispatch(cpu, 0x%02Xu, 0x%04Xu, 0x%06Xu);\n",
                              target_bank, target_addr, instr_addr24);
            }
            out << buf;
            std::snprintf(buf, sizeof(buf),
                          "    cpu->pb = 0x%02Xu;\n"
                          "    if (cpu->halted || cpu->waiting) {\n"
                          "        return;\n"
                          "    }\n"
                          "    if (cpu->pc != 0x%04Xu) {\n"
                          "        if (cpu->s <= entry_s) {\n"
                          "            runtime_dispatch(cpu, 0x%02Xu, cpu->pc, 0x%06Xu);\n"
                          "        }\n"
                          "        return;\n"
                          "    }\n"
                          "    if (cpu_mode_flags(cpu) != 0x%02Xu) {\n"
                          "        runtime_dispatch(cpu, 0x%02Xu, 0x%04Xu, 0x%06Xu);\n"
                          "        return;\n"
                          "    }\n",
                          bank, next_pc, bank, instr_addr24, instr.next_state.mode_flags(), bank,
                          next_pc, instr_addr24);
            out << buf;
            break;
        }

        case snes::Mnemonic::RTS:
            std::snprintf(buf, sizeof(buf),
                          "    val16 = (uint16_t)(cpu_pop16(cpu) + 1u);\n"
                          "    cpu->pc = val16;\n"
                          "    if (cpu->s <= entry_s) {\n"
                          "        runtime_dispatch(cpu, 0x%02Xu, val16, 0x%06Xu);\n"
                          "    }\n"
                          "    return;\n",
                          bank, instr_addr24);
            out << buf;
            break;

        case snes::Mnemonic::RTL:
            std::snprintf(buf, sizeof(buf),
                          "    val16 = (uint16_t)(cpu_pop16(cpu) + 1u);\n"
                          "    val8 = cpu_pop8(cpu);\n"
                          "    cpu->pc = val16;\n"
                          "    cpu->pb = val8;\n"
                          "    if (cpu->s <= entry_s) {\n"
                          "        runtime_dispatch(cpu, val8, val16, 0x%06Xu);\n"
                          "    }\n"
                          "    return;\n",
                          instr_addr24);
            out << buf;
            break;

        case snes::Mnemonic::RTI:
            out << "    cpu_unpack_flags(cpu, cpu_pop8(cpu));\n"
                << "    cpu->pc = cpu_pop16(cpu);\n"
                << "    if (!cpu->e) {\n"
                << "        cpu->pb = cpu_pop8(cpu);\n"
                << "    }\n"
                << "    return;\n";
            break;

        case snes::Mnemonic::BRK:
            std::snprintf(buf, sizeof(buf),
                          "    if (!cpu->e) {\n"
                          "        cpu_push8(cpu, 0x%02Xu);\n"
                          "        cpu_push16(cpu, 0x%04Xu);\n"
                          "        cpu_push8(cpu, cpu_pack_flags(cpu, 0u));\n"
                          "        cpu->i = 1u;\n"
                          "        cpu->d_flag = 0u;\n"
                          "        cpu->pb = 0x00u;\n"
                          "        cpu->pc = bus_read16_bank0(cpu, 0xFFE6u);\n"
                          "    } else {\n"
                          "        cpu_push16(cpu, 0x%04Xu);\n"
                          "        cpu_push8(cpu, cpu_pack_flags(cpu, 1u));\n"
                          "        cpu->i = 1u;\n"
                          "        cpu->d_flag = 0u;\n"
                          "        cpu->pb = 0x00u;\n"
                          "        cpu->pc = bus_read16_bank0(cpu, 0xFFFEu);\n"
                          "    }\n"
                          "    runtime_dispatch(cpu, 0x00u, cpu->pc, 0x%06Xu);\n"
                          "    return;\n",
                          bank, static_cast<uint16_t>(instr.addr + 2U),
                          static_cast<uint16_t>(instr.addr + 2U), instr_addr24);
            out << buf;
            break;

        case snes::Mnemonic::COP:
            std::snprintf(buf, sizeof(buf),
                          "    if (!cpu->e) {\n"
                          "        cpu_push8(cpu, 0x%02Xu);\n"
                          "        cpu_push16(cpu, 0x%04Xu);\n"
                          "        cpu_push8(cpu, cpu_pack_flags(cpu, 0u));\n"
                          "        cpu->i = 1u;\n"
                          "        cpu->d_flag = 0u;\n"
                          "        cpu->pb = 0x00u;\n"
                          "        cpu->pc = bus_read16_bank0(cpu, 0xFFE4u);\n"
                          "    } else {\n"
                          "        cpu_push16(cpu, 0x%04Xu);\n"
                          "        cpu_push8(cpu, cpu_pack_flags(cpu, 1u));\n"
                          "        cpu->i = 1u;\n"
                          "        cpu->d_flag = 0u;\n"
                          "        cpu->pb = 0x00u;\n"
                          "        cpu->pc = bus_read16_bank0(cpu, 0xFFF4u);\n"
                          "    }\n"
                          "    runtime_dispatch(cpu, 0x00u, cpu->pc, 0x%06Xu);\n"
                          "    return;\n",
                          bank, static_cast<uint16_t>(instr.addr + 2U),
                          static_cast<uint16_t>(instr.addr + 2U), instr_addr24);
            out << buf;
            break;

        case snes::Mnemonic::MVN:
            std::snprintf(buf, sizeof(buf), "    cpu_mvn(cpu, 0x%02Xu, 0x%02Xu);\n", instr.op1,
                          instr.op2);
            out << buf;
            break;

        case snes::Mnemonic::MVP:
            std::snprintf(buf, sizeof(buf), "    cpu_mvp(cpu, 0x%02Xu, 0x%02Xu);\n", instr.op1,
                          instr.op2);
            out << buf;
            break;

        case snes::Mnemonic::WAI:
            std::snprintf(buf, sizeof(buf),
                          "    cpu->pc = 0x%04Xu;\n"
                          "    runtime_wai(cpu);\n"
                          "    if (cpu->halted) {\n"
                          "        return;\n"
                          "    }\n",
                          next_pc);
            out << buf;
            break;

        case snes::Mnemonic::STP:
            std::snprintf(buf, sizeof(buf),
                          "    cpu->pc = 0x%04Xu;\n"
                          "    cpu->halted = 1u;\n"
                          "    return;\n",
                          next_pc);
            out << buf;
            break;

        case snes::Mnemonic::NOP:
        case snes::Mnemonic::WDM:
            break;
    }
}

bool ends_with_control_flow(const Instruction& instr, const ProgramCFG& cfg,
                            uint8_t bank) noexcept {
    if (snes::is_branch_mnemonic(instr.mnemonic)) {
        return true;
    }
    if (instr.mnemonic == snes::Mnemonic::JSR && instr.mode == snes::AddrMode::Abs &&
        cfg.is_jump_engine(bank, instr.operand_u16())) {
        return true;
    }
    switch (instr.mnemonic) {
        case snes::Mnemonic::JMP:
        case snes::Mnemonic::JML:
        case snes::Mnemonic::RTS:
        case snes::Mnemonic::RTL:
        case snes::Mnemonic::RTI:
        case snes::Mnemonic::BRK:
        case snes::Mnemonic::COP:
        case snes::Mnemonic::WAI:
        case snes::Mnemonic::STP:
            return true;
        default:
            return false;
    }
}

void emit_function(std::ostream& out, const Function& func, const ProgramCFG& cfg) {
    char buf[256];
    if (!func.name.empty()) {
        out << "/* " << func.name << " ($" << std::hex << static_cast<unsigned>(func.key.bank)
            << ":" << func.key.addr << std::dec << ") */\n";
    }
    out << "void " << func.c_symbol() << "(CpuState* cpu) {\n"
        << "    uint16_t entry_s = cpu->s;\n"
        << "    uint32_t addr = 0u;\n"
        << "    uint16_t base16 = 0u;\n"
        << "    uint16_t val16 = 0u;\n"
        << "    uint8_t val8 = 0u;\n"
        << "    (void)entry_s;\n"
        << "    (void)addr;\n"
        << "    (void)base16;\n"
        << "    (void)val16;\n"
        << "    (void)val8;\n";

    std::snprintf(buf, sizeof(buf),
                  "    cpu->pb = 0x%02Xu;\n"
                  "    cpu->pc = 0x%04Xu;\n",
                  func.key.bank, func.key.addr);
    out << buf;
    out << "    if (cpu->master_cycles >= cpu->next_event) {\n"
        << "        runtime_poll(cpu);\n"
        << "        if (cpu->halted) {\n"
        << "            return;\n"
        << "        }\n"
        << "    }\n";

    const BlockKey entry_bkey{func.key.addr, func.key.mode};
    auto entry_it = func.blocks.find(entry_bkey);
    if (entry_it != func.blocks.end()) {
        out << "    goto " << entry_it->second.label_name() << ";\n";
    } else if (!func.blocks.empty()) {
        out << "    goto " << func.blocks.begin()->second.label_name() << ";\n";
    }

    for (const auto& [bkey, block] : func.blocks) {
        out << "\n" << block.label_name() << ":\n" << "    (void)0;\n";
        for (const auto& instr : block.instructions) {
            emit_instruction(out, instr, cfg, func);
        }

        const Instruction& last = block.instructions.back();
        if (!ends_with_control_flow(last, cfg, func.key.bank)) {
            const uint16_t fallthrough_pc = static_cast<uint16_t>(last.addr + last.length);
            const BlockKey fallthrough_key{fallthrough_pc, last.next_state};
            emit_target_jump(out, fallthrough_key, func.key.bank, last.addr, cfg, func, "    ");
        }
    }

    out << "}\n\n";
}

}  // namespace

GeneratedFiles emit_c99(const ProgramCFG& cfg, const GameConfig& config,
                        const EmitOptions& options) {
    (void)options.enable_flag_opt;
    GeneratedFiles result;

    // 1. Header file (recomp_functions.h)
    {
        std::ostringstream hdr;
        hdr << "/* Auto-generated by snesrecomp for " << config.name << " — DO NOT EDIT */\n"
            << "#pragma once\n\n"
            << "#include <stddef.h>\n"
            << "#include \"snes/cpu_state.h\"\n\n"
            << "#ifdef __cplusplus\n"
            << "extern \"C\" {\n"
            << "#endif\n\n";

        for (const auto& [fkey, func] : cfg.functions) {
            (void)fkey;
            hdr << "void " << func.c_symbol() << "(CpuState* cpu);\n";
        }

        hdr << "\nextern const RecompFuncEntry kRecompDispatchTable[];\n"
            << "extern const size_t kRecompDispatchTableCount;\n"
            << "extern const uint32_t kRecompStackTrickTargets[];\n"
            << "extern const size_t kRecompStackTrickTargetCount;\n\n"
            << "#ifdef __cplusplus\n"
            << "}\n"
            << "#endif\n";

        result.header_content = hdr.str();
    }

    // 2. Function implementation chunks (recomp_funcs_N.c)
    const size_t split = (options.split_every > 0U) ? options.split_every : 200U;
    std::ostringstream chunk;
    size_t count_in_chunk = 0U;

    auto start_chunk = [&]() {
        chunk.str("");
        chunk.clear();
        chunk << "/* Auto-generated by snesrecomp — C99 function chunk */\n"
              << "#include \"recomp_functions.h\"\n\n";
        count_in_chunk = 0U;
    };

    start_chunk();
    for (const auto& [fkey, func] : cfg.functions) {
        (void)fkey;
        emit_function(chunk, func, cfg);
        ++count_in_chunk;
        if (count_in_chunk >= split) {
            result.source_chunks.push_back(chunk.str());
            start_chunk();
        }
    }
    if (count_in_chunk > 0U || result.source_chunks.empty()) {
        result.source_chunks.push_back(chunk.str());
    }

    // 3. Sorted dispatch table (recomp_dispatch.c)
    {
        std::ostringstream disp;
        disp << "/* Auto-generated by snesrecomp — sorted dispatch table */\n"
             << "#include \"recomp_functions.h\"\n\n"
             << "const RecompFuncEntry kRecompDispatchTable[] = {\n";

        for (const auto& [fkey, func] : cfg.functions) {
            char line[128];
            std::snprintf(line, sizeof(line), "    {0x%04Xu, 0x%02Xu, 0x%02Xu, &%s},\n", fkey.addr,
                          fkey.bank, fkey.mode.mode_flags(), func.c_symbol().c_str());
            disp << line;
        }
        if (cfg.functions.empty()) {
            disp << "    {0x0000u, 0x00u, 0x00u, NULL},\n";
        }
        disp << "};\n\n"
             << "const size_t kRecompDispatchTableCount = " << cfg.functions.size() << "u;\n\n";

        // Stack-trick / jump-engine dispatch targets (config-driven). Consumed by
        // the runtime to decide when an RTS inside runtime_dispatch must NOT end
        // the dispatch loop (replaces a former hardcoded address). A bare 16-bit
        // value matches any bank; a 24-bit value matches (bank, addr) exactly.
        disp << "const uint32_t kRecompStackTrickTargets[] = {\n";
        for (uint32_t t : config.stack_tricks) {
            char line[48];
            std::snprintf(line, sizeof(line), "    0x%Xu,\n", t);
            disp << line;
        }
        if (config.stack_tricks.empty()) {
            disp << "    0x0u,\n";  // placeholder; count is 0 so it is never read
        }
        disp << "};\n\n"
             << "const size_t kRecompStackTrickTargetCount = " << config.stack_tricks.size()
             << "u;\n";

        result.dispatch_source = disp.str();
    }

    return result;
}

std::vector<std::filesystem::path> write_generated_output(const ProgramCFG& cfg,
                                                          const GameConfig& config,
                                                          const std::filesystem::path& output_dir,
                                                          const EmitOptions& options) {
    std::filesystem::create_directories(output_dir);
    const GeneratedFiles gen = emit_c99(cfg, config, options);
    std::vector<std::filesystem::path> written;

    const auto hdr_path = output_dir / "recomp_functions.h";
    write_text_file(hdr_path, gen.header_content);
    written.push_back(hdr_path);

    for (size_t i = 0U; i < gen.source_chunks.size(); ++i) {
        const auto src_path = output_dir / ("recomp_funcs_" + std::to_string(i) + ".c");
        write_text_file(src_path, gen.source_chunks[i]);
        written.push_back(src_path);
    }

    const auto disp_path = output_dir / "recomp_dispatch.c";
    write_text_file(disp_path, gen.dispatch_source);
    written.push_back(disp_path);

    const auto list_path = output_dir / "disassembly.txt";
    write_text_file(list_path, format_listing(cfg));
    written.push_back(list_path);

    return written;
}

}  // namespace dragon::snesrecomp
