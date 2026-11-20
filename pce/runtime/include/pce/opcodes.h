#pragma once

// HuC6280 256-opcode metadata table shared by interpreter, disassembler, recompiler, and tests.
// Reference: docs/PCE_SPEC.md §2, Charles MacDonald pcetech.txt.

#include <cstdint>

namespace dragon::pce {

enum class AddrMode : uint8_t {
    Imp,     // Implied (1 byte)
    Acc,     // Accumulator (1 byte)
    Imm,     // #imm (2 bytes)
    Zp,      // zp (2 bytes)
    ZpX,     // zp,X (2 bytes)
    ZpY,     // zp,Y (2 bytes)
    Abs,     // abs (3 bytes)
    AbsX,    // abs,X (3 bytes)
    AbsY,    // abs,Y (3 bytes)
    Rel,     // rel (2 bytes)
    Ind,     // (abs) (3 bytes)
    IndX,    // (abs,X) (3 bytes)
    ZpInd,   // (zp) (2 bytes)
    ZpIndX,  // (zp,X) (2 bytes)
    ZpIndY,  // (zp),Y (2 bytes)
    ZpRel,   // BBR/BBS: zp, rel (3 bytes)
    ImmZp,   // TST: #imm, zp (3 bytes)
    ImmAbs,  // TST: #imm, abs (4 bytes)
    ImmZpX,  // TST: #imm, zp,X (3 bytes)
    ImmAbsX, // TST: #imm, abs,X (4 bytes)
    Block,   // TII/TDD/TIN/TIA/TAI: src16, dst16, len16 (7 bytes)
};

struct OpcodeInfo {
    const char* mnemonic;
    AddrMode mode;
    uint8_t length;
    uint8_t cycles;
};

const OpcodeInfo& opcode_info(uint8_t op) noexcept;

}  // namespace dragon::pce
