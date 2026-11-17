#include "gbrecomp/emitter.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "dragon/file_io.h"

namespace dragon::gbrecomp {

namespace {

std::string format_condition(uint8_t op) {
    switch (op) {
        case 0x20:
        case 0xC2:
        case 0xC0:
        case 0xC4:  // NZ
            return "!(cpu->f & GB_FLAG_Z)";
        case 0x28:
        case 0xCA:
        case 0xC8:
        case 0xCC:  // Z
            return "(cpu->f & GB_FLAG_Z)";
        case 0x30:
        case 0xD2:
        case 0xD0:
        case 0xD4:  // NC
            return "!(cpu->f & GB_FLAG_C)";
        case 0x38:
        case 0xDA:
        case 0xD8:
        case 0xDC:  // C
            return "(cpu->f & GB_FLAG_C)";
        default:
            return "1";
    }
}

void emit_instruction(const Instruction& inst, const Function& func, const ProgramCFG& cfg,
                      std::ostringstream& out) {
    char buf[512];
    out << "        /* " << inst.mnemonic << " */\n";

    if (inst.is_cb) {
        // CB instruction
        uint8_t op = inst.opcode;
        uint8_t reg_idx = op & 0x07U;
        uint8_t bit = (op >> 3U) & 0x07U;

        const char* reg_names[8] = {"cpu->b", "cpu->c", "cpu->d", "cpu->e",
                                    "cpu->h", "cpu->l", nullptr,  "cpu->a"};
        const char* r = reg_names[reg_idx];

        if (op < 0x40) {
            // Rotates / shifts / swap
            const char* helper_names[8] = {"gb_cb_rlc", "gb_cb_rrc", "gb_cb_rl",   "gb_cb_rr",
                                           "gb_cb_sla", "gb_cb_sra", "gb_cb_swap", "gb_cb_srl"};
            const char* h = helper_names[(op >> 3U) & 0x07U];
            if (reg_idx == 6) {  // (HL)
                out << "        gb_bus_write(cpu, cpu_hl(cpu), " << h
                    << "(cpu, gb_bus_read(cpu, cpu_hl(cpu))));\n";
            } else {
                out << "        " << r << " = " << h << "(cpu, " << r << ");\n";
            }
        } else if (op < 0x80) {  // BIT
            if (reg_idx == 6) {
                out << "        gb_cb_bit(cpu, " << (int)bit
                    << ", gb_bus_read(cpu, cpu_hl(cpu)));\n";
            } else {
                out << "        gb_cb_bit(cpu, " << (int)bit << ", " << r << ");\n";
            }
        } else if (op < 0xC0) {  // RES
            if (reg_idx == 6) {
                out << "        gb_bus_write(cpu, cpu_hl(cpu), (uint8_t)(gb_bus_read(cpu, "
                       "cpu_hl(cpu)) & ~(1U << "
                    << (int)bit << ")));\n";
            } else {
                out << "        " << r << " = (uint8_t)(" << r << " & ~(1U << " << (int)bit
                    << "));\n";
            }
        } else {  // SET
            if (reg_idx == 6) {
                out << "        gb_bus_write(cpu, cpu_hl(cpu), (uint8_t)(gb_bus_read(cpu, "
                       "cpu_hl(cpu)) | (1U << "
                    << (int)bit << ")));\n";
            } else {
                out << "        " << r << " = (uint8_t)(" << r << " | (1U << " << (int)bit
                    << "));\n";
            }
        }
        out << "        cpu->cycles += " << (int)inst.cycles << ";\n";
        return;
    }

    uint8_t op = inst.opcode;

    switch (op) {
        case 0x00:  // NOP
            out << "        cpu->cycles += 4;\n";
            break;
        case 0x01:  // LD BC, d16
            std::snprintf(buf, sizeof(buf), "        cpu_set_bc(cpu, 0x%04XU);\n",
                          inst.operand_u16());
            out << buf << "        cpu->cycles += 12;\n";
            break;
        case 0x02:  // LD (BC), A
            out << "        gb_bus_write(cpu, cpu_bc(cpu), cpu->a);\n        cpu->cycles += 8;\n";
            break;
        case 0x03:  // INC BC
            out << "        cpu_set_bc(cpu, (uint16_t)(cpu_bc(cpu) + 1U));\n        cpu->cycles += "
                   "8;\n";
            break;
        case 0x04:  // INC B
            out << "        cpu->b = gb_alu_inc(cpu, cpu->b);\n        cpu->cycles += 4;\n";
            break;
        case 0x05:  // DEC B
            out << "        cpu->b = gb_alu_dec(cpu, cpu->b);\n        cpu->cycles += 4;\n";
            break;
        case 0x06:  // LD B, d8
            std::snprintf(buf, sizeof(buf), "        cpu->b = 0x%02XU;\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;
        case 0x07:  // RLCA
            out << "        { uint8_t c = (cpu->a >> 7U) & 1U; cpu->a = (uint8_t)((cpu->a << 1U) | "
                   "c); cpu->f = c ? GB_FLAG_C : 0; }\n        cpu->cycles += 4;\n";
            break;
        case 0x08:  // LD (a16), SP
            std::snprintf(buf, sizeof(buf), "        gb_bus_write16(cpu, 0x%04XU, cpu->sp);\n",
                          inst.operand_u16());
            out << buf << "        cpu->cycles += 20;\n";
            break;
        case 0x09:  // ADD HL, BC
            out << "        gb_alu_add_hl(cpu, cpu_bc(cpu));\n        cpu->cycles += 8;\n";
            break;
        case 0x0A:  // LD A, (BC)
            out << "        cpu->a = gb_bus_read(cpu, cpu_bc(cpu));\n        cpu->cycles += 8;\n";
            break;
        case 0x0B:  // DEC BC
            out << "        cpu_set_bc(cpu, (uint16_t)(cpu_bc(cpu) - 1U));\n        cpu->cycles += "
                   "8;\n";
            break;
        case 0x0C:  // INC C
            out << "        cpu->c = gb_alu_inc(cpu, cpu->c);\n        cpu->cycles += 4;\n";
            break;
        case 0x0D:  // DEC C
            out << "        cpu->c = gb_alu_dec(cpu, cpu->c);\n        cpu->cycles += 4;\n";
            break;
        case 0x0E:  // LD C, d8
            std::snprintf(buf, sizeof(buf), "        cpu->c = 0x%02XU;\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;
        case 0x0F:  // RRCA
            out << "        { uint8_t c = cpu->a & 1U; cpu->a = (uint8_t)((cpu->a >> 1U) | (c << "
                   "7U)); cpu->f = c ? GB_FLAG_C : 0; }\n        cpu->cycles += 4;\n";
            break;

        case 0x10: {  // STOP
            std::snprintf(buf, sizeof(buf),
                          "        cpu->pc = 0x%04XU;\n"
                          "        cpu->cycles += 4;\n"
                          "        gb_runtime_stop(cpu);\n"
                          "        if (cpu->halted) return;\n",
                          inst.addr + 2);
            out << buf;
            break;
        }

        case 0x11:  // LD DE, d16
            std::snprintf(buf, sizeof(buf), "        cpu_set_de(cpu, 0x%04XU);\n",
                          inst.operand_u16());
            out << buf << "        cpu->cycles += 12;\n";
            break;
        case 0x12:  // LD (DE), A
            out << "        gb_bus_write(cpu, cpu_de(cpu), cpu->a);\n        cpu->cycles += 8;\n";
            break;
        case 0x13:  // INC DE
            out << "        cpu_set_de(cpu, (uint16_t)(cpu_de(cpu) + 1U));\n        cpu->cycles += "
                   "8;\n";
            break;
        case 0x14:  // INC D
            out << "        cpu->d = gb_alu_inc(cpu, cpu->d);\n        cpu->cycles += 4;\n";
            break;
        case 0x15:  // DEC D
            out << "        cpu->d = gb_alu_dec(cpu, cpu->d);\n        cpu->cycles += 4;\n";
            break;
        case 0x16:  // LD D, d8
            std::snprintf(buf, sizeof(buf), "        cpu->d = 0x%02XU;\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;
        case 0x17:  // RLA
            out << "        { uint8_t old_c = (cpu->f & GB_FLAG_C) ? 1U : 0U; uint8_t new_c = "
                   "(cpu->a >> 7U) & 1U; cpu->a = (uint8_t)((cpu->a << 1U) | old_c); cpu->f = "
                   "new_c ? GB_FLAG_C : 0; }\n        cpu->cycles += 4;\n";
            break;
        case 0x18: {  // JR r8
            int8_t rel = static_cast<int8_t>(inst.op1);
            uint16_t target = static_cast<uint16_t>(inst.addr + 2 + rel);
            if (func.block_addrs.count(target)) {
                if (target <= inst.addr) {
                    std::snprintf(buf, sizeof(buf),
                                  "        if (cpu->cycles >= cpu->next_event) { cpu->pc = "
                                  "0x%04XU; gb_runtime_poll(cpu); "
                                  "if (cpu->halted || cpu->sp > entry_sp) return; }\n",
                                  target);
                    out << buf;
                }
                std::snprintf(buf, sizeof(buf),
                              "        cpu->cycles += 12;\n        goto L_%04X;\n", target);
            } else {
                std::snprintf(buf, sizeof(buf),
                              "        cpu->cycles += 12;\n        gb_runtime_dispatch(cpu, "
                              "0x%04XU, 0x%04XU);\n        return;\n",
                              target, inst.addr);
            }
            out << buf;
            break;
        }
        case 0x19:  // ADD HL, DE
            out << "        gb_alu_add_hl(cpu, cpu_de(cpu));\n        cpu->cycles += 8;\n";
            break;
        case 0x1A:  // LD A, (DE)
            out << "        cpu->a = gb_bus_read(cpu, cpu_de(cpu));\n        cpu->cycles += 8;\n";
            break;
        case 0x1B:  // DEC DE
            out << "        cpu_set_de(cpu, (uint16_t)(cpu_de(cpu) - 1U));\n        cpu->cycles += "
                   "8;\n";
            break;
        case 0x1C:  // INC E
            out << "        cpu->e = gb_alu_inc(cpu, cpu->e);\n        cpu->cycles += 4;\n";
            break;
        case 0x1D:  // DEC E
            out << "        cpu->e = gb_alu_dec(cpu, cpu->e);\n        cpu->cycles += 4;\n";
            break;
        case 0x1E:  // LD E, d8
            std::snprintf(buf, sizeof(buf), "        cpu->e = 0x%02XU;\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;
        case 0x1F:  // RRA
            out << "        { uint8_t old_c = (cpu->f & GB_FLAG_C) ? 1U : 0U; uint8_t new_c = "
                   "cpu->a & 1U; cpu->a = (uint8_t)((cpu->a >> 1U) | (old_c << 7U)); cpu->f = "
                   "new_c ? GB_FLAG_C : 0; }\n        cpu->cycles += 4;\n";
            break;

        case 0x20:
        case 0x28:
        case 0x30:
        case 0x38: {  // JR cc, r8
            int8_t rel = static_cast<int8_t>(inst.op1);
            uint16_t target = static_cast<uint16_t>(inst.addr + 2 + rel);
            std::string cond = format_condition(op);
            out << "        if (" << cond << ") {\n            cpu->cycles += 12;\n";
            if (func.block_addrs.count(target)) {
                if (target <= inst.addr) {
                    std::snprintf(
                        buf, sizeof(buf),
                        "            if (cpu->cycles >= cpu->next_event) { cpu->pc = 0x%04XU; "
                        "gb_runtime_poll(cpu); "
                        "if (cpu->halted || cpu->sp > entry_sp) return; }\n",
                        target);
                    out << buf;
                }
                std::snprintf(buf, sizeof(buf), "            goto L_%04X;\n", target);
            } else {
                std::snprintf(buf, sizeof(buf),
                              "            gb_runtime_dispatch(cpu, 0x%04XU, 0x%04XU);\n           "
                              " return;\n",
                              target, inst.addr);
            }
            out << buf << "        }\n        cpu->cycles += 8;\n";
            break;
        }

        case 0x21:  // LD HL, d16
            std::snprintf(buf, sizeof(buf), "        cpu_set_hl(cpu, 0x%04XU);\n",
                          inst.operand_u16());
            out << buf << "        cpu->cycles += 12;\n";
            break;
        case 0x22:  // LD (HL+), A
            out << "        gb_bus_write(cpu, cpu_hl(cpu), cpu->a);\n        cpu_set_hl(cpu, "
                   "(uint16_t)(cpu_hl(cpu) + 1U));\n        cpu->cycles += 8;\n";
            break;
        case 0x23:  // INC HL
            out << "        cpu_set_hl(cpu, (uint16_t)(cpu_hl(cpu) + 1U));\n        cpu->cycles += "
                   "8;\n";
            break;
        case 0x24:  // INC H
            out << "        cpu->h = gb_alu_inc(cpu, cpu->h);\n        cpu->cycles += 4;\n";
            break;
        case 0x25:  // DEC H
            out << "        cpu->h = gb_alu_dec(cpu, cpu->h);\n        cpu->cycles += 4;\n";
            break;
        case 0x26:  // LD H, d8
            std::snprintf(buf, sizeof(buf), "        cpu->h = 0x%02XU;\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;
        case 0x27:  // DAA
            out << "        gb_alu_daa(cpu);\n        cpu->cycles += 4;\n";
            break;
        case 0x29:  // ADD HL, HL
            out << "        gb_alu_add_hl(cpu, cpu_hl(cpu));\n        cpu->cycles += 8;\n";
            break;
        case 0x2A:  // LD A, (HL+)
            out << "        cpu->a = gb_bus_read(cpu, cpu_hl(cpu));\n        cpu_set_hl(cpu, "
                   "(uint16_t)(cpu_hl(cpu) + 1U));\n        cpu->cycles += 8;\n";
            break;
        case 0x2B:  // DEC HL
            out << "        cpu_set_hl(cpu, (uint16_t)(cpu_hl(cpu) - 1U));\n        cpu->cycles += "
                   "8;\n";
            break;
        case 0x2C:  // INC L
            out << "        cpu->l = gb_alu_inc(cpu, cpu->l);\n        cpu->cycles += 4;\n";
            break;
        case 0x2D:  // DEC L
            out << "        cpu->l = gb_alu_dec(cpu, cpu->l);\n        cpu->cycles += 4;\n";
            break;
        case 0x2E:  // LD L, d8
            std::snprintf(buf, sizeof(buf), "        cpu->l = 0x%02XU;\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;
        case 0x2F:  // CPL
            out << "        gb_alu_cpl(cpu);\n        cpu->cycles += 4;\n";
            break;

        case 0x31:  // LD SP, d16
            std::snprintf(buf, sizeof(buf), "        cpu->sp = 0x%04XU;\n", inst.operand_u16());
            out << buf << "        cpu->cycles += 12;\n";
            break;
        case 0x32:  // LD (HL-), A
            out << "        gb_bus_write(cpu, cpu_hl(cpu), cpu->a);\n        cpu_set_hl(cpu, "
                   "(uint16_t)(cpu_hl(cpu) - 1U));\n        cpu->cycles += 8;\n";
            break;
        case 0x33:  // INC SP
            out << "        cpu->sp++;\n        cpu->cycles += 8;\n";
            break;
        case 0x34:  // INC (HL)
            out << "        gb_bus_write(cpu, cpu_hl(cpu), gb_alu_inc(cpu, gb_bus_read(cpu, "
                   "cpu_hl(cpu))));\n        cpu->cycles += 12;\n";
            break;
        case 0x35:  // DEC (HL)
            out << "        gb_bus_write(cpu, cpu_hl(cpu), gb_alu_dec(cpu, gb_bus_read(cpu, "
                   "cpu_hl(cpu))));\n        cpu->cycles += 12;\n";
            break;
        case 0x36:  // LD (HL), d8
            std::snprintf(buf, sizeof(buf), "        gb_bus_write(cpu, cpu_hl(cpu), 0x%02XU);\n",
                          inst.op1);
            out << buf << "        cpu->cycles += 12;\n";
            break;
        case 0x37:  // SCF
            out << "        cpu->f = (cpu->f & GB_FLAG_Z) | GB_FLAG_C;\n        cpu->cycles += "
                   "4;\n";
            break;
        case 0x39:  // ADD HL, SP
            out << "        gb_alu_add_hl(cpu, cpu->sp);\n        cpu->cycles += 8;\n";
            break;
        case 0x3A:  // LD A, (HL-)
            out << "        cpu->a = gb_bus_read(cpu, cpu_hl(cpu));\n        cpu_set_hl(cpu, "
                   "(uint16_t)(cpu_hl(cpu) - 1U));\n        cpu->cycles += 8;\n";
            break;
        case 0x3B:  // DEC SP
            out << "        cpu->sp--;\n        cpu->cycles += 8;\n";
            break;
        case 0x3C:  // INC A
            out << "        cpu->a = gb_alu_inc(cpu, cpu->a);\n        cpu->cycles += 4;\n";
            break;
        case 0x3D:  // DEC A
            out << "        cpu->a = gb_alu_dec(cpu, cpu->a);\n        cpu->cycles += 4;\n";
            break;
        case 0x3E:  // LD A, d8
            std::snprintf(buf, sizeof(buf), "        cpu->a = 0x%02XU;\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;
        case 0x3F:  // CCF
            out << "        cpu->f = (cpu->f & GB_FLAG_Z) | ((cpu->f & GB_FLAG_C) ? 0 : "
                   "GB_FLAG_C);\n        cpu->cycles += 4;\n";
            break;

        case 0x40:
        case 0x41:
        case 0x42:
        case 0x43:
        case 0x44:
        case 0x45:
        case 0x46:
        case 0x47:
        case 0x48:
        case 0x49:
        case 0x4A:
        case 0x4B:
        case 0x4C:
        case 0x4D:
        case 0x4E:
        case 0x4F:
        case 0x50:
        case 0x51:
        case 0x52:
        case 0x53:
        case 0x54:
        case 0x55:
        case 0x56:
        case 0x57:
        case 0x58:
        case 0x59:
        case 0x5A:
        case 0x5B:
        case 0x5C:
        case 0x5D:
        case 0x5E:
        case 0x5F:
        case 0x60:
        case 0x61:
        case 0x62:
        case 0x63:
        case 0x64:
        case 0x65:
        case 0x66:
        case 0x67:
        case 0x68:
        case 0x69:
        case 0x6A:
        case 0x6B:
        case 0x6C:
        case 0x6D:
        case 0x6E:
        case 0x6F:
        case 0x70:
        case 0x71:
        case 0x72:
        case 0x73:
        case 0x74:
        case 0x75:
        case 0x77:
        case 0x78:
        case 0x79:
        case 0x7A:
        case 0x7B:
        case 0x7C:
        case 0x7D:
        case 0x7E:
        case 0x7F: {
            // LD r, r'
            const char* reg_names[8] = {"cpu->b", "cpu->c", "cpu->d", "cpu->e",
                                        "cpu->h", "cpu->l", nullptr,  "cpu->a"};
            uint8_t dst_idx = (op >> 3U) & 0x07U;
            uint8_t src_idx = op & 0x07U;
            if (dst_idx == 6) {
                out << "        gb_bus_write(cpu, cpu_hl(cpu), " << reg_names[src_idx]
                    << ");\n        cpu->cycles += 8;\n";
            } else if (src_idx == 6) {
                out << "        " << reg_names[dst_idx]
                    << " = gb_bus_read(cpu, cpu_hl(cpu));\n        cpu->cycles += 8;\n";
            } else if (dst_idx != src_idx) {
                out << "        " << reg_names[dst_idx] << " = " << reg_names[src_idx]
                    << ";\n        cpu->cycles += 4;\n";
            } else {
                out << "        cpu->cycles += 4;\n";
            }
            break;
        }

        case 0x76: {  // HALT
            std::snprintf(buf, sizeof(buf),
                          "        cpu->pc = 0x%04XU;\n"
                          "        cpu->halted = 1;\n"
                          "        cpu->cycles += 4;\n"
                          "        return;\n",
                          inst.addr + 1);
            out << buf;
            break;
        }

        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
        case 0x88:
        case 0x89:
        case 0x8A:
        case 0x8B:
        case 0x8C:
        case 0x8D:
        case 0x8E:
        case 0x8F:
        case 0x90:
        case 0x91:
        case 0x92:
        case 0x93:
        case 0x94:
        case 0x95:
        case 0x96:
        case 0x97:
        case 0x98:
        case 0x99:
        case 0x9A:
        case 0x9B:
        case 0x9C:
        case 0x9D:
        case 0x9E:
        case 0x9F:
        case 0xA0:
        case 0xA1:
        case 0xA2:
        case 0xA3:
        case 0xA4:
        case 0xA5:
        case 0xA6:
        case 0xA7:
        case 0xA8:
        case 0xA9:
        case 0xAA:
        case 0xAB:
        case 0xAC:
        case 0xAD:
        case 0xAE:
        case 0xAF:
        case 0xB0:
        case 0xB1:
        case 0xB2:
        case 0xB3:
        case 0xB4:
        case 0xB5:
        case 0xB6:
        case 0xB7:
        case 0xB8:
        case 0xB9:
        case 0xBA:
        case 0xBB:
        case 0xBC:
        case 0xBD:
        case 0xBE:
        case 0xBF: {
            // ALU r, (HL)
            const char* reg_names[8] = {"cpu->b", "cpu->c", "cpu->d", "cpu->e",
                                        "cpu->h", "cpu->l", nullptr,  "cpu->a"};
            const char* alu_helpers[8] = {"gb_alu_add", "gb_alu_adc", "gb_alu_sub", "gb_alu_sbc",
                                          "gb_alu_and", "gb_alu_xor", "gb_alu_or",  "gb_alu_cp"};
            uint8_t alu_op = (op >> 3U) & 0x07U;
            uint8_t src_idx = op & 0x07U;
            if (src_idx == 6) {
                out << "        " << alu_helpers[alu_op]
                    << "(cpu, gb_bus_read(cpu, cpu_hl(cpu)));\n        cpu->cycles += 8;\n";
            } else {
                out << "        " << alu_helpers[alu_op] << "(cpu, " << reg_names[src_idx]
                    << ");\n        cpu->cycles += 4;\n";
            }
            break;
        }

        case 0xC3: {  // JP a16
            uint16_t target = inst.operand_u16();
            if (func.block_addrs.count(target)) {
                if (target <= inst.addr) {
                    std::snprintf(buf, sizeof(buf),
                                  "        if (cpu->cycles >= cpu->next_event) { cpu->pc = "
                                  "0x%04XU; gb_runtime_poll(cpu); "
                                  "if (cpu->halted || cpu->sp > entry_sp) return; }\n",
                                  target);
                    out << buf;
                }
                std::snprintf(buf, sizeof(buf),
                              "        cpu->cycles += 16;\n        goto L_%04X;\n", target);
            } else {
                std::snprintf(buf, sizeof(buf),
                              "        cpu->cycles += 16;\n        gb_runtime_dispatch(cpu, "
                              "0x%04XU, 0x%04XU);\n        return;\n",
                              target, inst.addr);
            }
            out << buf;
            break;
        }

        case 0xC2:
        case 0xCA:
        case 0xD2:
        case 0xDA: {  // JP cc, a16
            uint16_t target = inst.operand_u16();
            std::string cond = format_condition(op);
            out << "        if (" << cond << ") {\n            cpu->cycles += 16;\n";
            if (func.block_addrs.count(target)) {
                if (target <= inst.addr) {
                    std::snprintf(
                        buf, sizeof(buf),
                        "            if (cpu->cycles >= cpu->next_event) { cpu->pc = 0x%04XU; "
                        "gb_runtime_poll(cpu); "
                        "if (cpu->halted || cpu->sp > entry_sp) return; }\n",
                        target);
                    out << buf;
                }
                std::snprintf(buf, sizeof(buf), "            goto L_%04X;\n", target);
            } else {
                std::snprintf(buf, sizeof(buf),
                              "            gb_runtime_dispatch(cpu, 0x%04XU, 0x%04XU);\n           "
                              " return;\n",
                              target, inst.addr);
            }
            out << buf << "        }\n        cpu->cycles += 12;\n";
            break;
        }

        case 0xC4:
        case 0xCC:
        case 0xD4:
        case 0xDC: {  // CALL cc, a16
            uint16_t target = inst.operand_u16();
            uint16_t ret_addr = inst.addr + 3;
            std::string cond = format_condition(op);
            out << "        if (" << cond << ") {\n";
            std::snprintf(buf, sizeof(buf),
                          "            cpu_push(cpu, 0x%04XU);\n            cpu->cycles += 24;\n",
                          ret_addr);
            out << buf;
            const Function* target_func = cfg.find_function(func.bank, target);
            if (target_func) {
                out << "            " << target_func->c_symbol() << "(cpu);\n";
            } else {
                std::snprintf(buf, sizeof(buf),
                              "            gb_runtime_dispatch(cpu, 0x%04XU, 0x%04XU);\n", target,
                              inst.addr);
                out << buf;
            }
            std::snprintf(buf, sizeof(buf),
                          "            if (cpu->halted || cpu->sp > entry_sp || cpu->pc != "
                          "0x%04XU) return;\n",
                          ret_addr);
            out << buf;
            out << "        } else {\n";
            out << "            cpu->cycles += 12;\n";
            out << "        }\n";
            break;
        }

        case 0xCD: {  // CALL a16
            uint16_t target = inst.operand_u16();
            uint16_t ret_addr = inst.addr + 3;
            std::snprintf(buf, sizeof(buf),
                          "        cpu_push(cpu, 0x%04XU);\n        cpu->cycles += 24;\n",
                          ret_addr);
            out << buf;

            const Function* target_func = cfg.find_function(func.bank, target);
            if (target_func) {
                out << "        " << target_func->c_symbol() << "(cpu);\n";
            } else {
                std::snprintf(buf, sizeof(buf),
                              "        gb_runtime_dispatch(cpu, 0x%04XU, 0x%04XU);\n", target,
                              inst.addr);
                out << buf;
            }
            std::snprintf(
                buf, sizeof(buf),
                "        if (cpu->halted || cpu->sp > entry_sp || cpu->pc != 0x%04XU) return;\n",
                ret_addr);
            out << buf;
            break;
        }

        case 0xC9:  // RET
            out << "        cpu->pc = cpu_pop(cpu);\n        cpu->cycles += 16;\n        return;\n";
            break;
        case 0xD9:  // RETI
            out << "        cpu->pc = cpu_pop(cpu);\n        cpu->ime = 1;\n        cpu->cycles += "
                   "16;\n        return;\n";
            break;

        case 0xC0:
        case 0xC8:
        case 0xD0:
        case 0xD8: {  // RET cc
            std::string cond = format_condition(op);
            out << "        if (" << cond
                << ") {\n            cpu->pc = cpu_pop(cpu);\n            cpu->cycles += 20;\n     "
                   "       return;\n        }\n        cpu->cycles += 8;\n";
            break;
        }

        case 0xC5:
            out << "        cpu_push(cpu, cpu_bc(cpu));\n        cpu->cycles += 16;\n";
            break;
        case 0xC1:
            out << "        cpu_set_bc(cpu, cpu_pop(cpu));\n        cpu->cycles += 12;\n";
            break;
        case 0xD5:
            out << "        cpu_push(cpu, cpu_de(cpu));\n        cpu->cycles += 16;\n";
            break;
        case 0xD1:
            out << "        cpu_set_de(cpu, cpu_pop(cpu));\n        cpu->cycles += 12;\n";
            break;
        case 0xE5:
            out << "        cpu_push(cpu, cpu_hl(cpu));\n        cpu->cycles += 16;\n";
            break;
        case 0xE1:
            out << "        cpu_set_hl(cpu, cpu_pop(cpu));\n        cpu->cycles += 12;\n";
            break;
        case 0xF5:
            out << "        cpu_push(cpu, cpu_af(cpu));\n        cpu->cycles += 16;\n";
            break;
        case 0xF1:
            out << "        cpu_set_af(cpu, cpu_pop(cpu));\n        cpu->cycles += 12;\n";
            break;

        case 0xE0:  // LDH (a8), A
            std::snprintf(buf, sizeof(buf), "        gb_bus_write(cpu, 0xFF%02XU, cpu->a);\n",
                          inst.op1);
            out << buf << "        cpu->cycles += 12;\n";
            break;
        case 0xF0:  // LDH A, (a8)
            std::snprintf(buf, sizeof(buf), "        cpu->a = gb_bus_read(cpu, 0xFF%02XU);\n",
                          inst.op1);
            out << buf << "        cpu->cycles += 12;\n";
            break;
        case 0xEA:  // LD (a16), A
            std::snprintf(buf, sizeof(buf), "        gb_bus_write(cpu, 0x%04XU, cpu->a);\n",
                          inst.operand_u16());
            out << buf << "        cpu->cycles += 16;\n";
            break;
        case 0xFA:  // LD A, (a16)
            std::snprintf(buf, sizeof(buf), "        cpu->a = gb_bus_read(cpu, 0x%04XU);\n",
                          inst.operand_u16());
            out << buf << "        cpu->cycles += 16;\n";
            break;

        case 0xC6:  // ADD A, d8
            std::snprintf(buf, sizeof(buf), "        gb_alu_add(cpu, 0x%02XU);\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;
        case 0xCE:  // ADC A, d8
            std::snprintf(buf, sizeof(buf), "        gb_alu_adc(cpu, 0x%02XU);\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;
        case 0xD6:  // SUB d8
            std::snprintf(buf, sizeof(buf), "        gb_alu_sub(cpu, 0x%02XU);\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;
        case 0xDE:  // SBC A, d8
            std::snprintf(buf, sizeof(buf), "        gb_alu_sbc(cpu, 0x%02XU);\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;
        case 0xE6:  // AND d8
            std::snprintf(buf, sizeof(buf), "        gb_alu_and(cpu, 0x%02XU);\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;
        case 0xEE:  // XOR d8
            std::snprintf(buf, sizeof(buf), "        gb_alu_xor(cpu, 0x%02XU);\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;
        case 0xF6:  // OR d8
            std::snprintf(buf, sizeof(buf), "        gb_alu_or(cpu, 0x%02XU);\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;
        case 0xFE:  // CP d8
            std::snprintf(buf, sizeof(buf), "        gb_alu_cp(cpu, 0x%02XU);\n", inst.op1);
            out << buf << "        cpu->cycles += 8;\n";
            break;

        case 0xC7:
        case 0xCF:
        case 0xD7:
        case 0xDF:
        case 0xE7:
        case 0xEF:
        case 0xF7:
        case 0xFF: {  // RST n
            uint16_t target = op & 0x38U;
            uint16_t ret_addr = static_cast<uint16_t>(inst.addr + 1);
            std::snprintf(buf, sizeof(buf),
                          "        cpu_push(cpu, 0x%04XU);\n        cpu->cycles += 16;\n",
                          ret_addr);
            out << buf;
            const Function* target_func = cfg.find_function(0, target);
            if (target_func) {
                out << "        " << target_func->c_symbol() << "(cpu);\n";
            } else {
                std::snprintf(buf, sizeof(buf),
                              "        gb_runtime_dispatch(cpu, 0x%04XU, 0x%04XU);\n", target,
                              inst.addr);
                out << buf;
            }
            std::snprintf(
                buf, sizeof(buf),
                "        if (cpu->halted || cpu->sp > entry_sp || cpu->pc != 0x%04XU) return;\n",
                ret_addr);
            out << buf;
            break;
        }

        case 0xE2:  // LD (C), A
            out << "        gb_bus_write(cpu, (uint16_t)(0xFF00U + cpu->c), cpu->a);\n        "
                   "cpu->cycles += 8;\n";
            break;
        case 0xF2:  // LD A, (C)
            out << "        cpu->a = gb_bus_read(cpu, (uint16_t)(0xFF00U + cpu->c));\n        "
                   "cpu->cycles += 8;\n";
            break;

        case 0xE8: {  // ADD SP, r8
            int8_t rel = static_cast<int8_t>(inst.op1);
            std::snprintf(buf, sizeof(buf),
                          "        { int rel = (int8_t)0x%02XU;\n"
                          "          cpu->f = (((cpu->sp & 0x0FU) + (rel & 0x0FU)) > 0x0FU ? "
                          "GB_FLAG_H : 0) |\n"
                          "                   (((cpu->sp & 0xFFU) + (rel & 0xFFU)) > 0xFFU ? "
                          "GB_FLAG_C : 0);\n"
                          "          cpu->sp = (uint16_t)(cpu->sp + rel); }\n"
                          "        cpu->cycles += 16;\n",
                          static_cast<uint8_t>(rel));
            out << buf;
            break;
        }

        case 0xF8: {  // LD HL, SP+r8
            int8_t rel = static_cast<int8_t>(inst.op1);
            std::snprintf(buf, sizeof(buf),
                          "        { int rel = (int8_t)0x%02XU;\n"
                          "          cpu->f = (((cpu->sp & 0x0FU) + (rel & 0x0FU)) > 0x0FU ? "
                          "GB_FLAG_H : 0) |\n"
                          "                   (((cpu->sp & 0xFFU) + (rel & 0xFFU)) > 0xFFU ? "
                          "GB_FLAG_C : 0);\n"
                          "          cpu_set_hl(cpu, (uint16_t)(cpu->sp + rel)); }\n"
                          "        cpu->cycles += 12;\n",
                          static_cast<uint8_t>(rel));
            out << buf;
            break;
        }

        case 0xE9: {  // JP (HL)
            out << "        cpu->cycles += 4;\n"
                   "        cpu->pc = cpu_hl(cpu);\n"
                   "        return;\n";
            break;
        }

        case 0xF3:  // DI
            out << "        cpu->ime = 0;\n        cpu->cycles += 4;\n";
            break;

        case 0xFB:  // EI
            out << "        cpu->ime = 1;\n        cpu->cycles += 4;\n";
            break;

        case 0xF9:  // LD SP, HL
            out << "        cpu->sp = cpu_hl(cpu);\n        cpu->cycles += 8;\n";
            break;

        default:
            // For any other opcode, fall back to interpreter step at runtime
            std::snprintf(buf, sizeof(buf),
                          "        cpu->pc = 0x%04XU;\n        dragon_gb_step_fallback(cpu);\n",
                          inst.addr);
            out << buf;
            break;
    }
}

}  // namespace

GeneratedFiles emit_c99(const ProgramCFG& cfg, const GameConfig& config,
                        const EmitOptions& options) {
    (void)config;
    GeneratedFiles files;

    // 1. Header content
    std::ostringstream hdr;
    hdr << "#pragma once\n\n";
    hdr << "#include <stddef.h>\n";
    hdr << "#include \"gb/cpu_state.h\"\n\n";
    hdr << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";

    for (const auto& [bank, bank_cfg] : cfg.banks) {
        for (const auto& [addr, func] : bank_cfg.functions) {
            hdr << "void " << func.c_symbol() << "(CpuState* cpu);\n";
        }
    }

    hdr << "\nextern const RecompFuncEntry kRecompDispatchTable[];\n";
    hdr << "extern const size_t kRecompDispatchTableCount;\n\n";
    hdr << "#ifdef __cplusplus\n}\n#endif\n";
    files.header_content = hdr.str();

    // 2. Function chunks
    std::ostringstream src;
    src << "#include \"recomp_functions.h\"\n\n";
    src << "static inline void dragon_gb_step_fallback(CpuState* cpu) {\n";
    src << "    gb_runtime_step(cpu);\n";
    src << "}\n\n";

    size_t func_count = 0;
    for (const auto& [bank, bank_cfg] : cfg.banks) {
        for (const auto& [addr, func] : bank_cfg.functions) {
            src << "void " << func.c_symbol() << "(CpuState* cpu) {\n";
            src << "    uint16_t const entry_sp = cpu->sp;\n";
            char ebuf[64];
            std::snprintf(ebuf, sizeof(ebuf), "    cpu->pc = 0x%04XU;\n", func.entry_addr);
            src << ebuf;
            src << "    if (cpu->cycles >= cpu->next_event) {\n";
            src << "        gb_runtime_poll(cpu);\n";
            src << "        if (cpu->halted || cpu->sp > entry_sp) return;\n";
            src << "    }\n";
            std::snprintf(ebuf, sizeof(ebuf), "    goto L_%04X;\n", func.entry_addr);
            src << ebuf;

            for (uint16_t b_addr : func.block_addrs) {
                auto b_it = bank_cfg.blocks.find(b_addr);
                if (b_it == bank_cfg.blocks.end())
                    continue;

                char lbuf[256];
                std::snprintf(lbuf, sizeof(lbuf), "    L_%04X:\n", b_addr);
                src << lbuf;

                for (const auto& inst : b_it->second.instructions) {
                    emit_instruction(inst, func, cfg, src);
                }

                if (!b_it->second.instructions.empty()) {
                    const Instruction& last = b_it->second.instructions.back();
                    const bool is_uncond =
                        !last.is_cb &&
                        (last.opcode == 0xC3 || last.opcode == 0x18 || last.opcode == 0xE9 ||
                         last.opcode == 0xC9 || last.opcode == 0xD9 || last.opcode == 0x76);
                    if (!is_uncond) {
                        uint16_t target = b_it->second.end_addr;
                        if (func.block_addrs.count(target)) {
                            if (target <= last.addr) {
                                std::snprintf(lbuf, sizeof(lbuf),
                                              "        if (cpu->cycles >= cpu->next_event) { "
                                              "cpu->pc = 0x%04XU; "
                                              "gb_runtime_poll(cpu); if (cpu->halted || cpu->sp > "
                                              "entry_sp) "
                                              "return; }\n",
                                              target);
                                src << lbuf;
                            }
                            std::snprintf(lbuf, sizeof(lbuf), "        goto L_%04X;\n", target);
                            src << lbuf;
                        } else {
                            std::snprintf(lbuf, sizeof(lbuf),
                                          "        gb_runtime_dispatch(cpu, 0x%04XU, 0x%04XU);\n"
                                          "        return;\n",
                                          target, last.addr);
                            src << lbuf;
                        }
                    }
                }
            }

            src << "}\n\n";
            func_count++;

            if (func_count >= options.split_every) {
                files.source_chunks.push_back(src.str());
                src.str("");
                src.clear();
                src << "#include \"recomp_functions.h\"\n\n";
                src << "static inline void dragon_gb_step_fallback(CpuState* cpu) {\n";
                src << "    gb_runtime_step(cpu);\n";
                src << "}\n\n";
                func_count = 0;
            }
        }
    }

    if (!src.str().empty()) {
        files.source_chunks.push_back(src.str());
    }

    // 3. Dispatch source
    std::ostringstream disp;
    disp << "#include \"recomp_functions.h\"\n\n";
    disp << "const RecompFuncEntry kRecompDispatchTable[] = {\n";

    size_t count = 0;
    for (const auto& [bank, bank_cfg] : cfg.banks) {
        for (const auto& [addr, func] : bank_cfg.functions) {
            char dbuf[128];
            std::snprintf(dbuf, sizeof(dbuf), "    { 0x%04XU, %uU, %s },\n", addr, bank,
                          func.c_symbol().c_str());
            disp << dbuf;
            count++;
        }
    }

    disp << "};\n\n";
    disp << "const size_t kRecompDispatchTableCount = " << count << ";\n";
    files.dispatch_source = disp.str();

    return files;
}

std::vector<std::filesystem::path> write_generated_output(const ProgramCFG& cfg,
                                                          const GameConfig& config,
                                                          const std::filesystem::path& output_dir,
                                                          const EmitOptions& options) {
    std::vector<std::filesystem::path> paths;
    std::filesystem::create_directories(output_dir);

    // Clean any stale generated files from previous runs
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(output_dir, ec)) {
        if (entry.is_regular_file()) {
            const auto fname = entry.path().filename().string();
            if (fname.rfind("recomp_funcs_", 0) == 0 || fname == "recomp_dispatch.c" ||
                fname == "recomp_functions.h") {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    GeneratedFiles files = emit_c99(cfg, config, options);

    auto hdr_path = output_dir / "recomp_functions.h";
    dragon::write_text_file(hdr_path, files.header_content);
    paths.push_back(hdr_path);

    for (size_t i = 0; i < files.source_chunks.size(); ++i) {
        char sname[64];
        std::snprintf(sname, sizeof(sname), "recomp_funcs_%zu.c", i);
        auto src_path = output_dir / sname;
        dragon::write_text_file(src_path, files.source_chunks[i]);
        paths.push_back(src_path);
    }

    auto disp_path = output_dir / "recomp_dispatch.c";
    dragon::write_text_file(disp_path, files.dispatch_source);
    paths.push_back(disp_path);

    return paths;
}

}  // namespace dragon::gbrecomp
