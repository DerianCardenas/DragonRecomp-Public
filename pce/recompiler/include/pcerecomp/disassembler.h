#pragma once

// HuC6280 static disassembler & CFG builder for pcerecomp.
// Reference: docs/PCE_SPEC.md §5.

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "dragon/config.h"
#include "pce/cartridge.h"
#include "pce/opcodes.h"

namespace dragon::pcerecomp {

struct DecodedInstr {
    uint16_t addr = 0;
    uint8_t bank = 0;
    uint8_t opcode = 0;
    pce::AddrMode mode = pce::AddrMode::Imp;
    uint8_t length = 1;
    uint8_t cycles = 2;

    uint8_t imm8 = 0;
    uint8_t zp = 0;
    int8_t rel = 0;
    uint16_t addr16 = 0;

    // Block transfer operands (TII/TDD/TIN/TIA/TAI)
    uint16_t blk_src = 0;
    uint16_t blk_dst = 0;
    uint16_t blk_len = 0;

    uint16_t target_addr = 0;
    bool has_branch = false;
    bool is_call = false;
    bool is_conditional = false;
    bool is_terminator = false;
    bool is_indirect_jump = false;
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

DecodedInstr decode_instruction(const pce::Cartridge& cart, uint8_t bank, uint16_t addr) noexcept;
DisassemblyResult disassemble_rom(const pce::Cartridge& cart, const dragon::GameConfig& cfg);

}  // namespace dragon::pcerecomp
