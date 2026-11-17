#pragma once

// Game Boy / Game Boy Color Recursive-Descent Disassembler & CFG Builder.
// Reference: docs/GB_SPEC.md §8, docs/SPEC.md §2.

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "dragon/config.h"
#include "gb/opcodes.h"

namespace dragon::gbrecomp {

struct Instruction {
    uint16_t addr = 0;
    uint8_t opcode = 0;
    bool is_cb = false;
    uint8_t op1 = 0;
    uint8_t op2 = 0;
    uint8_t length = 1;
    const char* mnemonic = "";
    uint8_t cycles = 4;
    uint8_t cycles_branch = 4;

    uint16_t operand_u16() const noexcept {
        if (length == 2) {
            return op1;
        }
        return static_cast<uint16_t>((static_cast<uint16_t>(op2) << 8U) | op1);
    }
};

struct BasicBlock {
    uint16_t start_addr = 0;
    uint16_t end_addr = 0;
    std::vector<Instruction> instructions;
    std::vector<uint16_t> successors;
};

struct Function {
    uint16_t entry_addr = 0;
    uint8_t bank = 0;
    bool has_bank = false;
    std::string name;
    std::set<uint16_t> block_addrs;

    std::string c_symbol() const;
};

struct BankCFG {
    uint8_t bank = 0;
    std::map<uint16_t, BasicBlock> blocks;
    std::map<uint16_t, Function> functions;
};

struct ProgramCFG {
    std::map<uint8_t, BankCFG> banks;

    const Function* find_function(uint8_t bank, uint16_t addr) const;
};

ProgramCFG disassemble_game_boy(std::span<const uint8_t> rom, const GameConfig& config);

}  // namespace dragon::gbrecomp
