#include "nesrecomp/disassembler.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <queue>
#include <sstream>
#include <stdexcept>

#include "nes/bus.h"

namespace dragon::nesrecomp {

std::string Function::c_symbol() const {
    char buf[32];
    if (has_bank) {
        std::snprintf(buf, sizeof(buf), "func_%02X_%04X", bank, entry_addr);
    } else {
        std::snprintf(buf, sizeof(buf), "func_%04X", entry_addr);
    }
    return buf;
}

namespace {

struct PrgWindowView {
    std::span<const uint8_t> prg_rom;
    std::array<uint32_t, 4> slot_offsets{0U, 0x2000U, 0x4000U, 0x6000U};
    uint16_t min_code_addr = 0x8000U;
    uint16_t max_code_addr = 0xFFFAU;

    uint8_t read8(uint16_t addr) const noexcept {
        if (prg_rom.empty() || addr < 0x8000U) {
            return 0U;
        }
        const uint8_t slot = static_cast<uint8_t>((addr >> 13U) & 0x03U);
        const size_t offset =
            static_cast<size_t>(slot_offsets[slot]) + static_cast<size_t>(addr & 0x1FFFU);
        return (offset < prg_rom.size()) ? prg_rom[offset] : 0U;
    }

    uint16_t read16(uint16_t addr) const noexcept {
        const uint8_t lo = read8(addr);
        const uint8_t hi = read8(static_cast<uint16_t>(addr + 1U));
        return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
    }
};

PrgWindowView make_default_view(std::span<const uint8_t> prg_rom, int mapper) {
    PrgWindowView view;
    view.prg_rom = prg_rom;
    const uint32_t sz = static_cast<uint32_t>(prg_rom.size());
    const uint32_t num_8k = (sz >= 0x2000U) ? (sz >> 13U) : 1U;
    const uint32_t last_8k = (num_8k > 0U) ? (num_8k - 1U) : 0U;
    const uint32_t second_last_8k = (num_8k > 1U) ? (num_8k - 2U) : 0U;

    if (sz <= 0x4000U) {
        view.slot_offsets = {0U, (sz >= 0x2000U) ? 0x2000U : 0U, 0U,
                             (sz >= 0x2000U) ? 0x2000U : 0U};
    } else if (mapper == 1 || mapper == 2 || mapper == 4) {
        view.slot_offsets = {0U, 0x2000U, second_last_8k * 0x2000U, last_8k * 0x2000U};
    } else if (mapper == 40) {
        view.slot_offsets = {4U * 0x2000U, 5U * 0x2000U, 0U, 7U * 0x2000U};
    } else {
        view.slot_offsets = {0U, 0x2000U, 0x4000U, 0x6000U};
    }
    return view;
}

bool is_in_configured_data(const GameConfig& config, uint16_t addr, int bank = -1) {
    for (const auto& dr : config.data_ranges) {
        if ((dr.bank < 0 || bank < 0 || dr.bank == bank) && addr >= dr.start && addr <= dr.end) {
            return true;
        }
    }
    return false;
}

bool can_reach_target(const std::map<uint16_t, BasicBlock>& blocks,
                      const std::set<uint16_t>& function_seeds, uint16_t start, uint16_t goal) {
    if (start == goal) {
        return true;
    }
    std::set<uint16_t> visited;
    std::queue<uint16_t> q;
    q.push(start);
    visited.insert(start);

    while (!q.empty()) {
        const uint16_t cur = q.front();
        q.pop();
        auto it = blocks.find(cur);
        if (it == blocks.end()) {
            continue;
        }
        for (uint16_t succ : it->second.successors) {
            if (succ == goal) {
                return true;
            }
            if (visited.find(succ) == visited.end()) {
                visited.insert(succ);
                (void)function_seeds;
                q.push(succ);
            }
        }
    }
    return false;
}

void build_blocks_and_functions(const std::map<uint16_t, Instruction>& instructions,
                                const std::set<uint16_t>& block_leaders,
                                const std::set<uint16_t>& function_seeds,
                                const std::map<uint16_t, std::string>& function_names,
                                const std::set<uint16_t>& jump_engine_targets, uint8_t bank,
                                bool has_bank, std::map<uint16_t, BasicBlock>& out_blocks,
                                std::map<uint16_t, Function>& out_functions) {
    for (uint16_t leader : block_leaders) {
        auto it = instructions.find(leader);
        if (it == instructions.end()) {
            continue;
        }

        BasicBlock block;
        block.start_addr = leader;
        uint16_t pc = leader;

        while (true) {
            auto cur_it = instructions.find(pc);
            if (cur_it == instructions.end()) {
                break;
            }
            const Instruction& instr = cur_it->second;
            block.instructions.push_back(instr);
            const uint16_t next_pc = static_cast<uint16_t>(pc + instr.length);
            block.end_addr = next_pc;

            if (nes::is_branch_mnemonic(instr.mnemonic)) {
                block.successors.push_back(instr.operand_u16());
                block.successors.push_back(next_pc);
                break;
            }
            if (instr.mnemonic == nes::Mnemonic::JMP) {
                if (instr.mode == nes::AddrMode::Absolute) {
                    block.successors.push_back(instr.operand_u16());
                }
                break;
            }
            if (instr.mnemonic == nes::Mnemonic::JSR &&
                jump_engine_targets.count(instr.operand_u16()) > 0) {
                break;
            }
            if (instr.mnemonic == nes::Mnemonic::RTS || instr.mnemonic == nes::Mnemonic::RTI ||
                instr.mnemonic == nes::Mnemonic::BRK || instr.mnemonic == nes::Mnemonic::KIL) {
                break;
            }

            if (block_leaders.count(next_pc) > 0 ||
                instructions.find(next_pc) == instructions.end()) {
                if (instructions.find(next_pc) != instructions.end()) {
                    block.successors.push_back(next_pc);
                }
                break;
            }
            pc = next_pc;
        }

        out_blocks[leader] = std::move(block);
    }

    for (uint16_t seed : function_seeds) {
        if (out_blocks.find(seed) == out_blocks.end()) {
            continue;
        }
        Function func;
        func.entry_addr = seed;
        func.bank = bank;
        func.has_bank = has_bank;
        auto name_it = function_names.find(seed);
        if (name_it != function_names.end()) {
            func.name = name_it->second;
        }

        std::queue<uint16_t> bq;
        bq.push(seed);
        func.block_addrs.insert(seed);

        while (!bq.empty()) {
            const uint16_t b_addr = bq.front();
            bq.pop();
            auto b_it = out_blocks.find(b_addr);
            if (b_it == out_blocks.end()) {
                continue;
            }
            for (uint16_t succ : b_it->second.successors) {
                if (out_blocks.find(succ) == out_blocks.end()) {
                    continue;
                }
                if (func.block_addrs.count(succ) > 0) {
                    continue;
                }
                if (succ != seed && function_seeds.count(succ) > 0) {
                    if (!can_reach_target(out_blocks, function_seeds, succ, seed)) {
                        continue;
                    }
                }
                func.block_addrs.insert(succ);
                bq.push(succ);
            }
        }

        out_functions[seed] = std::move(func);
    }
}

void analyze_window(
    const PrgWindowView& view, const GameConfig& config, int bank_id, uint8_t func_bank,
    bool has_bank, const std::vector<std::pair<uint16_t, std::string>>& initial_seeds,
    const std::set<uint16_t>& jump_engine_targets,
    std::map<uint16_t, Instruction>& out_instructions, std::map<uint16_t, BasicBlock>& out_blocks,
    std::map<uint16_t, Function>& out_functions, std::vector<JumpEngineTable>& out_je_tables,
    std::vector<IndirectJumpSite>& out_indirect_jumps, std::vector<uint16_t>& out_warnings,
    std::set<uint16_t>& out_external_call_targets) {
    std::set<uint16_t> function_seeds;
    std::map<uint16_t, std::string> function_names;
    std::set<uint16_t> block_leaders;
    std::queue<uint16_t> worklist;

    auto add_function_seed = [&](uint16_t addr, std::string_view name = {}) {
        if (addr < view.min_code_addr || addr >= view.max_code_addr) {
            if (addr >= 0x8000U && addr < 0xFFFAU) {
                out_external_call_targets.insert(addr);
            }
            return false;
        }
        if (is_in_configured_data(config, addr, bank_id)) {
            return false;
        }
        const bool is_new = function_seeds.insert(addr).second;
        block_leaders.insert(addr);
        if (!name.empty() && function_names[addr].empty()) {
            function_names[addr] = std::string(name);
        }
        if (is_new) {
            worklist.push(addr);
        }
        return is_new;
    };

    for (const auto& [addr, name] : initial_seeds) {
        add_function_seed(addr, name);
    }

    for (const auto& jt : config.jump_tables) {
        if (jt.bank >= 0 && bank_id >= 0 && jt.bank != bank_id) {
            continue;
        }
        for (size_t i = 0; i < jt.count; ++i) {
            uint16_t target = 0;
            if (jt.format == JumpTableFormat::Word) {
                target = view.read16(static_cast<uint16_t>(jt.addr + i * 2));
            } else if (jt.format == JumpTableFormat::WordMinusOne) {
                target =
                    static_cast<uint16_t>(view.read16(static_cast<uint16_t>(jt.addr + i * 2)) + 1U);
            } else if (jt.format == JumpTableFormat::Split) {
                const uint8_t lo = view.read8(static_cast<uint16_t>(jt.lo + i));
                const uint8_t hi = view.read8(static_cast<uint16_t>(jt.hi + i));
                target = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
            }
            if (target >= 0x8000U) {
                add_function_seed(target);
            }
        }
    }

    std::set<uint16_t> jump_engine_call_sites;
    std::set<uint16_t> warned_unofficial;
    std::map<uint16_t, uint16_t> seen_indirect_jmps;

    while (true) {
        while (!worklist.empty()) {
            uint16_t pc = worklist.front();
            worklist.pop();

            while (pc >= view.min_code_addr && pc < view.max_code_addr &&
                   !is_in_configured_data(config, pc, bank_id)) {
                if (out_instructions.find(pc) != out_instructions.end()) {
                    break;
                }

                const uint8_t op = view.read8(pc);
                const nes::OpcodeInfo& info = nes::get_opcode_info(op);

                if (!info.official && !(config.allow_unofficial && info.stable_unofficial)) {
                    if (warned_unofficial.insert(pc).second) {
                        out_warnings.push_back(pc);
                    }
                    break;
                }

                if (static_cast<uint32_t>(pc) + info.length > view.max_code_addr) {
                    break;
                }

                Instruction instr;
                instr.addr = pc;
                instr.opcode = op;
                instr.length = info.length;
                instr.mnemonic = info.mnemonic;
                instr.mode = info.mode;
                instr.base_cycles = info.base_cycles;
                instr.page_cross_penalty = info.page_cross_penalty;
                instr.official = info.official;
                instr.stable_unofficial = info.stable_unofficial;
                if (info.length >= 2) {
                    instr.op1 = view.read8(static_cast<uint16_t>(pc + 1U));
                }
                if (info.length >= 3) {
                    instr.op2 = view.read8(static_cast<uint16_t>(pc + 2U));
                }

                out_instructions[pc] = instr;

                if (nes::is_branch_mnemonic(instr.mnemonic)) {
                    const uint16_t target = instr.operand_u16();
                    const uint16_t next_pc = static_cast<uint16_t>(pc + instr.length);
                    block_leaders.insert(target);
                    block_leaders.insert(next_pc);
                    if (target >= view.min_code_addr && target < view.max_code_addr &&
                        out_instructions.find(target) == out_instructions.end()) {
                        worklist.push(target);
                    } else if (target >= 0x8000U && target < 0xFFFAU) {
                        out_external_call_targets.insert(target);
                    }
                    pc = next_pc;
                } else if (instr.mnemonic == nes::Mnemonic::JMP) {
                    if (instr.mode == nes::AddrMode::Absolute) {
                        const uint16_t target = instr.operand_u16();
                        block_leaders.insert(target);
                        if (target >= view.min_code_addr && target < view.max_code_addr &&
                            out_instructions.find(target) == out_instructions.end()) {
                            worklist.push(target);
                        } else if (target >= 0x8000U && target < 0xFFFAU) {
                            out_external_call_targets.insert(target);
                        }
                    } else {
                        seen_indirect_jmps[pc] = instr.operand_u16();
                    }
                    break;
                } else if (instr.mnemonic == nes::Mnemonic::JSR) {
                    const uint16_t target = instr.operand_u16();
                    add_function_seed(target);
                    if (jump_engine_targets.count(target) > 0) {
                        jump_engine_call_sites.insert(pc);
                        break;
                    }
                    pc = static_cast<uint16_t>(pc + instr.length);
                } else if (instr.mnemonic == nes::Mnemonic::RTS ||
                           instr.mnemonic == nes::Mnemonic::RTI ||
                           instr.mnemonic == nes::Mnemonic::BRK ||
                           instr.mnemonic == nes::Mnemonic::KIL) {
                    break;
                } else {
                    pc = static_cast<uint16_t>(pc + instr.length);
                }
            }
        }

        // Expand inline word tables after JSR <jump_engine>.
        bool added_new_seed = false;
        out_je_tables.clear();
        for (uint16_t jsr_pc : jump_engine_call_sites) {
            const uint16_t tbl_start = static_cast<uint16_t>(jsr_pc + 3U);
            uint32_t min_after = view.max_code_addr;
            for (const auto& [instr_addr, _] : out_instructions) {
                if (instr_addr > tbl_start && instr_addr < min_after) {
                    min_after = instr_addr;
                }
            }
            for (uint16_t leader : block_leaders) {
                if (leader > tbl_start && leader < min_after) {
                    min_after = leader;
                }
            }

            JumpEngineTable table;
            table.jsr_addr = jsr_pc;
            table.table_addr = tbl_start;

            uint32_t pos = tbl_start;
            while (pos + 2U <= min_after && pos + 2U <= view.max_code_addr) {
                if (is_in_configured_data(config, static_cast<uint16_t>(pos), bank_id)) {
                    break;
                }
                const uint16_t w = view.read16(static_cast<uint16_t>(pos));
                if (w < 0x8000U || w >= 0xFFFAU || is_in_configured_data(config, w, bank_id)) {
                    break;
                }
                table.entries.push_back(w);
                if (w > tbl_start && w < min_after) {
                    min_after = w;
                }
                if (add_function_seed(w)) {
                    added_new_seed = true;
                }
                pos += 2U;
            }
            out_je_tables.push_back(std::move(table));
        }

        if (!added_new_seed && worklist.empty()) {
            break;
        }
    }

    build_blocks_and_functions(out_instructions, block_leaders, function_seeds, function_names,
                               jump_engine_targets, func_bank, has_bank, out_blocks, out_functions);

    for (const auto& [jmp_pc, ptr_addr] : seen_indirect_jmps) {
        IndirectJumpSite site;
        site.instr_addr = jmp_pc;
        site.ptr_addr = ptr_addr;
        for (uint16_t je_target : jump_engine_targets) {
            auto f_it = out_functions.find(je_target);
            if (f_it != out_functions.end()) {
                for (uint16_t b_addr : f_it->second.block_addrs) {
                    const BasicBlock& b = out_blocks.at(b_addr);
                    if (jmp_pc >= b.start_addr && jmp_pc < b.end_addr) {
                        site.is_jump_engine = true;
                        break;
                    }
                }
            }
        }
        out_indirect_jumps.push_back(site);
    }
}

}  // namespace

ProgramCFG analyze_prg_rom(std::span<const uint8_t> prg_rom, const GameConfig& config) {
    if (prg_rom.empty()) {
        throw std::runtime_error("Cannot analyze empty PRG ROM");
    }

    ProgramCFG cfg;
    for (uint32_t je : config.jump_engine) {
        cfg.jump_engine_targets.insert(static_cast<uint16_t>(je));
    }
    for (const auto& idc : config.inline_data_calls) {
        if (idc.kind == "jump_engine") {
            cfg.jump_engine_targets.insert(static_cast<uint16_t>(idc.target));
        }
    }

    const PrgWindowView default_view = make_default_view(prg_rom, config.mapper);
    cfg.nmi_addr = default_view.read16(0xFFFAU);
    cfg.reset_addr = default_view.read16(0xFFFCU);
    cfg.irq_addr = default_view.read16(0xFFFEU);

    // Games flagged interpreter_only emit no recompiled functions; the native
    // build then runs the cycle-accurate interpreter. Used for titles whose
    // interrupt handlers depend on cycle-exact timing a block recompiler cannot
    // reproduce (stack-trick "wait for NMI" routines, e.g. the SMB2(J) hack).
    if (config.interpreter_only) {
        cfg.fixed_window_start = 0x8000U;
        return cfg;
    }

    const bool is_banked = (config.mapper == 1 || config.mapper == 2 || config.mapper == 4);
    cfg.is_banked = is_banked;

    // Mappers whose PRG stays fixed across $8000-$FFFF can be recompiled as a
    // single flat window. Mappers 0 (NROM) and 3 (CNROM, CHR-only switching)
    // qualify; 99 (Vs. System) uses a fixed PRG for the supported titles. Any
    // other non-banked mapper (e.g. 189, which swaps the whole 32 KiB window)
    // cannot be modelled by the flat or fixed-window+bank schemes here: emitting
    // bank-agnostic functions would run the wrong bank's code after a PRG swap.
    // For those we emit no functions and let the native build fall back to the
    // interpreter, which banks correctly through the Bus.
    const bool is_flat_safe = (config.mapper == 0 || config.mapper == 3 || config.mapper == 99);

    if (!is_banked) {
        cfg.fixed_window_start = 0x8000U;
        if (!is_flat_safe) {
            return cfg;  // interpreter-only (no recompiled functions)
        }
        std::vector<std::pair<uint16_t, std::string>> seeds = {
            {cfg.reset_addr, "RESET"},
            {cfg.nmi_addr, "NMI"},
            {cfg.irq_addr, "IRQ"},
        };
        for (const auto& ep : config.entry_points) {
            seeds.emplace_back(static_cast<uint16_t>(ep.addr), ep.name);
        }
        std::set<uint16_t> external_targets;
        analyze_window(default_view, config, -1, 0U, false, seeds, cfg.jump_engine_targets,
                       cfg.instructions, cfg.blocks, cfg.functions, cfg.jump_engine_tables,
                       cfg.indirect_jumps, cfg.unofficial_warnings, external_targets);
        return cfg;
    }

    // Banked mapper analysis (Mappers 1, 2, 4)
    cfg.fixed_window_start = 0xC000U;
    PrgWindowView fixed_view = default_view;
    fixed_view.min_code_addr = cfg.fixed_window_start;
    fixed_view.max_code_addr = 0xFFFAU;

    const uint32_t bank_size = (config.mapper == 4) ? 0x2000U : 0x4000U;
    const size_t num_banks = std::max<size_t>(1U, prg_rom.size() / bank_size);

    std::vector<std::pair<uint16_t, std::string>> fixed_seeds = {
        {cfg.reset_addr, "RESET"},
        {cfg.nmi_addr, "NMI"},
        {cfg.irq_addr, "IRQ"},
    };
    for (const auto& ep : config.entry_points) {
        if (ep.addr >= cfg.fixed_window_start &&
            (ep.bank < 0 || static_cast<size_t>(ep.bank) >= num_banks - 2)) {
            fixed_seeds.emplace_back(static_cast<uint16_t>(ep.addr), ep.name);
        }
    }

    std::set<uint16_t> cross_bank_targets;
    if (cfg.reset_addr >= 0x8000U && cfg.reset_addr < cfg.fixed_window_start) {
        cross_bank_targets.insert(cfg.reset_addr);
    }
    if (cfg.nmi_addr >= 0x8000U && cfg.nmi_addr < cfg.fixed_window_start) {
        cross_bank_targets.insert(cfg.nmi_addr);
    }
    if (cfg.irq_addr >= 0x8000U && cfg.irq_addr < cfg.fixed_window_start) {
        cross_bank_targets.insert(cfg.irq_addr);
    }

    analyze_window(fixed_view, config, -1, nes::Bus::kAnyBank, false, fixed_seeds,
                   cfg.jump_engine_targets, cfg.instructions, cfg.blocks, cfg.functions,
                   cfg.jump_engine_tables, cfg.indirect_jumps, cfg.unofficial_warnings,
                   cross_bank_targets);

    std::set<uint16_t> extra_fixed_targets;

    for (size_t b = 0; b < num_banks; ++b) {
        std::vector<std::pair<uint16_t, std::string>> bank_seeds;
        for (uint16_t t : cross_bank_targets) {
            if (t >= 0x8000U && t < cfg.fixed_window_start) {
                bank_seeds.emplace_back(t, "");
            }
        }
        for (const auto& ep : config.entry_points) {
            if (ep.addr >= 0x8000U && ep.addr < cfg.fixed_window_start &&
                (ep.bank < 0 || static_cast<size_t>(ep.bank) == b)) {
                bank_seeds.emplace_back(static_cast<uint16_t>(ep.addr), ep.name);
            }
        }
        if (bank_seeds.empty()) {
            continue;
        }

        PrgWindowView bank_view = default_view;
        bank_view.min_code_addr = 0x8000U;
        bank_view.max_code_addr = cfg.fixed_window_start;
        if (config.mapper == 4) {
            const uint32_t off = static_cast<uint32_t>(b * 0x2000U);
            bank_view.slot_offsets[0] = off;
            bank_view.slot_offsets[1] = off;
        } else {
            const uint32_t off = static_cast<uint32_t>(b * 0x4000U);
            bank_view.slot_offsets[0] = off;
            bank_view.slot_offsets[1] = off + 0x2000U;
        }

        BankCFG bcfg;
        bcfg.bank = static_cast<uint8_t>(b);
        std::vector<JumpEngineTable> b_je;
        std::vector<IndirectJumpSite> b_ind;
        std::vector<uint16_t> b_warn;
        std::set<uint16_t> b_ext;

        analyze_window(bank_view, config, static_cast<int>(b), static_cast<uint8_t>(b), true,
                       bank_seeds, cfg.jump_engine_targets, bcfg.instructions, bcfg.blocks,
                       bcfg.functions, b_je, b_ind, b_warn, b_ext);

        for (uint16_t ext : b_ext) {
            if (ext >= cfg.fixed_window_start && ext < 0xFFFAU &&
                cfg.functions.find(ext) == cfg.functions.end()) {
                extra_fixed_targets.insert(ext);
            }
        }

        if (!bcfg.functions.empty()) {
            cfg.bank_cfgs.push_back(std::move(bcfg));
        }
    }

    if (!extra_fixed_targets.empty()) {
        for (uint16_t ext : extra_fixed_targets) {
            fixed_seeds.emplace_back(ext, "");
        }
        cfg.instructions.clear();
        cfg.blocks.clear();
        cfg.functions.clear();
        cfg.jump_engine_tables.clear();
        cfg.indirect_jumps.clear();
        cfg.unofficial_warnings.clear();
        std::set<uint16_t> dummy_ext;
        analyze_window(fixed_view, config, -1, nes::Bus::kAnyBank, false, fixed_seeds,
                       cfg.jump_engine_targets, cfg.instructions, cfg.blocks, cfg.functions,
                       cfg.jump_engine_tables, cfg.indirect_jumps, cfg.unofficial_warnings,
                       dummy_ext);
    }

    return cfg;
}

std::string format_listing(const ProgramCFG& cfg) {
    std::ostringstream out;
    char buf[128];

    size_t total_funcs = cfg.functions.size();
    size_t total_blocks = cfg.blocks.size();
    size_t total_instrs = cfg.instructions.size();
    for (const auto& bcfg : cfg.bank_cfgs) {
        total_funcs += bcfg.functions.size();
        total_blocks += bcfg.blocks.size();
        total_instrs += bcfg.instructions.size();
    }

    std::snprintf(buf, sizeof(buf),
                  "# nesrecomp disassembly listing\n"
                  "# Vectors: RESET=$%04X NMI=$%04X IRQ=$%04X\n"
                  "# Functions: %zu, Basic blocks: %zu, Instructions: %zu\n\n",
                  cfg.reset_addr, cfg.nmi_addr, cfg.irq_addr, total_funcs, total_blocks,
                  total_instrs);
    out << buf;

    if (!cfg.jump_engine_tables.empty()) {
        out << "# JumpEngine inline tables:\n";
        for (const auto& tbl : cfg.jump_engine_tables) {
            std::snprintf(buf, sizeof(buf),
                          "#   JSR at $%04X (table $%04X, %zu entries):", tbl.jsr_addr,
                          tbl.table_addr, tbl.entries.size());
            out << buf;
            for (uint16_t e : tbl.entries) {
                std::snprintf(buf, sizeof(buf), " $%04X", e);
                out << buf;
            }
            out << "\n";
        }
        out << "\n";
    }

    auto print_funcs = [&](const std::map<uint16_t, Function>& funcs,
                           const std::map<uint16_t, BasicBlock>& blocks) {
        for (const auto& [f_addr, func] : funcs) {
            (void)f_addr;
            out << "=== " << func.c_symbol();
            if (!func.name.empty()) {
                out << " (" << func.name << ")";
            }
            out << " ===\n";
            for (uint16_t b_addr : func.block_addrs) {
                const BasicBlock& b = blocks.at(b_addr);
                std::snprintf(buf, sizeof(buf), "L_%04X:\n", b_addr);
                out << buf;
                for (const Instruction& ins : b.instructions) {
                    std::snprintf(buf, sizeof(buf), "  %04X: %-4s\n", ins.addr,
                                  nes::get_opcode_info(ins.opcode).name);
                    out << buf;
                }
            }
            out << "\n";
        }
    };

    print_funcs(cfg.functions, cfg.blocks);
    for (const auto& bcfg : cfg.bank_cfgs) {
        print_funcs(bcfg.functions, bcfg.blocks);
    }

    return out.str();
}

}  // namespace dragon::nesrecomp
