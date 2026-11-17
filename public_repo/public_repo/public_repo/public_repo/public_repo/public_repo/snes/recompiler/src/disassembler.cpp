#include "snesrecomp/disassembler.h"

#include <cstdio>
#include <queue>
#include <sstream>

namespace dragon::snesrecomp {

std::string BasicBlock::label_name() const {
    char buf[32];
    if (key.mode.e != 0U) {
        std::snprintf(buf, sizeof(buf), "L_%04X_e1", key.addr);
    } else {
        std::snprintf(buf, sizeof(buf), "L_%04X_m%ux%u", key.addr, key.mode.m, key.mode.xf);
    }
    return buf;
}

std::string Function::c_symbol() const {
    char buf[48];
    if (key.mode.e != 0U) {
        std::snprintf(buf, sizeof(buf), "func_%02X_%04X_e1", key.bank, key.addr);
    } else {
        std::snprintf(buf, sizeof(buf), "func_%02X_%04X_m%ux%u", key.bank, key.addr, key.mode.m,
                      key.mode.xf);
    }
    return buf;
}

namespace {

bool is_in_configured_data(const GameConfig& config, uint8_t bank, uint16_t addr) noexcept {
    const uint32_t addr24 = (static_cast<uint32_t>(bank) << 16U) | addr;
    for (const auto& dr : config.data_ranges) {
        if (dr.bank >= 0) {
            if (static_cast<uint8_t>(dr.bank) == bank && addr >= dr.start && addr <= dr.end) {
                return true;
            }
        } else if (dr.end > 0xFFFFU) {
            if (addr24 >= dr.start && addr24 <= dr.end) {
                return true;
            }
        } else {
            if (addr >= dr.start && addr <= dr.end) {
                return true;
            }
        }
    }
    return false;
}

bool is_header_or_vector_area(uint8_t bank, uint16_t addr) noexcept {
    const uint8_t low_bank = static_cast<uint8_t>(bank & 0x7FU);
    if ((low_bank == 0x00U || bank == 0xFFU || bank == 0xC0U) && addr >= 0xFFC0U) {
        return true;
    }
    return false;
}

bool is_valid_code_address(const snes::Cartridge& cart, const GameConfig& config, uint8_t bank,
                           uint16_t addr) noexcept {
    if (is_header_or_vector_area(bank, addr)) {
        return false;
    }
    if (is_in_configured_data(config, bank, addr)) {
        return false;
    }
    return snes::map_rom_address(cart.map_mode, cart.rom.size(), bank, addr) >= 0;
}

uint8_t read_rom8(const snes::Cartridge& cart, uint8_t bank, uint16_t addr) noexcept {
    const int64_t idx = snes::map_rom_address(cart.map_mode, cart.rom.size(), bank, addr);
    if (idx >= 0 && static_cast<size_t>(idx) < cart.rom.size()) {
        return cart.rom[static_cast<size_t>(idx)];
    }
    return 0U;
}

uint16_t read_rom16(const snes::Cartridge& cart, uint8_t bank, uint16_t addr) noexcept {
    const uint8_t lo = read_rom8(cart, bank, addr);
    const uint8_t hi = read_rom8(cart, bank, static_cast<uint16_t>(addr + 1U));
    return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
}

bool modifies_carry_unknown(snes::Mnemonic m) noexcept {
    switch (m) {
        case snes::Mnemonic::ADC:
        case snes::Mnemonic::SBC:
        case snes::Mnemonic::CMP:
        case snes::Mnemonic::CPX:
        case snes::Mnemonic::CPY:
        case snes::Mnemonic::ASL:
        case snes::Mnemonic::LSR:
        case snes::Mnemonic::ROL:
        case snes::Mnemonic::ROR:
        case snes::Mnemonic::PLP:
        case snes::Mnemonic::JSR:
        case snes::Mnemonic::JSL:
            return true;
        default:
            return false;
    }
}

bool is_block_terminator(const Instruction& instr, const ProgramCFG& cfg) noexcept {
    if (snes::is_branch_mnemonic(instr.mnemonic)) {
        return true;
    }
    if (instr.mnemonic == snes::Mnemonic::JSR && instr.mode == snes::AddrMode::Abs &&
        cfg.is_jump_engine(instr.bank, instr.operand_u16())) {
        return true;
    }
    if (instr.mnemonic == snes::Mnemonic::JSL && instr.mode == snes::AddrMode::AbsLong &&
        cfg.is_jump_engine(instr.op3, instr.operand_u16())) {
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
        case snes::Mnemonic::STP:
            return true;
        default:
            return false;
    }
}

}  // namespace

ProgramCFG analyze_snes_rom(const snes::Cartridge& cart, const GameConfig& config) {
    ProgramCFG cfg;
    if (config.interpreter_only) {
        return cfg;
    }
    cfg.map_mode = cart.map_mode;
    cfg.reset_addr = cart.emu_vectors.reset;
    cfg.native_nmi_addr = cart.native_vectors.nmi;
    cfg.native_irq_addr = cart.native_vectors.irq;
    cfg.emu_nmi_addr = cart.emu_vectors.nmi;
    cfg.emu_irq_addr = cart.emu_vectors.irq;

    for (uint32_t je : config.jump_engine) {
        cfg.jump_engine_targets.insert(je);
    }
    for (const auto& idc : config.inline_data_calls) {
        if (idc.kind == "jump_engine") {
            cfg.jump_engine_targets.insert(idc.target);
        }
    }

    struct JumpEngineCallSite {
        uint8_t bank = 0;
        uint16_t jsr_addr = 0;
        uint8_t call_len = 3;
        CpuModeState mode{};

        auto operator<=>(const JumpEngineCallSite&) const = default;
    };
    std::set<JumpEngineCallSite> jump_engine_call_sites;

    std::map<uint32_t, CpuModeState> hint_map;
    for (const auto& h : config.mx_hints) {
        CpuModeState st{};
        st.m = (h.m != 0U) ? 1U : 0U;
        st.xf = (h.x != 0U) ? 1U : 0U;
        st.e = 0U;
        hint_map[h.addr & 0xFFFFFFU] = st;
    }

    std::set<FunctionKey> seeded_functions;
    std::map<FunctionKey, std::string> function_names;
    std::queue<FunctionKey> func_worklist;

    auto add_function_seed = [&](uint8_t bank, uint16_t addr, CpuModeState mode,
                                 std::string_view name = {}) {
        if (!is_valid_code_address(cart, config, bank, addr)) {
            return;
        }
        const uint32_t addr24 = (static_cast<uint32_t>(bank) << 16U) | addr;
        auto h_it = hint_map.find(addr24);
        if (h_it != hint_map.end() && mode.e == 0U) {
            mode = h_it->second;
        }

        FunctionKey fkey{bank, addr, mode};
        if (!name.empty() && function_names[fkey].empty()) {
            function_names[fkey] = std::string(name);
        }
        if (seeded_functions.insert(fkey).second) {
            func_worklist.push(fkey);
        }
    };

    // 1. Seed hardware vectors in bank $00
    if (cfg.reset_addr >= 0x8000U) {
        add_function_seed(0x00U, cfg.reset_addr, CpuModeState{1U, 1U, 1U}, "Reset_Emu");
        add_function_seed(0x00U, cfg.reset_addr, CpuModeState{1U, 1U, 0U}, "Reset_Native");
    }
    if (cfg.native_nmi_addr >= 0x8000U) {
        add_function_seed(0x00U, cfg.native_nmi_addr, CpuModeState{1U, 1U, 0U}, "NMI_Native");
        add_function_seed(0x00U, cfg.native_nmi_addr, CpuModeState{0U, 1U, 0U}, "NMI_Native");
        add_function_seed(0x00U, cfg.native_nmi_addr, CpuModeState{1U, 0U, 0U}, "NMI_Native");
        add_function_seed(0x00U, cfg.native_nmi_addr, CpuModeState{0U, 0U, 0U}, "NMI_Native");
    }
    if (cfg.native_irq_addr >= 0x8000U) {
        add_function_seed(0x00U, cfg.native_irq_addr, CpuModeState{1U, 1U, 0U}, "IRQ_Native");
        add_function_seed(0x00U, cfg.native_irq_addr, CpuModeState{0U, 1U, 0U}, "IRQ_Native");
        add_function_seed(0x00U, cfg.native_irq_addr, CpuModeState{1U, 0U, 0U}, "IRQ_Native");
        add_function_seed(0x00U, cfg.native_irq_addr, CpuModeState{0U, 0U, 0U}, "IRQ_Native");
    }
    if (cfg.emu_nmi_addr >= 0x8000U && cfg.emu_nmi_addr != cfg.native_nmi_addr) {
        add_function_seed(0x00U, cfg.emu_nmi_addr, CpuModeState{1U, 1U, 1U}, "NMI_Emu");
    }
    if (cfg.emu_irq_addr >= 0x8000U && cfg.emu_irq_addr != cfg.native_irq_addr) {
        add_function_seed(0x00U, cfg.emu_irq_addr, CpuModeState{1U, 1U, 1U}, "IRQ_Emu");
    }

    // 2. Seed configured entry_points
    for (const auto& ep : config.entry_points) {
        const uint8_t bank = (ep.bank >= 0) ? static_cast<uint8_t>(ep.bank)
                                            : static_cast<uint8_t>((ep.addr >> 16U) & 0xFFU);
        const uint16_t addr16 = static_cast<uint16_t>(ep.addr & 0xFFFFU);
        add_function_seed(bank, addr16, CpuModeState{1U, 1U, 0U}, ep.name);
    }

    // 3. Seed configured jump_tables
    for (const auto& jt : config.jump_tables) {
        const uint8_t bank = (jt.bank >= 0) ? static_cast<uint8_t>(jt.bank)
                                            : static_cast<uint8_t>((jt.addr >> 16U) & 0xFFU);
        const uint16_t base_addr = static_cast<uint16_t>(jt.addr & 0xFFFFU);
        for (size_t i = 0U; i < jt.count; ++i) {
            if (jt.format == JumpTableFormat::Word) {
                const uint16_t target =
                    read_rom16(cart, bank, static_cast<uint16_t>(base_addr + i * 2U));
                add_function_seed(bank, target, CpuModeState{1U, 1U, 0U});
            } else if (jt.format == JumpTableFormat::WordMinusOne) {
                const uint16_t target = static_cast<uint16_t>(
                    read_rom16(cart, bank, static_cast<uint16_t>(base_addr + i * 2U)) + 1U);
                add_function_seed(bank, target, CpuModeState{1U, 1U, 0U});
            } else if (jt.format == JumpTableFormat::Split) {
                const uint8_t lo =
                    read_rom8(cart, bank, static_cast<uint16_t>((jt.lo & 0xFFFFU) + i));
                const uint8_t hi =
                    read_rom8(cart, bank, static_cast<uint16_t>((jt.hi & 0xFFFFU) + i));
                const uint16_t target =
                    static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
                add_function_seed(bank, target, CpuModeState{1U, 1U, 0U});
            } else if (jt.format == JumpTableFormat::Long) {
                const uint16_t target =
                    read_rom16(cart, bank, static_cast<uint16_t>(base_addr + i * 3U));
                const uint8_t target_bank =
                    read_rom8(cart, bank, static_cast<uint16_t>(base_addr + i * 3U + 2U));
                add_function_seed(target_bank, target, CpuModeState{1U, 1U, 0U});
            }
        }
    }

    std::map<FunctionKey, CpuModeState> func_exit_modes;
    std::map<FunctionKey, std::set<FunctionKey>> callee_to_callers;
    std::set<FunctionKey> exit_mode_propagated;

    struct TraceItem {
        BlockKey bkey;
        int carry_state = -1;
        int php_mode = -1;
        bool plp_unknown = false;
    };

    // 4. Process function worklist with iterative JumpEngine table expansion
    while (true) {
        while (!func_worklist.empty()) {
            const FunctionKey fkey = func_worklist.front();
            func_worklist.pop();

            Function func;
            func.key = fkey;
            auto name_it = function_names.find(fkey);
            if (name_it != function_names.end()) {
                func.name = name_it->second;
            }

            const uint8_t bank = fkey.bank;
            std::map<BlockKey, Instruction> decoded_instrs;
            std::set<BlockKey> block_leaders;
            std::set<BlockKey> queued_leaders;
            std::queue<TraceItem> trace_queue;

            bool has_known_return_mode = false;
            bool return_mode_ambiguous = false;
            CpuModeState observed_return_mode{};

            auto enqueue_leader = [&](BlockKey bkey, int carry_state = -1, int php_mode = -1,
                                      bool plp_unknown = false) {
                if (!is_valid_code_address(cart, config, bank, bkey.addr)) {
                    return;
                }
                block_leaders.insert(bkey);
                if (queued_leaders.insert(bkey).second) {
                    trace_queue.push(TraceItem{bkey, carry_state, php_mode, plp_unknown});
                }
            };

            enqueue_leader(BlockKey{fkey.addr, fkey.mode}, -1, -1, false);

            while (!trace_queue.empty()) {
                const TraceItem item = trace_queue.front();
                trace_queue.pop();

                uint16_t pc = item.bkey.addr;
                CpuModeState cur_mode = item.bkey.mode;
                int cur_carry = item.carry_state;
                int cur_php_mode = item.php_mode;
                bool cur_plp_unknown = item.plp_unknown;

                while (is_valid_code_address(cart, config, bank, pc)) {
                    const uint32_t addr24 = (static_cast<uint32_t>(bank) << 16U) | pc;
                    auto h_it = hint_map.find(addr24);
                    if (h_it != hint_map.end() && cur_mode.e == 0U) {
                        cur_mode = h_it->second;
                    }

                    const BlockKey ikey{pc, cur_mode};
                    if (decoded_instrs.find(ikey) != decoded_instrs.end()) {
                        block_leaders.insert(ikey);
                        break;
                    }

                    const uint8_t op = read_rom8(cart, bank, pc);
                    const snes::OpcodeInfo& info = snes::get_opcode_info(op);
                    const uint8_t len = snes::instruction_length(
                        info.mode, cur_mode.eff_m(), cur_mode.eff_x(), cur_mode.e != 0U);

                    if (static_cast<uint32_t>(pc) + len > 0x10000U) {
                        break;
                    }

                    Instruction instr;
                    instr.bank = bank;
                    instr.addr = pc;
                    instr.state = cur_mode;
                    instr.opcode = op;
                    instr.length = len;
                    instr.mnemonic = info.mnemonic;
                    instr.mode = info.mode;
                    instr.base_cycles = info.base_cycles;
                    instr.page_cross_penalty = info.page_cross_penalty;

                    if (len >= 2U) {
                        instr.op1 = read_rom8(cart, bank, static_cast<uint16_t>(pc + 1U));
                    }
                    if (len >= 3U) {
                        instr.op2 = read_rom8(cart, bank, static_cast<uint16_t>(pc + 2U));
                    }
                    if (len >= 4U) {
                        instr.op3 = read_rom8(cart, bank, static_cast<uint16_t>(pc + 3U));
                    }

                    CpuModeState next_mode = cur_mode;
                    int next_carry = cur_carry;
                    int next_php_mode = cur_php_mode;
                    bool next_plp_unknown = cur_plp_unknown;

                    if (instr.mnemonic == snes::Mnemonic::CLC) {
                        next_carry = 0;
                    } else if (instr.mnemonic == snes::Mnemonic::SEC) {
                        next_carry = 1;
                    } else if (instr.mnemonic == snes::Mnemonic::PHP) {
                        next_php_mode = static_cast<int>(cur_mode.mode_flags());
                    } else if (instr.mnemonic == snes::Mnemonic::PLP) {
                        if (cur_mode.e == 0U) {
                            if (cur_php_mode >= 0) {
                                next_mode.m =
                                    ((static_cast<uint8_t>(cur_php_mode) & 0x02U) != 0U) ? 1U : 0U;
                                next_mode.xf =
                                    ((static_cast<uint8_t>(cur_php_mode) & 0x01U) != 0U) ? 1U : 0U;
                                next_php_mode = -1;
                            } else {
                                next_plp_unknown = true;
                            }
                        }
                        next_carry = -1;
                    } else if (instr.mnemonic == snes::Mnemonic::XCE) {
                        if (cur_carry == 0) {
                            next_mode = CpuModeState{1U, 1U, 0U};
                            next_carry = static_cast<int>(cur_mode.e);
                        } else if (cur_carry == 1) {
                            next_mode = CpuModeState{1U, 1U, 1U};
                            next_carry = static_cast<int>(cur_mode.e);
                        } else if (cur_mode.e != 0U) {
                            next_mode = CpuModeState{1U, 1U, 0U};
                            next_carry = 1;
                        }
                    } else if (instr.mnemonic == snes::Mnemonic::SEP) {
                        if (cur_mode.e == 0U) {
                            if ((instr.op1 & 0x20U) != 0U) {
                                next_mode.m = 1U;
                            }
                            if ((instr.op1 & 0x10U) != 0U) {
                                next_mode.xf = 1U;
                            }
                        }
                        if ((instr.op1 & 0x01U) != 0U) {
                            next_carry = 1;
                        }
                    } else if (instr.mnemonic == snes::Mnemonic::REP) {
                        if (cur_mode.e == 0U) {
                            if ((instr.op1 & 0x20U) != 0U) {
                                next_mode.m = 0U;
                            }
                            if ((instr.op1 & 0x10U) != 0U) {
                                next_mode.xf = 0U;
                            }
                        }
                        if ((instr.op1 & 0x01U) != 0U) {
                            next_carry = 0;
                        }
                    } else if (modifies_carry_unknown(instr.mnemonic)) {
                        next_carry = -1;
                    }

                    if (instr.mnemonic == snes::Mnemonic::JSR &&
                        instr.mode == snes::AddrMode::Abs) {
                        const uint16_t target_addr = instr.operand_u16();
                        CpuModeState callee_mode = cur_mode;
                        const uint32_t callee_addr24 =
                            (static_cast<uint32_t>(bank) << 16U) | target_addr;
                        auto ch_it = hint_map.find(callee_addr24);
                        if (ch_it != hint_map.end() && callee_mode.e == 0U) {
                            callee_mode = ch_it->second;
                        }
                        const FunctionKey callee_key{bank, target_addr, callee_mode};
                        add_function_seed(bank, target_addr, callee_mode);
                        callee_to_callers[callee_key].insert(fkey);
                        auto exit_it = func_exit_modes.find(callee_key);
                        if (exit_it != func_exit_modes.end()) {
                            next_mode = exit_it->second;
                        }
                    } else if (instr.mnemonic == snes::Mnemonic::JSL &&
                               instr.mode == snes::AddrMode::AbsLong) {
                        const uint8_t target_bank = instr.op3;
                        const uint16_t target_addr = instr.operand_u16();
                        CpuModeState callee_mode = cur_mode;
                        const uint32_t callee_addr24 =
                            (static_cast<uint32_t>(target_bank) << 16U) | target_addr;
                        auto ch_it = hint_map.find(callee_addr24);
                        if (ch_it != hint_map.end() && callee_mode.e == 0U) {
                            callee_mode = ch_it->second;
                        }
                        const FunctionKey callee_key{target_bank, target_addr, callee_mode};
                        add_function_seed(target_bank, target_addr, callee_mode);
                        callee_to_callers[callee_key].insert(fkey);
                        auto exit_it = func_exit_modes.find(callee_key);
                        if (exit_it != func_exit_modes.end()) {
                            next_mode = exit_it->second;
                        }
                    }

                    const uint16_t next_pc = static_cast<uint16_t>(pc + len);
                    const uint32_t next_addr24 = (static_cast<uint32_t>(bank) << 16U) | next_pc;
                    auto next_h_it = hint_map.find(next_addr24);
                    if (next_h_it != hint_map.end() && next_mode.e == 0U) {
                        next_mode = next_h_it->second;
                    }

                    instr.next_state = next_mode;
                    decoded_instrs[ikey] = instr;

                    if (instr.mnemonic == snes::Mnemonic::RTS ||
                        instr.mnemonic == snes::Mnemonic::RTL) {
                        if (cur_plp_unknown) {
                            return_mode_ambiguous = true;
                        } else if (!has_known_return_mode) {
                            has_known_return_mode = true;
                            observed_return_mode = cur_mode;
                        } else if (observed_return_mode != cur_mode) {
                            return_mode_ambiguous = true;
                        }
                    }

                    if (instr.mnemonic == snes::Mnemonic::JSR &&
                        instr.mode == snes::AddrMode::Abs) {
                        if (cfg.is_jump_engine(bank, instr.operand_u16())) {
                            jump_engine_call_sites.insert(
                                JumpEngineCallSite{bank, pc, 3U, cur_mode});
                            break;
                        }
                    } else if (instr.mnemonic == snes::Mnemonic::JSL &&
                               instr.mode == snes::AddrMode::AbsLong) {
                        if (cfg.is_jump_engine(instr.op3, instr.operand_u16())) {
                            jump_engine_call_sites.insert(
                                JumpEngineCallSite{bank, pc, 4U, cur_mode});
                            break;
                        }
                    } else if (instr.mnemonic == snes::Mnemonic::JML &&
                               instr.mode == snes::AddrMode::AbsLong) {
                        add_function_seed(instr.op3, instr.operand_u16(), next_mode);
                        break;
                    } else if (snes::is_conditional_branch(instr.mnemonic)) {
                        enqueue_leader(BlockKey{instr.operand_u16(), next_mode}, next_carry,
                                       next_php_mode, next_plp_unknown);
                        enqueue_leader(BlockKey{next_pc, next_mode}, next_carry, next_php_mode,
                                       next_plp_unknown);
                        break;
                    } else if (instr.mnemonic == snes::Mnemonic::BRA ||
                               instr.mnemonic == snes::Mnemonic::BRL ||
                               (instr.mnemonic == snes::Mnemonic::JMP &&
                                instr.mode == snes::AddrMode::Abs)) {
                        enqueue_leader(BlockKey{instr.operand_u16(), next_mode}, next_carry,
                                       next_php_mode, next_plp_unknown);
                        break;
                    } else if (is_block_terminator(instr, cfg)) {
                        break;
                    }

                    pc = next_pc;
                    cur_mode = next_mode;
                    cur_carry = next_carry;
                    cur_php_mode = next_php_mode;
                    cur_plp_unknown = next_plp_unknown;
                }
            }

            if (auto old_it = cfg.functions.find(fkey); old_it != cfg.functions.end()) {
                cfg.total_blocks -= old_it->second.blocks.size();
                for (const auto& [obk, oblk] : old_it->second.blocks) {
                    (void)obk;
                    cfg.total_instructions -= oblk.instructions.size();
                }
                cfg.functions.erase(old_it);
            }

            // Build BasicBlocks for this function from block_leaders
            for (const BlockKey& leader : block_leaders) {
                if (decoded_instrs.find(leader) == decoded_instrs.end()) {
                    continue;
                }
                BasicBlock block;
                block.key = leader;
                BlockKey cur_key = leader;

                while (true) {
                    auto it = decoded_instrs.find(cur_key);
                    if (it == decoded_instrs.end()) {
                        break;
                    }
                    const Instruction& instr = it->second;
                    block.instructions.push_back(instr);
                    const uint16_t next_pc = static_cast<uint16_t>(instr.addr + instr.length);
                    block.end_addr = next_pc;
                    const BlockKey fallthrough_key{next_pc, instr.next_state};

                    if (snes::is_conditional_branch(instr.mnemonic)) {
                        block.successors.push_back(BlockKey{instr.operand_u16(), instr.next_state});
                        block.successors.push_back(fallthrough_key);
                        break;
                    }
                    if (instr.mnemonic == snes::Mnemonic::BRA ||
                        instr.mnemonic == snes::Mnemonic::BRL ||
                        (instr.mnemonic == snes::Mnemonic::JMP &&
                         instr.mode == snes::AddrMode::Abs)) {
                        block.successors.push_back(BlockKey{instr.operand_u16(), instr.next_state});
                        break;
                    }
                    if (is_block_terminator(instr, cfg)) {
                        break;
                    }

                    if (block_leaders.count(fallthrough_key) > 0 ||
                        decoded_instrs.find(fallthrough_key) == decoded_instrs.end()) {
                        if (decoded_instrs.find(fallthrough_key) != decoded_instrs.end()) {
                            block.successors.push_back(fallthrough_key);
                        }
                        break;
                    }
                    cur_key = fallthrough_key;
                }

                if (!block.instructions.empty()) {
                    cfg.total_instructions += block.instructions.size();
                    cfg.total_blocks += 1U;
                    func.blocks[leader] = std::move(block);
                }
            }

            if (!func.blocks.empty()) {
                cfg.functions[fkey] = std::move(func);
            }

            if (has_known_return_mode && !return_mode_ambiguous &&
                observed_return_mode != fkey.mode) {
                func_exit_modes[fkey] = observed_return_mode;
                if (exit_mode_propagated.insert(fkey).second) {
                    auto c_it = callee_to_callers.find(fkey);
                    if (c_it != callee_to_callers.end()) {
                        for (const FunctionKey& caller_key : c_it->second) {
                            if (cfg.functions.find(caller_key) != cfg.functions.end()) {
                                func_worklist.push(caller_key);
                            }
                        }
                    }
                }
            }
        }

        // Expand inline word tables after JSR/JSL <jump_engine>.
        bool added_new_seed = false;
        cfg.jump_engine_tables.clear();
        for (const auto& cs : jump_engine_call_sites) {
            const uint16_t tbl_start = static_cast<uint16_t>(cs.jsr_addr + cs.call_len);
            uint32_t min_after = 0x10000U;
            for (const auto& [fkey, func] : cfg.functions) {
                if (fkey.bank == cs.bank && fkey.addr > tbl_start && fkey.addr < min_after) {
                    min_after = fkey.addr;
                }
                for (const auto& [bkey, blk] : func.blocks) {
                    if (fkey.bank == cs.bank && bkey.addr > tbl_start && bkey.addr < min_after) {
                        min_after = bkey.addr;
                    }
                    for (const auto& ins : blk.instructions) {
                        if (ins.bank == cs.bank && ins.addr > tbl_start && ins.addr < min_after) {
                            min_after = ins.addr;
                        }
                    }
                }
            }

            JumpEngineTable table;
            table.bank = cs.bank;
            table.jsr_addr = cs.jsr_addr;
            table.table_addr = tbl_start;

            uint32_t pos = tbl_start;
            while (pos + 2U <= min_after && pos + 2U <= 0x10000U) {
                if (is_in_configured_data(config, cs.bank, static_cast<uint16_t>(pos))) {
                    break;
                }
                const uint16_t w = read_rom16(cart, cs.bank, static_cast<uint16_t>(pos));
                if (w < 0x8000U || !is_valid_code_address(cart, config, cs.bank, w) ||
                    is_in_configured_data(config, cs.bank, w)) {
                    break;
                }
                table.entries.push_back(w);
                if (w > tbl_start && w < min_after) {
                    min_after = w;
                }
                const FunctionKey target_key{cs.bank, w, cs.mode};
                if (seeded_functions.find(target_key) == seeded_functions.end()) {
                    add_function_seed(cs.bank, w, cs.mode);
                    added_new_seed = true;
                }
                pos += 2U;
            }
            cfg.jump_engine_tables.push_back(std::move(table));
        }

        if (!added_new_seed && func_worklist.empty()) {
            break;
        }
    }

    return cfg;
}

std::string format_listing(const ProgramCFG& cfg) {
    std::ostringstream out;
    out << "; snesrecomp disassembly listing\n";
    out << "; Functions: " << cfg.functions.size() << ", Basic blocks: " << cfg.total_blocks
        << ", Instructions: " << cfg.total_instructions << "\n\n";

    if (!cfg.jump_engine_tables.empty()) {
        out << "; JumpEngine inline tables:\n";
        for (const auto& tbl : cfg.jump_engine_tables) {
            char tbuf[128];
            std::snprintf(tbuf, sizeof(tbuf),
                          ";   JSR at $%02X:%04X (table $%04X, %zu entries):", tbl.bank,
                          tbl.jsr_addr, tbl.table_addr, tbl.entries.size());
            out << tbuf;
            for (uint16_t e : tbl.entries) {
                std::snprintf(tbuf, sizeof(tbuf), " $%04X", e);
                out << tbuf;
            }
            out << "\n";
        }
        out << "\n";
    }

    for (const auto& [fkey, func] : cfg.functions) {
        char hdr[128];
        std::snprintf(hdr, sizeof(hdr), "=== %s ($%02X:%04X, m=%u x=%u e=%u) %s ===\n",
                      func.c_symbol().c_str(), fkey.bank, fkey.addr, fkey.mode.m, fkey.mode.xf,
                      fkey.mode.e, func.name.c_str());
        out << hdr;

        for (const auto& [bkey, block] : func.blocks) {
            out << "  " << block.label_name() << ":\n";
            for (const auto& ins : block.instructions) {
                char line[128];
                std::snprintf(line, sizeof(line), "    $%02X:%04X [m%ux%ue%u]  %-4s (len=%u)\n",
                              ins.bank, ins.addr, ins.state.m, ins.state.xf, ins.state.e,
                              snes::get_opcode_info(ins.opcode).name, ins.length);
                out << line;
            }
        }
        out << "\n";
    }
    return out.str();
}

}  // namespace dragon::snesrecomp
