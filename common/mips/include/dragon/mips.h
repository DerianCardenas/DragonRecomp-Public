#pragma once

#include <cstdint>
#include <string>

namespace dragon::mips {

enum class MipsProfile : uint8_t {
    R3000A = 0,    // PlayStation 1 (MIPS I + GTE)
    Allegrex = 1,  // PlayStation Portable (MIPS32 + Allegrex + FPU + VFPU)
    R4300i = 2,    // Nintendo 64 (MIPS III 64-bit)
};

enum class MipsOp : uint16_t {
    Unknown = 0,
    Nop,

    // Shifts
    Sll,
    Srl,
    Sra,
    Sllv,
    Srlv,
    Srav,
    Rotr,   // Allegrex
    Rotrv,  // Allegrex

    // Jumps & branches
    J,
    Jal,
    Jr,
    Jalr,
    Beq,
    Bne,
    Blez,
    Bgtz,
    Bltz,
    Bgez,
    Bltzal,
    Bgezal,
    // Branch-likely (MIPS II / Allegrex / R4300i)
    Beql,
    Bnel,
    Blezl,
    Bgtzl,
    Bltzl,
    Bgezl,
    Bltzall,
    Bgezall,

    // ALU register-register & immediate
    Add,
    Addu,
    Sub,
    Subu,
    Addi,
    Addiu,
    And,
    Or,
    Xor,
    Nor,
    Andi,
    Ori,
    Xori,
    Lui,
    Slt,
    Sltu,
    Slti,
    Sltiu,

    // Conditional moves & Allegrex integer extensions
    Movz,
    Movn,
    Clz,
    Clo,
    Max,
    Min,
    Ext,
    Ins,
    Wsbh,
    Wsbw,
    Seb,
    Seh,
    Bitrev,

    // Multiply / Divide / Accumulate
    Mult,
    Multu,
    Div,
    Divu,
    Madd,
    Maddu,
    Msub,
    Msubu,
    Mfhi,
    Mthi,
    Mflo,
    Mtlo,

    // Loads & Stores (32-bit)
    Lb,
    Lbu,
    Lh,
    Lhu,
    Lw,
    Lwl,
    Lwr,
    Ll,
    Sb,
    Sh,
    Sw,
    Swl,
    Swr,
    Sc,
    Cache,
    Sync,

    // System & Traps
    Syscall,
    Break,
    Halt,
    Mfic,
    Mtic,

    // COP0
    Mfc0,
    Mtc0,
    Eret,
    Rfe,

    // COP1 (Single-precision FPU)
    Mfc1,
    Cfc1,
    Mtc1,
    Ctc1,
    Lwc1,
    Swc1,
    Bc1f,
    Bc1t,
    Bc1fl,
    Bc1tl,
    AddS,
    SubS,
    MulS,
    DivS,
    SqrtS,
    AbsS,
    MovS,
    NegS,
    RoundWS,
    TruncWS,
    CeilWS,
    FloorWS,
    CvtWS,
    CvtSW,
    CCondS,

    // Allegrex VFPU
    Lvs,
    Lvlr,
    Lvq,
    Svs,
    Svlr,
    Svq,
    Vflush,
    Vfpu0,
    Vfpu1,
    Vfpu3,
    Vfpu4,
    Vfpu5,
    Vfpu6,

    // COP2 (PS1 GTE / generic)
    Lwc2,
    Swc2,
    Cop2,

    // R4300i 64-bit extensions (Phase 3.2 N64)
    Dadd,
    Daddu,
    Dsub,
    Dsubu,
    Daddi,
    Daddiu,
    Dmult,
    Dmultu,
    Ddiv,
    Ddivu,
    Dsll,
    Dsrl,
    Dsra,
    Dsll32,
    Dsrl32,
    Dsra32,
    Dsllv,
    Dsrlv,
    Dsrav,
    Ld,
    Sd,
    Lwu,
    Ldl,
    Ldr,
    Sdl,
    Sdr,
    Ldc1,
    Sdc1,
    Dmfc1,
    Dmtc1,
};

struct DecodedMips {
    uint32_t raw = 0;
    uint32_t pc = 0;
    MipsOp op = MipsOp::Unknown;

    uint8_t opcode = 0;   // bits 31..26
    uint8_t rs = 0;       // bits 25..21
    uint8_t rt = 0;       // bits 20..16
    uint8_t rd = 0;       // bits 15..11
    uint8_t sa = 0;       // bits 10..6
    uint8_t funct = 0;    // bits 5..0
    uint16_t imm = 0;     // bits 15..0
    int16_t simm = 0;     // sign-extended bits 15..0
    uint32_t target = 0;  // resolved branch or jump target address (when applicable)
    uint32_t syscall_code = 0;

    bool has_delay_slot = false;
    bool is_branch = false;
    bool is_branch_likely = false;
    bool is_call = false;
    bool is_jump = false;
    bool is_indirect_jump = false;
    bool is_return = false;  // jr $ra
};

// Decode a single 32-bit little-endian MIPS instruction. Hot-path safe (no heap allocation).
DecodedMips decode_mips(uint32_t raw, uint32_t pc,
                        MipsProfile profile = MipsProfile::Allegrex) noexcept;

// Human-readable mnemonic name (useful for disassembly, diagnostics, and tests).
const char* mips_op_name(MipsOp op) noexcept;

// Standard MIPS ABI register name ($zero, $at, $v0, ..., $ra).
const char* mips_reg_name(uint8_t reg) noexcept;

// Format a decoded instruction into a readable disassembly string.
std::string format_mips(const DecodedMips& insn);

}  // namespace dragon::mips
