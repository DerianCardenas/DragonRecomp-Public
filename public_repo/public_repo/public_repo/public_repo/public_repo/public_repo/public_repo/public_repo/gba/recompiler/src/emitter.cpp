#include "gbarecomp/emitter.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace dragon::gbarecomp {

namespace {

std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08XU", v);
    return buf;
}

std::string label_name(uint32_t addr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "L_%08X", addr);
    return buf;
}

std::string reg_read(uint32_t r, uint32_t pc_val) {
    if (r == 15U) {
        return hex32(pc_val);
    }
    return "cpu->r[" + std::to_string(r) + "]";
}

void emit_arm_instruction(std::ostringstream& os, const Instruction& instr, const Function& func,
                          const ProgramCFG& cfg) {
    const uint32_t raw = instr.raw;
    const uint32_t pc = instr.addr;
    const uint32_t pc8 = pc + 8U;
    const uint32_t next_pc = pc + 4U;
    const uint8_t cond = static_cast<uint8_t>((raw >> 28) & 0x0FU);

    os << "    cpu->r[15] = " << hex32(next_pc) << ";\n";
    os << "    cpu->cycles += 1U;\n";

    if (cond == 0x0FU) {
        return;
    }

    std::string indent = "    ";
    if (cond != 0x0EU) {
        os << "    if (gba_check_cond(cpu, " << static_cast<unsigned>(cond) << "U)) {\n";
        indent = "        ";
    }

    // 1. Branch and Exchange (BX Rn)
    if ((raw & 0x0FFFFFF0U) == 0x012FFF10U) {
        const uint32_t rn = raw & 0x0FU;
        os << indent << "{\n";
        os << indent << "    uint32_t target = " << reg_read(rn, pc8) << ";\n";
        os << indent << "    cpu->cycles += 2U;\n";
        os << indent << "    gba_bx(cpu, target);\n";
        if (rn == 14U) {
            os << indent << "    return;\n";
        } else {
            os << indent << "    goto dispatch_pc;\n";
        }
        os << indent << "}\n";
    }
    // 2. Software Interrupt (SWI)
    else if ((raw & 0x0F000000U) == 0x0F000000U) {
        const uint8_t swi_num = static_cast<uint8_t>((raw >> 16) & 0xFFU);
        os << indent << "gba_swi(cpu, " << static_cast<unsigned>(swi_num) << "U);\n";
        if (swi_num == 0x00U) {
            os << indent << "return;\n";
        } else {
            os << indent << "gba_runtime_poll(cpu);\n";
            os << indent << "if (cpu->halted) { return; }\n";
        }
    }
    // 3. Branch / Branch with Link (B / BL)
    else if ((raw & 0x0E000000U) == 0x0A000000U) {
        const bool link = (raw & (1U << 24)) != 0U;
        int32_t offset = static_cast<int32_t>(raw & 0x00FFFFFFU);
        if ((offset & 0x00800000) != 0) {
            offset |= static_cast<int32_t>(0xFF000000U);
        }
        const uint32_t target = (pc8 + static_cast<uint32_t>(offset * 4)) & ~3U;
        os << indent << "cpu->cycles += 2U;\n";
        if (link) {
            os << indent << "cpu->r[14] = " << hex32(next_pc) << ";\n";
            os << indent << "cpu->r[15] = " << hex32(target) << ";\n";
            if (const Function* callee = cfg.find_function(target, false)) {
                os << indent << callee->c_symbol() << "(cpu);\n";
            } else {
                os << indent << "gba_runtime_dispatch(cpu, " << hex32(target) << ", " << hex32(pc)
                   << ");\n";
            }
            os << indent << "if (cpu->halted) { return; }\n";
            os << indent << "if ((cpu->r[15] & ~3U) != " << hex32(next_pc)
               << " || cpu->thumb != 0U) { goto dispatch_pc; }\n";
        } else {
            os << indent << "cpu->r[15] = " << hex32(target) << ";\n";
            if (func.block_addrs.contains(target)) {
                os << indent << "gba_runtime_poll(cpu);\n";
                os << indent << "if (cpu->halted) { return; }\n";
                os << indent << "goto " << label_name(target) << ";\n";
            } else if (const Function* callee = cfg.find_function(target, false)) {
                os << indent << callee->c_symbol() << "(cpu);\n";
                os << indent << "return;\n";
            } else {
                os << indent << "goto dispatch_pc;\n";
            }
        }
    }
    // 4. Multiply / Multiply-Accumulate (MUL / MLA)
    else if ((raw & 0x0FC000F0U) == 0x00000090U) {
        const bool accum = (raw & (1U << 21)) != 0U;
        const bool set_flags = (raw & (1U << 20)) != 0U;
        const uint32_t rd = (raw >> 16) & 0x0FU;
        const uint32_t rn = (raw >> 12) & 0x0FU;
        const uint32_t rs = (raw >> 8) & 0x0FU;
        const uint32_t rm = raw & 0x0FU;
        os << indent << "{\n";
        os << indent << "    uint32_t res = " << reg_read(rm, pc8) << " * " << reg_read(rs, pc8);
        if (accum) {
            os << " + " << reg_read(rn, pc8);
        }
        os << ";\n";
        if (rd != 15U) {
            os << indent << "    cpu->r[" << rd << "] = res;\n";
        }
        if (set_flags) {
            os << indent << "    gba_set_nz(cpu, res);\n";
        }
        os << indent << "    cpu->cycles += 2U;\n";
        os << indent << "}\n";
    }
    // 5. Multiply Long (UMULL, UMLAL, SMULL, SMLAL)
    else if ((raw & 0x0F8000F0U) == 0x00800090U) {
        const bool is_signed = (raw & (1U << 22)) != 0U;
        const bool accum = (raw & (1U << 21)) != 0U;
        const bool set_flags = (raw & (1U << 20)) != 0U;
        const uint32_t rd_hi = (raw >> 16) & 0x0FU;
        const uint32_t rd_lo = (raw >> 12) & 0x0FU;
        const uint32_t rs = (raw >> 8) & 0x0FU;
        const uint32_t rm = raw & 0x0FU;
        os << indent << "{\n";
        if (is_signed) {
            os << indent << "    int64_t prod = (int64_t)(int32_t)" << reg_read(rm, pc8)
               << " * (int64_t)(int32_t)" << reg_read(rs, pc8) << ";\n";
            if (accum) {
                os << indent << "    uint64_t acc = ((uint64_t)" << reg_read(rd_hi, pc8)
                   << " << 32) | (uint64_t)" << reg_read(rd_lo, pc8) << ";\n";
                os << indent << "    prod += (int64_t)acc;\n";
            }
            os << indent << "    uint64_t ures = (uint64_t)prod;\n";
        } else {
            os << indent << "    uint64_t ures = (uint64_t)" << reg_read(rm, pc8) << " * (uint64_t)"
               << reg_read(rs, pc8) << ";\n";
            if (accum) {
                os << indent << "    uint64_t acc = ((uint64_t)" << reg_read(rd_hi, pc8)
                   << " << 32) | (uint64_t)" << reg_read(rd_lo, pc8) << ";\n";
                os << indent << "    ures += acc;\n";
            }
        }
        if (rd_lo != 15U) {
            os << indent << "    cpu->r[" << rd_lo << "] = (uint32_t)ures;\n";
        }
        if (rd_hi != 15U) {
            os << indent << "    cpu->r[" << rd_hi << "] = (uint32_t)(ures >> 32);\n";
        }
        if (set_flags) {
            os << indent << "    cpu->n = (uint8_t)((ures >> 63U) & 1U);\n";
            os << indent << "    cpu->z = (ures == 0ULL) ? 1U : 0U;\n";
        }
        os << indent << "    cpu->cycles += 3U;\n";
        os << indent << "}\n";
    }
    // 6. Single Data Swap (SWP / SWPB)
    else if ((raw & 0x0FB00FF0U) == 0x01000090U) {
        const bool byte_swap = (raw & (1U << 22)) != 0U;
        const uint32_t rn = (raw >> 16) & 0x0FU;
        const uint32_t rd = (raw >> 12) & 0x0FU;
        const uint32_t rm = raw & 0x0FU;
        os << indent << "{\n";
        os << indent << "    uint32_t addr = " << reg_read(rn, pc8) << ";\n";
        os << indent << "    uint32_t src = " << reg_read(rm, pc8) << ";\n";
        if (byte_swap) {
            os << indent << "    uint8_t old_val = gba_bus_read8(cpu, addr);\n";
            os << indent << "    gba_bus_write8(cpu, addr, (uint8_t)src);\n";
            if (rd != 15U) {
                os << indent << "    cpu->r[" << rd << "] = old_val;\n";
            }
        } else {
            os << indent << "    uint32_t old_val = gba_ldr32(cpu, addr);\n";
            os << indent << "    gba_bus_write32(cpu, addr & ~3U, src);\n";
            if (rd != 15U) {
                os << indent << "    cpu->r[" << rd << "] = old_val;\n";
            }
        }
        os << indent << "    cpu->cycles += 3U;\n";
        os << indent << "}\n";
    }
    // 7. MRS (transfer PSR to register)
    else if ((raw & 0x0FBF0FFFU) == 0x010F0000U) {
        const bool use_spsr = (raw & (1U << 22)) != 0U;
        const uint32_t rd = (raw >> 12) & 0x0FU;
        if (rd != 15U) {
            if (use_spsr) {
                os << indent << "cpu->r[" << rd << "] = cpu->spsr;\n";
            } else {
                os << indent << "cpu->r[" << rd << "] = gba_pack_cpsr(cpu);\n";
            }
        }
    }
    // 8. MSR (transfer register/immediate to PSR)
    else if ((raw & 0x0DB0F000U) == 0x0120F000U) {
        const bool is_imm = (raw & (1U << 25)) != 0U;
        const bool use_spsr = (raw & (1U << 22)) != 0U;
        const uint32_t field_mask = (raw >> 16) & 0x0FU;
        uint32_t write_mask = 0;
        if (field_mask & 1U)
            write_mask |= 0x000000FFU;
        if (field_mask & 2U)
            write_mask |= 0x0000FF00U;
        if (field_mask & 4U)
            write_mask |= 0x00FF0000U;
        if (field_mask & 8U)
            write_mask |= 0xFF000000U;

        std::string operand_str;
        if (is_imm) {
            const uint32_t imm8 = raw & 0xFFU;
            const uint32_t rot = ((raw >> 8) & 0x0FU) * 2U;
            const uint32_t val = (rot == 0U) ? imm8 : ((imm8 >> rot) | (imm8 << (32U - rot)));
            operand_str = hex32(val);
        } else {
            operand_str = reg_read(raw & 0x0FU, pc8);
        }

        os << indent << "{\n";
        os << indent << "    uint32_t op_val = " << operand_str << ";\n";
        if (use_spsr) {
            os << indent << "    cpu->spsr = (cpu->spsr & ~" << hex32(write_mask)
               << ") | (op_val & " << hex32(write_mask) << ");\n";
        } else {
            os << indent << "    uint32_t cur_psr = gba_pack_cpsr(cpu);\n";
            os << indent << "    uint32_t new_psr = (cur_psr & ~" << hex32(write_mask)
               << ") | (op_val & " << hex32(write_mask) << ");\n";
            os << indent << "    gba_unpack_cpsr(cpu, new_psr);\n";
        }
        os << indent << "}\n";
    }
    // 9. Halfword and Signed Data Transfer (LDRH, STRH, LDRSB, LDRSH)
    else if ((raw & 0x0E000090U) == 0x00000090U && (raw & 0x00000060U) != 0U) {
        const bool pre = (raw & (1U << 24)) != 0U;
        const bool up = (raw & (1U << 23)) != 0U;
        const bool imm_off = (raw & (1U << 22)) != 0U;
        const bool writeback = (raw & (1U << 21)) != 0U;
        const bool load = (raw & (1U << 20)) != 0U;
        const uint32_t rn = (raw >> 16) & 0x0FU;
        const uint32_t rd = (raw >> 12) & 0x0FU;
        const uint32_t sh = (raw >> 5) & 0x03U;

        std::string off_expr;
        if (imm_off) {
            const uint32_t off8 = ((raw >> 4) & 0xF0U) | (raw & 0x0FU);
            off_expr = hex32(off8);
        } else {
            off_expr = reg_read(raw & 0x0FU, pc8);
        }

        os << indent << "{\n";
        os << indent << "    uint32_t base = " << reg_read(rn, pc8) << ";\n";
        os << indent << "    uint32_t offset = " << off_expr << ";\n";
        os << indent << "    uint32_t mod_addr = base " << (up ? "+ " : "- ") << "offset;\n";
        os << indent << "    uint32_t addr = " << (pre ? "mod_addr" : "base") << ";\n";
        if (load) {
            os << indent << "    uint32_t val = ";
            if (sh == 1U) {
                os << "gba_ldrh16(cpu, addr);\n";
            } else if (sh == 2U) {
                os << "gba_ldrsb8(cpu, addr);\n";
            } else {
                os << "gba_ldrsh16(cpu, addr);\n";
            }
            if ((!pre || writeback) && rn != 15U && rn != rd) {
                os << indent << "    cpu->r[" << rn << "] = mod_addr;\n";
            }
            if (rd == 15U) {
                os << indent << "    cpu->r[15] = val & ~3U;\n";
                os << indent << "    goto dispatch_pc;\n";
            } else {
                os << indent << "    cpu->r[" << rd << "] = val;\n";
            }
        } else {
            if (sh == 1U) {
                os << indent << "    gba_bus_write16(cpu, addr & ~1U, (uint16_t)"
                   << reg_read(rd, pc + 12U) << ");\n";
            }
            if ((!pre || writeback) && rn != 15U) {
                os << indent << "    cpu->r[" << rn << "] = mod_addr;\n";
            }
        }
        os << indent << "    cpu->cycles += 2U;\n";
        os << indent << "}\n";
    }
    // 10. Data Processing (ALU)
    else if ((raw & 0x0C000000U) == 0x00000000U) {
        const bool is_imm = (raw & (1U << 25)) != 0U;
        const uint32_t op = (raw >> 21) & 0x0FU;
        const bool set_flags = (raw & (1U << 20)) != 0U;
        const uint32_t rn = (raw >> 16) & 0x0FU;
        const uint32_t rd = (raw >> 12) & 0x0FU;
        const bool reg_shift = !is_imm && ((raw & (1U << 4)) != 0U);
        const uint32_t pc_for_reg = reg_shift ? (pc + 12U) : pc8;

        os << indent << "{\n";
        os << indent << "    uint8_t carry_out = cpu->c;\n";
        if (is_imm) {
            const uint32_t imm8 = raw & 0xFFU;
            const uint32_t rot = ((raw >> 8) & 0x0FU) * 2U;
            const uint32_t val = (rot == 0U) ? imm8 : ((imm8 >> rot) | (imm8 << (32U - rot)));
            os << indent << "    uint32_t op2 = " << hex32(val) << ";\n";
            if (rot != 0U) {
                os << indent << "    carry_out = " << ((val >> 31) & 1U) << "U;\n";
            }
        } else {
            const uint32_t rm = raw & 0x0FU;
            const uint32_t shift_type = (raw >> 5) & 0x03U;
            if (!reg_shift) {
                const uint32_t shift_imm = (raw >> 7) & 0x1FU;
                os << indent << "    uint32_t op2 = gba_shift_imm(" << reg_read(rm, pc_for_reg)
                   << ", " << shift_type << "U, " << shift_imm << "U, &carry_out);\n";
            } else {
                const uint32_t rs = (raw >> 8) & 0x0FU;
                os << indent << "    uint32_t op2 = gba_shift_reg(" << reg_read(rm, pc_for_reg)
                   << ", " << shift_type << "U, (uint8_t)(" << reg_read(rs, pc8)
                   << " & 0xFFU), &carry_out);\n";
            }
        }

        os << indent << "    uint32_t op1 = " << reg_read(rn, pc_for_reg) << ";\n";
        os << indent << "    uint32_t res = 0;\n";

        const int sf = (set_flags && rd != 15U) ? 1 : 0;
        bool write_rd = true;
        bool is_logic = false;
        switch (op) {
            case 0x00:  // AND
                os << indent << "    res = op1 & op2;\n";
                is_logic = true;
                break;
            case 0x01:  // EOR
                os << indent << "    res = op1 ^ op2;\n";
                is_logic = true;
                break;
            case 0x02:  // SUB
                os << indent << "    res = gba_sub(cpu, op1, op2, " << sf << ");\n";
                break;
            case 0x03:  // RSB
                os << indent << "    res = gba_sub(cpu, op2, op1, " << sf << ");\n";
                break;
            case 0x04:  // ADD
                os << indent << "    res = gba_add(cpu, op1, op2, " << sf << ");\n";
                break;
            case 0x05:  // ADC
                os << indent << "    res = gba_adc(cpu, op1, op2, " << sf << ");\n";
                break;
            case 0x06:  // SBC
                os << indent << "    res = gba_sbc(cpu, op1, op2, " << sf << ");\n";
                break;
            case 0x07:  // RSC
                os << indent << "    res = gba_sbc(cpu, op2, op1, " << sf << ");\n";
                break;
            case 0x08:  // TST
                os << indent << "    res = op1 & op2;\n";
                write_rd = false;
                is_logic = true;
                break;
            case 0x09:  // TEQ
                os << indent << "    res = op1 ^ op2;\n";
                write_rd = false;
                is_logic = true;
                break;
            case 0x0A:  // CMP
                os << indent << "    (void)gba_sub(cpu, op1, op2, 1);\n";
                write_rd = false;
                break;
            case 0x0B:  // CMN
                os << indent << "    (void)gba_add(cpu, op1, op2, 1);\n";
                write_rd = false;
                break;
            case 0x0C:  // ORR
                os << indent << "    res = op1 | op2;\n";
                is_logic = true;
                break;
            case 0x0D:  // MOV
                os << indent << "    res = op2;\n";
                is_logic = true;
                break;
            case 0x0E:  // BIC
                os << indent << "    res = op1 & ~op2;\n";
                is_logic = true;
                break;
            case 0x0F:  // MVN
                os << indent << "    res = ~op2;\n";
                is_logic = true;
                break;
        }

        if (set_flags && is_logic && rd != 15U) {
            os << indent << "    gba_set_nz(cpu, res);\n";
            os << indent << "    cpu->c = carry_out;\n";
        }

        if (write_rd) {
            if (rd == 15U) {
                if (set_flags) {
                    os << indent << "    gba_unpack_cpsr(cpu, cpu->spsr);\n";
                }
                os << indent << "    cpu->r[15] = res & (cpu->thumb ? ~1U : ~3U);\n";
                const bool is_mov_pc_lr = (op == 0x0DU) && !is_imm &&
                                          ((raw & 0x0FF0U) == 0x0000U) && ((raw & 0x0FU) == 14U);
                const bool is_subs_pc_lr = (op == 0x02U) && set_flags && (rn == 14U);
                if (is_mov_pc_lr || is_subs_pc_lr) {
                    os << indent << "    return;\n";
                } else {
                    os << indent << "    goto dispatch_pc;\n";
                }
            } else {
                os << indent << "    cpu->r[" << rd << "] = res;\n";
            }
        }
        os << indent << "    (void)op1; (void)res; (void)carry_out;\n";
        os << indent << "}\n";
    }
    // 11. Single Data Transfer (LDR, STR, LDRB, STRB)
    else if ((raw & 0x0C000000U) == 0x04000000U) {
        const bool reg_off = (raw & (1U << 25)) != 0U;
        const bool pre = (raw & (1U << 24)) != 0U;
        const bool up = (raw & (1U << 23)) != 0U;
        const bool byte_transfer = (raw & (1U << 22)) != 0U;
        const bool writeback = (raw & (1U << 21)) != 0U;
        const bool load = (raw & (1U << 20)) != 0U;
        const uint32_t rn = (raw >> 16) & 0x0FU;
        const uint32_t rd = (raw >> 12) & 0x0FU;

        os << indent << "{\n";
        os << indent << "    uint32_t base = " << reg_read(rn, pc8) << ";\n";
        if (!reg_off) {
            os << indent << "    uint32_t offset = " << hex32(raw & 0x0FFFU) << ";\n";
        } else {
            const uint32_t rm = raw & 0x0FU;
            const uint32_t shift_type = (raw >> 5) & 0x03U;
            const uint32_t shift_imm = (raw >> 7) & 0x1FU;
            os << indent << "    uint8_t dummy_c = cpu->c;\n";
            os << indent << "    uint32_t offset = gba_shift_imm(" << reg_read(rm, pc8) << ", "
               << shift_type << "U, " << shift_imm << "U, &dummy_c);\n";
        }
        os << indent << "    uint32_t mod_addr = base " << (up ? "+ " : "- ") << "offset;\n";
        os << indent << "    uint32_t addr = " << (pre ? "mod_addr" : "base") << ";\n";
        if (load) {
            if (byte_transfer) {
                os << indent << "    uint32_t val = gba_bus_read8(cpu, addr);\n";
            } else {
                os << indent << "    uint32_t val = gba_ldr32(cpu, addr);\n";
            }
            if ((!pre || writeback) && rn != 15U && rn != rd) {
                os << indent << "    cpu->r[" << rn << "] = mod_addr;\n";
            }
            if (rd == 15U) {
                os << indent << "    cpu->r[15] = val & ~3U;\n";
                os << indent << "    cpu->cycles += 2U;\n";
                os << indent << "    goto dispatch_pc;\n";
            } else {
                os << indent << "    cpu->r[" << rd << "] = val;\n";
                os << indent << "    cpu->cycles += 2U;\n";
            }
        } else {
            if (byte_transfer) {
                os << indent << "    gba_bus_write8(cpu, addr, (uint8_t)" << reg_read(rd, pc + 12U)
                   << ");\n";
            } else {
                os << indent << "    gba_bus_write32(cpu, addr & ~3U, " << reg_read(rd, pc + 12U)
                   << ");\n";
            }
            if ((!pre || writeback) && rn != 15U) {
                os << indent << "    cpu->r[" << rn << "] = mod_addr;\n";
            }
            os << indent << "    cpu->cycles += 1U;\n";
        }
        os << indent << "}\n";
    }
    // 12. Block Data Transfer (LDM / STM)
    else if ((raw & 0x0E000000U) == 0x08000000U) {
        const bool pre = (raw & (1U << 24)) != 0U;
        const bool up = (raw & (1U << 23)) != 0U;
        const bool s_bit = (raw & (1U << 22)) != 0U;
        const bool writeback = (raw & (1U << 21)) != 0U;
        const bool load = (raw & (1U << 20)) != 0U;
        const uint32_t rn = (raw >> 16) & 0x0FU;
        const uint16_t rlist = static_cast<uint16_t>(raw & 0xFFFFU);

        uint32_t count = 0;
        for (int i = 0; i < 16; ++i) {
            if (rlist & (1U << i))
                ++count;
        }

        os << indent << "{\n";
        os << indent << "    uint32_t base = " << reg_read(rn, pc8) << ";\n";
        if (rlist == 0U) {
            os << indent << "    uint32_t new_base = base " << (up ? "+ 0x40U" : "- 0x40U")
               << ";\n";
            if (writeback && rn != 15U) {
                os << indent << "    cpu->r[" << rn << "] = new_base;\n";
            }
        } else {
            os << indent << "    uint32_t start_addr = base;\n";
            os << indent << "    uint32_t wb_addr = base " << (up ? "+ " : "- ") << (count * 4U)
               << "U;\n";
            if (up) {
                if (pre)
                    os << indent << "    start_addr += 4U;\n";
            } else {
                os << indent << "    start_addr -= " << (count * 4U) << "U;\n";
                if (!pre)
                    os << indent << "    start_addr += 4U;\n";
            }
            const bool user_bank_transfer = s_bit && (!load || (rlist & (1U << 15)) == 0U);
            os << indent << "    uint8_t prev_mode = (uint8_t)(cpu->cpsr & GBA_CPSR_MODE_MASK);\n";
            if (user_bank_transfer) {
                os << indent << "    gba_switch_mode(cpu, GBA_MODE_USR);\n";
            }
            os << indent << "    uint32_t cur = start_addr & ~3U;\n";
            for (uint32_t r = 0; r < 16U; ++r) {
                if ((rlist & (1U << r)) == 0U)
                    continue;
                if (load) {
                    os << indent << "    cpu->r[" << r << "] = gba_bus_read32(cpu, cur);\n";
                } else {
                    if (r == 15U) {
                        os << indent << "    gba_bus_write32(cpu, cur, " << hex32(pc + 12U)
                           << ");\n";
                    } else {
                        os << indent << "    gba_bus_write32(cpu, cur, cpu->r[" << r << "]);\n";
                    }
                }
                os << indent << "    cur += 4U;\n";
            }
            if (user_bank_transfer) {
                os << indent << "    gba_switch_mode(cpu, prev_mode);\n";
            } else {
                os << indent << "    (void)prev_mode;\n";
            }
            if (writeback && rn != 15U) {
                if (!load || (rlist & (1U << rn)) == 0U) {
                    os << indent << "    cpu->r[" << rn << "] = wb_addr;\n";
                }
            }
            os << indent << "    cpu->cycles += " << (count + 1U) << "U;\n";
            if (load && (rlist & (1U << 15)) != 0U) {
                if (s_bit) {
                    os << indent << "    gba_unpack_cpsr(cpu, cpu->spsr);\n";
                }
                os << indent << "    cpu->r[15] &= cpu->thumb ? ~1U : ~3U;\n";
                if (rn == 13U) {
                    os << indent << "    return;\n";
                } else {
                    os << indent << "    goto dispatch_pc;\n";
                }
            }
        }
        os << indent << "}\n";
    }

    if (cond != 0x0EU) {
        os << "    }\n";
    }
}

void emit_thumb_instruction(std::ostringstream& os, const Instruction& instr, const Function& func,
                            const ProgramCFG& cfg) {
    const uint32_t pc = instr.addr;
    const uint32_t pc4 = pc + 4U;

    // Combined 32-bit Thumb BL instruction
    if (instr.is_thumb_bl32) {
        const uint32_t next_pc = pc + 4U;
        const uint32_t target = instr.branch_target & ~1U;
        os << "    cpu->r[14] = " << hex32(next_pc | 1U) << ";\n";
        os << "    cpu->r[15] = " << hex32(target) << ";\n";
        os << "    cpu->cycles += 3U;\n";
        if (const Function* callee = cfg.find_function(target, true)) {
            os << "    " << callee->c_symbol() << "(cpu);\n";
        } else {
            os << "    gba_runtime_dispatch(cpu, " << hex32(target | 1U) << ", " << hex32(pc)
               << ");\n";
        }
        os << "    if (cpu->halted) { return; }\n";
        os << "    if ((cpu->r[15] & ~1U) != " << hex32(next_pc) << " || cpu->thumb == 0U";
        if (func.instructions.contains(next_pc) && func.instructions.at(next_pc).thumb) {
            os << ") { goto dispatch_pc; }\n";
        } else {
            os << " || 1) { goto dispatch_pc; }\n";
        }
        return;
    }

    const uint16_t raw = static_cast<uint16_t>(instr.raw & 0xFFFFU);
    const uint32_t next_pc = pc + 2U;
    os << "    cpu->r[15] = " << hex32(next_pc) << ";\n";
    os << "    cpu->cycles += 1U;\n";

    // Format 13: Add offset to Stack Pointer (10110000 S Imm7)
    if ((raw & 0xFF00U) == 0xB000U) {
        const uint32_t imm = (raw & 0x7FU) << 2U;
        if ((raw & 0x80U) != 0U) {
            os << "    cpu->r[13] -= " << hex32(imm) << ";\n";
        } else {
            os << "    cpu->r[13] += " << hex32(imm) << ";\n";
        }
        return;
    }

    // Format 14: Push / Pop registers (1011 L 10 R Rlist)
    if ((raw & 0xF600U) == 0xB400U) {
        const bool load = (raw & (1U << 11)) != 0U;
        const bool pclr = (raw & (1U << 8)) != 0U;
        const uint8_t rlist = static_cast<uint8_t>(raw & 0xFFU);
        os << "    {\n";
        os << "        uint32_t sp = cpu->r[13];\n";
        if (!load) {
            uint32_t count = pclr ? 1U : 0U;
            for (int i = 0; i < 8; ++i) {
                if (rlist & (1U << i))
                    ++count;
            }
            os << "        sp -= " << (count * 4U) << "U;\n";
            os << "        cpu->r[13] = sp;\n";
            os << "        uint32_t cur = sp & ~3U;\n";
            for (int i = 0; i < 8; ++i) {
                if (rlist & (1U << i)) {
                    os << "        gba_bus_write32(cpu, cur, cpu->r[" << i << "]);\n";
                    os << "        cur += 4U;\n";
                }
            }
            if (pclr) {
                os << "        gba_bus_write32(cpu, cur, cpu->r[14]);\n";
            }
            os << "        cpu->cycles += " << (count + 1U) << "U;\n";
        } else {
            uint32_t count = pclr ? 1U : 0U;
            for (int i = 0; i < 8; ++i) {
                if (rlist & (1U << i)) {
                    os << "        cpu->r[" << i << "] = gba_bus_read32(cpu, sp & ~3U);\n";
                    os << "        sp += 4U;\n";
                    ++count;
                }
            }
            if (pclr) {
                os << "        uint32_t popped_pc = gba_bus_read32(cpu, sp & ~3U);\n";
                os << "        sp += 4U;\n";
                os << "        cpu->r[13] = sp;\n";
                os << "        cpu->r[15] = popped_pc & ~1U;\n";
                os << "        cpu->cycles += " << (count + 2U) << "U;\n";
                os << "        return;\n";
            } else {
                os << "        cpu->r[13] = sp;\n";
                os << "        cpu->cycles += " << (count + 1U) << "U;\n";
            }
        }
        os << "    }\n";
        return;
    }

    // Format 5: Hi register operations / branch exchange (010001 Op H1 H2 Rs Rd)
    if ((raw & 0xFC00U) == 0x4400U) {
        const uint32_t op = (raw >> 8) & 0x03U;
        const uint32_t h1 = (raw >> 7) & 0x01U;
        const uint32_t h2 = (raw >> 6) & 0x01U;
        const uint32_t rs = ((raw >> 3) & 0x07U) | (h2 << 3);
        const uint32_t rd = (raw & 0x07U) | (h1 << 3);
        const std::string vs = reg_read(rs, pc4);
        const std::string vd = reg_read(rd, pc4);

        if (op == 0U) {  // ADD
            if (rd == 15U) {
                os << "    cpu->r[15] = (" << vd << " + " << vs << ") & ~1U;\n";
                os << "    cpu->cycles += 2U;\n";
                os << "    goto dispatch_pc;\n";
            } else {
                os << "    cpu->r[" << rd << "] = " << vd << " + " << vs << ";\n";
            }
        } else if (op == 1U) {  // CMP
            os << "    (void)gba_sub(cpu, " << vd << ", " << vs << ", 1);\n";
        } else if (op == 2U) {  // MOV
            if (rd == 15U) {
                os << "    cpu->r[15] = " << vs << " & ~1U;\n";
                os << "    cpu->cycles += 2U;\n";
                if (rs == 14U) {
                    os << "    return;\n";
                } else {
                    os << "    goto dispatch_pc;\n";
                }
            } else {
                os << "    cpu->r[" << rd << "] = " << vs << ";\n";
            }
        } else {  // BX
            os << "    {\n";
            os << "        uint32_t target = " << vs << ";\n";
            os << "        cpu->cycles += 2U;\n";
            os << "        gba_bx(cpu, target);\n";
            if (rs == 14U) {
                os << "        return;\n";
            } else {
                os << "        goto dispatch_pc;\n";
            }
            os << "    }\n";
        }
        return;
    }

    // Format 4: ALU operations (010000 Op Rs Rd)
    if ((raw & 0xFC00U) == 0x4000U) {
        const uint32_t op = (raw >> 6) & 0x0FU;
        const uint32_t rs = (raw >> 3) & 0x07U;
        const uint32_t rd = raw & 0x07U;
        os << "    {\n";
        os << "        uint8_t carry = cpu->c;\n";
        os << "        uint32_t a = cpu->r[" << rd << "];\n";
        os << "        uint32_t b = cpu->r[" << rs << "];\n";
        switch (op) {
            case 0x00:
                os << "        cpu->r[" << rd << "] = a & b;\n";
                os << "        gba_set_nz(cpu, cpu->r[" << rd << "]);\n";
                break;
            case 0x01:
                os << "        cpu->r[" << rd << "] = a ^ b;\n";
                os << "        gba_set_nz(cpu, cpu->r[" << rd << "]);\n";
                break;
            case 0x02:
                os << "        cpu->r[" << rd
                   << "] = gba_shift_reg(a, 0U, (uint8_t)(b & 0xFFU), &carry);\n";
                os << "        gba_set_nz(cpu, cpu->r[" << rd << "]);\n";
                os << "        cpu->c = carry;\n";
                break;
            case 0x03:
                os << "        cpu->r[" << rd
                   << "] = gba_shift_reg(a, 1U, (uint8_t)(b & 0xFFU), &carry);\n";
                os << "        gba_set_nz(cpu, cpu->r[" << rd << "]);\n";
                os << "        cpu->c = carry;\n";
                break;
            case 0x04:
                os << "        cpu->r[" << rd
                   << "] = gba_shift_reg(a, 2U, (uint8_t)(b & 0xFFU), &carry);\n";
                os << "        gba_set_nz(cpu, cpu->r[" << rd << "]);\n";
                os << "        cpu->c = carry;\n";
                break;
            case 0x05:
                os << "        cpu->r[" << rd << "] = gba_adc(cpu, a, b, 1);\n";
                break;
            case 0x06:
                os << "        cpu->r[" << rd << "] = gba_sbc(cpu, a, b, 1);\n";
                break;
            case 0x07:
                os << "        cpu->r[" << rd
                   << "] = gba_shift_reg(a, 3U, (uint8_t)(b & 0xFFU), &carry);\n";
                os << "        gba_set_nz(cpu, cpu->r[" << rd << "]);\n";
                os << "        cpu->c = carry;\n";
                break;
            case 0x08:
                os << "        gba_set_nz(cpu, a & b);\n";
                break;
            case 0x09:
                os << "        cpu->r[" << rd << "] = gba_sub(cpu, 0U, b, 1);\n";
                break;
            case 0x0A:
                os << "        (void)gba_sub(cpu, a, b, 1);\n";
                break;
            case 0x0B:
                os << "        (void)gba_add(cpu, a, b, 1);\n";
                break;
            case 0x0C:
                os << "        cpu->r[" << rd << "] = a | b;\n";
                os << "        gba_set_nz(cpu, cpu->r[" << rd << "]);\n";
                break;
            case 0x0D:
                os << "        cpu->r[" << rd << "] = a * b;\n";
                os << "        gba_set_nz(cpu, cpu->r[" << rd << "]);\n";
                break;
            case 0x0E:
                os << "        cpu->r[" << rd << "] = a & ~b;\n";
                os << "        gba_set_nz(cpu, cpu->r[" << rd << "]);\n";
                break;
            case 0x0F:
                os << "        cpu->r[" << rd << "] = ~b;\n";
                os << "        gba_set_nz(cpu, cpu->r[" << rd << "]);\n";
                break;
        }
        os << "        (void)carry; (void)a; (void)b;\n";
        os << "    }\n";
        return;
    }

    // Format 2: Add / Subtract (00011 I Op Rn Rs Rd)
    if ((raw & 0xF800U) == 0x1800U) {
        const bool is_imm = (raw & (1U << 10)) != 0U;
        const bool is_sub = (raw & (1U << 9)) != 0U;
        const uint32_t rn_imm = (raw >> 6) & 0x07U;
        const uint32_t rs = (raw >> 3) & 0x07U;
        const uint32_t rd = raw & 0x07U;
        const std::string op2 = is_imm ? hex32(rn_imm) : ("cpu->r[" + std::to_string(rn_imm) + "]");
        if (is_sub) {
            os << "    cpu->r[" << rd << "] = gba_sub(cpu, cpu->r[" << rs << "], " << op2
               << ", 1);\n";
        } else {
            os << "    cpu->r[" << rd << "] = gba_add(cpu, cpu->r[" << rs << "], " << op2
               << ", 1);\n";
        }
        return;
    }

    // Format 1: Move shifted register (000 Op Offset5 Rs Rd)
    if ((raw & 0xE000U) == 0x0000U) {
        const uint32_t op = (raw >> 11) & 0x03U;
        const uint32_t imm5 = (raw >> 6) & 0x1FU;
        const uint32_t rs = (raw >> 3) & 0x07U;
        const uint32_t rd = raw & 0x07U;
        os << "    {\n";
        os << "        uint8_t carry = cpu->c;\n";
        os << "        cpu->r[" << rd << "] = gba_shift_imm(cpu->r[" << rs << "], " << op << "U, "
           << imm5 << "U, &carry);\n";
        os << "        gba_set_nz(cpu, cpu->r[" << rd << "]);\n";
        os << "        cpu->c = carry;\n";
        os << "    }\n";
        return;
    }

    // Format 3: Move / Compare / Add / Subtract immediate (001 Op Rd Offset8)
    if ((raw & 0xE000U) == 0x2000U) {
        const uint32_t op = (raw >> 11) & 0x03U;
        const uint32_t rd = (raw >> 8) & 0x07U;
        const uint32_t imm8 = raw & 0xFFU;
        switch (op) {
            case 0U:
                os << "    cpu->r[" << rd << "] = " << hex32(imm8) << ";\n";
                os << "    gba_set_nz(cpu, " << hex32(imm8) << ");\n";
                break;
            case 1U:
                os << "    (void)gba_sub(cpu, cpu->r[" << rd << "], " << hex32(imm8) << ", 1);\n";
                break;
            case 2U:
                os << "    cpu->r[" << rd << "] = gba_add(cpu, cpu->r[" << rd << "], "
                   << hex32(imm8) << ", 1);\n";
                break;
            case 3U:
                os << "    cpu->r[" << rd << "] = gba_sub(cpu, cpu->r[" << rd << "], "
                   << hex32(imm8) << ", 1);\n";
                break;
        }
        return;
    }

    // Format 6: PC-relative load (01001 Rd Word8)
    if ((raw & 0xF800U) == 0x4800U) {
        const uint32_t rd = (raw >> 8) & 0x07U;
        const uint32_t addr = (pc4 & ~2U) + ((raw & 0xFFU) << 2U);
        os << "    cpu->r[" << rd << "] = gba_ldr32(cpu, " << hex32(addr) << ");\n";
        os << "    cpu->cycles += 2U;\n";
        return;
    }

    // Format 7 & 8: Load/store with register offset (0101 xx x Ro Rb Rd)
    if ((raw & 0xF000U) == 0x5000U) {
        const uint32_t ro = (raw >> 6) & 0x07U;
        const uint32_t rb = (raw >> 3) & 0x07U;
        const uint32_t rd = raw & 0x07U;
        os << "    {\n";
        os << "        uint32_t addr = cpu->r[" << rb << "] + cpu->r[" << ro << "];\n";
        if ((raw & 0x0200U) == 0U) {
            const uint32_t op = (raw >> 10) & 0x03U;
            if (op == 0U) {
                os << "        gba_bus_write32(cpu, addr & ~3U, cpu->r[" << rd << "]);\n";
            } else if (op == 1U) {
                os << "        gba_bus_write8(cpu, addr, (uint8_t)cpu->r[" << rd << "]);\n";
            } else if (op == 2U) {
                os << "        cpu->r[" << rd << "] = gba_ldr32(cpu, addr);\n";
            } else {
                os << "        cpu->r[" << rd << "] = gba_bus_read8(cpu, addr);\n";
            }
        } else {
            const uint32_t op = (raw >> 10) & 0x03U;
            if (op == 0U) {
                os << "        gba_bus_write16(cpu, addr & ~1U, (uint16_t)cpu->r[" << rd << "]);\n";
            } else if (op == 1U) {
                os << "        cpu->r[" << rd << "] = gba_ldrsb8(cpu, addr);\n";
            } else if (op == 2U) {
                os << "        cpu->r[" << rd << "] = gba_ldrh16(cpu, addr);\n";
            } else {
                os << "        cpu->r[" << rd << "] = gba_ldrsh16(cpu, addr);\n";
            }
        }
        os << "        cpu->cycles += 2U;\n";
        os << "    }\n";
        return;
    }

    // Format 9: Load/store with immediate offset (011 B L Offset5 Rb Rd)
    if ((raw & 0xE000U) == 0x6000U) {
        const bool byte_transfer = (raw & (1U << 12)) != 0U;
        const bool load = (raw & (1U << 11)) != 0U;
        const uint32_t imm5 = (raw >> 6) & 0x1FU;
        const uint32_t rb = (raw >> 3) & 0x07U;
        const uint32_t rd = raw & 0x07U;
        const uint32_t offset = byte_transfer ? imm5 : (imm5 << 2U);
        os << "    {\n";
        os << "        uint32_t addr = cpu->r[" << rb << "] + " << hex32(offset) << ";\n";
        if (load) {
            if (byte_transfer) {
                os << "        cpu->r[" << rd << "] = gba_bus_read8(cpu, addr);\n";
            } else {
                os << "        cpu->r[" << rd << "] = gba_ldr32(cpu, addr);\n";
            }
        } else {
            if (byte_transfer) {
                os << "        gba_bus_write8(cpu, addr, (uint8_t)cpu->r[" << rd << "]);\n";
            } else {
                os << "        gba_bus_write32(cpu, addr & ~3U, cpu->r[" << rd << "]);\n";
            }
        }
        os << "        cpu->cycles += 2U;\n";
        os << "    }\n";
        return;
    }

    // Format 10: Load/store halfword (1000 L Offset5 Rb Rd)
    if ((raw & 0xF000U) == 0x8000U) {
        const bool load = (raw & (1U << 11)) != 0U;
        const uint32_t offset = ((raw >> 6) & 0x1FU) << 1U;
        const uint32_t rb = (raw >> 3) & 0x07U;
        const uint32_t rd = raw & 0x07U;
        os << "    {\n";
        os << "        uint32_t addr = cpu->r[" << rb << "] + " << hex32(offset) << ";\n";
        if (load) {
            os << "        cpu->r[" << rd << "] = gba_ldrh16(cpu, addr);\n";
        } else {
            os << "        gba_bus_write16(cpu, addr & ~1U, (uint16_t)cpu->r[" << rd << "]);\n";
        }
        os << "        cpu->cycles += 2U;\n";
        os << "    }\n";
        return;
    }

    // Format 11: SP-relative load/store (1001 L Rd Word8)
    if ((raw & 0xF000U) == 0x9000U) {
        const bool load = (raw & (1U << 11)) != 0U;
        const uint32_t rd = (raw >> 8) & 0x07U;
        const uint32_t offset = (raw & 0xFFU) << 2U;
        os << "    {\n";
        os << "        uint32_t addr = cpu->r[13] + " << hex32(offset) << ";\n";
        if (load) {
            os << "        cpu->r[" << rd << "] = gba_ldr32(cpu, addr);\n";
        } else {
            os << "        gba_bus_write32(cpu, addr & ~3U, cpu->r[" << rd << "]);\n";
        }
        os << "        cpu->cycles += 2U;\n";
        os << "    }\n";
        return;
    }

    // Format 12: Load address (1010 SP Rd Word8)
    if ((raw & 0xF000U) == 0xA000U) {
        const bool use_sp = (raw & (1U << 11)) != 0U;
        const uint32_t rd = (raw >> 8) & 0x07U;
        const uint32_t imm = (raw & 0xFFU) << 2U;
        if (use_sp) {
            os << "    cpu->r[" << rd << "] = cpu->r[13] + " << hex32(imm) << ";\n";
        } else {
            os << "    cpu->r[" << rd << "] = " << hex32((pc4 & ~2U) + imm) << ";\n";
        }
        return;
    }

    // Format 15: Multiple load/store (1100 L Rb Rlist)
    if ((raw & 0xF000U) == 0xC000U) {
        const bool load = (raw & (1U << 11)) != 0U;
        const uint32_t rb = (raw >> 8) & 0x07U;
        const uint8_t rlist = static_cast<uint8_t>(raw & 0xFFU);
        os << "    {\n";
        os << "        uint32_t addr = cpu->r[" << rb << "] & ~3U;\n";
        for (int i = 0; i < 8; ++i) {
            if (rlist & (1U << i)) {
                if (load) {
                    os << "        cpu->r[" << i << "] = gba_bus_read32(cpu, addr);\n";
                } else {
                    os << "        gba_bus_write32(cpu, addr, cpu->r[" << i << "]);\n";
                }
                os << "        addr += 4U;\n";
            }
        }
        if (!load || (rlist & (1U << rb)) == 0U) {
            os << "        cpu->r[" << rb << "] = addr;\n";
        }
        os << "    }\n";
        return;
    }

    // Format 16 & 17: Conditional branch / SWI (1101 Cond Offset8)
    if ((raw & 0xF000U) == 0xD000U) {
        const uint8_t cond = static_cast<uint8_t>((raw >> 8) & 0x0FU);
        if (cond == 0x0FU) {
            const uint8_t swi_num = static_cast<uint8_t>(raw & 0xFFU);
            os << "    gba_swi(cpu, " << static_cast<unsigned>(swi_num) << "U);\n";
            if (swi_num == 0x00U) {
                os << "    return;\n";
            } else {
                os << "    gba_runtime_poll(cpu);\n";
                os << "    if (cpu->halted) { return; }\n";
            }
            return;
        }
        if (cond < 0x0EU) {
            const int32_t offset = static_cast<int32_t>(static_cast<int8_t>(raw & 0xFFU)) * 2;
            const uint32_t target = (pc4 + static_cast<uint32_t>(offset)) & ~1U;
            os << "    if (gba_check_cond(cpu, " << static_cast<unsigned>(cond) << "U)) {\n";
            os << "        cpu->r[15] = " << hex32(target) << ";\n";
            os << "        cpu->cycles += 2U;\n";
            if (func.block_addrs.contains(target)) {
                os << "        gba_runtime_poll(cpu);\n";
                os << "        if (cpu->halted) { return; }\n";
                os << "        goto " << label_name(target) << ";\n";
            } else {
                os << "        goto dispatch_pc;\n";
            }
            os << "    }\n";
        }
        return;
    }

    // Format 18: Unconditional branch (11100 Offset11)
    if ((raw & 0xF800U) == 0xE000U) {
        int32_t offset = static_cast<int32_t>(raw & 0x07FFU);
        if ((offset & 0x0400) != 0) {
            offset |= static_cast<int32_t>(0xFFFFF800U);
        }
        const uint32_t target = (pc4 + static_cast<uint32_t>(offset * 2)) & ~1U;
        os << "    cpu->r[15] = " << hex32(target) << ";\n";
        os << "    cpu->cycles += 2U;\n";
        if (func.block_addrs.contains(target)) {
            os << "    gba_runtime_poll(cpu);\n";
            os << "    if (cpu->halted) { return; }\n";
            os << "    goto " << label_name(target) << ";\n";
        } else if (const Function* callee = cfg.find_function(target, true)) {
            os << "    " << callee->c_symbol() << "(cpu);\n";
            os << "    return;\n";
        } else {
            os << "    goto dispatch_pc;\n";
        }
        return;
    }

    // Format 19 (unpaired BL high/low halfwords)
    if ((raw & 0xF800U) == 0xF000U) {
        int32_t hi = static_cast<int32_t>(raw & 0x07FFU);
        if ((hi & 0x0400) != 0) {
            hi |= static_cast<int32_t>(0xFFFFF800U);
        }
        os << "    cpu->r[14] = " << hex32(pc4 + static_cast<uint32_t>(hi << 12)) << ";\n";
        return;
    }
    if ((raw & 0xF800U) == 0xF800U) {
        const uint32_t lo = (raw & 0x07FFU) << 1U;
        os << "    {\n";
        os << "        uint32_t target = (cpu->r[14] + " << hex32(lo) << ") & ~1U;\n";
        os << "        cpu->r[14] = " << hex32(next_pc | 1U) << ";\n";
        os << "        cpu->r[15] = target;\n";
        os << "        gba_runtime_dispatch(cpu, target | 1U, " << hex32(pc) << ");\n";
        os << "        if ((cpu->r[15] & ~1U) != " << hex32(next_pc) << ") { goto dispatch_pc; }\n";
        os << "    }\n";
        return;
    }
}

void emit_function(std::ostringstream& os, const Function& func, const ProgramCFG& cfg) {
    os << "void " << func.c_symbol() << "(CpuState* cpu) {\n";
    if (func.instructions.empty()) {
        os << "    (void)cpu;\n";
        os << "    return;\n";
        os << "}\n\n";
        return;
    }

    const uint32_t entry_addr = func.entry_addr;
    os << "    const uint32_t entry_lr = cpu->r[14];\n";
    if (func.thumb) {
        os << "    cpu->thumb = 1U;\n";
        os << "    cpu->cpsr |= GBA_CPSR_T;\n";
    } else {
        os << "    cpu->thumb = 0U;\n";
        os << "    cpu->cpsr &= ~GBA_CPSR_T;\n";
    }
    os << "    cpu->r[15] = " << hex32(entry_addr) << ";\n";
    os << "    gba_runtime_poll(cpu);\n";
    os << "    if (cpu->halted) {\n";
    os << "        return;\n";
    os << "    }\n";
    os << "    if (cpu->r[15] != " << hex32(entry_addr) << ") {\n";
    os << "        goto dispatch_pc;\n";
    os << "    }\n";
    os << "    goto " << label_name(entry_addr) << ";\n\n";

    os << "dispatch_pc:\n";
    os << "    gba_runtime_poll(cpu);\n";
    os << "    if (cpu->halted) {\n";
    os << "        return;\n";
    os << "    }\n";
    os << "    if ((cpu->thumb != 0U) == " << (func.thumb ? "1" : "0") << ") {\n";
    os << "        switch (cpu->r[15] & " << (func.thumb ? "~1U" : "~3U") << ") {\n";
    for (uint32_t b_addr : func.block_addrs) {
        if (func.instructions.contains(b_addr)) {
            os << "            case " << hex32(b_addr) << ": goto " << label_name(b_addr) << ";\n";
        }
    }
    os << "            default: break;\n";
    os << "        }\n";
    os << "    }\n";
    os << "    if ((cpu->r[15] & ~1U) == (entry_lr & ~1U) && (entry_lr & ~1U) != "
       << hex32(entry_addr) << ") {\n";
    os << "        return;\n";
    os << "    }\n";
    os << "    gba_runtime_dispatch(cpu, cpu->r[15] | (cpu->thumb ? 1U : 0U), " << hex32(entry_addr)
       << ");\n";
    os << "    return;\n\n";

    for (auto it = func.instructions.begin(); it != func.instructions.end(); ++it) {
        const uint32_t addr = it->first;
        const Instruction& instr = it->second;
        if (func.block_addrs.contains(addr)) {
            os << label_name(addr) << ":;\n";
        }
        if (instr.thumb) {
            emit_thumb_instruction(os, instr, func, cfg);
        } else {
            emit_arm_instruction(os, instr, func, cfg);
        }
        auto next_it = std::next(it);
        if (next_it != func.instructions.end() && next_it->first != addr + instr.length) {
            os << "    goto dispatch_pc;\n";
        }
    }

    os << "    goto dispatch_pc;\n";
    os << "}\n\n";
}

}  // namespace

GeneratedFiles emit_c99(const ProgramCFG& cfg, const GameConfig& config,
                        const EmitOptions& options) {
    (void)config;
    GeneratedFiles result;

    // 1. Header file (recomp_functions.h)
    {
        std::ostringstream hdr;
        hdr << "// Auto-generated by gbarecomp. Do not edit manually.\n";
        hdr << "#pragma once\n\n";
        hdr << "#include <stddef.h>\n";
        hdr << "#include \"gba/cpu_state.h\"\n\n";
        hdr << "#ifdef __cplusplus\n";
        hdr << "extern \"C\" {\n";
        hdr << "#endif\n\n";

        for (const auto& [key, func] : cfg.functions) {
            (void)key;
            hdr << "void " << func.c_symbol() << "(CpuState* cpu);\n";
        }

        hdr << "\nextern const RecompFuncEntry kRecompDispatchTable[];\n";
        hdr << "extern const size_t kRecompDispatchTableCount;\n";

        hdr << "\n#ifdef __cplusplus\n";
        hdr << "}\n";
        hdr << "#endif\n";
        result.header_content = hdr.str();
    }

    // 2. Source chunks (recomp_000.c, ...)
    const size_t split_every = std::max<size_t>(1, options.split_every);
    std::ostringstream current_chunk;
    size_t count_in_chunk = 0;

    auto start_chunk = [&]() {
        current_chunk.str("");
        current_chunk.clear();
        current_chunk << "// Auto-generated by gbarecomp. Do not edit manually.\n";
        current_chunk << "#include \"recomp_functions.h\"\n\n";
        count_in_chunk = 0;
    };

    start_chunk();
    for (const auto& [key, func] : cfg.functions) {
        (void)key;
        emit_function(current_chunk, func, cfg);
        ++count_in_chunk;
        if (count_in_chunk >= split_every) {
            result.source_chunks.push_back(current_chunk.str());
            start_chunk();
        }
    }
    if (count_in_chunk > 0 || result.source_chunks.empty()) {
        result.source_chunks.push_back(current_chunk.str());
    }

    // 3. Dispatch table appended to the last source chunk
    {
        std::ostringstream disp;
        disp << "\nconst RecompFuncEntry kRecompDispatchTable[] = {\n";
        for (const auto& [key, func] : cfg.functions) {
            (void)key;
            disp << "    {" << hex32(func.entry_addr) << ", " << (func.thumb ? "1U" : "0U") << ", &"
                 << func.c_symbol() << "},\n";
        }
        disp << "    {0U, 0U, (RecompFunc)0},\n";
        disp << "};\n\n";
        disp << "const size_t kRecompDispatchTableCount = " << cfg.functions.size() << "U;\n";
        result.dispatch_source = disp.str();
        result.source_chunks.back() += result.dispatch_source;
    }

    return result;
}

std::vector<std::filesystem::path> write_generated_output(const ProgramCFG& cfg,
                                                          const GameConfig& config,
                                                          const std::filesystem::path& output_dir,
                                                          const EmitOptions& options) {
    (void)config;
    std::filesystem::create_directories(output_dir);
    // Remove any stale recomp_*.c / recomp_functions.h files from previous runs
    for (const auto& entry : std::filesystem::directory_iterator(output_dir)) {
        if (entry.is_regular_file()) {
            const auto name = entry.path().filename().string();
            if (name.starts_with("recomp_") && (name.ends_with(".c") || name.ends_with(".h"))) {
                std::error_code ec;
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }
    std::vector<std::filesystem::path> written;

    // 1. Write header file directly to disk
    const auto hdr_path = output_dir / "recomp_functions.h";
    {
        std::ofstream hdr(hdr_path);
        if (!hdr) {
            throw std::runtime_error("failed to write " + hdr_path.string());
        }
        hdr << "// Auto-generated by gbarecomp. Do not edit manually.\n";
        hdr << "#pragma once\n\n";
        hdr << "#include <stddef.h>\n";
        hdr << "#include \"gba/cpu_state.h\"\n\n";
        hdr << "#ifdef __cplusplus\n";
        hdr << "extern \"C\" {\n";
        hdr << "#endif\n\n";

        for (const auto& [key, func] : cfg.functions) {
            (void)key;
            hdr << "void " << func.c_symbol() << "(CpuState* cpu);\n";
        }

        hdr << "\nextern const RecompFuncEntry kRecompDispatchTable[];\n";
        hdr << "extern const size_t kRecompDispatchTableCount;\n";
        hdr << "\n#ifdef __cplusplus\n";
        hdr << "}\n";
        hdr << "#endif\n";
        written.push_back(hdr_path);
    }

    // 2. Stream source chunks directly to disk to keep peak RAM usage minimal
    const size_t split_every = std::max<size_t>(1, options.split_every);
    size_t chunk_idx = 0;
    size_t count_in_chunk = 0;
    std::ofstream chunk_out;
    std::filesystem::path last_chunk_path;

    auto open_next_chunk = [&]() {
        if (chunk_out.is_open()) {
            chunk_out.close();
        }
        char name[32];
        std::snprintf(name, sizeof(name), "recomp_%03zu.c", chunk_idx++);
        last_chunk_path = output_dir / name;
        chunk_out.open(last_chunk_path);
        if (!chunk_out) {
            throw std::runtime_error("failed to write " + last_chunk_path.string());
        }
        chunk_out << "// Auto-generated by gbarecomp. Do not edit manually.\n";
        chunk_out << "#include \"recomp_functions.h\"\n\n";
        written.push_back(last_chunk_path);
        count_in_chunk = 0;
    };

    open_next_chunk();
    std::ostringstream fn_buf;
    for (const auto& [key, func] : cfg.functions) {
        (void)key;
        if (count_in_chunk >= split_every) {
            open_next_chunk();
        }
        fn_buf.str("");
        fn_buf.clear();
        emit_function(fn_buf, func, cfg);
        chunk_out << fn_buf.str();
        ++count_in_chunk;
    }

    // 3. Append dispatch table to the final source chunk
    chunk_out << "\nconst RecompFuncEntry kRecompDispatchTable[] = {\n";
    for (const auto& [key, func] : cfg.functions) {
        (void)key;
        chunk_out << "    {" << hex32(func.entry_addr) << ", " << (func.thumb ? "1U" : "0U")
                  << ", &" << func.c_symbol() << "},\n";
    }
    chunk_out << "    {0U, 0U, (RecompFunc)0},\n";
    chunk_out << "};\n\n";
    chunk_out << "const size_t kRecompDispatchTableCount = " << cfg.functions.size() << "U;\n";
    chunk_out.close();

    return written;
}

}  // namespace dragon::gbarecomp
