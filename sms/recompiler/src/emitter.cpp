#include "smsrecomp/emitter.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

namespace dragon::smsrecomp {

namespace {

std::string hex2(uint8_t v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02X", static_cast<unsigned>(v));
    return buf;
}

std::string hex4(uint16_t v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04X", static_cast<unsigned>(v));
    return buf;
}

std::string func_name(uint8_t bank, uint16_t addr) {
    return "func_" + hex2(bank) + "_" + hex4(addr);
}

std::string r8_read_expr(uint8_t idx) {
    switch (idx & 7U) {
        case 0: return "cpu->b";
        case 1: return "cpu->c";
        case 2: return "cpu->d";
        case 3: return "cpu->e";
        case 4: return "cpu->h";
        case 5: return "cpu->l";
        case 6: return "sms_bus_read(cpu, sms_hl(cpu))";
        case 7: return "cpu->a";
        default: return "0";
    }
}

void emit_r8_write(std::ostringstream& os, uint8_t idx, const std::string& expr) {
    switch (idx & 7U) {
        case 0: os << "    cpu->b = " << expr << ";\n"; break;
        case 1: os << "    cpu->c = " << expr << ";\n"; break;
        case 2: os << "    cpu->d = " << expr << ";\n"; break;
        case 3: os << "    cpu->e = " << expr << ";\n"; break;
        case 4: os << "    cpu->h = " << expr << ";\n"; break;
        case 5: os << "    cpu->l = " << expr << ";\n"; break;
        case 6: os << "    sms_bus_write(cpu, sms_hl(cpu), " << expr << ");\n"; break;
        case 7: os << "    cpu->a = " << expr << ";\n"; break;
        default: break;
    }
}

std::string rp_read_expr(uint8_t idx) {
    switch (idx & 3U) {
        case 0: return "sms_bc(cpu)";
        case 1: return "sms_de(cpu)";
        case 2: return "sms_hl(cpu)";
        case 3: return "cpu->sp";
        default: return "0";
    }
}

void emit_rp_write(std::ostringstream& os, uint8_t idx, const std::string& expr) {
    switch (idx & 3U) {
        case 0: os << "    sms_set_bc(cpu, " << expr << ");\n"; break;
        case 1: os << "    sms_set_de(cpu, " << expr << ");\n"; break;
        case 2: os << "    sms_set_hl(cpu, " << expr << ");\n"; break;
        case 3: os << "    cpu->sp = " << expr << ";\n"; break;
        default: break;
    }
}

std::string cc_expr(uint8_t cc) {
    switch (cc & 7U) {
        case 0: return "((cpu->f & SMS_FLAG_Z) == 0U)";
        case 1: return "((cpu->f & SMS_FLAG_Z) != 0U)";
        case 2: return "((cpu->f & SMS_FLAG_C) == 0U)";
        case 3: return "((cpu->f & SMS_FLAG_C) != 0U)";
        case 4: return "((cpu->f & SMS_FLAG_PV) == 0U)";
        case 5: return "((cpu->f & SMS_FLAG_PV) != 0U)";
        case 6: return "((cpu->f & SMS_FLAG_S) == 0U)";
        case 7: return "((cpu->f & SMS_FLAG_S) != 0U)";
        default: return "0";
    }
}

void emit_alu(std::ostringstream& os, uint8_t op_idx, const std::string& val_expr) {
    switch (op_idx & 7U) {
        case 0: os << "    sms_add8(cpu, " << val_expr << ", 0U);\n"; break;
        case 1: os << "    sms_add8(cpu, " << val_expr << ", (cpu->f & SMS_FLAG_C) ? 1U : 0U);\n"; break;
        case 2: os << "    sms_sub8(cpu, " << val_expr << ", 0U);\n"; break;
        case 3: os << "    sms_sub8(cpu, " << val_expr << ", (cpu->f & SMS_FLAG_C) ? 1U : 0U);\n"; break;
        case 4: os << "    sms_and8(cpu, " << val_expr << ");\n"; break;
        case 5: os << "    sms_xor8(cpu, " << val_expr << ");\n"; break;
        case 6: os << "    sms_or8(cpu, " << val_expr << ");\n"; break;
        case 7: os << "    sms_cp8(cpu, " << val_expr << ");\n"; break;
        default: break;
    }
}

void emit_poll_if_needed(std::ostringstream& os, uint16_t resume_pc) {
    os << "    if (cpu->cycles >= cpu->next_event) { cpu->pc = 0x" << hex4(resume_pc)
       << "U; if (sms_runtime_poll(cpu)) return; }\n";
}

void emit_tail_dispatch(std::ostringstream& os, const std::string& target_expr, uint16_t from_pc) {
    os << "    if (sms_runtime_dispatch(cpu, " << target_expr << ", 0x" << hex4(from_pc)
       << "U)) return;\n"
       << "    if ((int16_t)(cpu->sp - _fn_sp) <= 0) { goto L_tail_resume; }\n"
       << "    return;\n";
}

void emit_branch_target(std::ostringstream& os, const FunctionCfg& fn, uint16_t target,
                        uint16_t from_pc) {
    if (fn.instructions.count(target) > 0U && fn.local_labels.count(target) > 0U) {
        if (target <= from_pc) {
            emit_poll_if_needed(os, target);
        }
        os << "    goto L_" << hex4(target) << ";\n";
    } else {
        emit_tail_dispatch(os, "0x" + hex4(target) + "U", from_pc);
    }
}

void emit_step_fallback(std::ostringstream& os, uint16_t addr) {
    os << "    cpu->pc = 0x" << hex4(addr) << "U;\n";
    os << "    if (sms_runtime_step(cpu)) return;\n";
}

void emit_call_sequence(std::ostringstream& os, uint16_t addr, uint16_t next_pc,
                        uint16_t target, uint32_t cycles) {
    os << "    {\n"
       << "        uint16_t _sp = cpu->sp;\n"
       << "        sms_push(cpu, 0x" << hex4(next_pc) << "U);\n"
       << "        cpu->cycles += " << cycles << "U;\n"
       << "        if (sms_runtime_dispatch(cpu, 0x" << hex4(target) << "U, 0x" << hex4(addr)
       << "U)) return;\n"
       << "        if (cpu->pc != 0x" << hex4(next_pc) << "U) {\n"
       << "            if ((int16_t)(cpu->sp - _sp) <= 0) {\n"
       << "                sms_runtime_dispatch(cpu, cpu->pc, 0x" << hex4(addr) << "U);\n"
       << "            }\n"
       << "            return;\n"
       << "        }\n"
       << "    }\n";
}

void emit_instruction(std::ostringstream& os, const FunctionCfg& fn, const DecodedInstr& ins) {
    const uint16_t next_pc = static_cast<uint16_t>(ins.addr + ins.length);

    // Prefixed CB opcodes: direct native C99 emission
    if (ins.prefix == Prefix::CB) {
        const uint8_t r = ins.opcode & 7U;
        const uint8_t y = (ins.opcode >> 3U) & 7U;
        const uint8_t x = (ins.opcode >> 6U) & 3U;
        const std::string src = r8_read_expr(r);
        if (x == 0U) {
            static const char* kRotFns[8] = {"sms_rlc", "sms_rrc", "sms_rl",  "sms_rr",
                                             "sms_sla", "sms_sra", "sms_sll", "sms_srl"};
            emit_r8_write(os, r, std::string(kRotFns[y]) + "(cpu, " + src + ")");
        } else if (x == 1U) {
            os << "    sms_bit(cpu, " << static_cast<unsigned>(y) << "U, " << src << ");\n";
        } else if (x == 2U) {
            const uint8_t mask = static_cast<uint8_t>(~(1U << y));
            emit_r8_write(os, r, "(uint8_t)(" + src + " & 0x" + hex2(mask) + "U)");
        } else {
            const uint8_t mask = static_cast<uint8_t>(1U << y);
            emit_r8_write(os, r, "(uint8_t)(" + src + " | 0x" + hex2(mask) + "U)");
        }
        os << "    cpu->cycles += " << static_cast<unsigned>(ins.cycles) << "U;\n";
        return;
    }

    // Prefixed ED opcodes: direct C99 for common hot ops, exact step fallback for rare ones
    if (ins.prefix == Prefix::ED) {
        const uint8_t op = ins.opcode;
        if (op == 0x45U || op == 0x4DU || op == 0x55U || op == 0x5DU || op == 0x65U ||
            op == 0x6DU || op == 0x75U || op == 0x7DU) {
            // RETN / RETI
            os << "    cpu->pc = sms_pop(cpu);\n";
            os << "    cpu->iff1 = cpu->iff2;\n";
            os << "    cpu->cycles += 14U;\n";
            os << "    return;\n";
            return;
        }
        if (op == 0x44U || op == 0x4CU || op == 0x54U || op == 0x5CU || op == 0x64U ||
            op == 0x6CU || op == 0x74U || op == 0x7CU) {
            os << "    { uint8_t _oa = cpu->a; cpu->a = 0U; sms_sub8(cpu, _oa, 0U); }\n";
            os << "    cpu->cycles += 8U;\n";
            return;
        }
        if (op == 0x4AU || op == 0x5AU || op == 0x6AU || op == 0x7AU) {
            os << "    sms_adc_hl(cpu, " << rp_read_expr((op >> 4U) & 3U) << ");\n";
            os << "    cpu->cycles += 15U;\n";
            return;
        }
        if (op == 0x42U || op == 0x52U || op == 0x62U || op == 0x72U) {
            os << "    sms_sbc_hl(cpu, " << rp_read_expr((op >> 4U) & 3U) << ");\n";
            os << "    cpu->cycles += 15U;\n";
            return;
        }
        if (op == 0x43U || op == 0x53U || op == 0x63U || op == 0x73U) {
            os << "    sms_bus_write16(cpu, 0x" << hex4(ins.imm) << "U, "
               << rp_read_expr((op >> 4U) & 3U) << ");\n";
            os << "    cpu->cycles += 20U;\n";
            return;
        }
        if (op == 0x4BU || op == 0x5BU || op == 0x6BU || op == 0x7BU) {
            emit_rp_write(os, (op >> 4U) & 3U, "sms_bus_read16(cpu, 0x" + hex4(ins.imm) + "U)");
            os << "    cpu->cycles += 20U;\n";
            return;
        }
        if (op == 0xB0U || op == 0xB8U) {
            // LDIR / LDDR native C99 loop (massive win on SMS VRAM/WRAM copies)
            const int d = (op == 0xB8U) ? -1 : 1;
            os << "    do {\n"
               << "        uint16_t _hl = sms_hl(cpu), _de = sms_de(cpu);\n"
               << "        uint8_t _v = sms_bus_read(cpu, _hl);\n"
               << "        sms_bus_write(cpu, _de, _v);\n"
               << "        sms_set_hl(cpu, (uint16_t)(_hl + (" << d << ")));\n"
               << "        sms_set_de(cpu, (uint16_t)(_de + (" << d << ")));\n"
               << "        uint16_t _bc = (uint16_t)(sms_bc(cpu) - 1U);\n"
               << "        sms_set_bc(cpu, _bc);\n"
               << "        cpu->f = (uint8_t)((cpu->f & (SMS_FLAG_S | SMS_FLAG_Z | SMS_FLAG_C)) | (_bc ? SMS_FLAG_PV : 0U));\n"
               << "        cpu->cycles += (_bc != 0U) ? 21U : 16U;\n"
               << "        if (cpu->cycles >= cpu->next_event) { cpu->pc = 0x"
               << hex4(ins.addr) << "U; if (sms_runtime_poll(cpu)) return; }\n"
               << "    } while (sms_bc(cpu) != 0U);\n";
            return;
        }
        // Remaining ED ops (IM, LD A,I/R, INI/OUTI/OTIR, RRD/RLD) -> exact runtime step
        emit_step_fallback(os, ins.addr);
        return;
    }

    // DD / FD / DDCB / FDCB indexed opcodes
    if (ins.prefix == Prefix::DD || ins.prefix == Prefix::FD) {
        const std::string reg = (ins.prefix == Prefix::DD) ? "cpu->ix" : "cpu->iy";
        const uint8_t op = ins.opcode;
        if (op == 0x21U) {
            os << "    " << reg << " = 0x" << hex4(ins.imm) << "U; cpu->cycles += 14U;\n";
            return;
        }
        if (op == 0x23U) {
            os << "    " << reg << " = (uint16_t)(" << reg << " + 1U); cpu->cycles += 10U;\n";
            return;
        }
        if (op == 0x2BU) {
            os << "    " << reg << " = (uint16_t)(" << reg << " - 1U); cpu->cycles += 10U;\n";
            return;
        }
        if (op == 0xE1U) {
            os << "    " << reg << " = sms_pop(cpu); cpu->cycles += 14U;\n";
            return;
        }
        if (op == 0xE5U) {
            os << "    sms_push(cpu, " << reg << "); cpu->cycles += 15U;\n";
            return;
        }
        if (op == 0xE9U) {
            os << "    cpu->cycles += 8U;\n";
            emit_tail_dispatch(os, reg, ins.addr);
            return;
        }
        emit_step_fallback(os, ins.addr);
        return;
    }

    if (ins.prefix == Prefix::DDCB || ins.prefix == Prefix::FDCB) {
        emit_step_fallback(os, ins.addr);
        return;
    }

    // Unprefixed base Z80 opcodes
    const uint8_t op = ins.opcode;

    // 0x40..0x7F: LD r, r' + HALT
    if (op >= 0x40U && op <= 0x7FU) {
        if (op == 0x76U) {
            os << "    cpu->pc = 0x" << hex4(next_pc)
               << "U; cpu->cycles += 4U; if (sms_runtime_halt(cpu)) return;\n";
            return;
        }
        emit_r8_write(os, (op >> 3U) & 7U, r8_read_expr(op & 7U));
        os << "    cpu->cycles += " << static_cast<unsigned>(ins.cycles) << "U;\n";
        return;
    }

    // 0x80..0xBF: ALU r
    if (op >= 0x80U && op <= 0xBFU) {
        emit_alu(os, (op >> 3U) & 7U, r8_read_expr(op & 7U));
        os << "    cpu->cycles += " << static_cast<unsigned>(ins.cycles) << "U;\n";
        return;
    }

    switch (op) {
        case 0x00U: os << "    cpu->cycles += 4U;\n"; break;
        case 0x01U: case 0x11U: case 0x21U: case 0x31U:
            emit_rp_write(os, (op >> 4U) & 3U, "0x" + hex4(ins.imm) + "U");
            os << "    cpu->cycles += 10U;\n";
            break;
        case 0x02U: os << "    sms_bus_write(cpu, sms_bc(cpu), cpu->a); cpu->cycles += 7U;\n"; break;
        case 0x12U: os << "    sms_bus_write(cpu, sms_de(cpu), cpu->a); cpu->cycles += 7U;\n"; break;
        case 0x22U: os << "    sms_bus_write16(cpu, 0x" << hex4(ins.imm) << "U, sms_hl(cpu)); cpu->cycles += 16U;\n"; break;
        case 0x32U: os << "    sms_bus_write(cpu, 0x" << hex4(ins.imm) << "U, cpu->a); cpu->cycles += 13U;\n"; break;
        case 0x0AU: os << "    cpu->a = sms_bus_read(cpu, sms_bc(cpu)); cpu->cycles += 7U;\n"; break;
        case 0x1AU: os << "    cpu->a = sms_bus_read(cpu, sms_de(cpu)); cpu->cycles += 7U;\n"; break;
        case 0x2AU: os << "    sms_set_hl(cpu, sms_bus_read16(cpu, 0x" << hex4(ins.imm) << "U)); cpu->cycles += 16U;\n"; break;
        case 0x3AU: os << "    cpu->a = sms_bus_read(cpu, 0x" << hex4(ins.imm) << "U); cpu->cycles += 13U;\n"; break;

        case 0x03U: case 0x13U: case 0x23U: case 0x33U:
            emit_rp_write(os, (op >> 4U) & 3U, "(uint16_t)(" + rp_read_expr((op >> 4U) & 3U) + " + 1U)");
            os << "    cpu->cycles += 6U;\n";
            break;
        case 0x0BU: case 0x1BU: case 0x2BU: case 0x3BU:
            emit_rp_write(os, (op >> 4U) & 3U, "(uint16_t)(" + rp_read_expr((op >> 4U) & 3U) + " - 1U)");
            os << "    cpu->cycles += 6U;\n";
            break;
        case 0x04U: case 0x0CU: case 0x14U: case 0x1CU:
        case 0x24U: case 0x2CU: case 0x34U: case 0x3CU: {
            const uint8_t r = (op >> 3U) & 7U;
            emit_r8_write(os, r, "sms_inc8(cpu, " + r8_read_expr(r) + ")");
            os << "    cpu->cycles += " << static_cast<unsigned>(ins.cycles) << "U;\n";
            break;
        }
        case 0x05U: case 0x0DU: case 0x15U: case 0x1DU:
        case 0x25U: case 0x2DU: case 0x35U: case 0x3DU: {
            const uint8_t r = (op >> 3U) & 7U;
            emit_r8_write(os, r, "sms_dec8(cpu, " + r8_read_expr(r) + ")");
            os << "    cpu->cycles += " << static_cast<unsigned>(ins.cycles) << "U;\n";
            break;
        }
        case 0x06U: case 0x0EU: case 0x16U: case 0x1EU:
        case 0x26U: case 0x2EU: case 0x36U: case 0x3EU: {
            const uint8_t r = (op >> 3U) & 7U;
            emit_r8_write(os, r, "0x" + hex2(static_cast<uint8_t>(ins.imm)) + "U");
            os << "    cpu->cycles += " << static_cast<unsigned>(ins.cycles) << "U;\n";
            break;
        }
        case 0x09U: case 0x19U: case 0x29U: case 0x39U:
            os << "    sms_set_hl(cpu, sms_add16(cpu, sms_hl(cpu), "
               << rp_read_expr((op >> 4U) & 3U) << ")); cpu->cycles += 11U;\n";
            break;

        case 0x07U: case 0x0FU: case 0x17U: case 0x1FU:
        case 0x27U: case 0x2FU: case 0x37U: case 0x3FU:
        case 0xE3U:
            emit_step_fallback(os, ins.addr);
            break;

        case 0x08U: os << "    sms_ex_af(cpu); cpu->cycles += 4U;\n"; break;
        case 0xD9U: os << "    sms_exx(cpu); cpu->cycles += 4U;\n"; break;
        case 0xEBU: os << "    sms_ex_de_hl(cpu); cpu->cycles += 4U;\n"; break;

        case 0x10U:  // DJNZ d
            os << "    cpu->b = (uint8_t)(cpu->b - 1U);\n";
            os << "    if (cpu->b != 0U) {\n        cpu->cycles += 13U;\n";
            emit_branch_target(os, fn, ins.target_addr, ins.addr);
            os << "    }\n    cpu->cycles += 8U;\n";
            break;
        case 0x18U:  // JR d
            os << "    cpu->cycles += 12U;\n";
            emit_branch_target(os, fn, ins.target_addr, ins.addr);
            break;
        case 0x20U: case 0x28U: case 0x30U: case 0x38U:  // JR cc, d
            os << "    if " << cc_expr((op >> 3U) & 3U) << " {\n        cpu->cycles += 12U;\n";
            emit_branch_target(os, fn, ins.target_addr, ins.addr);
            os << "    }\n    cpu->cycles += 7U;\n";
            break;
        case 0xC2U: case 0xCAU: case 0xD2U: case 0xDAU:
        case 0xE2U: case 0xEAU: case 0xF2U: case 0xFAU:  // JP cc, nn
            os << "    cpu->cycles += 10U;\n";
            os << "    if " << cc_expr((op >> 3U) & 7U) << " {\n";
            emit_branch_target(os, fn, ins.target_addr, ins.addr);
            os << "    }\n";
            break;
        case 0xC3U:  // JP nn
            os << "    cpu->cycles += 10U;\n";
            emit_branch_target(os, fn, ins.target_addr, ins.addr);
            break;
        case 0xE9U:  // JP (HL)
            os << "    cpu->cycles += 4U;\n";
            emit_tail_dispatch(os, "sms_hl(cpu)", ins.addr);
            break;

        case 0xC0U: case 0xC8U: case 0xD0U: case 0xD8U:
        case 0xE0U: case 0xE8U: case 0xF0U: case 0xF8U:  // RET cc
            os << "    if " << cc_expr((op >> 3U) & 7U)
               << " { cpu->pc = sms_pop(cpu); cpu->cycles += 11U; return; }\n";
            os << "    cpu->cycles += 5U;\n";
            break;
        case 0xC9U:  // RET
            os << "    cpu->pc = sms_pop(cpu); cpu->cycles += 10U; return;\n";
            break;

        case 0xC4U: case 0xCCU: case 0xD4U: case 0xDCU:
        case 0xE4U: case 0xECU: case 0xF4U: case 0xFCU:  // CALL cc, nn
            os << "    if " << cc_expr((op >> 3U) & 7U) << " {\n";
            emit_call_sequence(os, ins.addr, next_pc, ins.target_addr, 17U);
            os << "    } else { cpu->cycles += 10U; }\n";
            break;
        case 0xCDU:  // CALL nn
            emit_call_sequence(os, ins.addr, next_pc, ins.target_addr, 17U);
            break;

        case 0xC7U: case 0xCFU: case 0xD7U: case 0xDFU:
        case 0xE7U: case 0xEFU: case 0xF7U: case 0xFFU:  // RST p
            emit_call_sequence(os, ins.addr, next_pc, ins.target_addr, 11U);
            break;

        case 0xC1U: case 0xD1U: case 0xE1U: case 0xF1U: {
            const uint8_t p = (op >> 4U) & 3U;
            if (p == 3U) os << "    sms_set_af(cpu, sms_pop(cpu));\n";
            else emit_rp_write(os, p, "sms_pop(cpu)");
            os << "    cpu->cycles += 10U;\n";
            break;
        }
        case 0xC5U: case 0xD5U: case 0xE5U: case 0xF5U: {
            const uint8_t p = (op >> 4U) & 3U;
            os << "    sms_push(cpu, " << (p == 3U ? "sms_af(cpu)" : rp_read_expr(p))
               << "); cpu->cycles += 11U;\n";
            break;
        }
        case 0xF9U: os << "    cpu->sp = sms_hl(cpu); cpu->cycles += 6U;\n"; break;

        case 0xC6U: case 0xCEU: case 0xD6U: case 0xDEU:
        case 0xE6U: case 0xEEU: case 0xF6U: case 0xFEU:
            emit_alu(os, (op >> 3U) & 7U, "0x" + hex2(static_cast<uint8_t>(ins.imm)) + "U");
            os << "    cpu->cycles += 7U;\n";
            break;

        case 0xD3U:
            os << "    sms_io_write(cpu, 0x" << hex2(static_cast<uint8_t>(ins.imm))
               << "U, cpu->a); cpu->cycles += 11U;\n";
            break;
        case 0xDBU:
            os << "    cpu->a = sms_io_read(cpu, 0x" << hex2(static_cast<uint8_t>(ins.imm))
               << "U); cpu->cycles += 11U;\n";
            break;

        case 0xF3U:
            os << "    cpu->iff1 = 0U; cpu->iff2 = 0U; cpu->ei_delay = 0U; cpu->cycles += 4U;\n";
            break;
        case 0xFBU:
            os << "    cpu->iff1 = 1U; cpu->iff2 = 1U; cpu->ei_delay = 0U; cpu->cycles += 4U;\n";
            break;

        default:
            emit_step_fallback(os, ins.addr);
            break;
    }
}

void emit_function(std::ostringstream& os, const FunctionCfg& fn) {
    // Pass 1: collect labels actually referenced by goto statements (prevents both
    // undefined-label errors and -Wunused-label warnings) and detect tail/indirect jumps.
    std::set<uint16_t> used_labels;
    std::set<uint16_t> resume_targets;
    bool has_tail_dispatch = false;

    const bool need_entry_jump =
        !fn.instructions.empty() && fn.instructions.begin()->first != fn.entry_addr &&
        fn.instructions.count(fn.entry_addr) > 0U;
    if (need_entry_jump) {
        used_labels.insert(fn.entry_addr);
    }

    for (auto it = fn.instructions.begin(); it != fn.instructions.end(); ++it) {
        const uint16_t addr = it->first;
        const DecodedInstr& ins = it->second;
        if (ins.is_indirect_jump) {
            has_tail_dispatch = true;
            const uint16_t after = static_cast<uint16_t>(addr + ins.length);
            if (fn.instructions.count(after) > 0U) {
                resume_targets.insert(after);
            }
        }
        if (ins.has_branch && !ins.is_call) {
            if (fn.instructions.count(ins.target_addr) > 0U &&
                fn.local_labels.count(ins.target_addr) > 0U) {
                used_labels.insert(ins.target_addr);
            } else {
                has_tail_dispatch = true;
            }
        }
        if (!ins.is_terminator) {
            const uint16_t next_pc = static_cast<uint16_t>(addr + ins.length);
            const auto next_it = std::next(it);
            if (next_it == fn.instructions.end() || next_it->first != next_pc) {
                if (fn.instructions.count(next_pc) > 0U) {
                    used_labels.insert(next_pc);
                } else {
                    has_tail_dispatch = true;
                }
            }
        }
    }

    // Detect Z80 synthetic indirect-call return targets: LD BC/DE/HL/IX/IY, nn followed by
    // PUSH rr; JP (HL) where nn is an instruction inside this function.
    if (has_tail_dispatch) {
        for (const auto& [addr, ins] : fn.instructions) {
            (void)addr;
            const bool loads_u16 =
                (ins.prefix == Prefix::None &&
                 (ins.opcode == 0x01U || ins.opcode == 0x11U || ins.opcode == 0x21U)) ||
                ((ins.prefix == Prefix::DD || ins.prefix == Prefix::FD) && ins.opcode == 0x21U);
            if (loads_u16 && fn.instructions.count(ins.imm) > 0U) {
                resume_targets.insert(ins.imm);
            }
        }
        for (uint16_t t : resume_targets) {
            used_labels.insert(t);
        }
    }

    // Pass 2: emit C99 function body.
    os << "void " << func_name(fn.bank, fn.entry_addr) << "(SmsCpuState* cpu) {\n";
    if (has_tail_dispatch) {
        os << "    uint16_t _fn_sp = cpu->sp;\n";
    }
    if (need_entry_jump) {
        os << "    goto L_" << hex4(fn.entry_addr) << ";\n";
    }
    for (auto it = fn.instructions.begin(); it != fn.instructions.end(); ++it) {
        const uint16_t addr = it->first;
        const DecodedInstr& ins = it->second;
        if (used_labels.count(addr) > 0U) {
            os << "L_" << hex4(addr) << ":\n";
        }
        emit_instruction(os, fn, ins);
        // Keep _fn_sp aligned if this function explicitly reloads SP (e.g. reset LD SP, nn).
        if (has_tail_dispatch) {
            const bool sets_sp =
                (ins.prefix == Prefix::None && (ins.opcode == 0x31U || ins.opcode == 0xF9U)) ||
                (ins.prefix == Prefix::ED && ins.opcode == 0x7BU) ||
                ((ins.prefix == Prefix::DD || ins.prefix == Prefix::FD) && ins.opcode == 0xF9U);
            if (sets_sp) {
                os << "    _fn_sp = cpu->sp;\n";
            }
        }
        if (!ins.is_terminator) {
            const uint16_t next_pc = static_cast<uint16_t>(addr + ins.length);
            const auto next_it = std::next(it);
            if (next_it == fn.instructions.end() || next_it->first != next_pc) {
                if (fn.instructions.count(next_pc) > 0U) {
                    if (next_pc <= addr) {
                        emit_poll_if_needed(os, next_pc);
                    }
                    os << "    goto L_" << hex4(next_pc) << ";\n";
                } else {
                    emit_tail_dispatch(os, "0x" + hex4(next_pc) + "U", addr);
                }
            }
        }
    }
    if (has_tail_dispatch) {
        os << "L_tail_resume:\n";
        os << "    while ((int16_t)(cpu->sp - _fn_sp) <= 0) {\n";
        if (!resume_targets.empty()) {
            os << "        switch (cpu->pc) {\n";
            for (uint16_t t : resume_targets) {
                os << "            case 0x" << hex4(t) << "U: goto L_" << hex4(t) << ";\n";
            }
            os << "            default: break;\n";
            os << "        }\n";
        }
        os << "        if (sms_runtime_dispatch(cpu, cpu->pc, 0x" << hex4(fn.entry_addr)
           << "U)) return;\n";
        os << "    }\n";
        os << "    return;\n";
    }
    os << "}\n\n";
}

}  // namespace

EmitResult emit_c99(const DisassemblyResult& disasm, const EmitOptions& opts) {
    EmitResult res{};
    std::error_code ec;
    std::filesystem::create_directories(opts.output_dir, ec);
    if (ec) {
        res.error = "No se pudo crear el directorio de salida: " + opts.output_dir.string();
        return res;
    }

    // 1. Emit recomp_functions.h
    {
        std::ofstream hdr(opts.output_dir / "recomp_functions.h", std::ios::trunc);
        if (!hdr.is_open()) {
            res.error = "No se pudo escribir recomp_functions.h";
            return res;
        }
        hdr << "#pragma once\n"
            << "#include <stddef.h>\n"
            << "#include \"sms/cpu_state.h\"\n\n"
            << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
        for (const auto& fn : disasm.functions) {
            hdr << "void " << func_name(fn.bank, fn.entry_addr) << "(SmsCpuState* cpu);\n";
        }
        hdr << "\nextern const SmsRecompFuncEntry kRecompDispatchTable[];\n"
            << "extern const size_t kRecompDispatchTableCount;\n\n"
            << "#ifdef __cplusplus\n}\n#endif\n";
        ++res.files_written;
    }

    // 2. Emit dispatch table + C99 function bodies
    if (opts.single_file || disasm.functions.size() <= opts.split_every) {
        std::ostringstream body;
        body << "/* Auto-generated by smsrecomp — DO NOT EDIT */\n"
             << "#include \"recomp_functions.h\"\n\n";
        for (const auto& fn : disasm.functions) {
            emit_function(body, fn);
        }
        body << "const SmsRecompFuncEntry kRecompDispatchTable[] = {\n";
        if (disasm.functions.empty()) {
            body << "    { 0x0000U, 0x00U, NULL }\n";
        } else {
            for (const auto& fn : disasm.functions) {
                body << "    { 0x" << hex4(fn.entry_addr) << "U, 0x" << hex2(fn.bank) << "U, "
                     << func_name(fn.bank, fn.entry_addr) << " },\n";
            }
        }
        body << "};\n"
             << "const size_t kRecompDispatchTableCount = " << disasm.functions.size() << "U;\n";

        std::ofstream out(opts.output_dir / "recomp_all.c", std::ios::trunc);
        if (!out.is_open()) {
            res.error = "No se pudo escribir recomp_all.c";
            return res;
        }
        out << body.str();
        ++res.files_written;
    } else {
        const size_t chunk = std::max<size_t>(1U, opts.split_every);
        size_t part = 0;
        for (size_t i = 0; i < disasm.functions.size(); i += chunk, ++part) {
            std::ostringstream body;
            body << "/* Auto-generated by smsrecomp (part " << part << ") */\n"
                 << "#include \"recomp_functions.h\"\n\n";
            const size_t end = std::min(disasm.functions.size(), i + chunk);
            for (size_t j = i; j < end; ++j) {
                emit_function(body, disasm.functions[j]);
            }
            std::ofstream out(opts.output_dir / ("recomp_part_" + std::to_string(part) + ".c"),
                              std::ios::trunc);
            if (!out.is_open()) {
                res.error = "No se pudo escribir recomp_part_" + std::to_string(part) + ".c";
                return res;
            }
            out << body.str();
            ++res.files_written;
        }

        // Dispatch table file
        std::ofstream tbl(opts.output_dir / "recomp_dispatch.c", std::ios::trunc);
        if (!tbl.is_open()) {
            res.error = "No se pudo escribir recomp_dispatch.c";
            return res;
        }
        tbl << "#include \"recomp_functions.h\"\n\n"
            << "const SmsRecompFuncEntry kRecompDispatchTable[] = {\n";
        for (const auto& fn : disasm.functions) {
            tbl << "    { 0x" << hex4(fn.entry_addr) << "U, 0x" << hex2(fn.bank) << "U, "
                << func_name(fn.bank, fn.entry_addr) << " },\n";
        }
        tbl << "};\n"
            << "const size_t kRecompDispatchTableCount = " << disasm.functions.size() << "U;\n";
        ++res.files_written;
    }

    res.success = true;
    res.functions_emitted = disasm.functions.size();
    return res;
}

}  // namespace dragon::smsrecomp
