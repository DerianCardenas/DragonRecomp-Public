#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "dragon/config.h"
#include "nes/opcodes.h"

namespace dragon::nesrecomp {

struct Instruction {
    uint16_t addr = 0;
    uint8_t opcode = 0;
    uint8_t op1 = 0;
    uint8_t op2 = 0;
    uint8_t length = 1;
    nes::Mnemonic mnemonic = nes::Mnemonic::NOP;
    nes::AddrMode mode = nes::AddrMode::Implied;
    uint8_t base_cycles = 2;
    bool page_cross_penalty = false;
    bool official = true;
    bool stable_unofficial = false;

    uint16_t operand_u16() const noexcept {
        if (mode == nes::AddrMode::Relative) {
            const auto rel = static_cast<int8_t>(op1);
            return static_cast<uint16_t>(static_cast<int32_t>(addr + 2U) + rel);
        }
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

struct JumpEngineTable {
    uint16_t jsr_addr = 0;
    uint16_t table_addr = 0;
    std::vector<uint16_t> entries;
};

struct IndirectJumpSite {
    uint16_t instr_addr = 0;
    uint16_t ptr_addr = 0;
    bool is_jump_engine = false;
};

struct BankCFG {
    uint8_t bank = 0;
    std::map<uint16_t, Instruction> instructions;
    std::map<uint16_t, BasicBlock> blocks;
    std::map<uint16_t, Function> functions;
};

struct ProgramCFG {
    uint16_t nmi_addr = 0;
    uint16_t reset_addr = 0;
    uint16_t irq_addr = 0;

    bool is_banked = false;
    uint16_t fixed_window_start = 0x8000;

    std::map<uint16_t, Instruction> instructions;
    std::map<uint16_t, BasicBlock> blocks;
    std::map<uint16_t, Function> functions;
    std::vector<BankCFG> bank_cfgs;
    std::vector<JumpEngineTable> jump_engine_tables;
    std::vector<IndirectJumpSite> indirect_jumps;
    std::vector<uint16_t> unofficial_warnings;
    std::set<uint16_t> jump_engine_targets;
};

// Disassemble PRG ROM mapped at $8000-$FFFF (unbanked or banked Mappers 0/1/2/3/4)
// and build the control-flow graph (basic blocks and functions).
ProgramCFG analyze_prg_rom(std::span<const uint8_t> prg_rom, const GameConfig& config);

// Format a human-readable disassembly & CFG summary listing.
std::string format_listing(const ProgramCFG& cfg);

}  // namespace dragon::nesrecomp
