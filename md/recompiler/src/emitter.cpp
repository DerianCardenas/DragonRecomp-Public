#include "mdrecomp/emitter.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace dragon::mdrecomp {

namespace {

std::string hex6(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06X", v & 0x00FFFFFFU);
    return buf;
}

std::string escape_c_str(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
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

void emit_insn(std::ostream& out, const DecodedInsn& insn, const FunctionCfg& fn,
               const ProgramCfg& cfg) {
    const uint32_t next_pc = (insn.addr + insn.length) & 0x00FFFFFFU;
    const uint16_t op = insn.op;

    // Poll hardware on loop back-edges or calls
    auto emit_poll = [&]() {
        out << "    if (cpu->cycles >= cpu->next_event) { if (md_runtime_poll(cpu)) return; }\n";
    };

    // Direct C99 translation of common hot instructions & control flow
    if (op == 0x4E71U) {  // NOP
        out << "    cpu->cycles += 4U;\n";
        return;
    }
    if ((op & 0xF100U) == 0x7000U) {  // MOVEQ #imm8, Dn
        const uint8_t dn = static_cast<uint8_t>((op >> 9U) & 7U);
        const uint32_t v = static_cast<uint32_t>(static_cast<int8_t>(op & 0xFFU));
        out << "    cpu->d[" << static_cast<int>(dn) << "] = 0x" << std::hex << v << std::dec
            << "U; md_set_nz32(cpu, cpu->d[" << static_cast<int>(dn) << "]); cpu->cycles += 4U;\n";
        return;
    }
    if ((op & 0xFFF8U) == 0x4840U) {  // SWAP Dn
        const uint8_t dn = static_cast<uint8_t>(op & 7U);
        out << "    cpu->d[" << static_cast<int>(dn) << "] = (cpu->d[" << static_cast<int>(dn)
            << "] >> 16U) | (cpu->d[" << static_cast<int>(dn)
            << "] << 16U); md_set_nz32(cpu, cpu->d[" << static_cast<int>(dn)
            << "]); cpu->cycles += 4U;\n";
        return;
    }
    if (op == 0x4E75U) {  // RTS
        out << "    cpu->pc = md_bus_read32(cpu, cpu->a[7]) & 0x00FFFFFFU; cpu->a[7] += 4U; "
               "cpu->cycles += 16U; return;\n";
        return;
    }
    if (op == 0x4E73U) {  // RTE
        out << "    { uint16_t nsr = md_bus_read16(cpu, cpu->a[7]); cpu->a[7] += 2U; "
               "uint32_t npc = md_bus_read32(cpu, cpu->a[7]) & 0x00FFFFFFU; cpu->a[7] += 4U; "
               "md_set_sr(cpu, nsr); cpu->pc = npc; cpu->cycles += 20U; return; }\n";
        return;
    }
    if (op == 0x4E72U) {  // STOP #imm
        out << "    cpu->pc = 0x" << hex6(next_pc) << "U; if (md_runtime_stop(cpu, 0x" << std::hex
            << insn.ext0 << std::dec << "U)) return;\n";
        return;
    }
    if (insn.flow == FlowKind::BranchUncond && insn.has_target) {
        out << "    cpu->cycles += 10U;\n";
        if (insn.target <= insn.addr) {
            emit_poll();
        }
        if (fn.insns.count(insn.target) && fn.internal_labels.count(insn.target)) {
            out << "    goto L_" << hex6(insn.target) << ";\n";
        } else if (cfg.functions.count(insn.target)) {
            out << "    func_" << hex6(insn.target) << "(cpu); return;\n";
        } else {
            out << "    md_runtime_dispatch(cpu, 0x" << hex6(insn.target) << "U, 0x"
                << hex6(insn.addr) << "U); return;\n";
        }
        return;
    }
    if (insn.flow == FlowKind::Call && insn.has_target) {
        out << "    cpu->a[7] -= 4U; md_bus_write32(cpu, cpu->a[7], 0x" << hex6(next_pc)
            << "U); cpu->cycles += 18U;\n";
        emit_poll();
        if (cfg.functions.count(insn.target)) {
            out << "    func_" << hex6(insn.target) << "(cpu);\n";
        } else {
            out << "    md_runtime_dispatch(cpu, 0x" << hex6(insn.target) << "U, 0x"
                << hex6(insn.addr) << "U);\n";
        }
        out << "    if (cpu->stopped == 2U) return;\n";
        out << "    if (cpu->pc != 0x" << hex6(next_pc)
            << "U) { md_runtime_dispatch(cpu, cpu->pc, 0x" << hex6(insn.addr) << "U); return; }\n";
        return;
    }
    if (insn.flow == FlowKind::BranchCond && insn.has_target) {
        const uint8_t line = static_cast<uint8_t>(op >> 12U);
        const uint8_t cc = static_cast<uint8_t>((op >> 8U) & 0x0FU);
        if (line == 0x6U) {  // Bcc
            out << "    if (md_cc_test(cpu->sr, " << static_cast<int>(cc)
                << "U)) { cpu->cycles += 10U; ";
            if (insn.target <= insn.addr) {
                out << "if (cpu->cycles >= cpu->next_event) { if (md_runtime_poll(cpu)) return; } ";
            }
            if (fn.insns.count(insn.target) && fn.internal_labels.count(insn.target)) {
                out << "goto L_" << hex6(insn.target) << "; ";
            } else {
                out << "md_runtime_dispatch(cpu, 0x" << hex6(insn.target) << "U, 0x"
                    << hex6(insn.addr) << "U); return; ";
            }
            out << "} else { cpu->cycles += 8U; }\n";
            return;
        }
        if (line == 0x5U) {  // DBcc
            const uint8_t dn = static_cast<uint8_t>(op & 7U);
            out << "    if (!md_cc_test(cpu->sr, " << static_cast<int>(cc)
                << "U)) { uint16_t cnt = (uint16_t)((cpu->d[" << static_cast<int>(dn)
                << "] & 0xFFFFU) - 1U); cpu->d[" << static_cast<int>(dn) << "] = (cpu->d["
                << static_cast<int>(dn)
                << "] & 0xFFFF0000U) | cnt; if (cnt != 0xFFFFU) { cpu->cycles += 10U; ";
            if (insn.target <= insn.addr) {
                out << "if (cpu->cycles >= cpu->next_event) { if (md_runtime_poll(cpu)) return; } ";
            }
            if (fn.insns.count(insn.target) && fn.internal_labels.count(insn.target)) {
                out << "goto L_" << hex6(insn.target) << "; ";
            } else {
                out << "md_runtime_dispatch(cpu, 0x" << hex6(insn.target) << "U, 0x"
                    << hex6(insn.addr) << "U); return; ";
            }
            out << "} else { cpu->cycles += 14U; } } else { cpu->cycles += 12U; }\n";
            return;
        }
    }
    if (insn.flow == FlowKind::JumpIndirect) {
        out << "    cpu->pc = 0x" << hex6(insn.addr) << "U; md_runtime_step(cpu);\n";
        emit_poll();
        out << "    md_runtime_dispatch(cpu, cpu->pc, 0x" << hex6(insn.addr) << "U); return;\n";
        return;
    }
    if (insn.flow == FlowKind::CallIndirect) {
        out << "    cpu->pc = 0x" << hex6(insn.addr) << "U; md_runtime_step(cpu);\n";
        emit_poll();
        out << "    md_runtime_dispatch(cpu, cpu->pc, 0x" << hex6(insn.addr) << "U);\n";
        out << "    if (cpu->stopped == 2U) return;\n";
        out << "    if (cpu->pc != 0x" << hex6(next_pc)
            << "U) { md_runtime_dispatch(cpu, cpu->pc, 0x" << hex6(insn.addr) << "U); return; }\n";
        return;
    }

    // All other M68000 instructions delegate cleanly to cycle-accurate md_runtime_step(cpu)
    out << "    cpu->pc = 0x" << hex6(insn.addr) << "U; md_runtime_step(cpu);\n";
}

}  // namespace

EmitResult emit_c99(const dragon::md::Cartridge& cart, const ProgramCfg& cfg,
                    const EmitOptions& opts) {
    EmitResult res;
    std::error_code ec;
    std::filesystem::create_directories(opts.output_dir, ec);
    if (ec) {
        res.error_message = "Failed to create output directory: " + opts.output_dir.string();
        return res;
    }

    // 1. recomp_decls.h
    {
        std::ofstream hdr(opts.output_dir / "recomp_decls.h");
        if (!hdr) {
            res.error_message = "Cannot write recomp_decls.h";
            return res;
        }
        hdr << "#pragma once\n#include \"md/cpu_state.h\"\n\n";
        if (!opts.interpreter_only) {
            for (const auto& [addr, fn] : cfg.functions) {
                (void)fn;
                hdr << "void func_" << hex6(addr) << "(MdCpuState* cpu);\n";
            }
        }
        ++res.files_written;
    }

    // 2. recomp_dispatch.c
    {
        std::ofstream disp(opts.output_dir / "recomp_dispatch.c");
        if (!disp) {
            res.error_message = "Cannot write recomp_dispatch.c";
            return res;
        }
        disp << "#include \"recomp_decls.h\"\n\n";
        disp << "const char* md_recomp_default_rom_path = \""
             << escape_c_str(opts.rom_path.string()) << "\";\n";
        disp << "const char* md_recomp_expected_sha1 = \"" << cart.sha1() << "\";\n\n";
        disp << "const MdRecompFuncEntry md_recomp_functions[] = {\n";
        if (!opts.interpreter_only) {
            for (const auto& [addr, fn] : cfg.functions) {
                (void)fn;
                disp << "    { 0x" << hex6(addr) << "U, func_" << hex6(addr) << " },\n";
            }
        }
        disp << "    { 0U, NULL }\n};\n\n";
        disp << "const size_t md_recomp_function_count = "
             << (opts.interpreter_only ? 0U : cfg.functions.size()) << "U;\n";
        ++res.files_written;
    }

    // 3. recomp_funcs_N.c
    if (!opts.interpreter_only && !cfg.functions.empty()) {
        size_t file_idx = 0;
        size_t count_in_file = 0;
        std::ofstream cur;

        auto open_next = [&]() -> bool {
            if (cur.is_open()) {
                cur.close();
            }
            const auto path =
                opts.output_dir / ("recomp_funcs_" + std::to_string(file_idx++) + ".c");
            cur.open(path);
            if (!cur)
                return false;
            cur << "#include \"recomp_decls.h\"\n\n";
            count_in_file = 0;
            ++res.files_written;
            return true;
        };

        for (const auto& [entry, fn] : cfg.functions) {
            if (!cur.is_open() || count_in_file >= opts.funcs_per_file) {
                if (!open_next()) {
                    res.error_message = "Cannot write recomp_funcs file";
                    return res;
                }
            }
            cur << "void func_" << hex6(entry) << "(MdCpuState* cpu) {\n";
            cur << "    if (cpu->stopped == 2U) return;\n";
            if (!fn.insns.empty() && fn.insns.begin()->first != entry) {
                cur << "    goto L_" << hex6(entry) << ";\n";
            }
            for (const auto& [addr, insn] : fn.insns) {
                if (fn.internal_labels.count(addr)) {
                    cur << "L_" << hex6(addr) << ":\n";
                }
                emit_insn(cur, insn, fn, cfg);
            }
            cur << "    return;\n}\n\n";
            ++count_in_file;
        }
    }

    // 4. report.toml
    {
        std::ofstream rep(opts.output_dir / "report.toml");
        if (rep) {
            rep << "platform = \"genesis\"\n";
            rep << "game = \"" << escape_c_str(opts.game_name) << "\"\n";
            rep << "sha1 = \"" << cart.sha1() << "\"\n";
            rep << "interpreter_only = " << (opts.interpreter_only ? "true" : "false") << "\n";
            rep << "functions = " << (opts.interpreter_only ? 0U : cfg.functions.size()) << "\n";
            rep << "blocks = " << (opts.interpreter_only ? 0U : cfg.total_blocks) << "\n";
            rep << "instructions = " << (opts.interpreter_only ? 0U : cfg.total_instructions)
                << "\n";
            rep << "indirect_jumps = " << cfg.indirect_jumps << "\n";
            ++res.files_written;
        }
    }

    res.success = true;
    return res;
}

}  // namespace dragon::mdrecomp
