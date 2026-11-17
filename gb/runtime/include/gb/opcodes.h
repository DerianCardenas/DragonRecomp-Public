#pragma once

/*
 * Sharp SM83 (Game Boy) opcode and cycle reference table.
 * Shared by interpreter, recompiler (gbrecomp), and disassembler.
 *
 * All cycles are expressed in master T-cycles (4 T-cycles = 1 M-cycle).
 * Reference: https://gbdev.io/pandocs/CPU_Instruction_Set.html
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OpcodeInfo {
    uint8_t opcode;
    const char* mnemonic;
    uint8_t length;        /* Instruction length in bytes (1, 2, or 3) */
    uint8_t cycles;        /* Standard T-cycles (min / untaken) */
    uint8_t cycles_branch; /* T-cycles if branch taken (or same as cycles) */
    uint8_t is_valid;      /* 1 for valid official opcodes, 0 for unused/lock */
} OpcodeInfo;

extern const OpcodeInfo kGbBaseOpcodeTable[256];
extern const OpcodeInfo kGbCbOpcodeTable[256];

#ifdef __cplusplus
}
#endif
