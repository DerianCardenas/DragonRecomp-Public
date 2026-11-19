#pragma once

// Single source of truth for Zilog Z80 instruction lengths and T-cycle counts.
// Shared by smsrecomp disassembler, Z80 interpreter, and unit tests.
// Reference: Zilog Z80 CPU User Manual (UM0080) & docs/SMS_SPEC.md §2.2.

#include <cstdint>

namespace dragon::sms {

struct OpcodeInfo {
    uint8_t length;       // Total bytes including prefix (0 = prefix or invalid)
    uint8_t cycles;       // Base T-cycles (not-taken branch / non-repeating block)
    uint8_t cycles_taken; // T-cycles when branch taken or block op repeats
};

// Base (unprefixed) 256-entry Z80 opcode table
const OpcodeInfo& opcode_info_base(uint8_t op) noexcept;

// CB-prefixed 256-entry table (length includes 0xCB byte = 2)
const OpcodeInfo& opcode_info_cb(uint8_t op) noexcept;

// ED-prefixed 256-entry table (length includes 0xED byte)
const OpcodeInfo& opcode_info_ed(uint8_t op) noexcept;

// DD/FD-prefixed (IX/IY) instruction info given second byte `op`
// (for 0xCB returns length 4 for DDCB/FDCB <d> <op>)
OpcodeInfo opcode_info_index(uint8_t op) noexcept;

}  // namespace dragon::sms
