#include "ndsrecomp/emitter.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

namespace dragon::ndsrecomp {

namespace {

std::string escape_c_str(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8U);
    for (char c : s) {
        if (c == '\\' || c == '"')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

void emit_function_body(std::ostringstream& os, const Function& fn, const ProgramCFG& cfg) {
    (void)cfg;
    std::set<uint32_t> needed_labels;
    needed_labels.insert(fn.entry_addr);
    for (const auto& [addr, ins] : fn.instructions) {
        (void)addr;
        if (ins.has_branch_target && fn.instructions.count(ins.branch_target) != 0U) {
            needed_labels.insert(ins.branch_target);
        }
    }

    os << "void " << fn.c_symbol() << "(NdsCpuState* cpu) {\n";
    os << "    goto L_" << std::hex << std::uppercase << fn.entry_addr << std::dec << ";\n";

    for (const auto& [addr, ins] : fn.instructions) {
        if (needed_labels.count(addr) != 0U) {
            os << "L_" << std::hex << std::uppercase << addr << std::dec << ":\n";
        }
        os << "    cpu->r[15] = 0x" << std::hex << addr << "U;\n" << std::dec;
        os << "    if (cpu->cycles >= cpu->next_event) { nds_runtime_poll(cpu); "
              "if (cpu->halted == 2U) return; }\n";

        if (!ins.thumb) {
            const uint32_t w = ins.raw;
            const uint32_t cond = w >> 28U;
            // ARM BL / BLX <label>
            if (cond == 0xFU && ((w & 0xFE000000U) == 0xFA000000U)) {
                os << "    cpu->r[14] = 0x" << std::hex << (addr + 4U)
                   << "U; cpu->cycles += 3ULL;\n"
                   << "    nds_runtime_dispatch(cpu, 0x" << (ins.branch_target | 1U) << "U, 0x"
                   << addr << "U);\n"
                   << "    if (cpu->r[15] != 0x" << (addr + 4U)
                   << "U || cpu->thumb != 0U || cpu->halted == 2U) return;\n"
                   << std::dec;
                continue;
            }
            if (cond != 0xFU && ((w & 0x0E000000U) == 0x0A000000U)) {
                const bool is_bl = (w & 0x01000000U) != 0U;
                const bool is_local = (!is_bl) && (fn.instructions.count(ins.branch_target) != 0U);
                if (cond != 0xEU) {
                    os << "    if (nds_cond_passed(cpu, " << cond << "U)) {\n";
                }
                if (is_bl) {
                    os << "        cpu->r[14] = 0x" << std::hex << (addr + 4U)
                       << "U; cpu->cycles += 3ULL;\n"
                       << "        nds_runtime_dispatch(cpu, 0x" << ins.branch_target << "U, 0x"
                       << addr << "U);\n"
                       << "        if (cpu->r[15] != 0x" << (addr + 4U)
                       << "U || cpu->thumb != 0U || cpu->halted == 2U) return;\n"
                       << std::dec;
                } else if (is_local) {
                    os << "        cpu->cycles += 3ULL; goto L_" << std::hex << std::uppercase
                       << ins.branch_target << std::dec << ";\n";
                } else {
                    os << "        cpu->cycles += 3ULL; nds_runtime_dispatch(cpu, 0x" << std::hex
                       << ins.branch_target << "U, 0x" << addr << "U); return;\n"
                       << std::dec;
                }
                if (cond != 0xEU) {
                    os << "    }\n";
                } else if (!is_bl) {
                    continue;
                }
                continue;
            }
            // ARM BX LR -> return
            if (w == 0xE12FFF1EU) {
                os << "    cpu->thumb = (uint8_t)(cpu->r[14] & 1U); "
                   << "cpu->r[15] = cpu->thumb ? (cpu->r[14] & ~1U) : (cpu->r[14] & ~3U); "
                   << "cpu->cycles += 3ULL; return;\n";
                continue;
            }
            // ARM CLZ
            if ((w & 0x0FFF0FF0U) == 0x016F0F10U) {
                const uint32_t rd = (w >> 12U) & 0xFU;
                const uint32_t rm = w & 0xFU;
                if (cond != 0xEU)
                    os << "    if (nds_cond_passed(cpu, " << cond << "U)) ";
                os << "cpu->r[" << rd << "] = nds_clz32(cpu->r[" << rm
                   << "]); cpu->cycles += 2ULL;\n";
                continue;
            }
        } else {
            // Thumb 32-bit BL / BLX
            if (ins.is_thumb_bl32 || ins.is_thumb_blx32) {
                const bool target_thumb = ins.is_thumb_bl32;
                os << "    cpu->r[14] = 0x" << std::hex << ((addr + 4U) | 1U)
                   << "U; cpu->cycles += 4ULL;\n"
                   << "    nds_runtime_dispatch(cpu, 0x"
                   << (ins.branch_target | (target_thumb ? 1U : 0U)) << "U, 0x" << addr << "U);\n"
                   << "    if (cpu->r[15] != 0x" << (addr + 4U)
                   << "U || cpu->thumb != 1U || cpu->halted == 2U) return;\n"
                   << std::dec;
                continue;
            }
            // Thumb BX LR -> return
            if (ins.raw == 0x4770U) {
                os << "    cpu->thumb = (uint8_t)(cpu->r[14] & 1U); "
                   << "cpu->r[15] = cpu->thumb ? (cpu->r[14] & ~1U) : (cpu->r[14] & ~3U); "
                   << "cpu->cycles += 3ULL; return;\n";
                continue;
            }
            // Thumb local B / Bcc
            if (ins.has_branch_target && fn.instructions.count(ins.branch_target) != 0U) {
                if ((ins.raw >> 12U) == 0x0DU) {
                    const uint32_t cond = (ins.raw >> 8U) & 0xFU;
                    os << "    if (nds_cond_passed(cpu, " << cond << "U)) { cpu->cycles += 3ULL; "
                       << "goto L_" << std::hex << std::uppercase << ins.branch_target << std::dec
                       << "; }\n";
                    continue;
                }
                if ((ins.raw >> 11U) == 0x1CU) {
                    os << "    cpu->cycles += 3ULL; goto L_" << std::hex << std::uppercase
                       << ins.branch_target << std::dec << ";\n";
                    continue;
                }
            }
        }

        // Exact hardware fallback step for remaining instructions (ensures 100% ARMv5TE/CP15
        // fidelity)
        const uint32_t next_pc = addr + ins.length;
        os << "    nds_runtime_step(cpu);\n";
        os << "    if (cpu->r[15] != 0x" << std::hex << next_pc
           << "U || cpu->thumb != " << (ins.thumb ? 1 : 0) << "U) return;\n"
           << std::dec;
    }

    os << "}\n\n";
}

}  // namespace

EmitResult emit_nds_c99(const ProgramCFG& cfg, const GameConfig& config,
                        const std::filesystem::path& out_dir, const std::string& rom_sha1,
                        const std::string& default_rom_path) {
    (void)config;
    std::filesystem::create_directories(out_dir);

    EmitResult res{};

    // 1. recomp_funcs.h
    {
        std::ofstream hdr(out_dir / "recomp_funcs.h");
        hdr << "#pragma once\n"
            << "#include \"nds/cpu_state.h\"\n\n";
        for (const auto& [key, fn] : cfg.functions) {
            hdr << "void " << fn.c_symbol() << "(NdsCpuState* cpu);\n";
        }
        ++res.files_written;
    }

    // 2. Partitioned recomp_funcs_<N>.c
    constexpr size_t kFuncsPerFile = 256U;
    std::vector<const Function*> all_fns;
    all_fns.reserve(cfg.functions.size());
    for (const auto& [key, fn] : cfg.functions) {
        all_fns.push_back(&fn);
    }

    const size_t chunks = (all_fns.size() + kFuncsPerFile - 1U) / kFuncsPerFile;
    const size_t num_files = (chunks == 0U) ? 1U : chunks;

    for (size_t chunk = 0; chunk < num_files; ++chunk) {
        std::ostringstream os;
        os << "#include \"recomp_funcs.h\"\n\n";
        const size_t begin = chunk * kFuncsPerFile;
        const size_t end = std::min(begin + kFuncsPerFile, all_fns.size());
        for (size_t i = begin; i < end; ++i) {
            emit_function_body(os, *all_fns[i], cfg);
            ++res.functions_emitted;
            res.instructions_emitted += all_fns[i]->instructions.size();
        }
        std::ofstream cf(out_dir / ("recomp_funcs_" + std::to_string(chunk) + ".c"));
        cf << os.str();
        ++res.files_written;
    }

    // 3. recomp_dispatch.c
    {
        std::ofstream disp(out_dir / "recomp_dispatch.c");
        disp << "#include \"recomp_funcs.h\"\n\n";
        disp << "const char* g_nds_expected_rom_sha1 = \"" << escape_c_str(rom_sha1) << "\";\n";
        disp << "const char* g_nds_default_rom_path = \"" << escape_c_str(default_rom_path)
             << "\";\n\n";
        disp << "const NdsRecompFuncEntry g_nds_recomp_functions[] = {\n";
        for (const auto* fn : all_fns) {
            char line[128];
            std::snprintf(line, sizeof(line), "    { 0x%08XU, %uU, %s },\n", fn->entry_addr,
                          fn->thumb ? 1U : 0U, fn->c_symbol().c_str());
            disp << line;
        }
        if (all_fns.empty()) {
            disp << "    { 0U, 0U, NULL },\n";
        }
        disp << "};\n\n";
        disp << "const size_t g_nds_recomp_function_count = " << all_fns.size() << "U;\n";
        ++res.files_written;
    }

    return res;
}

}  // namespace dragon::ndsrecomp
