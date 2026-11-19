#pragma once

// Zilog Z80 disassembler and bank-aware recursive-descent CFG builder for smsrecomp.
// Reference: docs/SMS_SPEC.md §6.

#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "dragon/config.h"
#include "sms/cartridge.h"

namespace dragon::smsrecomp {

enum class Prefix : uint8_t {
    None,
    CB,
    ED,
    DD,
    FD,
    DDCB,
    FDCB,
};

struct DecodedInstr {
    uint16_t addr = 0;
    uint8_t bank = 0;
    uint8_t length = 1;
    uint8_t cycles = 4;
    uint8_t cycles_taken = 4;
    Prefix prefix = Prefix::None;
    uint8_t opcode = 0;
    int8_t disp = 0;
    uint16_t imm = 0;

    bool has_branch = false;
    bool is_call = false;
    bool is_conditional = false;
    bool is_terminator = false;
    bool is_indirect_jump = false;
    uint16_t target_addr = 0;
};

struct FunctionCfg {
    uint8_t bank = 0;
    uint16_t entry_addr = 0;
    std::map<uint16_t, DecodedInstr> instructions;
    std::set<uint16_t> local_labels;
};

struct DisassemblyResult {
    std::vector<FunctionCfg> functions;
    size_t total_instructions = 0;
};

// Decode a single Z80 instruction at `(bank, addr)` from `cart`
DecodedInstr decode_instruction(const sms::Cartridge& cart, uint8_t bank, uint16_t addr) noexcept;

// Build bank-aware function CFGs starting from reset/IRQ/NMI + TOML entry points
DisassemblyResult disassemble_rom(const sms::Cartridge& cart, const dragon::GameConfig& cfg);

}  // namespace dragon::smsrecomp
