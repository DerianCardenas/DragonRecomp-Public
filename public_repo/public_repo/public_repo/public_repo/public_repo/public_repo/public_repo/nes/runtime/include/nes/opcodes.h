#pragma once

// Shared 6502 (Ricoh 2A03) opcode and cycle table.
// Single source of truth for nesrecomp, runtime interpreter, and unit tests.
// References:
//   https://www.nesdev.org/wiki/6502_instructions
//   https://www.nesdev.org/wiki/CPU_unofficial_opcodes
//   https://www.masswerk.at/6502/6502_instruction_set.html

#include <array>
#include <cstdint>
#include <string_view>

namespace dragon::nes {

enum class AddrMode : uint8_t {
    Implied,           // implicit (no operand)
    Accumulator,       // A
    Immediate,         // #$nn
    ZeroPage,          // $nn
    ZeroPageX,         // $nn,X
    ZeroPageY,         // $nn,Y
    Relative,          // $nn (signed 8-bit branch offset)
    Absolute,          // $nnnn
    AbsoluteX,         // $nnnn,X
    AbsoluteY,         // $nnnn,Y
    Indirect,          // ($nnnn)
    IndexedIndirectX,  // ($nn,X)
    IndirectIndexedY,  // ($nn),Y
};

enum class Mnemonic : uint8_t {
    // Official 56 mnemonics
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
    BRK,
    BVC,
    BVS,
    CLC,
    CLD,
    CLI,
    CLV,
    CMP,
    CPX,
    CPY,
    DEC,
    DEX,
    DEY,
    EOR,
    INC,
    INX,
    INY,
    JMP,
    JSR,
    LDA,
    LDX,
    LDY,
    LSR,
    NOP,
    ORA,
    PHA,
    PHP,
    PLA,
    PLP,
    ROL,
    ROR,
    RTI,
    RTS,
    SBC,
    SEC,
    SED,
    SEI,
    STA,
    STX,
    STY,
    TAX,
    TAY,
    TSX,
    TXA,
    TXS,
    TYA,

    // Stable unofficial mnemonics
    LAX,
    SAX,
    DCP,
    ISC,
    SLO,
    RLA,
    SRE,
    RRA,
    ANC,
    ALR,
    ARR,
    AXS,

    // Unstable / halt unofficial mnemonics
    KIL,
    XAA,
    AHX,
    TAS,
    SHY,
    SHX,
    LAS,
};

struct OpcodeInfo {
    uint8_t opcode;
    Mnemonic mnemonic;
    const char* name;
    AddrMode mode;
    uint8_t length;
    uint8_t base_cycles;
    bool page_cross_penalty;
    bool official;
    bool stable_unofficial;
};

constexpr std::array<OpcodeInfo, 256> kOpcodeTable = {{
    // 0x00 - 0x0F
    {0x00, Mnemonic::BRK, "BRK", AddrMode::Implied, 1, 7, false, true, false},
    {0x01, Mnemonic::ORA, "ORA", AddrMode::IndexedIndirectX, 2, 6, false, true, false},
    {0x02, Mnemonic::KIL, "KIL", AddrMode::Implied, 1, 2, false, false, false},
    {0x03, Mnemonic::SLO, "SLO", AddrMode::IndexedIndirectX, 2, 8, false, false, true},
    {0x04, Mnemonic::NOP, "NOP", AddrMode::ZeroPage, 2, 3, false, false, true},
    {0x05, Mnemonic::ORA, "ORA", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0x06, Mnemonic::ASL, "ASL", AddrMode::ZeroPage, 2, 5, false, true, false},
    {0x07, Mnemonic::SLO, "SLO", AddrMode::ZeroPage, 2, 5, false, false, true},
    {0x08, Mnemonic::PHP, "PHP", AddrMode::Implied, 1, 3, false, true, false},
    {0x09, Mnemonic::ORA, "ORA", AddrMode::Immediate, 2, 2, false, true, false},
    {0x0A, Mnemonic::ASL, "ASL", AddrMode::Accumulator, 1, 2, false, true, false},
    {0x0B, Mnemonic::ANC, "ANC", AddrMode::Immediate, 2, 2, false, false, true},
    {0x0C, Mnemonic::NOP, "NOP", AddrMode::Absolute, 3, 4, false, false, true},
    {0x0D, Mnemonic::ORA, "ORA", AddrMode::Absolute, 3, 4, false, true, false},
    {0x0E, Mnemonic::ASL, "ASL", AddrMode::Absolute, 3, 6, false, true, false},
    {0x0F, Mnemonic::SLO, "SLO", AddrMode::Absolute, 3, 6, false, false, true},

    // 0x10 - 0x1F
    {0x10, Mnemonic::BPL, "BPL", AddrMode::Relative, 2, 2, false, true, false},
    {0x11, Mnemonic::ORA, "ORA", AddrMode::IndirectIndexedY, 2, 5, true, true, false},
    {0x12, Mnemonic::KIL, "KIL", AddrMode::Implied, 1, 2, false, false, false},
    {0x13, Mnemonic::SLO, "SLO", AddrMode::IndirectIndexedY, 2, 8, false, false, true},
    {0x14, Mnemonic::NOP, "NOP", AddrMode::ZeroPageX, 2, 4, false, false, true},
    {0x15, Mnemonic::ORA, "ORA", AddrMode::ZeroPageX, 2, 4, false, true, false},
    {0x16, Mnemonic::ASL, "ASL", AddrMode::ZeroPageX, 2, 6, false, true, false},
    {0x17, Mnemonic::SLO, "SLO", AddrMode::ZeroPageX, 2, 6, false, false, true},
    {0x18, Mnemonic::CLC, "CLC", AddrMode::Implied, 1, 2, false, true, false},
    {0x19, Mnemonic::ORA, "ORA", AddrMode::AbsoluteY, 3, 4, true, true, false},
    {0x1A, Mnemonic::NOP, "NOP", AddrMode::Implied, 1, 2, false, false, true},
    {0x1B, Mnemonic::SLO, "SLO", AddrMode::AbsoluteY, 3, 7, false, false, true},
    {0x1C, Mnemonic::NOP, "NOP", AddrMode::AbsoluteX, 3, 4, true, false, true},
    {0x1D, Mnemonic::ORA, "ORA", AddrMode::AbsoluteX, 3, 4, true, true, false},
    {0x1E, Mnemonic::ASL, "ASL", AddrMode::AbsoluteX, 3, 7, false, true, false},
    {0x1F, Mnemonic::SLO, "SLO", AddrMode::AbsoluteX, 3, 7, false, false, true},

    // 0x20 - 0x2F
    {0x20, Mnemonic::JSR, "JSR", AddrMode::Absolute, 3, 6, false, true, false},
    {0x21, Mnemonic::AND, "AND", AddrMode::IndexedIndirectX, 2, 6, false, true, false},
    {0x22, Mnemonic::KIL, "KIL", AddrMode::Implied, 1, 2, false, false, false},
    {0x23, Mnemonic::RLA, "RLA", AddrMode::IndexedIndirectX, 2, 8, false, false, true},
    {0x24, Mnemonic::BIT, "BIT", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0x25, Mnemonic::AND, "AND", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0x26, Mnemonic::ROL, "ROL", AddrMode::ZeroPage, 2, 5, false, true, false},
    {0x27, Mnemonic::RLA, "RLA", AddrMode::ZeroPage, 2, 5, false, false, true},
    {0x28, Mnemonic::PLP, "PLP", AddrMode::Implied, 1, 4, false, true, false},
    {0x29, Mnemonic::AND, "AND", AddrMode::Immediate, 2, 2, false, true, false},
    {0x2A, Mnemonic::ROL, "ROL", AddrMode::Accumulator, 1, 2, false, true, false},
    {0x2B, Mnemonic::ANC, "ANC", AddrMode::Immediate, 2, 2, false, false, true},
    {0x2C, Mnemonic::BIT, "BIT", AddrMode::Absolute, 3, 4, false, true, false},
    {0x2D, Mnemonic::AND, "AND", AddrMode::Absolute, 3, 4, false, true, false},
    {0x2E, Mnemonic::ROL, "ROL", AddrMode::Absolute, 3, 6, false, true, false},
    {0x2F, Mnemonic::RLA, "RLA", AddrMode::Absolute, 3, 6, false, false, true},

    // 0x30 - 0x3F
    {0x30, Mnemonic::BMI, "BMI", AddrMode::Relative, 2, 2, false, true, false},
    {0x31, Mnemonic::AND, "AND", AddrMode::IndirectIndexedY, 2, 5, true, true, false},
    {0x32, Mnemonic::KIL, "KIL", AddrMode::Implied, 1, 2, false, false, false},
    {0x33, Mnemonic::RLA, "RLA", AddrMode::IndirectIndexedY, 2, 8, false, false, true},
    {0x34, Mnemonic::NOP, "NOP", AddrMode::ZeroPageX, 2, 4, false, false, true},
    {0x35, Mnemonic::AND, "AND", AddrMode::ZeroPageX, 2, 4, false, true, false},
    {0x36, Mnemonic::ROL, "ROL", AddrMode::ZeroPageX, 2, 6, false, true, false},
    {0x37, Mnemonic::RLA, "RLA", AddrMode::ZeroPageX, 2, 6, false, false, true},
    {0x38, Mnemonic::SEC, "SEC", AddrMode::Implied, 1, 2, false, true, false},
    {0x39, Mnemonic::AND, "AND", AddrMode::AbsoluteY, 3, 4, true, true, false},
    {0x3A, Mnemonic::NOP, "NOP", AddrMode::Implied, 1, 2, false, false, true},
    {0x3B, Mnemonic::RLA, "RLA", AddrMode::AbsoluteY, 3, 7, false, false, true},
    {0x3C, Mnemonic::NOP, "NOP", AddrMode::AbsoluteX, 3, 4, true, false, true},
    {0x3D, Mnemonic::AND, "AND", AddrMode::AbsoluteX, 3, 4, true, true, false},
    {0x3E, Mnemonic::ROL, "ROL", AddrMode::AbsoluteX, 3, 7, false, true, false},
    {0x3F, Mnemonic::RLA, "RLA", AddrMode::AbsoluteX, 3, 7, false, false, true},

    // 0x40 - 0x4F
    {0x40, Mnemonic::RTI, "RTI", AddrMode::Implied, 1, 6, false, true, false},
    {0x41, Mnemonic::EOR, "EOR", AddrMode::IndexedIndirectX, 2, 6, false, true, false},
    {0x42, Mnemonic::KIL, "KIL", AddrMode::Implied, 1, 2, false, false, false},
    {0x43, Mnemonic::SRE, "SRE", AddrMode::IndexedIndirectX, 2, 8, false, false, true},
    {0x44, Mnemonic::NOP, "NOP", AddrMode::ZeroPage, 2, 3, false, false, true},
    {0x45, Mnemonic::EOR, "EOR", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0x46, Mnemonic::LSR, "LSR", AddrMode::ZeroPage, 2, 5, false, true, false},
    {0x47, Mnemonic::SRE, "SRE", AddrMode::ZeroPage, 2, 5, false, false, true},
    {0x48, Mnemonic::PHA, "PHA", AddrMode::Implied, 1, 3, false, true, false},
    {0x49, Mnemonic::EOR, "EOR", AddrMode::Immediate, 2, 2, false, true, false},
    {0x4A, Mnemonic::LSR, "LSR", AddrMode::Accumulator, 1, 2, false, true, false},
    {0x4B, Mnemonic::ALR, "ALR", AddrMode::Immediate, 2, 2, false, false, true},
    {0x4C, Mnemonic::JMP, "JMP", AddrMode::Absolute, 3, 3, false, true, false},
    {0x4D, Mnemonic::EOR, "EOR", AddrMode::Absolute, 3, 4, false, true, false},
    {0x4E, Mnemonic::LSR, "LSR", AddrMode::Absolute, 3, 6, false, true, false},
    {0x4F, Mnemonic::SRE, "SRE", AddrMode::Absolute, 3, 6, false, false, true},

    // 0x50 - 0x5F
    {0x50, Mnemonic::BVC, "BVC", AddrMode::Relative, 2, 2, false, true, false},
    {0x51, Mnemonic::EOR, "EOR", AddrMode::IndirectIndexedY, 2, 5, true, true, false},
    {0x52, Mnemonic::KIL, "KIL", AddrMode::Implied, 1, 2, false, false, false},
    {0x53, Mnemonic::SRE, "SRE", AddrMode::IndirectIndexedY, 2, 8, false, false, true},
    {0x54, Mnemonic::NOP, "NOP", AddrMode::ZeroPageX, 2, 4, false, false, true},
    {0x55, Mnemonic::EOR, "EOR", AddrMode::ZeroPageX, 2, 4, false, true, false},
    {0x56, Mnemonic::LSR, "LSR", AddrMode::ZeroPageX, 2, 6, false, true, false},
    {0x57, Mnemonic::SRE, "SRE", AddrMode::ZeroPageX, 2, 6, false, false, true},
    {0x58, Mnemonic::CLI, "CLI", AddrMode::Implied, 1, 2, false, true, false},
    {0x59, Mnemonic::EOR, "EOR", AddrMode::AbsoluteY, 3, 4, true, true, false},
    {0x5A, Mnemonic::NOP, "NOP", AddrMode::Implied, 1, 2, false, false, true},
    {0x5B, Mnemonic::SRE, "SRE", AddrMode::AbsoluteY, 3, 7, false, false, true},
    {0x5C, Mnemonic::NOP, "NOP", AddrMode::AbsoluteX, 3, 4, true, false, true},
    {0x5D, Mnemonic::EOR, "EOR", AddrMode::AbsoluteX, 3, 4, true, true, false},
    {0x5E, Mnemonic::LSR, "LSR", AddrMode::AbsoluteX, 3, 7, false, true, false},
    {0x5F, Mnemonic::SRE, "SRE", AddrMode::AbsoluteX, 3, 7, false, false, true},

    // 0x60 - 0x6F
    {0x60, Mnemonic::RTS, "RTS", AddrMode::Implied, 1, 6, false, true, false},
    {0x61, Mnemonic::ADC, "ADC", AddrMode::IndexedIndirectX, 2, 6, false, true, false},
    {0x62, Mnemonic::KIL, "KIL", AddrMode::Implied, 1, 2, false, false, false},
    {0x63, Mnemonic::RRA, "RRA", AddrMode::IndexedIndirectX, 2, 8, false, false, true},
    {0x64, Mnemonic::NOP, "NOP", AddrMode::ZeroPage, 2, 3, false, false, true},
    {0x65, Mnemonic::ADC, "ADC", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0x66, Mnemonic::ROR, "ROR", AddrMode::ZeroPage, 2, 5, false, true, false},
    {0x67, Mnemonic::RRA, "RRA", AddrMode::ZeroPage, 2, 5, false, false, true},
    {0x68, Mnemonic::PLA, "PLA", AddrMode::Implied, 1, 4, false, true, false},
    {0x69, Mnemonic::ADC, "ADC", AddrMode::Immediate, 2, 2, false, true, false},
    {0x6A, Mnemonic::ROR, "ROR", AddrMode::Accumulator, 1, 2, false, true, false},
    {0x6B, Mnemonic::ARR, "ARR", AddrMode::Immediate, 2, 2, false, false, true},
    {0x6C, Mnemonic::JMP, "JMP", AddrMode::Indirect, 3, 5, false, true, false},
    {0x6D, Mnemonic::ADC, "ADC", AddrMode::Absolute, 3, 4, false, true, false},
    {0x6E, Mnemonic::ROR, "ROR", AddrMode::Absolute, 3, 6, false, true, false},
    {0x6F, Mnemonic::RRA, "RRA", AddrMode::Absolute, 3, 6, false, false, true},

    // 0x70 - 0x7F
    {0x70, Mnemonic::BVS, "BVS", AddrMode::Relative, 2, 2, false, true, false},
    {0x71, Mnemonic::ADC, "ADC", AddrMode::IndirectIndexedY, 2, 5, true, true, false},
    {0x72, Mnemonic::KIL, "KIL", AddrMode::Implied, 1, 2, false, false, false},
    {0x73, Mnemonic::RRA, "RRA", AddrMode::IndirectIndexedY, 2, 8, false, false, true},
    {0x74, Mnemonic::NOP, "NOP", AddrMode::ZeroPageX, 2, 4, false, false, true},
    {0x75, Mnemonic::ADC, "ADC", AddrMode::ZeroPageX, 2, 4, false, true, false},
    {0x76, Mnemonic::ROR, "ROR", AddrMode::ZeroPageX, 2, 6, false, true, false},
    {0x77, Mnemonic::RRA, "RRA", AddrMode::ZeroPageX, 2, 6, false, false, true},
    {0x78, Mnemonic::SEI, "SEI", AddrMode::Implied, 1, 2, false, true, false},
    {0x79, Mnemonic::ADC, "ADC", AddrMode::AbsoluteY, 3, 4, true, true, false},
    {0x7A, Mnemonic::NOP, "NOP", AddrMode::Implied, 1, 2, false, false, true},
    {0x7B, Mnemonic::RRA, "RRA", AddrMode::AbsoluteY, 3, 7, false, false, true},
    {0x7C, Mnemonic::NOP, "NOP", AddrMode::AbsoluteX, 3, 4, true, false, true},
    {0x7D, Mnemonic::ADC, "ADC", AddrMode::AbsoluteX, 3, 4, true, true, false},
    {0x7E, Mnemonic::ROR, "ROR", AddrMode::AbsoluteX, 3, 7, false, true, false},
    {0x7F, Mnemonic::RRA, "RRA", AddrMode::AbsoluteX, 3, 7, false, false, true},

    // 0x80 - 0x8F
    {0x80, Mnemonic::NOP, "NOP", AddrMode::Immediate, 2, 2, false, false, true},
    {0x81, Mnemonic::STA, "STA", AddrMode::IndexedIndirectX, 2, 6, false, true, false},
    {0x82, Mnemonic::NOP, "NOP", AddrMode::Immediate, 2, 2, false, false, true},
    {0x83, Mnemonic::SAX, "SAX", AddrMode::IndexedIndirectX, 2, 6, false, false, true},
    {0x84, Mnemonic::STY, "STY", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0x85, Mnemonic::STA, "STA", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0x86, Mnemonic::STX, "STX", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0x87, Mnemonic::SAX, "SAX", AddrMode::ZeroPage, 2, 3, false, false, true},
    {0x88, Mnemonic::DEY, "DEY", AddrMode::Implied, 1, 2, false, true, false},
    {0x89, Mnemonic::NOP, "NOP", AddrMode::Immediate, 2, 2, false, false, true},
    {0x8A, Mnemonic::TXA, "TXA", AddrMode::Implied, 1, 2, false, true, false},
    {0x8B, Mnemonic::XAA, "XAA", AddrMode::Immediate, 2, 2, false, false, false},
    {0x8C, Mnemonic::STY, "STY", AddrMode::Absolute, 3, 4, false, true, false},
    {0x8D, Mnemonic::STA, "STA", AddrMode::Absolute, 3, 4, false, true, false},
    {0x8E, Mnemonic::STX, "STX", AddrMode::Absolute, 3, 4, false, true, false},
    {0x8F, Mnemonic::SAX, "SAX", AddrMode::Absolute, 3, 4, false, false, true},

    // 0x90 - 0x9F
    {0x90, Mnemonic::BCC, "BCC", AddrMode::Relative, 2, 2, false, true, false},
    {0x91, Mnemonic::STA, "STA", AddrMode::IndirectIndexedY, 2, 6, false, true, false},
    {0x92, Mnemonic::KIL, "KIL", AddrMode::Implied, 1, 2, false, false, false},
    {0x93, Mnemonic::AHX, "AHX", AddrMode::IndirectIndexedY, 2, 6, false, false, false},
    {0x94, Mnemonic::STY, "STY", AddrMode::ZeroPageX, 2, 4, false, true, false},
    {0x95, Mnemonic::STA, "STA", AddrMode::ZeroPageX, 2, 4, false, true, false},
    {0x96, Mnemonic::STX, "STX", AddrMode::ZeroPageY, 2, 4, false, true, false},
    {0x97, Mnemonic::SAX, "SAX", AddrMode::ZeroPageY, 2, 4, false, false, true},
    {0x98, Mnemonic::TYA, "TYA", AddrMode::Implied, 1, 2, false, true, false},
    {0x99, Mnemonic::STA, "STA", AddrMode::AbsoluteY, 3, 5, false, true, false},
    {0x9A, Mnemonic::TXS, "TXS", AddrMode::Implied, 1, 2, false, true, false},
    {0x9B, Mnemonic::TAS, "TAS", AddrMode::AbsoluteY, 3, 5, false, false, false},
    {0x9C, Mnemonic::SHY, "SHY", AddrMode::AbsoluteX, 3, 5, false, false, false},
    {0x9D, Mnemonic::STA, "STA", AddrMode::AbsoluteX, 3, 5, false, true, false},
    {0x9E, Mnemonic::SHX, "SHX", AddrMode::AbsoluteY, 3, 5, false, false, false},
    {0x9F, Mnemonic::AHX, "AHX", AddrMode::AbsoluteY, 3, 5, false, false, false},

    // 0xA0 - 0xAF
    {0xA0, Mnemonic::LDY, "LDY", AddrMode::Immediate, 2, 2, false, true, false},
    {0xA1, Mnemonic::LDA, "LDA", AddrMode::IndexedIndirectX, 2, 6, false, true, false},
    {0xA2, Mnemonic::LDX, "LDX", AddrMode::Immediate, 2, 2, false, true, false},
    {0xA3, Mnemonic::LAX, "LAX", AddrMode::IndexedIndirectX, 2, 6, false, false, true},
    {0xA4, Mnemonic::LDY, "LDY", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0xA5, Mnemonic::LDA, "LDA", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0xA6, Mnemonic::LDX, "LDX", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0xA7, Mnemonic::LAX, "LAX", AddrMode::ZeroPage, 2, 3, false, false, true},
    {0xA8, Mnemonic::TAY, "TAY", AddrMode::Implied, 1, 2, false, true, false},
    {0xA9, Mnemonic::LDA, "LDA", AddrMode::Immediate, 2, 2, false, true, false},
    {0xAA, Mnemonic::TAX, "TAX", AddrMode::Implied, 1, 2, false, true, false},
    {0xAB, Mnemonic::LAX, "LAX", AddrMode::Immediate, 2, 2, false, false, false},
    {0xAC, Mnemonic::LDY, "LDY", AddrMode::Absolute, 3, 4, false, true, false},
    {0xAD, Mnemonic::LDA, "LDA", AddrMode::Absolute, 3, 4, false, true, false},
    {0xAE, Mnemonic::LDX, "LDX", AddrMode::Absolute, 3, 4, false, true, false},
    {0xAF, Mnemonic::LAX, "LAX", AddrMode::Absolute, 3, 4, false, false, true},

    // 0xB0 - 0xBF
    {0xB0, Mnemonic::BCS, "BCS", AddrMode::Relative, 2, 2, false, true, false},
    {0xB1, Mnemonic::LDA, "LDA", AddrMode::IndirectIndexedY, 2, 5, true, true, false},
    {0xB2, Mnemonic::KIL, "KIL", AddrMode::Implied, 1, 2, false, false, false},
    {0xB3, Mnemonic::LAX, "LAX", AddrMode::IndirectIndexedY, 2, 5, true, false, true},
    {0xB4, Mnemonic::LDY, "LDY", AddrMode::ZeroPageX, 2, 4, false, true, false},
    {0xB5, Mnemonic::LDA, "LDA", AddrMode::ZeroPageX, 2, 4, false, true, false},
    {0xB6, Mnemonic::LDX, "LDX", AddrMode::ZeroPageY, 2, 4, false, true, false},
    {0xB7, Mnemonic::LAX, "LAX", AddrMode::ZeroPageY, 2, 4, false, false, true},
    {0xB8, Mnemonic::CLV, "CLV", AddrMode::Implied, 1, 2, false, true, false},
    {0xB9, Mnemonic::LDA, "LDA", AddrMode::AbsoluteY, 3, 4, true, true, false},
    {0xBA, Mnemonic::TSX, "TSX", AddrMode::Implied, 1, 2, false, true, false},
    {0xBB, Mnemonic::LAS, "LAS", AddrMode::AbsoluteY, 3, 4, true, false, false},
    {0xBC, Mnemonic::LDY, "LDY", AddrMode::AbsoluteX, 3, 4, true, true, false},
    {0xBD, Mnemonic::LDA, "LDA", AddrMode::AbsoluteX, 3, 4, true, true, false},
    {0xBE, Mnemonic::LDX, "LDX", AddrMode::AbsoluteY, 3, 4, true, true, false},
    {0xBF, Mnemonic::LAX, "LAX", AddrMode::AbsoluteY, 3, 4, true, false, true},

    // 0xC0 - 0xCF
    {0xC0, Mnemonic::CPY, "CPY", AddrMode::Immediate, 2, 2, false, true, false},
    {0xC1, Mnemonic::CMP, "CMP", AddrMode::IndexedIndirectX, 2, 6, false, true, false},
    {0xC2, Mnemonic::NOP, "NOP", AddrMode::Immediate, 2, 2, false, false, true},
    {0xC3, Mnemonic::DCP, "DCP", AddrMode::IndexedIndirectX, 2, 8, false, false, true},
    {0xC4, Mnemonic::CPY, "CPY", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0xC5, Mnemonic::CMP, "CMP", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0xC6, Mnemonic::DEC, "DEC", AddrMode::ZeroPage, 2, 5, false, true, false},
    {0xC7, Mnemonic::DCP, "DCP", AddrMode::ZeroPage, 2, 5, false, false, true},
    {0xC8, Mnemonic::INY, "INY", AddrMode::Implied, 1, 2, false, true, false},
    {0xC9, Mnemonic::CMP, "CMP", AddrMode::Immediate, 2, 2, false, true, false},
    {0xCA, Mnemonic::DEX, "DEX", AddrMode::Implied, 1, 2, false, true, false},
    {0xCB, Mnemonic::AXS, "AXS", AddrMode::Immediate, 2, 2, false, false, true},
    {0xCC, Mnemonic::CPY, "CPY", AddrMode::Absolute, 3, 4, false, true, false},
    {0xCD, Mnemonic::CMP, "CMP", AddrMode::Absolute, 3, 4, false, true, false},
    {0xCE, Mnemonic::DEC, "DEC", AddrMode::Absolute, 3, 6, false, true, false},
    {0xCF, Mnemonic::DCP, "DCP", AddrMode::Absolute, 3, 6, false, false, true},

    // 0xD0 - 0xDF
    {0xD0, Mnemonic::BNE, "BNE", AddrMode::Relative, 2, 2, false, true, false},
    {0xD1, Mnemonic::CMP, "CMP", AddrMode::IndirectIndexedY, 2, 5, true, true, false},
    {0xD2, Mnemonic::KIL, "KIL", AddrMode::Implied, 1, 2, false, false, false},
    {0xD3, Mnemonic::DCP, "DCP", AddrMode::IndirectIndexedY, 2, 8, false, false, true},
    {0xD4, Mnemonic::NOP, "NOP", AddrMode::ZeroPageX, 2, 4, false, false, true},
    {0xD5, Mnemonic::CMP, "CMP", AddrMode::ZeroPageX, 2, 4, false, true, false},
    {0xD6, Mnemonic::DEC, "DEC", AddrMode::ZeroPageX, 2, 6, false, true, false},
    {0xD7, Mnemonic::DCP, "DCP", AddrMode::ZeroPageX, 2, 6, false, false, true},
    {0xD8, Mnemonic::CLD, "CLD", AddrMode::Implied, 1, 2, false, true, false},
    {0xD9, Mnemonic::CMP, "CMP", AddrMode::AbsoluteY, 3, 4, true, true, false},
    {0xDA, Mnemonic::NOP, "NOP", AddrMode::Implied, 1, 2, false, false, true},
    {0xDB, Mnemonic::DCP, "DCP", AddrMode::AbsoluteY, 3, 7, false, false, true},
    {0xDC, Mnemonic::NOP, "NOP", AddrMode::AbsoluteX, 3, 4, true, false, true},
    {0xDD, Mnemonic::CMP, "CMP", AddrMode::AbsoluteX, 3, 4, true, true, false},
    {0xDE, Mnemonic::DEC, "DEC", AddrMode::AbsoluteX, 3, 7, false, true, false},
    {0xDF, Mnemonic::DCP, "DCP", AddrMode::AbsoluteX, 3, 7, false, false, true},

    // 0xE0 - 0xEF
    {0xE0, Mnemonic::CPX, "CPX", AddrMode::Immediate, 2, 2, false, true, false},
    {0xE1, Mnemonic::SBC, "SBC", AddrMode::IndexedIndirectX, 2, 6, false, true, false},
    {0xE2, Mnemonic::NOP, "NOP", AddrMode::Immediate, 2, 2, false, false, true},
    {0xE3, Mnemonic::ISC, "ISC", AddrMode::IndexedIndirectX, 2, 8, false, false, true},
    {0xE4, Mnemonic::CPX, "CPX", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0xE5, Mnemonic::SBC, "SBC", AddrMode::ZeroPage, 2, 3, false, true, false},
    {0xE6, Mnemonic::INC, "INC", AddrMode::ZeroPage, 2, 5, false, true, false},
    {0xE7, Mnemonic::ISC, "ISC", AddrMode::ZeroPage, 2, 5, false, false, true},
    {0xE8, Mnemonic::INX, "INX", AddrMode::Implied, 1, 2, false, true, false},
    {0xE9, Mnemonic::SBC, "SBC", AddrMode::Immediate, 2, 2, false, true, false},
    {0xEA, Mnemonic::NOP, "NOP", AddrMode::Implied, 1, 2, false, true, false},
    {0xEB, Mnemonic::SBC, "SBC", AddrMode::Immediate, 2, 2, false, false, true},
    {0xEC, Mnemonic::CPX, "CPX", AddrMode::Absolute, 3, 4, false, true, false},
    {0xED, Mnemonic::SBC, "SBC", AddrMode::Absolute, 3, 4, false, true, false},
    {0xEE, Mnemonic::INC, "INC", AddrMode::Absolute, 3, 6, false, true, false},
    {0xEF, Mnemonic::ISC, "ISC", AddrMode::Absolute, 3, 6, false, false, true},

    // 0xF0 - 0xFF
    {0xF0, Mnemonic::BEQ, "BEQ", AddrMode::Relative, 2, 2, false, true, false},
    {0xF1, Mnemonic::SBC, "SBC", AddrMode::IndirectIndexedY, 2, 5, true, true, false},
    {0xF2, Mnemonic::KIL, "KIL", AddrMode::Implied, 1, 2, false, false, false},
    {0xF3, Mnemonic::ISC, "ISC", AddrMode::IndirectIndexedY, 2, 8, false, false, true},
    {0xF4, Mnemonic::NOP, "NOP", AddrMode::ZeroPageX, 2, 4, false, false, true},
    {0xF5, Mnemonic::SBC, "SBC", AddrMode::ZeroPageX, 2, 4, false, true, false},
    {0xF6, Mnemonic::INC, "INC", AddrMode::ZeroPageX, 2, 6, false, true, false},
    {0xF7, Mnemonic::ISC, "ISC", AddrMode::ZeroPageX, 2, 6, false, false, true},
    {0xF8, Mnemonic::SED, "SED", AddrMode::Implied, 1, 2, false, true, false},
    {0xF9, Mnemonic::SBC, "SBC", AddrMode::AbsoluteY, 3, 4, true, true, false},
    {0xFA, Mnemonic::NOP, "NOP", AddrMode::Implied, 1, 2, false, false, true},
    {0xFB, Mnemonic::ISC, "ISC", AddrMode::AbsoluteY, 3, 7, false, false, true},
    {0xFC, Mnemonic::NOP, "NOP", AddrMode::AbsoluteX, 3, 4, true, false, true},
    {0xFD, Mnemonic::SBC, "SBC", AddrMode::AbsoluteX, 3, 4, true, true, false},
    {0xFE, Mnemonic::INC, "INC", AddrMode::AbsoluteX, 3, 7, false, true, false},
    {0xFF, Mnemonic::ISC, "ISC", AddrMode::AbsoluteX, 3, 7, false, false, true},
}};

constexpr const OpcodeInfo& get_opcode_info(uint8_t op) {
    return kOpcodeTable[op];
}

constexpr bool is_branch_mnemonic(Mnemonic m) {
    return m == Mnemonic::BCC || m == Mnemonic::BCS || m == Mnemonic::BEQ || m == Mnemonic::BMI ||
           m == Mnemonic::BNE || m == Mnemonic::BPL || m == Mnemonic::BVC || m == Mnemonic::BVS;
}

constexpr bool is_rmw_mnemonic(Mnemonic m) {
    return m == Mnemonic::ASL || m == Mnemonic::LSR || m == Mnemonic::ROL || m == Mnemonic::ROR ||
           m == Mnemonic::INC || m == Mnemonic::DEC || m == Mnemonic::SLO || m == Mnemonic::RLA ||
           m == Mnemonic::SRE || m == Mnemonic::RRA || m == Mnemonic::DCP || m == Mnemonic::ISC;
}

}  // namespace dragon::nes
