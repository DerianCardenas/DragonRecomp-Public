#include "pce/opcodes.h"

namespace dragon::pce {

namespace {

using M = AddrMode;

// Full 256-entry Hudson Soft HuC6280 opcode table.
// Reference: Charles MacDonald pcetech.txt, docs/PCE_SPEC.md §2.
constexpr OpcodeInfo kOpcodes[256] = {
    // 0x00 - 0x0F
    {"BRK", M::Imm, 2, 8},     {"ORA", M::ZpIndX, 2, 7},  {"SXY", M::Imp, 1, 3},     {"ST0", M::Imm, 2, 5},
    {"TSB", M::Zp, 2, 6},      {"ORA", M::Zp, 2, 4},      {"ASL", M::Zp, 2, 6},      {"RMB0", M::Zp, 2, 7},
    {"PHP", M::Imp, 1, 3},     {"ORA", M::Imm, 2, 2},     {"ASL", M::Acc, 1, 2},     {"NOP", M::Imp, 1, 2},
    {"TSB", M::Abs, 3, 7},     {"ORA", M::Abs, 3, 5},     {"ASL", M::Abs, 3, 7},     {"BBR0", M::ZpRel, 3, 6},

    // 0x10 - 0x1F
    {"BPL", M::Rel, 2, 2},     {"ORA", M::ZpIndY, 2, 7},  {"ORA", M::ZpInd, 2, 7},   {"ST1", M::Imm, 2, 5},
    {"TRB", M::Zp, 2, 6},      {"ORA", M::ZpX, 2, 4},     {"ASL", M::ZpX, 2, 6},     {"RMB1", M::Zp, 2, 7},
    {"CLC", M::Imp, 1, 2},     {"ORA", M::AbsY, 3, 5},    {"INC", M::Acc, 1, 2},     {"NOP", M::Imp, 1, 2},
    {"TRB", M::Abs, 3, 7},     {"ORA", M::AbsX, 3, 5},    {"ASL", M::AbsX, 3, 7},    {"BBR1", M::ZpRel, 3, 6},

    // 0x20 - 0x2F
    {"JSR", M::Abs, 3, 7},     {"AND", M::ZpIndX, 2, 7},  {"SAX", M::Imp, 1, 3},     {"ST2", M::Imm, 2, 5},
    {"BIT", M::Zp, 2, 4},      {"AND", M::Zp, 2, 4},      {"ROL", M::Zp, 2, 6},      {"RMB2", M::Zp, 2, 7},
    {"PLP", M::Imp, 1, 4},     {"AND", M::Imm, 2, 2},     {"ROL", M::Acc, 1, 2},     {"NOP", M::Imp, 1, 2},
    {"BIT", M::Abs, 3, 5},     {"AND", M::Abs, 3, 5},     {"ROL", M::Abs, 3, 7},     {"BBR2", M::ZpRel, 3, 6},

    // 0x30 - 0x3F
    {"BMI", M::Rel, 2, 2},     {"AND", M::ZpIndY, 2, 7},  {"AND", M::ZpInd, 2, 7},   {"NOP", M::Imp, 1, 2},
    {"BIT", M::ZpX, 2, 4},     {"AND", M::ZpX, 2, 4},     {"ROL", M::ZpX, 2, 6},     {"RMB3", M::Zp, 2, 7},
    {"SEC", M::Imp, 1, 2},     {"AND", M::AbsY, 3, 5},    {"DEC", M::Acc, 1, 2},     {"NOP", M::Imp, 1, 2},
    {"BIT", M::AbsX, 3, 5},    {"AND", M::AbsX, 3, 5},    {"ROL", M::AbsX, 3, 7},    {"BBR3", M::ZpRel, 3, 6},

    // 0x40 - 0x4F
    {"RTI", M::Imp, 1, 7},     {"EOR", M::ZpIndX, 2, 7},  {"SAY", M::Imp, 1, 3},     {"TMA", M::Imm, 2, 4},
    {"BSR", M::Rel, 2, 8},     {"EOR", M::Zp, 2, 4},      {"LSR", M::Zp, 2, 6},      {"RMB4", M::Zp, 2, 7},
    {"PHA", M::Imp, 1, 3},     {"EOR", M::Imm, 2, 2},     {"LSR", M::Acc, 1, 2},     {"NOP", M::Imp, 1, 2},
    {"JMP", M::Abs, 3, 4},     {"EOR", M::Abs, 3, 5},     {"LSR", M::Abs, 3, 7},     {"BBR4", M::ZpRel, 3, 6},

    // 0x50 - 0x5F
    {"BVC", M::Rel, 2, 2},     {"EOR", M::ZpIndY, 2, 7},  {"EOR", M::ZpInd, 2, 7},   {"TAM", M::Imm, 2, 5},
    {"CSL", M::Imp, 1, 3},     {"EOR", M::ZpX, 2, 4},     {"LSR", M::ZpX, 2, 6},     {"RMB5", M::Zp, 2, 7},
    {"CLI", M::Imp, 1, 2},     {"EOR", M::AbsY, 3, 5},    {"PHY", M::Imp, 1, 3},     {"NOP", M::Imp, 1, 2},
    {"NOP", M::Imp, 1, 2},     {"EOR", M::AbsX, 3, 5},    {"LSR", M::AbsX, 3, 7},    {"BBR5", M::ZpRel, 3, 6},

    // 0x60 - 0x6F
    {"RTS", M::Imp, 1, 7},     {"ADC", M::ZpIndX, 2, 7},  {"CLA", M::Imp, 1, 2},     {"NOP", M::Imp, 1, 2},
    {"STZ", M::Zp, 2, 4},      {"ADC", M::Zp, 2, 4},      {"ROR", M::Zp, 2, 6},      {"RMB6", M::Zp, 2, 7},
    {"PLA", M::Imp, 1, 4},     {"ADC", M::Imm, 2, 2},     {"ROR", M::Acc, 1, 2},     {"NOP", M::Imp, 1, 2},
    {"JMP", M::Ind, 3, 7},     {"ADC", M::Abs, 3, 5},     {"ROR", M::Abs, 3, 7},     {"BBR6", M::ZpRel, 3, 6},

    // 0x70 - 0x7F
    {"BVS", M::Rel, 2, 2},     {"ADC", M::ZpIndY, 2, 7},  {"ADC", M::ZpInd, 2, 7},   {"TII", M::Block, 7, 17},
    {"STZ", M::ZpX, 2, 4},     {"ADC", M::ZpX, 2, 4},     {"ROR", M::ZpX, 2, 6},     {"RMB7", M::Zp, 2, 7},
    {"SEI", M::Imp, 1, 2},     {"ADC", M::AbsY, 3, 5},    {"PLY", M::Imp, 1, 4},     {"NOP", M::Imp, 1, 2},
    {"JMP", M::IndX, 3, 7},    {"ADC", M::AbsX, 3, 5},    {"ROR", M::AbsX, 3, 7},    {"BBR7", M::ZpRel, 3, 6},

    // 0x80 - 0x8F
    {"BRA", M::Rel, 2, 4},     {"STA", M::ZpIndX, 2, 7},  {"CLX", M::Imp, 1, 2},     {"TST", M::ImmZp, 3, 7},
    {"STY", M::Zp, 2, 4},      {"STA", M::Zp, 2, 4},      {"STX", M::Zp, 2, 4},      {"SMB0", M::Zp, 2, 7},
    {"DEY", M::Imp, 1, 2},     {"BIT", M::Imm, 2, 2},     {"TXA", M::Imp, 1, 2},     {"NOP", M::Imp, 1, 2},
    {"STY", M::Abs, 3, 5},     {"STA", M::Abs, 3, 5},     {"STX", M::Abs, 3, 5},     {"BBS0", M::ZpRel, 3, 6},

    // 0x90 - 0x9F
    {"BCC", M::Rel, 2, 2},     {"STA", M::ZpIndY, 2, 7},  {"STA", M::ZpInd, 2, 7},   {"TST", M::ImmAbs, 4, 8},
    {"STY", M::ZpX, 2, 4},     {"STA", M::ZpX, 2, 4},     {"STX", M::ZpY, 2, 4},     {"SMB1", M::Zp, 2, 7},
    {"TYA", M::Imp, 1, 2},     {"STA", M::AbsY, 3, 5},    {"TXS", M::Imp, 1, 2},     {"NOP", M::Imp, 1, 2},
    {"STZ", M::Abs, 3, 5},     {"STA", M::AbsX, 3, 5},    {"STZ", M::AbsX, 3, 5},    {"BBS1", M::ZpRel, 3, 6},

    // 0xA0 - 0xAF
    {"LDY", M::Imm, 2, 2},     {"LDA", M::ZpIndX, 2, 7},  {"LDX", M::Imm, 2, 2},     {"TST", M::ImmZpX, 3, 7},
    {"LDY", M::Zp, 2, 4},      {"LDA", M::Zp, 2, 4},      {"LDX", M::Zp, 2, 4},      {"SMB2", M::Zp, 2, 7},
    {"TAY", M::Imp, 1, 2},     {"LDA", M::Imm, 2, 2},     {"TAX", M::Imp, 1, 2},     {"NOP", M::Imp, 1, 2},
    {"LDY", M::Abs, 3, 5},     {"LDA", M::Abs, 3, 5},     {"LDX", M::Abs, 3, 5},     {"BBS2", M::ZpRel, 3, 6},

    // 0xB0 - 0xBF
    {"BCS", M::Rel, 2, 2},     {"LDA", M::ZpIndY, 2, 7},  {"LDA", M::ZpInd, 2, 7},   {"TST", M::ImmAbsX, 4, 8},
    {"LDY", M::ZpX, 2, 4},     {"LDA", M::ZpX, 2, 4},     {"LDX", M::ZpY, 2, 4},     {"SMB3", M::Zp, 2, 7},
    {"CLV", M::Imp, 1, 2},     {"LDA", M::AbsY, 3, 5},    {"TSX", M::Imp, 1, 2},     {"NOP", M::Imp, 1, 2},
    {"LDY", M::AbsX, 3, 5},    {"LDA", M::AbsX, 3, 5},    {"LDX", M::AbsY, 3, 5},    {"BBS3", M::ZpRel, 3, 6},

    // 0xC0 - 0xCF
    {"CPY", M::Imm, 2, 2},     {"CMP", M::ZpIndX, 2, 7},  {"CLY", M::Imp, 1, 2},     {"TDD", M::Block, 7, 17},
    {"CPY", M::Zp, 2, 4},      {"CMP", M::Zp, 2, 4},      {"DEC", M::Zp, 2, 6},      {"SMB4", M::Zp, 2, 7},
    {"INY", M::Imp, 1, 2},     {"CMP", M::Imm, 2, 2},     {"DEX", M::Imp, 1, 2},     {"NOP", M::Imp, 1, 2},
    {"CPY", M::Abs, 3, 5},     {"CMP", M::Abs, 3, 5},     {"DEC", M::Abs, 3, 7},     {"BBS4", M::ZpRel, 3, 6},

    // 0xD0 - 0xDF
    {"BNE", M::Rel, 2, 2},     {"CMP", M::ZpIndY, 2, 7},  {"CMP", M::ZpInd, 2, 7},   {"TIN", M::Block, 7, 17},
    {"CSH", M::Imp, 1, 3},     {"CMP", M::ZpX, 2, 4},     {"DEC", M::ZpX, 2, 6},     {"SMB5", M::Zp, 2, 7},
    {"CLD", M::Imp, 1, 2},     {"CMP", M::AbsY, 3, 5},    {"PHX", M::Imp, 1, 3},     {"NOP", M::Imp, 1, 2},
    {"NOP", M::Imp, 1, 2},     {"CMP", M::AbsX, 3, 5},    {"DEC", M::AbsX, 3, 7},    {"BBS5", M::ZpRel, 3, 6},

    // 0xE0 - 0xEF
    {"CPX", M::Imm, 2, 2},     {"SBC", M::ZpIndX, 2, 7},  {"NOP", M::Imp, 1, 2},     {"TIA", M::Block, 7, 17},
    {"CPX", M::Zp, 2, 4},      {"SBC", M::Zp, 2, 4},      {"INC", M::Zp, 2, 6},      {"SMB6", M::Zp, 2, 7},
    {"INX", M::Imp, 1, 2},     {"SBC", M::Imm, 2, 2},     {"NOP", M::Imp, 1, 2},     {"NOP", M::Imp, 1, 2},
    {"CPX", M::Abs, 3, 5},     {"SBC", M::Abs, 3, 5},     {"INC", M::Abs, 3, 7},     {"BBS6", M::ZpRel, 3, 6},

    // 0xF0 - 0xFF
    {"BEQ", M::Rel, 2, 2},     {"SBC", M::ZpIndY, 2, 7},  {"SBC", M::ZpInd, 2, 7},   {"TAI", M::Block, 7, 17},
    {"SET", M::Imp, 1, 2},     {"SBC", M::ZpX, 2, 4},     {"INC", M::ZpX, 2, 6},     {"SMB7", M::Zp, 2, 7},
    {"SED", M::Imp, 1, 2},     {"SBC", M::AbsY, 3, 5},    {"PLX", M::Imp, 1, 4},     {"NOP", M::Imp, 1, 2},
    {"NOP", M::Imp, 1, 2},     {"SBC", M::AbsX, 3, 5},    {"INC", M::AbsX, 3, 7},    {"BBS7", M::ZpRel, 3, 6},
};

}  // namespace

const OpcodeInfo& opcode_info(uint8_t op) noexcept {
    return kOpcodes[op];
}

}  // namespace dragon::pce
