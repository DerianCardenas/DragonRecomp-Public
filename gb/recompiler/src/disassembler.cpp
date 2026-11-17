#include "gbrecomp/disassembler.h"

#include <algorithm>
#include <cstdio>
#include <queue>
#include <set>

namespace dragon::gbrecomp {

std::string Function::c_symbol() const {
    char buf[32];
    if (has_bank) {
        std::snprintf(buf, sizeof(buf), "func_%02X_%04X", bank, entry_addr);
    } else {
        std::snprintf(buf, sizeof(buf), "func_%04X", entry_addr);
    }
    return buf;
}

const Function* ProgramCFG::find_function(uint8_t bank, uint16_t addr) const {
    auto b_it = banks.find(bank);
    if (b_it != banks.end()) {
        auto f_it = b_it->second.functions.find(addr);
        if (f_it != b_it->second.functions.end()) {
            return &f_it->second;
        }
    }
    if (bank != 0) {
        auto b0_it = banks.find(0);
        if (b0_it != banks.end()) {
            auto f_it = b0_it->second.functions.find(addr);
            if (f_it != b0_it->second.functions.end()) {
                return &f_it->second;
            }
        }
    }
    return nullptr;
}

namespace {

struct RomView {
    std::span<const uint8_t> rom;
    uint8_t bank = 0;

    uint8_t read8(uint16_t addr) const noexcept {
        if (rom.empty())
            return 0;
        size_t offset = 0;
        if (addr < 0x4000) {
            offset = addr;
        } else if (addr < 0x8000) {
            uint32_t b = (bank == 0) ? 1 : bank;
            offset = (static_cast<size_t>(b) * 0x4000U) + (addr - 0x4000U);
        } else {
            return 0;
        }
        return (offset < rom.size()) ? rom[offset] : 0;
    }

    uint16_t read16(uint16_t addr) const noexcept {
        uint8_t lo = read8(addr);
        uint8_t hi = read8(static_cast<uint16_t>(addr + 1U));
        return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
    }
};

Instruction decode_instruction(const RomView& view, uint16_t addr) {
    Instruction inst;
    inst.addr = addr;
    uint8_t op = view.read8(addr);

    if (op == 0xCB) {
        inst.is_cb = true;
        inst.opcode = view.read8(static_cast<uint16_t>(addr + 1U));
        inst.length = 2;
        const auto& info = kGbCbOpcodeTable[inst.opcode];
        inst.mnemonic = info.mnemonic;
        inst.cycles = info.cycles;
        inst.cycles_branch = info.cycles_branch;
    } else {
        inst.is_cb = false;
        inst.opcode = op;
        const auto& info = kGbBaseOpcodeTable[op];
        inst.length = info.length;
        inst.mnemonic = info.mnemonic;
        inst.cycles = info.cycles;
        inst.cycles_branch = info.cycles_branch;
        if (inst.length >= 2) {
            inst.op1 = view.read8(static_cast<uint16_t>(addr + 1U));
        }
        if (inst.length >= 3) {
            inst.op2 = view.read8(static_cast<uint16_t>(addr + 2U));
        }
    }
    return inst;
}

bool is_unconditional_jump(const Instruction& inst) {
    if (inst.is_cb)
        return false;
    return (inst.opcode == 0xC3 || inst.opcode == 0x18 || inst.opcode == 0xE9);
}

bool is_conditional_branch(const Instruction& inst) {
    if (inst.is_cb)
        return false;
    return (inst.opcode == 0x20 || inst.opcode == 0x28 || inst.opcode == 0x30 ||
            inst.opcode == 0x38 || inst.opcode == 0xC2 || inst.opcode == 0xCA ||
            inst.opcode == 0xD2 || inst.opcode == 0xDA);
}

bool is_call(const Instruction& inst) {
    if (inst.is_cb)
        return false;
    return (inst.opcode == 0xCD || inst.opcode == 0xC4 || inst.opcode == 0xCC ||
            inst.opcode == 0xD4 || inst.opcode == 0xDC ||
            (inst.opcode & 0xC7) == 0xC7);  // RST instructions
}

bool is_unconditional_return(const Instruction& inst) {
    if (inst.is_cb)
        return false;
    return (inst.opcode == 0xC9 || inst.opcode == 0xD9);
}

bool is_conditional_return(const Instruction& inst) {
    if (inst.is_cb)
        return false;
    return (inst.opcode == 0xC0 || inst.opcode == 0xC8 || inst.opcode == 0xD0 ||
            inst.opcode == 0xD8);
}

void disassemble_bank(uint8_t bank, std::span<const uint8_t> rom, const std::set<uint16_t>& seeds,
                      const GameConfig& config, BankCFG& bank_cfg,
                      std::map<uint8_t, std::set<uint16_t>>& out_cross_seeds) {
    (void)config;
    bank_cfg.bank = bank;
    RomView view{rom, bank};

    const uint16_t min_addr = (bank == 0) ? 0x0000U : 0x4000U;
    const uint16_t max_addr = (bank == 0) ? 0x4000U : 0x8000U;

    std::map<uint16_t, Instruction> instructions;
    std::set<uint16_t> block_leaders;
    std::set<uint16_t> function_seeds;
    std::queue<uint16_t> worklist;

    auto add_seed = [&](uint16_t addr, bool is_function) {
        if (addr >= min_addr && addr < max_addr) {
            block_leaders.insert(addr);
            if (is_function) {
                function_seeds.insert(addr);
            }
            if (instructions.find(addr) == instructions.end()) {
                worklist.push(addr);
            }
        } else if (addr < 0x8000U) {
            uint8_t target_bank = (addr < 0x4000U) ? 0U : ((bank == 0) ? 1U : bank);
            out_cross_seeds[target_bank].insert(addr);
        }
    };

    for (uint16_t s : seeds) {
        add_seed(s, true);
    }

    // Pass 1: Decode instructions and collect leaders & function seeds
    while (!worklist.empty()) {
        uint16_t pc = worklist.front();
        worklist.pop();

        while (pc >= min_addr && pc < max_addr) {
            if (instructions.find(pc) != instructions.end()) {
                break;
            }

            Instruction inst = decode_instruction(view, pc);
            if (inst.length == 0 || (pc + inst.length > max_addr)) {
                break;
            }

            instructions[pc] = inst;
            const uint16_t next_pc = static_cast<uint16_t>(pc + inst.length);

            if (is_unconditional_jump(inst)) {
                if (inst.opcode == 0x18) {  // JR r8
                    int8_t offset = static_cast<int8_t>(inst.op1);
                    uint16_t target = static_cast<uint16_t>(next_pc + offset);
                    add_seed(target, false);
                } else if (inst.opcode == 0xC3) {  // JP a16
                    uint16_t target = inst.operand_u16();
                    add_seed(target, false);
                }
                break;
            }

            if (is_conditional_branch(inst)) {
                uint16_t target = 0;
                if (inst.length == 2) {  // JR cc, r8
                    int8_t offset = static_cast<int8_t>(inst.op1);
                    target = static_cast<uint16_t>(next_pc + offset);
                } else {  // JP cc, a16
                    target = inst.operand_u16();
                }
                add_seed(target, false);
                add_seed(next_pc, false);
                pc = next_pc;
                continue;
            }

            if (!inst.is_cb && (inst.opcode == 0x76 || inst.opcode == 0x10)) {
                add_seed(next_pc, true);
                break;
            }

            if (is_unconditional_return(inst)) {
                break;
            }

            if (is_conditional_return(inst)) {
                add_seed(next_pc, false);
                pc = next_pc;
                continue;
            }

            if (is_call(inst)) {
                uint16_t target = 0;
                if ((inst.opcode & 0xC7) == 0xC7) {  // RST
                    target = inst.opcode & 0x38U;
                } else {  // CALL
                    target = inst.operand_u16();
                }
                add_seed(target, true);
                pc = next_pc;
                continue;
            }

            pc = next_pc;
        }
    }

    // Pass 2: Build disjoint basic blocks
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

            const Instruction& inst = cur_it->second;
            block.instructions.push_back(inst);
            const uint16_t next_pc = static_cast<uint16_t>(pc + inst.length);
            block.end_addr = next_pc;

            if (is_unconditional_jump(inst)) {
                if (inst.opcode == 0x18) {
                    int8_t offset = static_cast<int8_t>(inst.op1);
                    block.successors.push_back(static_cast<uint16_t>(next_pc + offset));
                } else if (inst.opcode == 0xC3) {
                    block.successors.push_back(inst.operand_u16());
                }
                break;
            }

            if (is_conditional_branch(inst)) {
                uint16_t target = 0;
                if (inst.length == 2) {
                    int8_t offset = static_cast<int8_t>(inst.op1);
                    target = static_cast<uint16_t>(next_pc + offset);
                } else {
                    target = inst.operand_u16();
                }
                block.successors.push_back(target);
                block.successors.push_back(next_pc);
                break;
            }

            if (!inst.is_cb && (inst.opcode == 0x76 || inst.opcode == 0x10)) {
                block.successors.push_back(next_pc);
                break;
            }

            if (is_unconditional_return(inst)) {
                break;
            }

            if (is_conditional_return(inst)) {
                block.successors.push_back(next_pc);
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

        bank_cfg.blocks[leader] = std::move(block);
    }

    // Pass 3: Build functions from function seeds
    for (uint16_t f_addr : function_seeds) {
        if (!bank_cfg.blocks.count(f_addr)) {
            continue;
        }

        Function func;
        func.entry_addr = f_addr;
        func.bank = bank;
        func.has_bank = (bank > 0);

        std::queue<uint16_t> q;
        std::set<uint16_t> visited;
        q.push(f_addr);
        visited.insert(f_addr);

        while (!q.empty()) {
            uint16_t addr = q.front();
            q.pop();
            func.block_addrs.insert(addr);

            auto b_it = bank_cfg.blocks.find(addr);
            if (b_it != bank_cfg.blocks.end()) {
                for (uint16_t succ : b_it->second.successors) {
                    if (function_seeds.count(succ) && succ != f_addr) {
                        continue;
                    }
                    if (!visited.count(succ) && bank_cfg.blocks.count(succ)) {
                        visited.insert(succ);
                        q.push(succ);
                    }
                }
            }
        }

        bank_cfg.functions[f_addr] = std::move(func);
    }
}

}  // namespace

ProgramCFG disassemble_game_boy(std::span<const uint8_t> rom, const GameConfig& config) {
    ProgramCFG cfg;
    if (config.interpreter_only) {
        return cfg;
    }
    std::map<uint8_t, std::set<uint16_t>> bank_seeds;

    // Seeds for Bank 0: Entry point ($0100)
    bank_seeds[0].insert(0x0100);

    // Restart vectors (RST $00-$38)
    constexpr uint16_t kRstVectors[] = {
        0x0000, 0x0008, 0x0010, 0x0018, 0x0020, 0x0028, 0x0030, 0x0038
    };
    for (uint16_t vec : kRstVectors) {
        if (vec < rom.size() && rom[vec] != 0x00 && rom[vec] != 0xFF) {
            bank_seeds[0].insert(vec);
        }
    }

    // Standard hardware interrupt vectors ($0040-$0060)
    constexpr uint16_t kStandardVectors[] = {
        0x0040, 0x0048, 0x0050, 0x0058, 0x0060  // Interrupts
    };
    for (uint16_t vec : kStandardVectors) {
        if (vec < rom.size() && rom[vec] != 0xFF) {
            bank_seeds[0].insert(vec);
        }
    }

    // Configured entry points
    for (const auto& ep : config.entry_points) {
        uint8_t b = (ep.bank >= 0) ? static_cast<uint8_t>(ep.bank) : (ep.addr >= 0x4000 ? 1 : 0);
        bank_seeds[b].insert(static_cast<uint16_t>(ep.addr));
    }

    std::map<uint8_t, std::set<uint16_t>> cross_seeds;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [b, s_set] : cross_seeds) {
            for (uint16_t s : s_set) {
                if (bank_seeds[b].insert(s).second) {
                    changed = true;
                }
            }
        }
        cross_seeds.clear();

        for (const auto& [bank, seeds] : bank_seeds) {
            BankCFG bank_cfg;
            disassemble_bank(bank, rom, seeds, config, bank_cfg, cross_seeds);
            cfg.banks[bank] = std::move(bank_cfg);
        }

        for (const auto& [b, s_set] : cross_seeds) {
            for (uint16_t s : s_set) {
                if (bank_seeds[b].find(s) == bank_seeds[b].end()) {
                    changed = true;
                    break;
                }
            }
            if (changed) {
                break;
            }
        }
    }

    return cfg;
}

}  // namespace dragon::gbrecomp
