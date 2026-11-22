#include "psprecomp/emitter.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

namespace dragon::psprecomp {

namespace {

std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08XU", v);
    return buf;
}

std::string func_symbol(uint32_t addr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "func_%08X", addr);
    return buf;
}

std::string escape_c_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8U);
    for (char c : s) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '"')
            out += "\\\"";
        else
            out.push_back(c);
    }
    return out;
}

void emit_non_branch_stmt(std::ostringstream& os, const dragon::mips::DecodedMips& d,
                          const char* indent) {
    using dragon::mips::MipsOp;
    auto gpr = [](uint8_t r) -> std::string {
        if (r == 0)
            return "0U";
        return "cpu->gpr[" + std::to_string(r) + "]";
    };
    auto set_gpr = [&](uint8_t r, const std::string& expr) {
        if (r != 0)
            os << indent << "cpu->gpr[" << static_cast<unsigned>(r) << "] = " << expr << ";\n";
    };
    const int32_t simm = static_cast<int32_t>(d.simm);

    switch (d.op) {
        case MipsOp::Nop:
        case MipsOp::Sync:
        case MipsOp::Cache:
        case MipsOp::Vflush:
        case MipsOp::Break:
            break;

        case MipsOp::Halt:
            os << indent << "cpu->halted = 1U; return;\n";
            break;

        case MipsOp::Sll:
            set_gpr(d.rd, "(" + gpr(d.rt) + " << " + std::to_string(d.sa) + "U)");
            break;
        case MipsOp::Srl:
            set_gpr(d.rd, "(" + gpr(d.rt) + " >> " + std::to_string(d.sa) + "U)");
            break;
        case MipsOp::Sra:
            set_gpr(d.rd, "(uint32_t)((int32_t)" + gpr(d.rt) + " >> " + std::to_string(d.sa) + ")");
            break;
        case MipsOp::Sllv:
            set_gpr(d.rd, "(" + gpr(d.rt) + " << (" + gpr(d.rs) + " & 31U))");
            break;
        case MipsOp::Srlv:
            set_gpr(d.rd, "(" + gpr(d.rt) + " >> (" + gpr(d.rs) + " & 31U))");
            break;
        case MipsOp::Srav:
            set_gpr(d.rd, "(uint32_t)((int32_t)" + gpr(d.rt) + " >> (" + gpr(d.rs) + " & 31U))");
            break;
        case MipsOp::Rotr:
            set_gpr(d.rd, "psp_rotr32(" + gpr(d.rt) + ", " + std::to_string(d.sa) + "U)");
            break;
        case MipsOp::Rotrv:
            set_gpr(d.rd, "psp_rotr32(" + gpr(d.rt) + ", " + gpr(d.rs) + ")");
            break;

        case MipsOp::Add:
        case MipsOp::Addu:
            set_gpr(d.rd, "(" + gpr(d.rs) + " + " + gpr(d.rt) + ")");
            break;
        case MipsOp::Sub:
        case MipsOp::Subu:
            set_gpr(d.rd, "(" + gpr(d.rs) + " - " + gpr(d.rt) + ")");
            break;
        case MipsOp::Addi:
        case MipsOp::Addiu:
            set_gpr(d.rt, "(" + gpr(d.rs) + " + (uint32_t)(" + std::to_string(simm) + "))");
            break;
        case MipsOp::And:
            set_gpr(d.rd, "(" + gpr(d.rs) + " & " + gpr(d.rt) + ")");
            break;
        case MipsOp::Or:
            set_gpr(d.rd, "(" + gpr(d.rs) + " | " + gpr(d.rt) + ")");
            break;
        case MipsOp::Xor:
            set_gpr(d.rd, "(" + gpr(d.rs) + " ^ " + gpr(d.rt) + ")");
            break;
        case MipsOp::Nor:
            set_gpr(d.rd, "(~(" + gpr(d.rs) + " | " + gpr(d.rt) + "))");
            break;
        case MipsOp::Andi:
            set_gpr(d.rt, "(" + gpr(d.rs) + " & " + hex32(d.imm) + ")");
            break;
        case MipsOp::Ori:
            set_gpr(d.rt, "(" + gpr(d.rs) + " | " + hex32(d.imm) + ")");
            break;
        case MipsOp::Xori:
            set_gpr(d.rt, "(" + gpr(d.rs) + " ^ " + hex32(d.imm) + ")");
            break;
        case MipsOp::Lui:
            set_gpr(d.rt, hex32(static_cast<uint32_t>(d.imm) << 16U));
            break;
        case MipsOp::Slt:
            set_gpr(d.rd, "((int32_t)" + gpr(d.rs) + " < (int32_t)" + gpr(d.rt) + " ? 1U : 0U)");
            break;
        case MipsOp::Sltu:
            set_gpr(d.rd, "(" + gpr(d.rs) + " < " + gpr(d.rt) + " ? 1U : 0U)");
            break;
        case MipsOp::Slti:
            set_gpr(d.rt, "((int32_t)" + gpr(d.rs) + " < " + std::to_string(simm) + " ? 1U : 0U)");
            break;
        case MipsOp::Sltiu:
            set_gpr(d.rt,
                    "(" + gpr(d.rs) + " < (uint32_t)(" + std::to_string(simm) + ") ? 1U : 0U)");
            break;

        case MipsOp::Movz:
            if (d.rd != 0) {
                os << indent << "if (" << gpr(d.rt) << " == 0U) cpu->gpr["
                   << static_cast<unsigned>(d.rd) << "] = " << gpr(d.rs) << ";\n";
            }
            break;
        case MipsOp::Movn:
            if (d.rd != 0) {
                os << indent << "if (" << gpr(d.rt) << " != 0U) cpu->gpr["
                   << static_cast<unsigned>(d.rd) << "] = " << gpr(d.rs) << ";\n";
            }
            break;
        case MipsOp::Max:
            set_gpr(d.rd, "((int32_t)" + gpr(d.rs) + " > (int32_t)" + gpr(d.rt) + " ? " +
                              gpr(d.rs) + " : " + gpr(d.rt) + ")");
            break;
        case MipsOp::Min:
            set_gpr(d.rd, "((int32_t)" + gpr(d.rs) + " < (int32_t)" + gpr(d.rt) + " ? " +
                              gpr(d.rs) + " : " + gpr(d.rt) + ")");
            break;
        case MipsOp::Seb:
            set_gpr(d.rd, "(uint32_t)(int32_t)(int8_t)(" + gpr(d.rt) + " & 0xFFU)");
            break;
        case MipsOp::Seh:
            set_gpr(d.rd, "(uint32_t)(int32_t)(int16_t)(" + gpr(d.rt) + " & 0xFFFFU)");
            break;
        case MipsOp::Wsbh:
            set_gpr(d.rd, "(((" + gpr(d.rt) + " & 0x00FF00FFU) << 8U) | ((" + gpr(d.rt) +
                              " & 0xFF00FF00U) >> 8U))");
            break;
        case MipsOp::Wsbw:
            set_gpr(d.rd, "(((" + gpr(d.rt) + " & 0x000000FFU) << 24U) | ((" + gpr(d.rt) +
                              " & 0x0000FF00U) << 8U) | ((" + gpr(d.rt) +
                              " & 0x00FF0000U) >> 8U) | ((" + gpr(d.rt) +
                              " & 0xFF000000U) >> 24U))");
            break;

        case MipsOp::Mult:
            os << indent << "{ int64_t _p = (int64_t)(int32_t)" << gpr(d.rs)
               << " * (int64_t)(int32_t)" << gpr(d.rt)
               << "; cpu->lo = (uint32_t)_p; cpu->hi = (uint32_t)((uint64_t)_p >> 32U); }\n";
            break;
        case MipsOp::Multu:
            os << indent << "{ uint64_t _p = (uint64_t)" << gpr(d.rs) << " * (uint64_t)"
               << gpr(d.rt) << "; cpu->lo = (uint32_t)_p; cpu->hi = (uint32_t)(_p >> 32U); }\n";
            break;
        case MipsOp::Div:
            os << indent << "{ int32_t _a = (int32_t)" << gpr(d.rs) << ", _b = (int32_t)"
               << gpr(d.rt)
               << "; if (_b != 0 && !(_a == (-2147483647 - 1) && _b == -1)) { cpu->lo = "
                  "(uint32_t)(_a / _b); cpu->hi = (uint32_t)(_a % _b); } }\n";
            break;
        case MipsOp::Divu:
            os << indent << "{ uint32_t _a = " << gpr(d.rs) << ", _b = " << gpr(d.rt)
               << "; if (_b != 0U) { cpu->lo = _a / _b; cpu->hi = _a % _b; } }\n";
            break;
        case MipsOp::Mfhi:
            set_gpr(d.rd, "cpu->hi");
            break;
        case MipsOp::Mthi:
            os << indent << "cpu->hi = " << gpr(d.rs) << ";\n";
            break;
        case MipsOp::Mflo:
            set_gpr(d.rd, "cpu->lo");
            break;
        case MipsOp::Mtlo:
            os << indent << "cpu->lo = " << gpr(d.rs) << ";\n";
            break;

        // Loads & Stores
        case MipsOp::Lb:
            set_gpr(d.rt, "(uint32_t)(int32_t)(int8_t)psp_read8(cpu, " + gpr(d.rs) +
                              " + (uint32_t)(" + std::to_string(simm) + "))");
            break;
        case MipsOp::Lbu:
            set_gpr(d.rt, "(uint32_t)psp_read8(cpu, " + gpr(d.rs) + " + (uint32_t)(" +
                              std::to_string(simm) + "))");
            break;
        case MipsOp::Lh:
            set_gpr(d.rt, "(uint32_t)(int32_t)(int16_t)psp_read16(cpu, " + gpr(d.rs) +
                              " + (uint32_t)(" + std::to_string(simm) + "))");
            break;
        case MipsOp::Lhu:
            set_gpr(d.rt, "(uint32_t)psp_read16(cpu, " + gpr(d.rs) + " + (uint32_t)(" +
                              std::to_string(simm) + "))");
            break;
        case MipsOp::Lw:
        case MipsOp::Ll:
            set_gpr(d.rt, "psp_read32(cpu, " + gpr(d.rs) + " + (uint32_t)(" + std::to_string(simm) +
                              "))");
            break;
        case MipsOp::Lwl:
            set_gpr(d.rt, "psp_lwl(cpu, " + gpr(d.rs) + " + (uint32_t)(" + std::to_string(simm) +
                              "), " + gpr(d.rt) + ")");
            break;
        case MipsOp::Lwr:
            set_gpr(d.rt, "psp_lwr(cpu, " + gpr(d.rs) + " + (uint32_t)(" + std::to_string(simm) +
                              "), " + gpr(d.rt) + ")");
            break;
        case MipsOp::Sb:
            os << indent << "psp_write8(cpu, " << gpr(d.rs) << " + (uint32_t)(" << simm
               << "), (uint8_t)(" << gpr(d.rt) << " & 0xFFU));\n";
            break;
        case MipsOp::Sh:
            os << indent << "psp_write16(cpu, " << gpr(d.rs) << " + (uint32_t)(" << simm
               << "), (uint16_t)(" << gpr(d.rt) << " & 0xFFFFU));\n";
            break;
        case MipsOp::Sw:
            os << indent << "psp_write32(cpu, " << gpr(d.rs) << " + (uint32_t)(" << simm << "), "
               << gpr(d.rt) << ");\n";
            break;
        case MipsOp::Swl:
            os << indent << "psp_swl(cpu, " << gpr(d.rs) << " + (uint32_t)(" << simm << "), "
               << gpr(d.rt) << ");\n";
            break;
        case MipsOp::Swr:
            os << indent << "psp_swr(cpu, " << gpr(d.rs) << " + (uint32_t)(" << simm << "), "
               << gpr(d.rt) << ");\n";
            break;

        case MipsOp::Syscall:
            os << indent << "psp_hle_syscall(cpu, " << hex32(d.syscall_code) << ");\n";
            break;

        // Basic FPU ops inlined directly in C99
        case MipsOp::AddS:
            os << indent << "cpu->f[" << static_cast<unsigned>(d.sa) << "] = cpu->f["
               << static_cast<unsigned>(d.rd) << "] + cpu->f[" << static_cast<unsigned>(d.rt)
               << "];\n";
            break;
        case MipsOp::SubS:
            os << indent << "cpu->f[" << static_cast<unsigned>(d.sa) << "] = cpu->f["
               << static_cast<unsigned>(d.rd) << "] - cpu->f[" << static_cast<unsigned>(d.rt)
               << "];\n";
            break;
        case MipsOp::MulS:
            os << indent << "cpu->f[" << static_cast<unsigned>(d.sa) << "] = cpu->f["
               << static_cast<unsigned>(d.rd) << "] * cpu->f[" << static_cast<unsigned>(d.rt)
               << "];\n";
            break;
        case MipsOp::DivS:
            os << indent << "cpu->f[" << static_cast<unsigned>(d.sa) << "] = cpu->f["
               << static_cast<unsigned>(d.rd) << "] / cpu->f[" << static_cast<unsigned>(d.rt)
               << "];\n";
            break;
        case MipsOp::MovS:
            os << indent << "cpu->f[" << static_cast<unsigned>(d.sa) << "] = cpu->f["
               << static_cast<unsigned>(d.rd) << "];\n";
            break;
        case MipsOp::NegS:
            os << indent << "cpu->f[" << static_cast<unsigned>(d.sa) << "] = -cpu->f["
               << static_cast<unsigned>(d.rd) << "];\n";
            break;

        default:
            // Fallback to interpreter for Allegrex bitops (clz/clo/ext/ins/bitrev/madd/msub) & VFPU
            os << indent << "psp_exec_fallback_insn(cpu, " << hex32(d.raw) << ", " << hex32(d.pc)
               << ");\n";
            break;
    }
}

std::string branch_cond_expr(const dragon::mips::DecodedMips& d) {
    using dragon::mips::MipsOp;
    auto gpr = [](uint8_t r) -> std::string {
        return (r == 0) ? "0U" : ("cpu->gpr[" + std::to_string(r) + "]");
    };
    switch (d.op) {
        case MipsOp::Beq:
        case MipsOp::Beql:
            return "(" + gpr(d.rs) + " == " + gpr(d.rt) + ")";
        case MipsOp::Bne:
        case MipsOp::Bnel:
            return "(" + gpr(d.rs) + " != " + gpr(d.rt) + ")";
        case MipsOp::Blez:
        case MipsOp::Blezl:
            return "((int32_t)" + gpr(d.rs) + " <= 0)";
        case MipsOp::Bgtz:
        case MipsOp::Bgtzl:
            return "((int32_t)" + gpr(d.rs) + " > 0)";
        case MipsOp::Bltz:
        case MipsOp::Bltzl:
        case MipsOp::Bltzal:
        case MipsOp::Bltzall:
            return "((int32_t)" + gpr(d.rs) + " < 0)";
        case MipsOp::Bgez:
        case MipsOp::Bgezl:
        case MipsOp::Bgezal:
        case MipsOp::Bgezall:
            return "((int32_t)" + gpr(d.rs) + " >= 0)";
        case MipsOp::Bc1f:
        case MipsOp::Bc1fl:
            return "(((cpu->fcr31 >> 23U) & 1U) == 0U)";
        case MipsOp::Bc1t:
        case MipsOp::Bc1tl:
            return "(((cpu->fcr31 >> 23U) & 1U) != 0U)";
        default:
            return "0";
    }
}

void emit_function(std::ostringstream& os, const PspFunction& fn, const PspCfg& cfg) {
    using dragon::mips::MipsOp;
    os << "void " << func_symbol(fn.entry_addr) << "(PspCpuState* cpu) {\n";

    std::set<uint32_t> skip_delay_slots;
    for (auto it = fn.instructions.begin(); it != fn.instructions.end(); ++it) {
        const uint32_t pc = it->first;
        const auto& d = it->second;

        if (skip_delay_slots.count(pc) != 0U) {
            if (fn.local_labels.count(pc) == 0U)
                continue;
        }

        if (fn.local_labels.count(pc) != 0U) {
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "L_%08X", pc);
            os << lbl << ":;\n";
        }

        os << "    cpu->cycles += 1ULL;\n";

        if (!d.has_delay_slot) {
            emit_non_branch_stmt(os, d, "    ");
            continue;
        }

        // Look up delay-slot instruction at pc + 4
        const auto ds_it = fn.instructions.find(pc + 4U);
        const bool has_ds = (ds_it != fn.instructions.end());
        if (has_ds)
            skip_delay_slots.insert(pc + 4U);

        if (d.op == MipsOp::Jal) {
            os << "    cpu->gpr[31] = " << hex32(pc + 8U) << ";\n";
            if (has_ds)
                emit_non_branch_stmt(os, ds_it->second, "    ");
            if (cfg.functions.count(d.target) != 0U) {
                os << "    " << func_symbol(d.target) << "(cpu);\n";
            } else {
                os << "    psp_runtime_dispatch(cpu, " << hex32(d.target) << ");\n";
            }
            os << "    if (cpu->halted || cpu->vblank_yield) { cpu->pc = " << hex32(pc + 8U)
               << "; cpu->npc = " << hex32(pc + 12U) << "; return; }\n";
        } else if (d.op == MipsOp::Jalr) {
            os << "    { uint32_t _t = cpu->gpr[" << static_cast<unsigned>(d.rs) << "]; ";
            if (d.rd != 0)
                os << "cpu->gpr[" << static_cast<unsigned>(d.rd) << "] = " << hex32(pc + 8U)
                   << ";\n";
            else
                os << "\n";
            if (has_ds)
                emit_non_branch_stmt(os, ds_it->second, "      ");
            os << "      psp_runtime_dispatch(cpu, _t); }\n";
            os << "    if (cpu->halted || cpu->vblank_yield) { cpu->pc = " << hex32(pc + 8U)
               << "; cpu->npc = " << hex32(pc + 12U) << "; return; }\n";
        } else if (d.op == MipsOp::Jr) {
            os << "    { uint32_t _t = cpu->gpr[" << static_cast<unsigned>(d.rs) << "];\n";
            if (has_ds)
                emit_non_branch_stmt(os, ds_it->second, "      ");
            if (d.is_return) {
                os << "      cpu->pc = _t; cpu->npc = _t + 4U; return; }\n";
            } else {
                os << "      psp_runtime_dispatch(cpu, _t); return; }\n";
            }
        } else if (d.op == MipsOp::J) {
            if (has_ds)
                emit_non_branch_stmt(os, ds_it->second, "    ");
            if (fn.local_labels.count(d.target) != 0U && fn.instructions.count(d.target) != 0U) {
                char lbl[32];
                std::snprintf(lbl, sizeof(lbl), "L_%08X", d.target);
                os << "    goto " << lbl << ";\n";
            } else if (cfg.functions.count(d.target) != 0U) {
                os << "    " << func_symbol(d.target) << "(cpu); return;\n";
            } else {
                os << "    psp_runtime_dispatch(cpu, " << hex32(d.target) << "); return;\n";
            }
        } else if (d.is_branch) {
            const std::string cond = branch_cond_expr(d);
            const bool is_local =
                (fn.local_labels.count(d.target) != 0U && fn.instructions.count(d.target) != 0U);
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "L_%08X", d.target);

            if (d.is_call) {
                os << "    cpu->gpr[31] = " << hex32(pc + 8U) << ";\n";
            }

            if (d.is_branch_likely) {
                os << "    if (" << cond << ") {\n";
                if (has_ds)
                    emit_non_branch_stmt(os, ds_it->second, "        ");
                if (is_local)
                    os << "        goto " << lbl << ";\n";
                else
                    os << "        psp_runtime_dispatch(cpu, " << hex32(d.target) << "); return;\n";
                os << "    }\n";
            } else {
                os << "    { int _c = (" << cond << ");\n";
                if (has_ds)
                    emit_non_branch_stmt(os, ds_it->second, "      ");
                if (is_local)
                    os << "      if (_c) goto " << lbl << "; }\n";
                else
                    os << "      if (_c) { psp_runtime_dispatch(cpu, " << hex32(d.target)
                       << "); return; } }\n";
            }
        }
    }

    os << "    cpu->pc = " << hex32(fn.end_addr) << "; cpu->npc = " << hex32(fn.end_addr + 4U)
       << ";\n";
    os << "}\n\n";
}

}  // namespace

EmitResult emit_psp_c99(const PspCfg& cfg, const dragon::GameConfig& config,
                        const std::filesystem::path& out_dir, const std::string& rom_sha1,
                        const std::string& default_rom_path) {
    (void)config;
    std::filesystem::create_directories(out_dir);
    EmitResult res{};

    // 1. psp_recomp.h
    {
        std::ostringstream hdr;
        hdr << "#ifndef DRAGON_PSP_RECOMP_GENERATED_H\n";
        hdr << "#define DRAGON_PSP_RECOMP_GENERATED_H\n\n";
        hdr << "#include \"psp/cpu_state.h\"\n\n";
        hdr << "static inline uint32_t psp_rotr32(uint32_t v, uint32_t sh) {\n";
        hdr << "    sh &= 31U;\n";
        hdr << "    return (sh == 0U) ? v : ((v >> sh) | (v << (32U - sh)));\n";
        hdr << "}\n\n";
        for (const auto& [addr, fn] : cfg.functions) {
            (void)fn;
            hdr << "void " << func_symbol(addr) << "(PspCpuState* cpu);\n";
        }
        hdr << "\n#endif\n";

        std::ofstream f(out_dir / "psp_recomp.h", std::ios::binary);
        f << hdr.str();
        ++res.files_written;
    }

    // 2. Partition functions across C99 files (~256 funcs per TU for fast parallel clang builds)
    std::vector<const PspFunction*> all_funcs;
    all_funcs.reserve(cfg.functions.size());
    for (const auto& [addr, fn] : cfg.functions) {
        (void)addr;
        all_funcs.push_back(&fn);
    }

    constexpr size_t kFuncsPerFile = 256U;
    const size_t num_chunks =
        std::max<size_t>(1U, (all_funcs.size() + kFuncsPerFile - 1U) / kFuncsPerFile);
    for (size_t c = 0; c < num_chunks; ++c) {
        std::ostringstream src;
        src << "#include \"psp_recomp.h\"\n\n";
        const size_t begin = c * kFuncsPerFile;
        const size_t end = std::min(all_funcs.size(), begin + kFuncsPerFile);
        for (size_t i = begin; i < end; ++i) {
            emit_function(src, *all_funcs[i], cfg);
            ++res.functions_emitted;
            res.instructions_emitted += all_funcs[i]->instructions.size();
        }
        char fname[32];
        std::snprintf(fname, sizeof(fname), "psp_funcs_%02zu.c", c);
        std::ofstream f(out_dir / fname, std::ios::binary);
        f << src.str();
        ++res.files_written;
    }

    // 3. psp_dispatch.c
    {
        std::ostringstream disp;
        disp << "#include \"psp_recomp.h\"\n\n";
        disp << "const char* g_psp_expected_rom_sha1 = \"" << escape_c_string(rom_sha1) << "\";\n";
        disp << "const char* g_psp_default_rom_path = \"" << escape_c_string(default_rom_path)
             << "\";\n\n";
        disp << "const PspRecompFuncEntry g_psp_recomp_functions[] = {\n";
        for (const auto& [addr, fn] : cfg.functions) {
            (void)fn;
            disp << "    { " << hex32(addr) << ", " << func_symbol(addr) << " },\n";
        }
        if (cfg.functions.empty()) {
            disp << "    { 0U, NULL },\n";
        }
        disp << "};\n";
        disp << "const size_t g_psp_recomp_function_count = " << cfg.functions.size() << "U;\n";

        std::ofstream f(out_dir / "psp_dispatch.c", std::ios::binary);
        f << disp.str();
        ++res.files_written;
    }

    return res;
}

}  // namespace dragon::psprecomp
