#include "nesrecomp/emitter.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <vector>

#include "dragon/file_io.h"

namespace dragon::nesrecomp {

namespace {

bool reads_nz_flags(nes::Mnemonic m) {
    return m == nes::Mnemonic::BPL || m == nes::Mnemonic::BMI || m == nes::Mnemonic::BEQ ||
           m == nes::Mnemonic::BNE || m == nes::Mnemonic::PHP || m == nes::Mnemonic::BRK;
}

bool is_control_or_sync(nes::Mnemonic m) {
    return nes::is_branch_mnemonic(m) || m == nes::Mnemonic::JMP || m == nes::Mnemonic::JSR ||
           m == nes::Mnemonic::RTS || m == nes::Mnemonic::RTI || m == nes::Mnemonic::BRK ||
           m == nes::Mnemonic::KIL;
}

bool overwrites_nz_flags(nes::Mnemonic m) {
    switch (m) {
        case nes::Mnemonic::LDA:
        case nes::Mnemonic::LDX:
        case nes::Mnemonic::LDY:
        case nes::Mnemonic::TAX:
        case nes::Mnemonic::TAY:
        case nes::Mnemonic::TXA:
        case nes::Mnemonic::TYA:
        case nes::Mnemonic::TSX:
        case nes::Mnemonic::PLA:
        case nes::Mnemonic::PLP:
        case nes::Mnemonic::AND:
        case nes::Mnemonic::EOR:
        case nes::Mnemonic::ORA:
        case nes::Mnemonic::BIT:
        case nes::Mnemonic::ADC:
        case nes::Mnemonic::SBC:
        case nes::Mnemonic::CMP:
        case nes::Mnemonic::CPX:
        case nes::Mnemonic::CPY:
        case nes::Mnemonic::INC:
        case nes::Mnemonic::DEC:
        case nes::Mnemonic::INX:
        case nes::Mnemonic::DEX:
        case nes::Mnemonic::INY:
        case nes::Mnemonic::DEY:
        case nes::Mnemonic::ASL:
        case nes::Mnemonic::LSR:
        case nes::Mnemonic::ROL:
        case nes::Mnemonic::ROR:
        case nes::Mnemonic::LAX:
        case nes::Mnemonic::DCP:
        case nes::Mnemonic::ISC:
        case nes::Mnemonic::SLO:
        case nes::Mnemonic::RLA:
        case nes::Mnemonic::SRE:
        case nes::Mnemonic::RRA:
        case nes::Mnemonic::ANC:
        case nes::Mnemonic::ALR:
        case nes::Mnemonic::ARR:
        case nes::Mnemonic::AXS:
            return true;
        default:
            return false;
    }
}

bool is_nz_write_dead(const BasicBlock& block, size_t idx, bool enable_flag_opt) {
    if (!enable_flag_opt) {
        return false;
    }
    for (size_t j = idx + 1; j < block.instructions.size(); ++j) {
        const nes::Mnemonic m = block.instructions[j].mnemonic;
        if (reads_nz_flags(m) || is_control_or_sync(m)) {
            return false;
        }
        if (overwrites_nz_flags(m)) {
            return true;
        }
    }
    return false;
}

void emit_addr_calculation(std::ostream& out, const Instruction& instr) {
    char buf[256];
    switch (instr.mode) {
        case nes::AddrMode::Implied:
        case nes::AddrMode::Accumulator:
        case nes::AddrMode::Immediate:
        case nes::AddrMode::Relative:
            break;
        case nes::AddrMode::ZeroPage:
            std::snprintf(buf, sizeof(buf), "    addr = 0x%02Xu;\n", instr.op1);
            out << buf;
            break;
        case nes::AddrMode::ZeroPageX:
            std::snprintf(buf, sizeof(buf), "    addr = (uint8_t)(0x%02Xu + cpu->x);\n", instr.op1);
            out << buf;
            break;
        case nes::AddrMode::ZeroPageY:
            std::snprintf(buf, sizeof(buf), "    addr = (uint8_t)(0x%02Xu + cpu->y);\n", instr.op1);
            out << buf;
            break;
        case nes::AddrMode::Absolute:
            std::snprintf(buf, sizeof(buf), "    addr = 0x%04Xu;\n", instr.operand_u16());
            out << buf;
            break;
        case nes::AddrMode::AbsoluteX:
            std::snprintf(buf, sizeof(buf),
                          "    base = 0x%04Xu;\n"
                          "    addr = (uint16_t)(base + cpu->x);\n",
                          instr.operand_u16());
            out << buf;
            if (instr.page_cross_penalty) {
                out << "    if ((base & 0xFF00u) != (addr & 0xFF00u)) {\n"
                    << "        cpu->cycles += 1u;\n"
                    << "    }\n";
            }
            break;
        case nes::AddrMode::AbsoluteY:
            std::snprintf(buf, sizeof(buf),
                          "    base = 0x%04Xu;\n"
                          "    addr = (uint16_t)(base + cpu->y);\n",
                          instr.operand_u16());
            out << buf;
            if (instr.page_cross_penalty) {
                out << "    if ((base & 0xFF00u) != (addr & 0xFF00u)) {\n"
                    << "        cpu->cycles += 1u;\n"
                    << "    }\n";
            }
            break;
        case nes::AddrMode::Indirect:
            std::snprintf(buf, sizeof(buf), "    addr = bus_read16_wrap(cpu, 0x%04Xu);\n",
                          instr.operand_u16());
            out << buf;
            break;
        case nes::AddrMode::IndexedIndirectX:
            std::snprintf(buf, sizeof(buf),
                          "    addr = bus_read16_zp(cpu, (uint8_t)(0x%02Xu + cpu->x));\n",
                          instr.op1);
            out << buf;
            break;
        case nes::AddrMode::IndirectIndexedY:
            std::snprintf(buf, sizeof(buf),
                          "    base = bus_read16_zp(cpu, 0x%02Xu);\n"
                          "    addr = (uint16_t)(base + cpu->y);\n",
                          instr.op1);
            out << buf;
            if (instr.page_cross_penalty) {
                out << "    if ((base & 0xFF00u) != (addr & 0xFF00u)) {\n"
                    << "        cpu->cycles += 1u;\n"
                    << "    }\n";
            }
            break;
    }
}

std::string operand_read_expr(const Instruction& instr) {
    if (instr.mode == nes::AddrMode::Immediate) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%02Xu", instr.op1);
        return buf;
    }
    return "bus_read(cpu, addr)";
}

void emit_load(std::ostream& out, const char* reg, const Instruction& instr, bool dead_nz) {
    out << "    cpu->" << reg << " = " << operand_read_expr(instr) << ";\n";
    if (!dead_nz) {
        out << "    cpu_set_nz(cpu, cpu->" << reg << ");\n";
    }
}

void emit_store(std::ostream& out, const char* expr) {
    out << "    bus_write(cpu, addr, " << expr << ");\n";
}

void emit_transfer(std::ostream& out, const char* dst, const char* src, bool update_nz,
                   bool dead_nz) {
    out << "    cpu->" << dst << " = cpu->" << src << ";\n";
    if (update_nz && !dead_nz) {
        out << "    cpu_set_nz(cpu, cpu->" << dst << ");\n";
    }
}

void emit_adc(std::ostream& out, const Instruction& instr) {
    out << "    cpu_adc(cpu, " << operand_read_expr(instr) << ");\n";
}

void emit_sbc(std::ostream& out, const Instruction& instr) {
    out << "    cpu_sbc(cpu, " << operand_read_expr(instr) << ");\n";
}

void emit_cmp(std::ostream& out, const char* reg, const Instruction& instr) {
    out << "    cpu_cmp(cpu, cpu->" << reg << ", " << operand_read_expr(instr) << ");\n";
}

void emit_bit(std::ostream& out, const Instruction& instr) {
    out << "    cpu_bit(cpu, " << operand_read_expr(instr) << ");\n";
}

void emit_bitwise(std::ostream& out, const char* op_str, const Instruction& instr, bool dead_nz) {
    out << "    cpu->a " << op_str << " " << operand_read_expr(instr) << ";\n";
    if (!dead_nz) {
        out << "    cpu_set_nz(cpu, cpu->a);\n";
    }
}

void emit_rmw_write(std::ostream& out) {
    out << "    if (addr >= 0x2000u && addr < 0x4020u) {\n"
        << "        bus_write(cpu, addr, old_val);\n"
        << "    }\n"
        << "    bus_write(cpu, addr, val);\n";
}

void emit_inc_dec_mem(std::ostream& out, bool is_inc, bool dead_nz) {
    out << "    old_val = bus_read(cpu, addr);\n"
        << "    val = (uint8_t)(old_val " << (is_inc ? "+ 1u" : "- 1u") << ");\n";
    emit_rmw_write(out);
    if (!dead_nz) {
        out << "    cpu_set_nz(cpu, val);\n";
    }
}

void emit_shift(std::ostream& out, const char* helper_fn, const Instruction& instr) {
    if (instr.mode == nes::AddrMode::Accumulator) {
        out << "    cpu->a = " << helper_fn << "(cpu, cpu->a);\n";
    } else {
        out << "    old_val = bus_read(cpu, addr);\n"
            << "    val = " << helper_fn << "(cpu, old_val);\n";
        emit_rmw_write(out);
    }
}

void emit_target_jump(std::ostream& out, uint16_t target, uint16_t from_pc, const ProgramCFG& cfg,
                      const Function& current_func, const char* indent) {
    char buf[256];
    if (from_pc >= target) {
        // runtime_poll may service an NMI/IRQ whose handler uses a stack trick
        // (manipulates the 6502 stack and RTSes past this JSR instead of RTI).
        // A normal interrupt RTIs back to the same stack level; a stack trick
        // leaves S higher than just before the poll. In that case return so the
        // native call stack unwinds too and the caller re-dispatches at the new
        // PC. (Comparing to the pre-poll S, not entry_s, avoids a false trigger
        // after the reset routine raises S via TXS.)
        out << indent << "if (cpu->cycles >= cpu->next_event) {\n"
            << indent << "    const uint8_t poll_s = cpu->s;\n"
            << indent << "    runtime_poll(cpu);\n"
            << indent << "    if (cpu->halted || (int8_t)(cpu->s - poll_s) > 0) {\n"
            << indent << "        return;\n"
            << indent << "    }\n"
            << indent << "}\n";
    }
    const bool direct_fixed_call = (!cfg.is_banked || target >= cfg.fixed_window_start) &&
                                   (cfg.functions.find(target) != cfg.functions.end());
    if (current_func.block_addrs.count(target) > 0) {
        std::snprintf(buf, sizeof(buf), "%sgoto L_%04X;\n", indent, target);
        out << buf;
    } else if (direct_fixed_call) {
        std::snprintf(buf, sizeof(buf),
                      "%sfunc_%04X(cpu);\n"
                      "%sreturn;\n",
                      indent, target, indent);
        out << buf;
    } else {
        std::snprintf(buf, sizeof(buf),
                      "%sruntime_dispatch(cpu, 0x%04Xu, 0x%04Xu);\n"
                      "%sreturn;\n",
                      indent, target, from_pc, indent);
        out << buf;
    }
}

void emit_branch(std::ostream& out, const Instruction& instr, const char* cond_expr,
                 const ProgramCFG& cfg, const Function& current_func) {
    const uint16_t target = instr.operand_u16();
    const uint16_t next_pc = static_cast<uint16_t>(instr.addr + 2U);
    const uint32_t taken_extra = ((next_pc & 0xFF00U) != (target & 0xFF00U)) ? 2U : 1U;

    out << "    if (" << cond_expr << ") {\n"
        << "        cpu->cycles += " << taken_extra << "u;\n";
    emit_target_jump(out, target, instr.addr, cfg, current_func, "        ");
    out << "    }\n";
    emit_target_jump(out, next_pc, instr.addr, cfg, current_func, "    ");
}

void emit_jsr(std::ostream& out, const Instruction& instr, const ProgramCFG& cfg) {
    const uint16_t target = instr.operand_u16();
    const uint16_t push_addr = static_cast<uint16_t>(instr.addr + 2U);
    const uint16_t ret_pc = static_cast<uint16_t>(instr.addr + 3U);
    char buf[512];
    const bool direct_fixed_call = (!cfg.is_banked || target >= cfg.fixed_window_start) &&
                                   (cfg.functions.find(target) != cfg.functions.end());

    if (cfg.jump_engine_targets.count(target) > 0) {
        // JSR to JumpEngine consumes the inline table and returns to our caller via RTS.
        if (direct_fixed_call) {
            std::snprintf(buf, sizeof(buf),
                          "    cpu_push16(cpu, 0x%04Xu);\n"
                          "    func_%04X(cpu);\n"
                          "    return;\n",
                          push_addr, target);
        } else {
            std::snprintf(buf, sizeof(buf),
                          "    cpu_push16(cpu, 0x%04Xu);\n"
                          "    runtime_dispatch(cpu, 0x%04Xu, 0x%04Xu);\n"
                          "    return;\n",
                          push_addr, target, instr.addr);
        }
        out << buf;
        return;
    }

    if (direct_fixed_call) {
        std::snprintf(buf, sizeof(buf),
                      "    {\n"
                      "        const uint8_t saved_s = cpu->s;\n"
                      "        cpu_push16(cpu, 0x%04Xu);\n"
                      "        func_%04X(cpu);\n"
                      "        if (cpu->halted || cpu->s != saved_s || cpu->pc != 0x%04Xu) {\n"
                      "            return;\n"
                      "        }\n"
                      "    }\n",
                      push_addr, target, ret_pc);
    } else {
        std::snprintf(buf, sizeof(buf),
                      "    {\n"
                      "        const uint8_t saved_s = cpu->s;\n"
                      "        cpu_push16(cpu, 0x%04Xu);\n"
                      "        runtime_dispatch(cpu, 0x%04Xu, 0x%04Xu);\n"
                      "        if (cpu->halted || cpu->s != saved_s || cpu->pc != 0x%04Xu) {\n"
                      "            return;\n"
                      "        }\n"
                      "    }\n",
                      push_addr, target, instr.addr, ret_pc);
    }
    out << buf;
}

void emit_rts(std::ostream& out, const Instruction& instr) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "    {\n"
                  "        const uint16_t target = (uint16_t)(cpu_pop16(cpu) + 1u);\n"
                  "        if ((uint8_t)(entry_s + 2u) == cpu->s && target == entry_ret) {\n"
                  "            cpu->pc = target;\n"
                  "            return;\n"
                  "        }\n"
                  "        if ((int8_t)(cpu->s - entry_s) <= 0) {\n"
                  "            runtime_dispatch(cpu, target, 0x%04Xu);\n"
                  "            return;\n"
                  "        }\n"
                  "        cpu->pc = target;\n"
                  "        return;\n"
                  "    }\n",
                  instr.addr);
    out << buf;
}

void emit_rti(std::ostream& out) {
    out << "    cpu_unpack_flags(cpu, cpu_pop8(cpu));\n"
        << "    cpu->pc = cpu_pop16(cpu);\n"
        << "    return;\n";
}

void emit_brk(std::ostream& out, const Instruction& instr) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "    cpu_push16(cpu, 0x%04Xu);\n"
                  "    cpu_push8(cpu, cpu_pack_flags(cpu, 1u));\n"
                  "    cpu->i = 1u;\n"
                  "    addr = bus_read16(cpu, 0xFFFEu);\n"
                  "    runtime_dispatch(cpu, addr, 0x%04Xu);\n"
                  "    return;\n",
                  static_cast<uint16_t>(instr.addr + 2U), instr.addr);
    out << buf;
}

void emit_instruction(std::ostream& out, const Instruction& instr, bool dead_nz,
                      const ProgramCFG& cfg, const Function& current_func) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "    /* $%04X: %s */\n", instr.addr,
                  nes::get_opcode_info(instr.opcode).name);
    out << buf;

    std::snprintf(buf, sizeof(buf), "    cpu->cycles += %uu;\n", instr.base_cycles);
    out << buf;

    emit_addr_calculation(out, instr);

    switch (instr.mnemonic) {
        case nes::Mnemonic::LDA:
            emit_load(out, "a", instr, dead_nz);
            break;
        case nes::Mnemonic::LDX:
            emit_load(out, "x", instr, dead_nz);
            break;
        case nes::Mnemonic::LDY:
            emit_load(out, "y", instr, dead_nz);
            break;
        case nes::Mnemonic::STA:
            emit_store(out, "cpu->a");
            break;
        case nes::Mnemonic::STX:
            emit_store(out, "cpu->x");
            break;
        case nes::Mnemonic::STY:
            emit_store(out, "cpu->y");
            break;
        case nes::Mnemonic::TAX:
            emit_transfer(out, "x", "a", true, dead_nz);
            break;
        case nes::Mnemonic::TAY:
            emit_transfer(out, "y", "a", true, dead_nz);
            break;
        case nes::Mnemonic::TXA:
            emit_transfer(out, "a", "x", true, dead_nz);
            break;
        case nes::Mnemonic::TYA:
            emit_transfer(out, "a", "y", true, dead_nz);
            break;
        case nes::Mnemonic::TSX:
            emit_transfer(out, "x", "s", true, dead_nz);
            break;
        case nes::Mnemonic::TXS:
            emit_transfer(out, "s", "x", false, true);
            break;
        case nes::Mnemonic::PHA:
            out << "    cpu_push8(cpu, cpu->a);\n";
            break;
        case nes::Mnemonic::PHP:
            out << "    cpu_push8(cpu, cpu_pack_flags(cpu, 1u));\n";
            break;
        case nes::Mnemonic::PLA:
            out << "    cpu->a = cpu_pop8(cpu);\n";
            if (!dead_nz) {
                out << "    cpu_set_nz(cpu, cpu->a);\n";
            }
            break;
        case nes::Mnemonic::PLP:
            out << "    cpu_unpack_flags(cpu, cpu_pop8(cpu));\n";
            break;
        case nes::Mnemonic::AND:
            emit_bitwise(out, "&=", instr, dead_nz);
            break;
        case nes::Mnemonic::EOR:
            emit_bitwise(out, "^=", instr, dead_nz);
            break;
        case nes::Mnemonic::ORA:
            emit_bitwise(out, "|=", instr, dead_nz);
            break;
        case nes::Mnemonic::BIT:
            emit_bit(out, instr);
            break;
        case nes::Mnemonic::ADC:
            emit_adc(out, instr);
            break;
        case nes::Mnemonic::SBC:
            emit_sbc(out, instr);
            break;
        case nes::Mnemonic::CMP:
            emit_cmp(out, "a", instr);
            break;
        case nes::Mnemonic::CPX:
            emit_cmp(out, "x", instr);
            break;
        case nes::Mnemonic::CPY:
            emit_cmp(out, "y", instr);
            break;
        case nes::Mnemonic::INC:
            emit_inc_dec_mem(out, true, dead_nz);
            break;
        case nes::Mnemonic::DEC:
            emit_inc_dec_mem(out, false, dead_nz);
            break;
        case nes::Mnemonic::INX:
            out << "    cpu->x = (uint8_t)(cpu->x + 1u);\n";
            if (!dead_nz) {
                out << "    cpu_set_nz(cpu, cpu->x);\n";
            }
            break;
        case nes::Mnemonic::DEX:
            out << "    cpu->x = (uint8_t)(cpu->x - 1u);\n";
            if (!dead_nz) {
                out << "    cpu_set_nz(cpu, cpu->x);\n";
            }
            break;
        case nes::Mnemonic::INY:
            out << "    cpu->y = (uint8_t)(cpu->y + 1u);\n";
            if (!dead_nz) {
                out << "    cpu_set_nz(cpu, cpu->y);\n";
            }
            break;
        case nes::Mnemonic::DEY:
            out << "    cpu->y = (uint8_t)(cpu->y - 1u);\n";
            if (!dead_nz) {
                out << "    cpu_set_nz(cpu, cpu->y);\n";
            }
            break;
        case nes::Mnemonic::ASL:
            emit_shift(out, "cpu_asl", instr);
            break;
        case nes::Mnemonic::LSR:
            emit_shift(out, "cpu_lsr", instr);
            break;
        case nes::Mnemonic::ROL:
            emit_shift(out, "cpu_rol", instr);
            break;
        case nes::Mnemonic::ROR:
            emit_shift(out, "cpu_ror", instr);
            break;
        case nes::Mnemonic::JMP:
            if (instr.mode == nes::AddrMode::Absolute) {
                emit_target_jump(out, instr.operand_u16(), instr.addr, cfg, current_func, "    ");
            } else {
                std::snprintf(buf, sizeof(buf),
                              "    runtime_dispatch(cpu, addr, 0x%04Xu);\n"
                              "    return;\n",
                              instr.addr);
                out << buf;
            }
            break;
        case nes::Mnemonic::JSR:
            emit_jsr(out, instr, cfg);
            break;
        case nes::Mnemonic::RTS:
            emit_rts(out, instr);
            break;
        case nes::Mnemonic::RTI:
            emit_rti(out);
            break;
        case nes::Mnemonic::BRK:
            emit_brk(out, instr);
            break;
        case nes::Mnemonic::BCC:
            emit_branch(out, instr, "cpu->c == 0u", cfg, current_func);
            break;
        case nes::Mnemonic::BCS:
            emit_branch(out, instr, "cpu->c != 0u", cfg, current_func);
            break;
        case nes::Mnemonic::BEQ:
            emit_branch(out, instr, "cpu->z == 0u", cfg, current_func);
            break;
        case nes::Mnemonic::BNE:
            emit_branch(out, instr, "cpu->z != 0u", cfg, current_func);
            break;
        case nes::Mnemonic::BMI:
            emit_branch(out, instr, "(cpu->n & 0x80u) != 0u", cfg, current_func);
            break;
        case nes::Mnemonic::BPL:
            emit_branch(out, instr, "(cpu->n & 0x80u) == 0u", cfg, current_func);
            break;
        case nes::Mnemonic::BVC:
            emit_branch(out, instr, "cpu->v == 0u", cfg, current_func);
            break;
        case nes::Mnemonic::BVS:
            emit_branch(out, instr, "cpu->v != 0u", cfg, current_func);
            break;
        case nes::Mnemonic::CLC:
            out << "    cpu->c = 0u;\n";
            break;
        case nes::Mnemonic::CLD:
            out << "    cpu->d = 0u;\n";
            break;
        case nes::Mnemonic::CLI:
            out << "    cpu->i = 0u;\n";
            break;
        case nes::Mnemonic::CLV:
            out << "    cpu->v = 0u;\n";
            break;
        case nes::Mnemonic::SEC:
            out << "    cpu->c = 1u;\n";
            break;
        case nes::Mnemonic::SED:
            out << "    cpu->d = 1u;\n";
            break;
        case nes::Mnemonic::SEI:
            out << "    cpu->i = 1u;\n";
            break;
        case nes::Mnemonic::NOP:
            if (instr.mode != nes::AddrMode::Implied && instr.mode != nes::AddrMode::Immediate) {
                out << "    (void)bus_read(cpu, addr);\n";
            }
            break;
        case nes::Mnemonic::LAX:
            out << "    val = " << operand_read_expr(instr) << ";\n"
                << "    cpu->a = val;\n"
                << "    cpu->x = val;\n";
            if (!dead_nz) {
                out << "    cpu_set_nz(cpu, val);\n";
            }
            break;
        case nes::Mnemonic::SAX:
            emit_store(out, "(uint8_t)(cpu->a & cpu->x)");
            break;
        case nes::Mnemonic::DCP:
            out << "    old_val = bus_read(cpu, addr);\n"
                << "    val = (uint8_t)(old_val - 1u);\n";
            emit_rmw_write(out);
            out << "    cpu_cmp(cpu, cpu->a, val);\n";
            break;
        case nes::Mnemonic::ISC:
            out << "    old_val = bus_read(cpu, addr);\n"
                << "    val = (uint8_t)(old_val + 1u);\n";
            emit_rmw_write(out);
            out << "    cpu_sbc(cpu, val);\n";
            break;
        case nes::Mnemonic::SLO:
            out << "    old_val = bus_read(cpu, addr);\n"
                << "    val = cpu_asl(cpu, old_val);\n";
            emit_rmw_write(out);
            out << "    cpu->a |= val;\n"
                << "    cpu_set_nz(cpu, cpu->a);\n";
            break;
        case nes::Mnemonic::RLA:
            out << "    old_val = bus_read(cpu, addr);\n"
                << "    val = cpu_rol(cpu, old_val);\n";
            emit_rmw_write(out);
            out << "    cpu->a &= val;\n"
                << "    cpu_set_nz(cpu, cpu->a);\n";
            break;
        case nes::Mnemonic::SRE:
            out << "    old_val = bus_read(cpu, addr);\n"
                << "    val = cpu_lsr(cpu, old_val);\n";
            emit_rmw_write(out);
            out << "    cpu->a ^= val;\n"
                << "    cpu_set_nz(cpu, cpu->a);\n";
            break;
        case nes::Mnemonic::RRA:
            out << "    old_val = bus_read(cpu, addr);\n"
                << "    val = cpu_ror(cpu, old_val);\n";
            emit_rmw_write(out);
            out << "    cpu_adc(cpu, val);\n";
            break;
        case nes::Mnemonic::ANC:
            out << "    cpu->a &= " << operand_read_expr(instr) << ";\n"
                << "    cpu_set_nz(cpu, cpu->a);\n"
                << "    cpu->c = (uint8_t)((cpu->a >> 7u) & 1u);\n";
            break;
        case nes::Mnemonic::ALR:
            out << "    cpu->a &= " << operand_read_expr(instr) << ";\n"
                << "    cpu->a = cpu_lsr(cpu, cpu->a);\n";
            break;
        case nes::Mnemonic::ARR:
            out << "    cpu->a &= " << operand_read_expr(instr) << ";\n"
                << "    val = (uint8_t)(cpu->c ? 0x80u : 0u);\n"
                << "    cpu->a = (uint8_t)((cpu->a >> 1u) | val);\n"
                << "    cpu_set_nz(cpu, cpu->a);\n"
                << "    cpu->c = (uint8_t)((cpu->a >> 6u) & 1u);\n"
                << "    cpu->v = (uint8_t)(((cpu->a >> 6u) ^ (cpu->a >> 5u)) & 1u);\n";
            break;
        case nes::Mnemonic::AXS:
            out << "    val = (uint8_t)(cpu->a & cpu->x);\n"
                << "    cpu->c = (uint8_t)(val >= " << operand_read_expr(instr) << ");\n"
                << "    cpu->x = (uint8_t)(val - " << operand_read_expr(instr) << ");\n"
                << "    cpu_set_nz(cpu, cpu->x);\n";
            break;
        case nes::Mnemonic::KIL:
        default:
            out << "    cpu->halted = 1u;\n"
                << "    return;\n";
            break;
    }
}

void emit_function(std::ostream& out, const Function& func,
                   const std::map<uint16_t, BasicBlock>& blocks, const ProgramCFG& cfg,
                   const EmitOptions& options) {
    char buf[1024];
    if (!func.name.empty()) {
        std::snprintf(buf, sizeof(buf), "/* %s ($%04X) */\n", func.name.c_str(), func.entry_addr);
        out << buf;
    }
    const std::string sym = func.c_symbol();
    std::snprintf(
        buf, sizeof(buf),
        "void %s(CpuState* cpu) {\n"
        "    const uint8_t entry_s = cpu->s;\n"
        "    const uint16_t entry_ret = (uint16_t)(cpu_peek16_stack(cpu, entry_s) + 1u);\n"
        "    uint16_t addr = 0u;\n"
        "    uint16_t base = 0u;\n"
        "    uint8_t old_val = 0u;\n"
        "    uint8_t val = 0u;\n"
        "    (void)entry_s;\n"
        "    (void)entry_ret;\n"
        "    (void)addr;\n"
        "    (void)base;\n"
        "    (void)old_val;\n"
        "    (void)val;\n"
        "    if (cpu->cycles >= cpu->next_event) {\n"
        "        const uint8_t poll_s = cpu->s;\n"
        "        runtime_poll(cpu);\n"
        "        if (cpu->halted || (int8_t)(cpu->s - poll_s) > 0) {\n"
        "            return;\n"
        "        }\n"
        "    }\n"
        "    goto L_%04X;\n",
        sym.c_str(), func.entry_addr);
    out << buf;

    for (uint16_t b_addr : func.block_addrs) {
        const BasicBlock& block = blocks.at(b_addr);
        std::snprintf(buf, sizeof(buf), "\nL_%04X:\n", b_addr);
        out << buf;

        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const Instruction& instr = block.instructions[i];
            const bool dead_nz = is_nz_write_dead(block, i, options.enable_flag_opt);
            emit_instruction(out, instr, dead_nz, cfg, func);
        }

        // If the last instruction of the block does not explicitly branch/jump/return,
        // emit the fallthrough transition to block.end_addr.
        if (!block.instructions.empty()) {
            const Instruction& last = block.instructions.back();
            const bool is_terminator =
                nes::is_branch_mnemonic(last.mnemonic) || last.mnemonic == nes::Mnemonic::JMP ||
                last.mnemonic == nes::Mnemonic::RTS || last.mnemonic == nes::Mnemonic::RTI ||
                last.mnemonic == nes::Mnemonic::BRK || last.mnemonic == nes::Mnemonic::KIL ||
                (last.mnemonic == nes::Mnemonic::JSR &&
                 cfg.jump_engine_targets.count(last.operand_u16()) > 0);
            if (!is_terminator) {
                emit_target_jump(out, block.end_addr, last.addr, cfg, func, "    ");
            }
        }
    }

    out << "}\n\n";
}

}  // namespace

GeneratedFiles emit_c99(const ProgramCFG& cfg, const GameConfig& config,
                        const EmitOptions& options) {
    GeneratedFiles result;
    char buf[256];

    // 1. Header file (recomp_functions.h)
    {
        std::ostringstream hdr;
        hdr << "/* Auto-generated by nesrecomp for " << config.name << " — DO NOT EDIT */\n"
            << "#pragma once\n\n"
            << "#include \"nes/cpu_state.h\"\n\n"
            << "#ifdef __cplusplus\n"
            << "extern \"C\" {\n"
            << "#endif\n\n";

        auto emit_proto = [&](const Function& func) {
            const std::string sym = func.c_symbol();
            std::snprintf(buf, sizeof(buf), "void %s(CpuState* cpu);", sym.c_str());
            hdr << buf;
            if (!func.name.empty()) {
                hdr << " /* " << func.name << " */";
            }
            hdr << "\n";
        };

        for (const auto& [addr, func] : cfg.functions) {
            (void)addr;
            emit_proto(func);
        }
        for (const auto& bcfg : cfg.bank_cfgs) {
            for (const auto& [addr, func] : bcfg.functions) {
                (void)addr;
                emit_proto(func);
            }
        }

        hdr << "\nextern const RecompFuncEntry kRecompDispatchTable[];\n"
            << "extern const size_t kRecompDispatchTableCount;\n\n"
            << "#ifdef __cplusplus\n"
            << "}  /* extern \"C\" */\n"
            << "#endif\n";
        result.header_content = hdr.str();
    }

    // 2. Function implementation chunks (recomp_funcs_N.c)
    const size_t split = (options.split_every > 0) ? options.split_every : 200;
    std::ostringstream chunk;
    size_t count_in_chunk = 0;

    auto start_chunk = [&]() {
        chunk.str("");
        chunk.clear();
        chunk << "/* Auto-generated by nesrecomp — DO NOT EDIT */\n"
              << "#include \"recomp_functions.h\"\n\n";
        count_in_chunk = 0;
    };

    start_chunk();
    for (const auto& [addr, func] : cfg.functions) {
        (void)addr;
        emit_function(chunk, func, cfg.blocks, cfg, options);
        ++count_in_chunk;
        if (count_in_chunk >= split) {
            result.source_chunks.push_back(chunk.str());
            start_chunk();
        }
    }
    for (const auto& bcfg : cfg.bank_cfgs) {
        for (const auto& [addr, func] : bcfg.functions) {
            (void)addr;
            emit_function(chunk, func, bcfg.blocks, cfg, options);
            ++count_in_chunk;
            if (count_in_chunk >= split) {
                result.source_chunks.push_back(chunk.str());
                start_chunk();
            }
        }
    }
    if (count_in_chunk > 0 || result.source_chunks.empty()) {
        result.source_chunks.push_back(chunk.str());
    }

    // 3. Dispatch table source (recomp_dispatch.c), sorted by (addr, bank)
    {
        struct DispatchRow {
            uint16_t addr;
            uint8_t bank;
            std::string sym;
        };
        std::vector<DispatchRow> rows;
        for (const auto& [addr, func] : cfg.functions) {
            rows.push_back({addr, func.bank, func.c_symbol()});
        }
        for (const auto& bcfg : cfg.bank_cfgs) {
            for (const auto& [addr, func] : bcfg.functions) {
                rows.push_back({addr, func.bank, func.c_symbol()});
            }
        }
        std::sort(rows.begin(), rows.end(), [](const DispatchRow& a, const DispatchRow& b) {
            return (a.addr < b.addr) || (a.addr == b.addr && a.bank < b.bank);
        });

        std::ostringstream disp;
        disp << "/* Auto-generated dispatch table by nesrecomp — DO NOT EDIT */\n"
             << "#include \"recomp_functions.h\"\n\n"
             << "const RecompFuncEntry kRecompDispatchTable[] = {\n";
        for (const auto& r : rows) {
            std::snprintf(buf, sizeof(buf), "    {0x%04Xu, %uu, %s},\n", r.addr, r.bank,
                          r.sym.c_str());
            disp << buf;
        }
        disp << "};\n\n";
        std::snprintf(buf, sizeof(buf), "const size_t kRecompDispatchTableCount = %zuu;\n",
                      rows.size());
        disp << buf;
        result.dispatch_source = disp.str();
    }

    return result;
}

std::vector<std::filesystem::path> write_generated_output(const ProgramCFG& cfg,
                                                          const GameConfig& config,
                                                          const std::filesystem::path& output_dir,
                                                          const EmitOptions& options) {
    std::filesystem::create_directories(output_dir);
    GeneratedFiles gen = emit_c99(cfg, config, options);
    std::vector<std::filesystem::path> written;

    const std::filesystem::path hdr_path = output_dir / "recomp_functions.h";
    write_text_file(hdr_path, gen.header_content);
    written.push_back(hdr_path);

    for (size_t i = 0; i < gen.source_chunks.size(); ++i) {
        char fname[64];
        std::snprintf(fname, sizeof(fname), "recomp_funcs_%zu.c", i);
        const std::filesystem::path c_path = output_dir / fname;
        write_text_file(c_path, gen.source_chunks[i]);
        written.push_back(c_path);
    }

    const std::filesystem::path disp_path = output_dir / "recomp_dispatch.c";
    write_text_file(disp_path, gen.dispatch_source);
    written.push_back(disp_path);

    const std::filesystem::path listing_path = output_dir / "listing.txt";
    write_text_file(listing_path, format_listing(cfg));
    written.push_back(listing_path);

    return written;
}

}  // namespace dragon::nesrecomp
