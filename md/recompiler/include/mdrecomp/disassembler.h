#pragma once

// Motorola 68000 static disassembler & recursive CFG builder for mdrecomp.

#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "md/cartridge.h"

namespace dragon::mdrecomp {

enum class FlowKind : uint8_t {
    Normal,
    BranchUncond,
    BranchCond,
    Call,
    CallIndirect,
    JumpIndirect,
    Return,
    Trap,
    Stop,
};

struct DecodedInsn {
    uint32_t addr = 0;
    uint16_t op = 0;
    uint8_t length = 2;
    FlowKind flow = FlowKind::Normal;
    uint32_t target = 0;
    bool has_target = false;
    uint16_t ext0 = 0;
    uint16_t ext1 = 0;
    uint32_t imm32 = 0;
};

struct FunctionCfg {
    uint32_t entry = 0;
    std::map<uint32_t, DecodedInsn> insns;
    std::set<uint32_t> internal_labels;
    std::set<uint32_t> callees;
};

struct ProgramCfg {
    std::map<uint32_t, FunctionCfg> functions;
    size_t total_blocks = 0;
    size_t total_instructions = 0;
    size_t indirect_jumps = 0;
};

// Decode a single M68000 instruction at `addr` from cartridge ROM.
DecodedInsn decode_m68k(const dragon::md::Cartridge& cart, uint32_t addr) noexcept;

// Build recursive CFG starting from Genesis vector table + `extra_entries`.
ProgramCfg build_cfg(const dragon::md::Cartridge& cart,
                     const std::vector<uint32_t>& extra_entries = {}, size_t max_functions = 8192U);

}  // namespace dragon::mdrecomp
