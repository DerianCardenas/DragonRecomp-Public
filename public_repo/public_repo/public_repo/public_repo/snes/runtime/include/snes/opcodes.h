#pragma once

// Shared 65816 (Ricoh 5A22) opcode and cycle table.
// Single source of truth for snesrecomp, runtime interpreter, and unit tests.
// References:
//   https://snes.nesdev.org/wiki/65816_opcodes
//   https://problemkaputt.de/fullsnes.htm#snescpu65c816opcodes
//   https://wiki.superfamicom.org/65816-reference

#include <array>
#include <cstdint>

namespace dragon::snes {

enum class AddrMode : uint8_t {
    Implied,       // implicit / accumulator (1 byte)
    ImmM,          // #$nn (M=1 or E=1: 2 bytes) / #$nnnn (M=0 and E=0: 3 bytes)
    ImmX,          // #$nn (X=1 or E=1: 2 bytes) / #$nnnn (X=0 and E=0: 3 bytes)
    Imm8,          // #$nn (always 2 bytes: SEP, REP, COP, WDM, BRK)
    Imm16,         // #$nnnn (always 3 bytes: PEA)
    DP,            // $dp (2 bytes)
    DPX,           // $dp,X (2 bytes)
    DPY,           // $dp,Y (2 bytes)
    DPInd,         // ($dp) (2 bytes)
    DPIndLong,     // [$dp] (2 bytes)
    DPXInd,        // ($dp,X) (2 bytes)
    DPIndY,        // ($dp),Y (2 bytes)
    DPIndLongY,    // [$dp],Y (2 bytes)
    StackRel,      // $sr,S (2 bytes)
    StackRelIndY,  // ($sr,S),Y (2 bytes)
    Abs,           // $nnnn (3 bytes)
    AbsX,          // $nnnn,X (3 bytes)
    AbsY,          // $nnnn,Y (3 bytes)
    AbsLong,       // $bbnnnn (4 bytes)
    AbsLongX,      // $bbnnnn,X (4 bytes)
    AbsInd,        // ($nnnn) (3 bytes)
    AbsIndLong,    // [$nnnn] (3 bytes)
    AbsXInd,       // ($nnnn,X) (3 bytes)
    PCRel8,        // rel8 branch (2 bytes)
    PCRel16,       // rel16 branch / PER (3 bytes)
    BlockMove,     // srcbk,dstbk (3 bytes: MVN, MVP)
};

enum class Mnemonic : uint8_t {
    ADC,
    AND,
    ASL,
    BCC,
    BCS,
    BEQ,
    BIT,
    BMI,
    BNE,
    BPL,
    BRA,
    BRK,
    BRL,
    BVC,
    BVS,
    CLC,
    CLD,
    CLI,
    CLV,
    CMP,
    COP,
    CPX,
    CPY,
    DEC,
    DEX,
    DEY,
    EOR,
    INC,
    INX,
    INY,
    JML,
    JMP,
    JSL,
    JSR,
    LDA,
    LDX,
    LDY,
    LSR,
    MVN,
    MVP,
    NOP,
    ORA,
    PEA,
    PEI,
    PER,
    PHA,
    PHB,
    PHD,
    PHK,
    PHP,
    PHX,
    PHY,
    PLA,
    PLB,
    PLD,
    PLP,
    PLX,
    PLY,
    REP,
    ROL,
    ROR,
    RTI,
    RTL,
    RTS,
    SBC,
    SEC,
    SED,
    SEI,
    SEP,
    STA,
    STP,
    STX,
    STY,
    STZ,
    TAX,
    TAY,
    TCD,
    TCS,
    TDC,
    TRB,
    TSB,
    TSC,
    TSX,
    TXA,
    TXS,
    TXY,
    TYA,
    TYX,
    WAI,
    WDM,
    XBA,
    XCE,
};

struct OpcodeInfo {
    uint8_t opcode;
    Mnemonic mnemonic;
    const char* name;
    AddrMode mode;
    uint8_t base_cycles;
    bool page_cross_penalty;
};

constexpr std::array<OpcodeInfo, 256> kOpcodeTable = {{
    // 0x00 - 0x0F
    {0x00, Mnemonic::BRK, "BRK", AddrMode::Imm8, 7, false},
    {0x01, Mnemonic::ORA, "ORA", AddrMode::DPXInd, 6, false},
    {0x02, Mnemonic::COP, "COP", AddrMode::Imm8, 7, false},
    {0x03, Mnemonic::ORA, "ORA", AddrMode::StackRel, 4, false},
    {0x04, Mnemonic::TSB, "TSB", AddrMode::DP, 5, false},
    {0x05, Mnemonic::ORA, "ORA", AddrMode::DP, 3, false},
    {0x06, Mnemonic::ASL, "ASL", AddrMode::DP, 5, false},
    {0x07, Mnemonic::ORA, "ORA", AddrMode::DPIndLong, 6, false},
    {0x08, Mnemonic::PHP, "PHP", AddrMode::Implied, 3, false},
    {0x09, Mnemonic::ORA, "ORA", AddrMode::ImmM, 2, false},
    {0x0A, Mnemonic::ASL, "ASL", AddrMode::Implied, 2, false},
    {0x0B, Mnemonic::PHD, "PHD", AddrMode::Implied, 4, false},
    {0x0C, Mnemonic::TSB, "TSB", AddrMode::Abs, 6, false},
    {0x0D, Mnemonic::ORA, "ORA", AddrMode::Abs, 4, false},
    {0x0E, Mnemonic::ASL, "ASL", AddrMode::Abs, 6, false},
    {0x0F, Mnemonic::ORA, "ORA", AddrMode::AbsLong, 5, false},

    // 0x10 - 0x1F
    {0x10, Mnemonic::BPL, "BPL", AddrMode::PCRel8, 2, false},
    {0x11, Mnemonic::ORA, "ORA", AddrMode::DPIndY, 5, true},
    {0x12, Mnemonic::ORA, "ORA", AddrMode::DPInd, 5, false},
    {0x13, Mnemonic::ORA, "ORA", AddrMode::StackRelIndY, 7, false},
    {0x14, Mnemonic::TRB, "TRB", AddrMode::DP, 5, false},
    {0x15, Mnemonic::ORA, "ORA", AddrMode::DPX, 4, false},
    {0x16, Mnemonic::ASL, "ASL", AddrMode::DPX, 6, false},
    {0x17, Mnemonic::ORA, "ORA", AddrMode::DPIndLongY, 6, false},
    {0x18, Mnemonic::CLC, "CLC", AddrMode::Implied, 2, false},
    {0x19, Mnemonic::ORA, "ORA", AddrMode::AbsY, 4, true},
    {0x1A, Mnemonic::INC, "INC", AddrMode::Implied, 2, false},
    {0x1B, Mnemonic::TCS, "TCS", AddrMode::Implied, 2, false},
    {0x1C, Mnemonic::TRB, "TRB", AddrMode::Abs, 6, false},
    {0x1D, Mnemonic::ORA, "ORA", AddrMode::AbsX, 4, true},
    {0x1E, Mnemonic::ASL, "ASL", AddrMode::AbsX, 7, false},
    {0x1F, Mnemonic::ORA, "ORA", AddrMode::AbsLongX, 5, false},

    // 0x20 - 0x2F
    {0x20, Mnemonic::JSR, "JSR", AddrMode::Abs, 6, false},
    {0x21, Mnemonic::AND, "AND", AddrMode::DPXInd, 6, false},
    {0x22, Mnemonic::JSL, "JSL", AddrMode::AbsLong, 8, false},
    {0x23, Mnemonic::AND, "AND", AddrMode::StackRel, 4, false},
    {0x24, Mnemonic::BIT, "BIT", AddrMode::DP, 3, false},
    {0x25, Mnemonic::AND, "AND", AddrMode::DP, 3, false},
    {0x26, Mnemonic::ROL, "ROL", AddrMode::DP, 5, false},
    {0x27, Mnemonic::AND, "AND", AddrMode::DPIndLong, 6, false},
    {0x28, Mnemonic::PLP, "PLP", AddrMode::Implied, 4, false},
    {0x29, Mnemonic::AND, "AND", AddrMode::ImmM, 2, false},
    {0x2A, Mnemonic::ROL, "ROL", AddrMode::Implied, 2, false},
    {0x2B, Mnemonic::PLD, "PLD", AddrMode::Implied, 5, false},
    {0x2C, Mnemonic::BIT, "BIT", AddrMode::Abs, 4, false},
    {0x2D, Mnemonic::AND, "AND", AddrMode::Abs, 4, false},
    {0x2E, Mnemonic::ROL, "ROL", AddrMode::Abs, 6, false},
    {0x2F, Mnemonic::AND, "AND", AddrMode::AbsLong, 5, false},

    // 0x30 - 0x3F
    {0x30, Mnemonic::BMI, "BMI", AddrMode::PCRel8, 2, false},
    {0x31, Mnemonic::AND, "AND", AddrMode::DPIndY, 5, true},
    {0x32, Mnemonic::AND, "AND", AddrMode::DPInd, 5, false},
    {0x33, Mnemonic::AND, "AND", AddrMode::StackRelIndY, 7, false},
    {0x34, Mnemonic::BIT, "BIT", AddrMode::DPX, 4, false},
    {0x35, Mnemonic::AND, "AND", AddrMode::DPX, 4, false},
    {0x36, Mnemonic::ROL, "ROL", AddrMode::DPX, 6, false},
    {0x37, Mnemonic::AND, "AND", AddrMode::DPIndLongY, 6, false},
    {0x38, Mnemonic::SEC, "SEC", AddrMode::Implied, 2, false},
    {0x39, Mnemonic::AND, "AND", AddrMode::AbsY, 4, true},
    {0x3A, Mnemonic::DEC, "DEC", AddrMode::Implied, 2, false},
    {0x3B, Mnemonic::TSC, "TSC", AddrMode::Implied, 2, false},
    {0x3C, Mnemonic::BIT, "BIT", AddrMode::AbsX, 4, true},
    {0x3D, Mnemonic::AND, "AND", AddrMode::AbsX, 4, true},
    {0x3E, Mnemonic::ROL, "ROL", AddrMode::AbsX, 7, false},
    {0x3F, Mnemonic::AND, "AND", AddrMode::AbsLongX, 5, false},

    // 0x40 - 0x4F
    {0x40, Mnemonic::RTI, "RTI", AddrMode::Implied, 6, false},
    {0x41, Mnemonic::EOR, "EOR", AddrMode::DPXInd, 6, false},
    {0x42, Mnemonic::WDM, "WDM", AddrMode::Imm8, 2, false},
    {0x43, Mnemonic::EOR, "EOR", AddrMode::StackRel, 4, false},
    {0x44, Mnemonic::MVP, "MVP", AddrMode::BlockMove, 7, false},
    {0x45, Mnemonic::EOR, "EOR", AddrMode::DP, 3, false},
    {0x46, Mnemonic::LSR, "LSR", AddrMode::DP, 5, false},
    {0x47, Mnemonic::EOR, "EOR", AddrMode::DPIndLong, 6, false},
    {0x48, Mnemonic::PHA, "PHA", AddrMode::Implied, 3, false},
    {0x49, Mnemonic::EOR, "EOR", AddrMode::ImmM, 2, false},
    {0x4A, Mnemonic::LSR, "LSR", AddrMode::Implied, 2, false},
    {0x4B, Mnemonic::PHK, "PHK", AddrMode::Implied, 3, false},
    {0x4C, Mnemonic::JMP, "JMP", AddrMode::Abs, 3, false},
    {0x4D, Mnemonic::EOR, "EOR", AddrMode::Abs, 4, false},
    {0x4E, Mnemonic::LSR, "LSR", AddrMode::Abs, 6, false},
    {0x4F, Mnemonic::EOR, "EOR", AddrMode::AbsLong, 5, false},

    // 0x50 - 0x5F
    {0x50, Mnemonic::BVC, "BVC", AddrMode::PCRel8, 2, false},
    {0x51, Mnemonic::EOR, "EOR", AddrMode::DPIndY, 5, true},
    {0x52, Mnemonic::EOR, "EOR", AddrMode::DPInd, 5, false},
    {0x53, Mnemonic::EOR, "EOR", AddrMode::StackRelIndY, 7, false},
    {0x54, Mnemonic::MVN, "MVN", AddrMode::BlockMove, 7, false},
    {0x55, Mnemonic::EOR, "EOR", AddrMode::DPX, 4, false},
    {0x56, Mnemonic::LSR, "LSR", AddrMode::DPX, 6, false},
    {0x57, Mnemonic::EOR, "EOR", AddrMode::DPIndLongY, 6, false},
    {0x58, Mnemonic::CLI, "CLI", AddrMode::Implied, 2, false},
    {0x59, Mnemonic::EOR, "EOR", AddrMode::AbsY, 4, true},
    {0x5A, Mnemonic::PHY, "PHY", AddrMode::Implied, 3, false},
    {0x5B, Mnemonic::TCD, "TCD", AddrMode::Implied, 2, false},
    {0x5C, Mnemonic::JML, "JML", AddrMode::AbsLong, 4, false},
    {0x5D, Mnemonic::EOR, "EOR", AddrMode::AbsX, 4, true},
    {0x5E, Mnemonic::LSR, "LSR", AddrMode::AbsX, 7, false},
    {0x5F, Mnemonic::EOR, "EOR", AddrMode::AbsLongX, 5, false},

    // 0x60 - 0x6F
    {0x60, Mnemonic::RTS, "RTS", AddrMode::Implied, 6, false},
    {0x61, Mnemonic::ADC, "ADC", AddrMode::DPXInd, 6, false},
    {0x62, Mnemonic::PER, "PER", AddrMode::PCRel16, 6, false},
    {0x63, Mnemonic::ADC, "ADC", AddrMode::StackRel, 4, false},
    {0x64, Mnemonic::STZ, "STZ", AddrMode::DP, 3, false},
    {0x65, Mnemonic::ADC, "ADC", AddrMode::DP, 3, false},
    {0x66, Mnemonic::ROR, "ROR", AddrMode::DP, 5, false},
    {0x67, Mnemonic::ADC, "ADC", AddrMode::DPIndLong, 6, false},
    {0x68, Mnemonic::PLA, "PLA", AddrMode::Implied, 4, false},
    {0x69, Mnemonic::ADC, "ADC", AddrMode::ImmM, 2, false},
    {0x6A, Mnemonic::ROR, "ROR", AddrMode::Implied, 2, false},
    {0x6B, Mnemonic::RTL, "RTL", AddrMode::Implied, 6, false},
    {0x6C, Mnemonic::JMP, "JMP", AddrMode::AbsInd, 5, false},
    {0x6D, Mnemonic::ADC, "ADC", AddrMode::Abs, 4, false},
    {0x6E, Mnemonic::ROR, "ROR", AddrMode::Abs, 6, false},
    {0x6F, Mnemonic::ADC, "ADC", AddrMode::AbsLong, 5, false},

    // 0x70 - 0x7F
    {0x70, Mnemonic::BVS, "BVS", AddrMode::PCRel8, 2, false},
    {0x71, Mnemonic::ADC, "ADC", AddrMode::DPIndY, 5, true},
    {0x72, Mnemonic::ADC, "ADC", AddrMode::DPInd, 5, false},
    {0x73, Mnemonic::ADC, "ADC", AddrMode::StackRelIndY, 7, false},
    {0x74, Mnemonic::STZ, "STZ", AddrMode::DPX, 4, false},
    {0x75, Mnemonic::ADC, "ADC", AddrMode::DPX, 4, false},
    {0x76, Mnemonic::ROR, "ROR", AddrMode::DPX, 6, false},
    {0x77, Mnemonic::ADC, "ADC", AddrMode::DPIndLongY, 6, false},
    {0x78, Mnemonic::SEI, "SEI", AddrMode::Implied, 2, false},
    {0x79, Mnemonic::ADC, "ADC", AddrMode::AbsY, 4, true},
    {0x7A, Mnemonic::PLY, "PLY", AddrMode::Implied, 4, false},
    {0x7B, Mnemonic::TDC, "TDC", AddrMode::Implied, 2, false},
    {0x7C, Mnemonic::JMP, "JMP", AddrMode::AbsXInd, 6, false},
    {0x7D, Mnemonic::ADC, "ADC", AddrMode::AbsX, 4, true},
    {0x7E, Mnemonic::ROR, "ROR", AddrMode::AbsX, 7, false},
    {0x7F, Mnemonic::ADC, "ADC", AddrMode::AbsLongX, 5, false},

    // 0x80 - 0x8F
    {0x80, Mnemonic::BRA, "BRA", AddrMode::PCRel8, 3, false},
    {0x81, Mnemonic::STA, "STA", AddrMode::DPXInd, 6, false},
    {0x82, Mnemonic::BRL, "BRL", AddrMode::PCRel16, 4, false},
    {0x83, Mnemonic::STA, "STA", AddrMode::StackRel, 4, false},
    {0x84, Mnemonic::STY, "STY", AddrMode::DP, 3, false},
    {0x85, Mnemonic::STA, "STA", AddrMode::DP, 3, false},
    {0x86, Mnemonic::STX, "STX", AddrMode::DP, 3, false},
    {0x87, Mnemonic::STA, "STA", AddrMode::DPIndLong, 6, false},
    {0x88, Mnemonic::DEY, "DEY", AddrMode::Implied, 2, false},
    {0x89, Mnemonic::BIT, "BIT", AddrMode::ImmM, 2, false},
    {0x8A, Mnemonic::TXA, "TXA", AddrMode::Implied, 2, false},
    {0x8B, Mnemonic::PHB, "PHB", AddrMode::Implied, 3, false},
    {0x8C, Mnemonic::STY, "STY", AddrMode::Abs, 4, false},
    {0x8D, Mnemonic::STA, "STA", AddrMode::Abs, 4, false},
    {0x8E, Mnemonic::STX, "STX", AddrMode::Abs, 4, false},
    {0x8F, Mnemonic::STA, "STA", AddrMode::AbsLong, 5, false},

    // 0x90 - 0x9F
    {0x90, Mnemonic::BCC, "BCC", AddrMode::PCRel8, 2, false},
    {0x91, Mnemonic::STA, "STA", AddrMode::DPIndY, 6, false},
    {0x92, Mnemonic::STA, "STA", AddrMode::DPInd, 5, false},
    {0x93, Mnemonic::STA, "STA", AddrMode::StackRelIndY, 7, false},
    {0x94, Mnemonic::STY, "STY", AddrMode::DPX, 4, false},
    {0x95, Mnemonic::STA, "STA", AddrMode::DPX, 4, false},
    {0x96, Mnemonic::STX, "STX", AddrMode::DPY, 4, false},
    {0x97, Mnemonic::STA, "STA", AddrMode::DPIndLongY, 6, false},
    {0x98, Mnemonic::TYA, "TYA", AddrMode::Implied, 2, false},
    {0x99, Mnemonic::STA, "STA", AddrMode::AbsY, 5, false},
    {0x9A, Mnemonic::TXS, "TXS", AddrMode::Implied, 2, false},
    {0x9B, Mnemonic::TXY, "TXY", AddrMode::Implied, 2, false},
    {0x9C, Mnemonic::STZ, "STZ", AddrMode::Abs, 4, false},
    {0x9D, Mnemonic::STA, "STA", AddrMode::AbsX, 5, false},
    {0x9E, Mnemonic::STZ, "STZ", AddrMode::AbsX, 5, false},
    {0x9F, Mnemonic::STA, "STA", AddrMode::AbsLongX, 5, false},

    // 0xA0 - 0xAF
    {0xA0, Mnemonic::LDY, "LDY", AddrMode::ImmX, 2, false},
    {0xA1, Mnemonic::LDA, "LDA", AddrMode::DPXInd, 6, false},
    {0xA2, Mnemonic::LDX, "LDX", AddrMode::ImmX, 2, false},
    {0xA3, Mnemonic::LDA, "LDA", AddrMode::StackRel, 4, false},
    {0xA4, Mnemonic::LDY, "LDY", AddrMode::DP, 3, false},
    {0xA5, Mnemonic::LDA, "LDA", AddrMode::DP, 3, false},
    {0xA6, Mnemonic::LDX, "LDX", AddrMode::DP, 3, false},
    {0xA7, Mnemonic::LDA, "LDA", AddrMode::DPIndLong, 6, false},
    {0xA8, Mnemonic::TAY, "TAY", AddrMode::Implied, 2, false},
    {0xA9, Mnemonic::LDA, "LDA", AddrMode::ImmM, 2, false},
    {0xAA, Mnemonic::TAX, "TAX", AddrMode::Implied, 2, false},
    {0xAB, Mnemonic::PLB, "PLB", AddrMode::Implied, 4, false},
    {0xAC, Mnemonic::LDY, "LDY", AddrMode::Abs, 4, false},
    {0xAD, Mnemonic::LDA, "LDA", AddrMode::Abs, 4, false},
    {0xAE, Mnemonic::LDX, "LDX", AddrMode::Abs, 4, false},
    {0xAF, Mnemonic::LDA, "LDA", AddrMode::AbsLong, 5, false},

    // 0xB0 - 0xBF
    {0xB0, Mnemonic::BCS, "BCS", AddrMode::PCRel8, 2, false},
    {0xB1, Mnemonic::LDA, "LDA", AddrMode::DPIndY, 5, true},
    {0xB2, Mnemonic::LDA, "LDA", AddrMode::DPInd, 5, false},
    {0xB3, Mnemonic::LDA, "LDA", AddrMode::StackRelIndY, 7, false},
    {0xB4, Mnemonic::LDY, "LDY", AddrMode::DPX, 4, false},
    {0xB5, Mnemonic::LDA, "LDA", AddrMode::DPX, 4, false},
    {0xB6, Mnemonic::LDX, "LDX", AddrMode::DPY, 4, false},
    {0xB7, Mnemonic::LDA, "LDA", AddrMode::DPIndLongY, 6, false},
    {0xB8, Mnemonic::CLV, "CLV", AddrMode::Implied, 2, false},
    {0xB9, Mnemonic::LDA, "LDA", AddrMode::AbsY, 4, true},
    {0xBA, Mnemonic::TSX, "TSX", AddrMode::Implied, 2, false},
    {0xBB, Mnemonic::TYX, "TYX", AddrMode::Implied, 2, false},
    {0xBC, Mnemonic::LDY, "LDY", AddrMode::AbsX, 4, true},
    {0xBD, Mnemonic::LDA, "LDA", AddrMode::AbsX, 4, true},
    {0xBE, Mnemonic::LDX, "LDX", AddrMode::AbsY, 4, true},
    {0xBF, Mnemonic::LDA, "LDA", AddrMode::AbsLongX, 5, false},

    // 0xC0 - 0xCF
    {0xC0, Mnemonic::CPY, "CPY", AddrMode::ImmX, 2, false},
    {0xC1, Mnemonic::CMP, "CMP", AddrMode::DPXInd, 6, false},
    {0xC2, Mnemonic::REP, "REP", AddrMode::Imm8, 3, false},
    {0xC3, Mnemonic::CMP, "CMP", AddrMode::StackRel, 4, false},
    {0xC4, Mnemonic::CPY, "CPY", AddrMode::DP, 3, false},
    {0xC5, Mnemonic::CMP, "CMP", AddrMode::DP, 3, false},
    {0xC6, Mnemonic::DEC, "DEC", AddrMode::DP, 5, false},
    {0xC7, Mnemonic::CMP, "CMP", AddrMode::DPIndLong, 6, false},
    {0xC8, Mnemonic::INY, "INY", AddrMode::Implied, 2, false},
    {0xC9, Mnemonic::CMP, "CMP", AddrMode::ImmM, 2, false},
    {0xCA, Mnemonic::DEX, "DEX", AddrMode::Implied, 2, false},
    {0xCB, Mnemonic::WAI, "WAI", AddrMode::Implied, 3, false},
    {0xCC, Mnemonic::CPY, "CPY", AddrMode::Abs, 4, false},
    {0xCD, Mnemonic::CMP, "CMP", AddrMode::Abs, 4, false},
    {0xCE, Mnemonic::DEC, "DEC", AddrMode::Abs, 6, false},
    {0xCF, Mnemonic::CMP, "CMP", AddrMode::AbsLong, 5, false},

    // 0xD0 - 0xDF
    {0xD0, Mnemonic::BNE, "BNE", AddrMode::PCRel8, 2, false},
    {0xD1, Mnemonic::CMP, "CMP", AddrMode::DPIndY, 5, true},
    {0xD2, Mnemonic::CMP, "CMP", AddrMode::DPInd, 5, false},
    {0xD3, Mnemonic::CMP, "CMP", AddrMode::StackRelIndY, 7, false},
    {0xD4, Mnemonic::PEI, "PEI", AddrMode::DPInd, 6, false},
    {0xD5, Mnemonic::CMP, "CMP", AddrMode::DPX, 4, false},
    {0xD6, Mnemonic::DEC, "DEC", AddrMode::DPX, 6, false},
    {0xD7, Mnemonic::CMP, "CMP", AddrMode::DPIndLongY, 6, false},
    {0xD8, Mnemonic::CLD, "CLD", AddrMode::Implied, 2, false},
    {0xD9, Mnemonic::CMP, "CMP", AddrMode::AbsY, 4, true},
    {0xDA, Mnemonic::PHX, "PHX", AddrMode::Implied, 3, false},
    {0xDB, Mnemonic::STP, "STP", AddrMode::Implied, 3, false},
    {0xDC, Mnemonic::JML, "JML", AddrMode::AbsIndLong, 6, false},
    {0xDD, Mnemonic::CMP, "CMP", AddrMode::AbsX, 4, true},
    {0xDE, Mnemonic::DEC, "DEC", AddrMode::AbsX, 7, false},
    {0xDF, Mnemonic::CMP, "CMP", AddrMode::AbsLongX, 5, false},

    // 0xE0 - 0xEF
    {0xE0, Mnemonic::CPX, "CPX", AddrMode::ImmX, 2, false},
    {0xE1, Mnemonic::SBC, "SBC", AddrMode::DPXInd, 6, false},
    {0xE2, Mnemonic::SEP, "SEP", AddrMode::Imm8, 3, false},
    {0xE3, Mnemonic::SBC, "SBC", AddrMode::StackRel, 4, false},
    {0xE4, Mnemonic::CPX, "CPX", AddrMode::DP, 3, false},
    {0xE5, Mnemonic::SBC, "SBC", AddrMode::DP, 3, false},
    {0xE6, Mnemonic::INC, "INC", AddrMode::DP, 5, false},
    {0xE7, Mnemonic::SBC, "SBC", AddrMode::DPIndLong, 6, false},
    {0xE8, Mnemonic::INX, "INX", AddrMode::Implied, 2, false},
    {0xE9, Mnemonic::SBC, "SBC", AddrMode::ImmM, 2, false},
    {0xEA, Mnemonic::NOP, "NOP", AddrMode::Implied, 2, false},
    {0xEB, Mnemonic::XBA, "XBA", AddrMode::Implied, 3, false},
    {0xEC, Mnemonic::CPX, "CPX", AddrMode::Abs, 4, false},
    {0xED, Mnemonic::SBC, "SBC", AddrMode::Abs, 4, false},
    {0xEE, Mnemonic::INC, "INC", AddrMode::Abs, 6, false},
    {0xEF, Mnemonic::SBC, "SBC", AddrMode::AbsLong, 5, false},

    // 0xF0 - 0xFF
    {0xF0, Mnemonic::BEQ, "BEQ", AddrMode::PCRel8, 2, false},
    {0xF1, Mnemonic::SBC, "SBC", AddrMode::DPIndY, 5, true},
    {0xF2, Mnemonic::SBC, "SBC", AddrMode::DPInd, 5, false},
    {0xF3, Mnemonic::SBC, "SBC", AddrMode::StackRelIndY, 7, false},
    {0xF4, Mnemonic::PEA, "PEA", AddrMode::Imm16, 5, false},
    {0xF5, Mnemonic::SBC, "SBC", AddrMode::DPX, 4, false},
    {0xF6, Mnemonic::INC, "INC", AddrMode::DPX, 6, false},
    {0xF7, Mnemonic::SBC, "SBC", AddrMode::DPIndLongY, 6, false},
    {0xF8, Mnemonic::SED, "SED", AddrMode::Implied, 2, false},
    {0xF9, Mnemonic::SBC, "SBC", AddrMode::AbsY, 4, true},
    {0xFA, Mnemonic::PLX, "PLX", AddrMode::Implied, 4, false},
    {0xFB, Mnemonic::XCE, "XCE", AddrMode::Implied, 2, false},
    {0xFC, Mnemonic::JSR, "JSR", AddrMode::AbsXInd, 8, false},
    {0xFD, Mnemonic::SBC, "SBC", AddrMode::AbsX, 4, true},
    {0xFE, Mnemonic::INC, "INC", AddrMode::AbsX, 7, false},
    {0xFF, Mnemonic::SBC, "SBC", AddrMode::AbsLongX, 5, false},
}};

constexpr const OpcodeInfo& get_opcode_info(uint8_t op) {
    return kOpcodeTable[op];
}

constexpr uint8_t instruction_length(AddrMode mode, bool m_8bit, bool x_8bit,
                                     bool emulation = false) {
    const bool eff_m = emulation || m_8bit;
    const bool eff_x = emulation || x_8bit;
    switch (mode) {
        case AddrMode::Implied:
            return 1;
        case AddrMode::ImmM:
            return eff_m ? 2 : 3;
        case AddrMode::ImmX:
            return eff_x ? 2 : 3;
        case AddrMode::Imm8:
        case AddrMode::DP:
        case AddrMode::DPX:
        case AddrMode::DPY:
        case AddrMode::DPInd:
        case AddrMode::DPIndLong:
        case AddrMode::DPXInd:
        case AddrMode::DPIndY:
        case AddrMode::DPIndLongY:
        case AddrMode::StackRel:
        case AddrMode::StackRelIndY:
        case AddrMode::PCRel8:
            return 2;
        case AddrMode::Imm16:
        case AddrMode::Abs:
        case AddrMode::AbsX:
        case AddrMode::AbsY:
        case AddrMode::AbsInd:
        case AddrMode::AbsIndLong:
        case AddrMode::AbsXInd:
        case AddrMode::PCRel16:
        case AddrMode::BlockMove:
            return 3;
        case AddrMode::AbsLong:
        case AddrMode::AbsLongX:
            return 4;
    }
    return 1;
}

constexpr uint8_t instruction_length(uint8_t op, bool m_8bit, bool x_8bit, bool emulation = false) {
    return instruction_length(kOpcodeTable[op].mode, m_8bit, x_8bit, emulation);
}

constexpr bool is_conditional_branch(Mnemonic m) {
    return m == Mnemonic::BCC || m == Mnemonic::BCS || m == Mnemonic::BEQ || m == Mnemonic::BMI ||
           m == Mnemonic::BNE || m == Mnemonic::BPL || m == Mnemonic::BVC || m == Mnemonic::BVS;
}

constexpr bool is_branch_mnemonic(Mnemonic m) {
    return is_conditional_branch(m) || m == Mnemonic::BRA || m == Mnemonic::BRL;
}

}  // namespace dragon::snes
