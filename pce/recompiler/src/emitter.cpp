#include "pcerecomp/emitter.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

namespace dragon::pcerecomp {

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

std::string ea_expr(const DecodedInstr& ins) {
    using M = pce::AddrMode;
    switch (ins.mode) {
        case M::Zp: return "(uint16_t)(0x2000U | 0x" + hex2(ins.zp) + "U)";
        case M::ZpX: return "(uint16_t)(0x2000U | (uint8_t)(0x" + hex2(ins.zp) + "U + cpu->x))";
        case M::ZpY: return "(uint16_t)(0x2000U | (uint8_t)(0x" + hex2(ins.zp) + "U + cpu->y))";
        case M::Abs: return "0x" + hex4(ins.addr16) + "U";
        case M::AbsX: return "(uint16_t)(0x" + hex4(ins.addr16) + "U + cpu->x)";
        case M::AbsY: return "(uint16_t)(0x" + hex4(ins.addr16) + "U + cpu->y)";
        case M::ZpInd: return "pce_zp_read16(cpu, 0x" + hex2(ins.zp) + "U)";
        case M::ZpIndX: return "pce_zp_read16(cpu, (uint8_t)(0x" + hex2(ins.zp) + "U + cpu->x))";
        case M::ZpIndY: return "(uint16_t)(pce_zp_read16(cpu, 0x" + hex2(ins.zp) + "U) + cpu->y)";
        case M::ImmZp: return "(uint16_t)(0x2000U | 0x" + hex2(ins.zp) + "U)";
        case M::ImmZpX: return "(uint16_t)(0x2000U | (uint8_t)(0x" + hex2(ins.zp) + "U + cpu->x))";
        case M::ImmAbs: return "0x" + hex4(ins.addr16) + "U";
        case M::ImmAbsX: return "(uint16_t)(0x" + hex4(ins.addr16) + "U + cpu->x)";
        default: return "0U";
    }
}

std::string val_expr(const DecodedInstr& ins) {
    if (ins.mode == pce::AddrMode::Imm) {
        return "0x" + hex2(ins.imm8) + "U";
    }
    return "pce_bus_read(cpu, " + ea_expr(ins) + ")";
}

void emit_poll_if_needed(std::ostringstream& os, uint16_t resume_pc) {
    os << "    if (cpu->cycles >= cpu->next_event) { cpu->pc = 0x" << hex4(resume_pc)
       << "U; if (pce_runtime_poll(cpu)) return; }\n";
}

void emit_tail_dispatch(std::ostringstream& os, const std::string& target_expr, uint16_t from_pc) {
    os << "    if (pce_runtime_dispatch(cpu, " << target_expr << ", 0x" << hex4(from_pc)
       << "U)) return;\n"
       << "    if ((int8_t)(cpu->s - _fn_s) <= 0) { goto L_tail_resume; }\n"
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

void emit_call_sequence(std::ostringstream& os, uint16_t addr, uint16_t next_pc,
                        uint16_t target, uint32_t cycles) {
    const uint16_t pushed = static_cast<uint16_t>(next_pc - 1U);
    os << "    {\n"
       << "        uint8_t _s = cpu->s;\n"
       << "        pce_push16(cpu, 0x" << hex4(pushed) << "U);\n"
       << "        pce_add_cycles(cpu, " << cycles << "U);\n"
       << "        if (pce_runtime_dispatch(cpu, 0x" << hex4(target) << "U, 0x" << hex4(addr)
       << "U)) return;\n"
       << "        while (cpu->pc != 0x" << hex4(next_pc) << "U && (int8_t)(cpu->s - _s) < 0) {\n"
       << "            if (pce_runtime_dispatch(cpu, cpu->pc, 0x" << hex4(addr) << "U)) return;\n"
       << "        }\n"
       << "        if (cpu->pc != 0x" << hex4(next_pc) << "U) return;\n"
       << "    }\n";
}

void emit_clear_t(std::ostringstream& os, uint8_t op) {
    // Non-SET instructions clear T flag (ALU ops clear T inside pce_*_t helpers)
    if (op != 0xF4U) {
        os << "    cpu->p &= (uint8_t)~PCE_FLAG_T;\n";
    }
}

void emit_instruction(std::ostringstream& os, const FunctionCfg& fn, const DecodedInstr& ins) {
    const uint8_t op = ins.opcode;
    const uint16_t next_pc = static_cast<uint16_t>(ins.addr + ins.length);

    // Block transfers (TII/TDD/TIN/TIA/TAI): fast native C99 loop
    if (ins.mode == pce::AddrMode::Block) {
        const uint32_t count = (ins.blk_len == 0U) ? 65536U : static_cast<uint32_t>(ins.blk_len);
        emit_clear_t(os, op);
        os << "    {\n"
           << "        uint16_t _src = 0x" << hex4(ins.blk_src) << "U, _dst = 0x"
           << hex4(ins.blk_dst) << "U;\n"
           << "        uint32_t _cnt = " << count << "U;\n";
        if (op == 0xE3U || op == 0xF3U) {
            os << "        uint8_t _alt = 0U;\n";
        }
        os << "        pce_add_cycles(cpu, 17U);\n"
           << "        for (uint32_t _i = 0U; _i < _cnt; ++_i) {\n"
           << "            uint8_t _v = pce_bus_read(cpu, _src);\n"
           << "            pce_bus_write(cpu, _dst, _v);\n";
        if (op == 0x73U) {
            os << "            _src = (uint16_t)(_src + 1U); _dst = (uint16_t)(_dst + 1U);\n";
        } else if (op == 0xC3U) {
            os << "            _src = (uint16_t)(_src - 1U); _dst = (uint16_t)(_dst - 1U);\n";
        } else if (op == 0xD3U) {
            os << "            _src = (uint16_t)(_src + 1U);\n";
        } else if (op == 0xE3U) {
            os << "            _src = (uint16_t)(_src + 1U); _dst = (uint16_t)(_alt ? (_dst - 1U) : (_dst + 1U)); _alt ^= 1U;\n";
        } else {
            os << "            _src = (uint16_t)(_alt ? (_src - 1U) : (_src + 1U)); _dst = (uint16_t)(_dst + 1U); _alt ^= 1U;\n";
        }
        os << "            pce_add_cycles(cpu, 6U);\n"
           << "        }\n"
           << "        if (cpu->cycles >= cpu->next_event) { cpu->pc = 0x" << hex4(next_pc)
           << "U; if (pce_runtime_poll(cpu)) return; }\n"
           << "    }\n";
        return;
    }

    switch (op) {
        // --- Control flow ---
        case 0x00U:  // BRK -> exact runtime step
            os << "    cpu->pc = 0x" << hex4(ins.addr) << "U;\n"
               << "    if (pce_runtime_step(cpu)) return;\n"
               << "    return;\n";
            return;
        case 0x20U:  // JSR abs
            emit_clear_t(os, op);
            emit_call_sequence(os, ins.addr, next_pc, ins.target_addr, 7U);
            return;
        case 0x44U:  // BSR rel
            emit_clear_t(os, op);
            emit_call_sequence(os, ins.addr, next_pc, ins.target_addr, 8U);
            return;
        case 0x40U:  // RTI
            os << "    cpu->p = (uint8_t)(pce_pop8(cpu) & (uint8_t)~(PCE_FLAG_B | PCE_FLAG_T));\n"
               << "    cpu->pc = pce_pop16(cpu); pce_add_cycles(cpu, 7U); return;\n";
            return;
        case 0x60U:  // RTS
            emit_clear_t(os, op);
            os << "    cpu->pc = (uint16_t)(pce_pop16(cpu) + 1U); pce_add_cycles(cpu, 7U); return;\n";
            return;
        case 0x4CU:  // JMP abs
            emit_clear_t(os, op);
            os << "    pce_add_cycles(cpu, 4U);\n";
            emit_branch_target(os, fn, ins.target_addr, ins.addr);
            return;
        case 0x6CU:  // JMP (abs)
            emit_clear_t(os, op);
            os << "    pce_add_cycles(cpu, 7U);\n";
            emit_tail_dispatch(os, "pce_bus_read16(cpu, 0x" + hex4(ins.addr16) + "U)", ins.addr);
            return;
        case 0x7CU:  // JMP (abs,X)
            emit_clear_t(os, op);
            os << "    pce_add_cycles(cpu, 7U);\n";
            emit_tail_dispatch(
                os, "pce_bus_read16(cpu, (uint16_t)(0x" + hex4(ins.addr16) + "U + cpu->x))",
                ins.addr);
            return;

        // --- Branches ---
        case 0x80U:  // BRA
            emit_clear_t(os, op);
            os << "    pce_add_cycles(cpu, 4U);\n";
            emit_branch_target(os, fn, ins.target_addr, ins.addr);
            return;
        case 0x10U: case 0x30U: case 0x50U: case 0x70U:
        case 0x90U: case 0xB0U: case 0xD0U: case 0xF0U: {
            const char* cond = "0";
            switch (op) {
                case 0x10U: cond = "((cpu->p & PCE_FLAG_N) == 0U)"; break;
                case 0x30U: cond = "((cpu->p & PCE_FLAG_N) != 0U)"; break;
                case 0x50U: cond = "((cpu->p & PCE_FLAG_V) == 0U)"; break;
                case 0x70U: cond = "((cpu->p & PCE_FLAG_V) != 0U)"; break;
                case 0x90U: cond = "((cpu->p & PCE_FLAG_C) == 0U)"; break;
                case 0xB0U: cond = "((cpu->p & PCE_FLAG_C) != 0U)"; break;
                case 0xD0U: cond = "((cpu->p & PCE_FLAG_Z) == 0U)"; break;
                case 0xF0U: cond = "((cpu->p & PCE_FLAG_Z) != 0U)"; break;
                default: break;
            }
            emit_clear_t(os, op);
            os << "    if " << cond << " {\n        pce_add_cycles(cpu, 4U);\n";
            emit_branch_target(os, fn, ins.target_addr, ins.addr);
            os << "    }\n    pce_add_cycles(cpu, 2U);\n";
            return;
        }
        case 0x0FU: case 0x1FU: case 0x2FU: case 0x3FU:
        case 0x4FU: case 0x5FU: case 0x6FU: case 0x7FU:  // BBR0..7
        case 0x8FU: case 0x9FU: case 0xAFU: case 0xBFU:
        case 0xCFU: case 0xDFU: case 0xEFU: case 0xFFU: {  // BBS0..7
            const uint8_t bit = static_cast<uint8_t>((op >> 4U) & 7U);
            const bool set_branch = (op >= 0x80U);
            emit_clear_t(os, op);
            os << "    if ((pce_zp_read(cpu, 0x" << hex2(ins.zp) << "U) & 0x"
               << hex2(static_cast<uint8_t>(1U << bit)) << "U) " << (set_branch ? "!= 0U" : "== 0U")
               << ") {\n        pce_add_cycles(cpu, 8U);\n";
            emit_branch_target(os, fn, ins.target_addr, ins.addr);
            os << "    }\n    pce_add_cycles(cpu, 6U);\n";
            return;
        }

        // --- HuC6280 RMB/SMB/ST0/ST1/ST2/TAM/TMA/CSL/CSH/SET/Swaps/Clears ---
        case 0x07U: case 0x17U: case 0x27U: case 0x37U:
        case 0x47U: case 0x57U: case 0x67U: case 0x77U: {
            const uint8_t mask = static_cast<uint8_t>(~(1U << ((op >> 4U) & 7U)));
            emit_clear_t(os, op);
            os << "    pce_zp_write(cpu, 0x" << hex2(ins.zp) << "U, (uint8_t)(pce_zp_read(cpu, 0x"
               << hex2(ins.zp) << "U) & 0x" << hex2(mask) << "U)); pce_add_cycles(cpu, 7U);\n";
            return;
        }
        case 0x87U: case 0x97U: case 0xA7U: case 0xB7U:
        case 0xC7U: case 0xD7U: case 0xE7U: case 0xF7U: {
            const uint8_t mask = static_cast<uint8_t>(1U << ((op >> 4U) & 7U));
            emit_clear_t(os, op);
            os << "    pce_zp_write(cpu, 0x" << hex2(ins.zp) << "U, (uint8_t)(pce_zp_read(cpu, 0x"
               << hex2(ins.zp) << "U) | 0x" << hex2(mask) << "U)); pce_add_cycles(cpu, 7U);\n";
            return;
        }
        case 0x03U: emit_clear_t(os, op); os << "    pce_st0(cpu, 0x" << hex2(ins.imm8) << "U); pce_add_cycles(cpu, 5U);\n"; return;
        case 0x13U: emit_clear_t(os, op); os << "    pce_st1(cpu, 0x" << hex2(ins.imm8) << "U); pce_add_cycles(cpu, 5U);\n"; return;
        case 0x23U: emit_clear_t(os, op); os << "    pce_st2(cpu, 0x" << hex2(ins.imm8) << "U); pce_add_cycles(cpu, 5U);\n"; return;
        case 0x43U: emit_clear_t(os, op); os << "    pce_tma(cpu, 0x" << hex2(ins.imm8) << "U); pce_add_cycles(cpu, 4U);\n"; return;
        case 0x53U: emit_clear_t(os, op); os << "    pce_tam(cpu, 0x" << hex2(ins.imm8) << "U); pce_add_cycles(cpu, 5U);\n"; return;
        case 0x54U: emit_clear_t(os, op); os << "    cpu->speed_high = 0U; pce_add_cycles(cpu, 3U);\n"; return;
        case 0xD4U: emit_clear_t(os, op); os << "    cpu->speed_high = 1U; pce_add_cycles(cpu, 3U);\n"; return;
        case 0xF4U: os << "    cpu->p |= PCE_FLAG_T; pce_add_cycles(cpu, 2U);\n"; return;
        case 0x02U: emit_clear_t(os, op); os << "    { uint8_t _t = cpu->x; cpu->x = cpu->y; cpu->y = _t; } pce_add_cycles(cpu, 3U);\n"; return;
        case 0x22U: emit_clear_t(os, op); os << "    { uint8_t _t = cpu->a; cpu->a = cpu->x; cpu->x = _t; } pce_add_cycles(cpu, 3U);\n"; return;
        case 0x42U: emit_clear_t(os, op); os << "    { uint8_t _t = cpu->a; cpu->a = cpu->y; cpu->y = _t; } pce_add_cycles(cpu, 3U);\n"; return;
        case 0x62U: emit_clear_t(os, op); os << "    cpu->a = 0U; pce_add_cycles(cpu, 2U);\n"; return;
        case 0x82U: emit_clear_t(os, op); os << "    cpu->x = 0U; pce_add_cycles(cpu, 2U);\n"; return;
        case 0xC2U: emit_clear_t(os, op); os << "    cpu->y = 0U; pce_add_cycles(cpu, 2U);\n"; return;

        // --- TST ---
        case 0x83U: case 0x93U: case 0xA3U: case 0xB3U:
            emit_clear_t(os, op);
            os << "    pce_tst_val(cpu, 0x" << hex2(ins.imm8) << "U, pce_bus_read(cpu, "
               << ea_expr(ins) << ")); pce_add_cycles(cpu, " << static_cast<unsigned>(ins.cycles)
               << "U);\n";
            return;

        // --- Flags & Stack ---
        case 0x18U: emit_clear_t(os, op); os << "    cpu->p &= (uint8_t)~PCE_FLAG_C; pce_add_cycles(cpu, 2U);\n"; return;
        case 0x38U: emit_clear_t(os, op); os << "    cpu->p |= PCE_FLAG_C; pce_add_cycles(cpu, 2U);\n"; return;
        case 0x58U: emit_clear_t(os, op); os << "    cpu->p &= (uint8_t)~PCE_FLAG_I; pce_add_cycles(cpu, 2U);\n"; return;
        case 0x78U: emit_clear_t(os, op); os << "    cpu->p |= PCE_FLAG_I; pce_add_cycles(cpu, 2U);\n"; return;
        case 0xB8U: emit_clear_t(os, op); os << "    cpu->p &= (uint8_t)~PCE_FLAG_V; pce_add_cycles(cpu, 2U);\n"; return;
        case 0xD8U: emit_clear_t(os, op); os << "    cpu->p &= (uint8_t)~PCE_FLAG_D; pce_add_cycles(cpu, 2U);\n"; return;
        case 0xF8U: emit_clear_t(os, op); os << "    cpu->p |= PCE_FLAG_D; pce_add_cycles(cpu, 2U);\n"; return;
        case 0x08U: emit_clear_t(os, op); os << "    pce_push8(cpu, (uint8_t)(cpu->p | PCE_FLAG_B)); pce_add_cycles(cpu, 3U);\n"; return;
        case 0x28U: os << "    cpu->p = (uint8_t)(pce_pop8(cpu) & (uint8_t)~(PCE_FLAG_B | PCE_FLAG_T)); pce_add_cycles(cpu, 4U);\n"; return;
        case 0x48U: emit_clear_t(os, op); os << "    pce_push8(cpu, cpu->a); pce_add_cycles(cpu, 3U);\n"; return;
        case 0x68U: emit_clear_t(os, op); os << "    cpu->a = pce_pop8(cpu); pce_set_nz(cpu, cpu->a); pce_add_cycles(cpu, 4U);\n"; return;
        case 0xDAU: emit_clear_t(os, op); os << "    pce_push8(cpu, cpu->x); pce_add_cycles(cpu, 3U);\n"; return;
        case 0xFAU: emit_clear_t(os, op); os << "    cpu->x = pce_pop8(cpu); pce_set_nz(cpu, cpu->x); pce_add_cycles(cpu, 4U);\n"; return;
        case 0x5AU: emit_clear_t(os, op); os << "    pce_push8(cpu, cpu->y); pce_add_cycles(cpu, 3U);\n"; return;
        case 0x7AU: emit_clear_t(os, op); os << "    cpu->y = pce_pop8(cpu); pce_set_nz(cpu, cpu->y); pce_add_cycles(cpu, 4U);\n"; return;

        // --- Transfers & Inc/Dec ---
        case 0xAAU: emit_clear_t(os, op); os << "    cpu->x = cpu->a; pce_set_nz(cpu, cpu->x); pce_add_cycles(cpu, 2U);\n"; return;
        case 0x8AU: emit_clear_t(os, op); os << "    cpu->a = cpu->x; pce_set_nz(cpu, cpu->a); pce_add_cycles(cpu, 2U);\n"; return;
        case 0xA8U: emit_clear_t(os, op); os << "    cpu->y = cpu->a; pce_set_nz(cpu, cpu->y); pce_add_cycles(cpu, 2U);\n"; return;
        case 0x98U: emit_clear_t(os, op); os << "    cpu->a = cpu->y; pce_set_nz(cpu, cpu->a); pce_add_cycles(cpu, 2U);\n"; return;
        case 0xBAU: emit_clear_t(os, op); os << "    cpu->x = cpu->s; pce_set_nz(cpu, cpu->x); pce_add_cycles(cpu, 2U);\n"; return;
        case 0x9AU: emit_clear_t(os, op); os << "    cpu->s = cpu->x; pce_add_cycles(cpu, 2U);\n"; return;
        case 0xE8U: emit_clear_t(os, op); os << "    cpu->x = (uint8_t)(cpu->x + 1U); pce_set_nz(cpu, cpu->x); pce_add_cycles(cpu, 2U);\n"; return;
        case 0xCAU: emit_clear_t(os, op); os << "    cpu->x = (uint8_t)(cpu->x - 1U); pce_set_nz(cpu, cpu->x); pce_add_cycles(cpu, 2U);\n"; return;
        case 0xC8U: emit_clear_t(os, op); os << "    cpu->y = (uint8_t)(cpu->y + 1U); pce_set_nz(cpu, cpu->y); pce_add_cycles(cpu, 2U);\n"; return;
        case 0x88U: emit_clear_t(os, op); os << "    cpu->y = (uint8_t)(cpu->y - 1U); pce_set_nz(cpu, cpu->y); pce_add_cycles(cpu, 2U);\n"; return;
        case 0x1AU: emit_clear_t(os, op); os << "    cpu->a = (uint8_t)(cpu->a + 1U); pce_set_nz(cpu, cpu->a); pce_add_cycles(cpu, 2U);\n"; return;
        case 0x3AU: emit_clear_t(os, op); os << "    cpu->a = (uint8_t)(cpu->a - 1U); pce_set_nz(cpu, cpu->a); pce_add_cycles(cpu, 2U);\n"; return;

        // --- Loads ---
        case 0xA9U: case 0xA5U: case 0xB5U: case 0xADU: case 0xBDU: case 0xB9U:
        case 0xA1U: case 0xB1U: case 0xB2U:
            emit_clear_t(os, op);
            os << "    cpu->a = " << val_expr(ins) << "; pce_set_nz(cpu, cpu->a); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0xA2U: case 0xA6U: case 0xB6U: case 0xAEU: case 0xBEU:
            emit_clear_t(os, op);
            os << "    cpu->x = " << val_expr(ins) << "; pce_set_nz(cpu, cpu->x); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0xA0U: case 0xA4U: case 0xB4U: case 0xACU: case 0xBCU:
            emit_clear_t(os, op);
            os << "    cpu->y = " << val_expr(ins) << "; pce_set_nz(cpu, cpu->y); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;

        // --- Stores ---
        case 0x85U: case 0x95U: case 0x8DU: case 0x9DU: case 0x99U:
        case 0x81U: case 0x91U: case 0x92U:
            emit_clear_t(os, op);
            os << "    pce_bus_write(cpu, " << ea_expr(ins) << ", cpu->a); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0x86U: case 0x96U: case 0x8EU:
            emit_clear_t(os, op);
            os << "    pce_bus_write(cpu, " << ea_expr(ins) << ", cpu->x); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0x84U: case 0x94U: case 0x8CU:
            emit_clear_t(os, op);
            os << "    pce_bus_write(cpu, " << ea_expr(ins) << ", cpu->y); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0x64U: case 0x74U: case 0x9CU: case 0x9EU:
            emit_clear_t(os, op);
            os << "    pce_bus_write(cpu, " << ea_expr(ins) << ", 0U); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;

        // --- T-flag-aware ALU (ORA / AND / EOR / ADC) ---
        case 0x09U: case 0x05U: case 0x15U: case 0x0DU: case 0x1DU: case 0x19U:
        case 0x01U: case 0x11U: case 0x12U:
            os << "    pce_ora_t(cpu, " << val_expr(ins) << "); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0x29U: case 0x25U: case 0x35U: case 0x2DU: case 0x3DU: case 0x39U:
        case 0x21U: case 0x31U: case 0x32U:
            os << "    pce_and_t(cpu, " << val_expr(ins) << "); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0x49U: case 0x45U: case 0x55U: case 0x4DU: case 0x5DU: case 0x59U:
        case 0x41U: case 0x51U: case 0x52U:
            os << "    pce_eor_t(cpu, " << val_expr(ins) << "); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0x69U: case 0x65U: case 0x75U: case 0x6DU: case 0x7DU: case 0x79U:
        case 0x61U: case 0x71U: case 0x72U:
            os << "    pce_adc_t(cpu, " << val_expr(ins) << "); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;

        // --- SBC & Compares ---
        case 0xE9U: case 0xE5U: case 0xF5U: case 0xEDU: case 0xFDU: case 0xF9U:
        case 0xE1U: case 0xF1U: case 0xF2U:
            emit_clear_t(os, op);
            os << "    cpu->a = pce_sbc_val(cpu, cpu->a, " << val_expr(ins) << "); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0xC9U: case 0xC5U: case 0xD5U: case 0xCDU: case 0xDDU: case 0xD9U:
        case 0xC1U: case 0xD1U: case 0xD2U: {
            emit_clear_t(os, op);
            const auto next_it = fn.instructions.find(next_pc);
            if (next_it != fn.instructions.end() &&
                (next_it->second.opcode == 0xF0U || next_it->second.opcode == 0xD0U) &&
                next_it->second.target_addr < ins.addr &&
                static_cast<unsigned>(ins.addr - next_it->second.target_addr) <= 4U) {
                os << "    if (cpu->cycles < cpu->next_event) { cpu->cycles = cpu->next_event; }\n";
                emit_poll_if_needed(os, ins.addr);
            }
            os << "    pce_cmp(cpu, cpu->a, " << val_expr(ins) << "); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        }
        case 0xE0U: case 0xE4U: case 0xECU: {
            emit_clear_t(os, op);
            const auto next_it = fn.instructions.find(next_pc);
            if (next_it != fn.instructions.end() &&
                (next_it->second.opcode == 0xF0U || next_it->second.opcode == 0xD0U) &&
                next_it->second.target_addr < ins.addr &&
                static_cast<unsigned>(ins.addr - next_it->second.target_addr) <= 4U) {
                os << "    if (cpu->cycles < cpu->next_event) { cpu->cycles = cpu->next_event; }\n";
                emit_poll_if_needed(os, ins.addr);
            }
            os << "    pce_cmp(cpu, cpu->x, " << val_expr(ins) << "); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        }
        case 0xC0U: case 0xC4U: case 0xCCU: {
            emit_clear_t(os, op);
            const auto next_it = fn.instructions.find(next_pc);
            if (next_it != fn.instructions.end() &&
                (next_it->second.opcode == 0xF0U || next_it->second.opcode == 0xD0U) &&
                next_it->second.target_addr < ins.addr &&
                static_cast<unsigned>(ins.addr - next_it->second.target_addr) <= 4U) {
                os << "    if (cpu->cycles < cpu->next_event) { cpu->cycles = cpu->next_event; }\n";
                emit_poll_if_needed(os, ins.addr);
            }
            os << "    pce_cmp(cpu, cpu->y, " << val_expr(ins) << "); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        }

        // --- BIT / TSB / TRB ---
        case 0x89U:
            emit_clear_t(os, op);
            os << "    cpu->p = (uint8_t)((cpu->p & (uint8_t)~PCE_FLAG_Z) | ((cpu->a & 0x"
               << hex2(ins.imm8) << "U) == 0U ? PCE_FLAG_Z : 0U)); pce_add_cycles(cpu, 2U);\n";
            return;
        case 0x24U: case 0x34U: case 0x2CU: case 0x3CU:
            emit_clear_t(os, op);
            os << "    pce_tst_val(cpu, cpu->a, " << val_expr(ins) << "); pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0x04U: case 0x0CU:
            emit_clear_t(os, op);
            os << "    { uint16_t _a = " << ea_expr(ins)
               << "; uint8_t _m = pce_bus_read(cpu, _a); pce_tst_val(cpu, cpu->a, _m); pce_bus_write(cpu, _a, (uint8_t)(_m | cpu->a)); } pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0x14U: case 0x1CU:
            emit_clear_t(os, op);
            os << "    { uint16_t _a = " << ea_expr(ins)
               << "; uint8_t _m = pce_bus_read(cpu, _a); pce_tst_val(cpu, cpu->a, _m); pce_bus_write(cpu, _a, (uint8_t)(_m & (uint8_t)~cpu->a)); } pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;

        // --- Shifts / Rotates / Memory Inc / Dec ---
        case 0x0AU: emit_clear_t(os, op); os << "    cpu->a = pce_asl(cpu, cpu->a); pce_add_cycles(cpu, 2U);\n"; return;
        case 0x4AU: emit_clear_t(os, op); os << "    cpu->a = pce_lsr(cpu, cpu->a); pce_add_cycles(cpu, 2U);\n"; return;
        case 0x2AU: emit_clear_t(os, op); os << "    cpu->a = pce_rol(cpu, cpu->a); pce_add_cycles(cpu, 2U);\n"; return;
        case 0x6AU: emit_clear_t(os, op); os << "    cpu->a = pce_ror(cpu, cpu->a); pce_add_cycles(cpu, 2U);\n"; return;

        case 0x06U: case 0x16U: case 0x0EU: case 0x1EU:
            emit_clear_t(os, op);
            os << "    { uint16_t _a = " << ea_expr(ins)
               << "; pce_bus_write(cpu, _a, pce_asl(cpu, pce_bus_read(cpu, _a))); } pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0x46U: case 0x56U: case 0x4EU: case 0x5EU:
            emit_clear_t(os, op);
            os << "    { uint16_t _a = " << ea_expr(ins)
               << "; pce_bus_write(cpu, _a, pce_lsr(cpu, pce_bus_read(cpu, _a))); } pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0x26U: case 0x36U: case 0x2EU: case 0x3EU:
            emit_clear_t(os, op);
            os << "    { uint16_t _a = " << ea_expr(ins)
               << "; pce_bus_write(cpu, _a, pce_rol(cpu, pce_bus_read(cpu, _a))); } pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0x66U: case 0x76U: case 0x6EU: case 0x7EU:
            emit_clear_t(os, op);
            os << "    { uint16_t _a = " << ea_expr(ins)
               << "; pce_bus_write(cpu, _a, pce_ror(cpu, pce_bus_read(cpu, _a))); } pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;

        case 0xE6U: case 0xF6U: case 0xEEU: case 0xFEU:
            emit_clear_t(os, op);
            os << "    { uint16_t _a = " << ea_expr(ins)
               << "; uint8_t _v = (uint8_t)(pce_bus_read(cpu, _a) + 1U); pce_bus_write(cpu, _a, _v); pce_set_nz(cpu, _v); } pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
        case 0xC6U: case 0xD6U: case 0xCEU: case 0xDEU:
            emit_clear_t(os, op);
            os << "    { uint16_t _a = " << ea_expr(ins)
               << "; uint8_t _v = (uint8_t)(pce_bus_read(cpu, _a) - 1U); pce_bus_write(cpu, _a, _v); pce_set_nz(cpu, _v); } pce_add_cycles(cpu, "
               << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;

        // NOPs
        default:
            emit_clear_t(os, op);
            os << "    pce_add_cycles(cpu, " << static_cast<unsigned>(ins.cycles) << "U);\n";
            return;
    }
}

void emit_function(std::ostringstream& os, const FunctionCfg& fn) {
    // Pass 1: collect strictly referenced labels & detect tail/indirect jumps
    std::set<uint16_t> used_labels;
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

    // Pass 2: emit C99 body
    os << "void " << func_name(fn.bank, fn.entry_addr) << "(PceCpuState* cpu) {\n";
    if (has_tail_dispatch) {
        os << "    uint8_t _fn_s = cpu->s;\n";
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
        if (has_tail_dispatch && ins.opcode == 0x9AU) {  // TXS resets stack base
            os << "    _fn_s = cpu->s;\n";
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
        os << "L_tail_resume:\n"
           << "    while ((int8_t)(cpu->s - _fn_s) <= 0) {\n"
           << "        if (pce_runtime_dispatch(cpu, cpu->pc, 0x" << hex4(fn.entry_addr)
           << "U)) return;\n"
           << "    }\n"
           << "    return;\n";
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
            << "#include \"pce/cpu_state.h\"\n\n"
            << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
        for (const auto& fn : disasm.functions) {
            hdr << "void " << func_name(fn.bank, fn.entry_addr) << "(PceCpuState* cpu);\n";
        }
        hdr << "\nextern const PceRecompFuncEntry kRecompDispatchTable[];\n"
            << "extern const size_t kRecompDispatchTableCount;\n\n"
            << "#ifdef __cplusplus\n}\n#endif\n";
        ++res.files_written;
    }

    // 2. Emit C99 bodies + dispatch table
    if (opts.single_file || disasm.functions.size() <= opts.split_every) {
        std::ostringstream body;
        body << "/* Auto-generated by pcerecomp — DO NOT EDIT */\n"
             << "#include \"recomp_functions.h\"\n\n";
        for (const auto& fn : disasm.functions) {
            emit_function(body, fn);
        }
        body << "const PceRecompFuncEntry kRecompDispatchTable[] = {\n";
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
            body << "/* Auto-generated by pcerecomp (part " << part << ") */\n"
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

        std::ofstream tbl(opts.output_dir / "recomp_dispatch.c", std::ios::trunc);
        if (!tbl.is_open()) {
            res.error = "No se pudo escribir recomp_dispatch.c";
            return res;
        }
        tbl << "#include \"recomp_functions.h\"\n\n"
            << "const PceRecompFuncEntry kRecompDispatchTable[] = {\n";
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

}  // namespace dragon::pcerecomp
