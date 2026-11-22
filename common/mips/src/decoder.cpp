#include "dragon/mips.h"

#include <cstdio>

namespace dragon::mips {

namespace {

inline uint32_t branch_target(uint32_t pc, int16_t simm) noexcept {
    return pc + 4U + (static_cast<uint32_t>(static_cast<int32_t>(simm)) << 2U);
}

inline uint32_t jump_target(uint32_t pc, uint32_t raw) noexcept {
    return ((pc + 4U) & 0xF0000000U) | ((raw & 0x03FFFFFFU) << 2U);
}

}  // namespace

DecodedMips decode_mips(uint32_t raw, uint32_t pc, MipsProfile profile) noexcept {
    DecodedMips d{};
    d.raw = raw;
    d.pc = pc;
    d.opcode = static_cast<uint8_t>((raw >> 26U) & 0x3FU);
    d.rs = static_cast<uint8_t>((raw >> 21U) & 0x1FU);
    d.rt = static_cast<uint8_t>((raw >> 16U) & 0x1FU);
    d.rd = static_cast<uint8_t>((raw >> 11U) & 0x1FU);
    d.sa = static_cast<uint8_t>((raw >> 6U) & 0x1FU);
    d.funct = static_cast<uint8_t>(raw & 0x3FU);
    d.imm = static_cast<uint16_t>(raw & 0xFFFFU);
    d.simm = static_cast<int16_t>(d.imm);

    if (raw == 0U) {
        d.op = MipsOp::Nop;
        return d;
    }

    switch (d.opcode) {
        case 0x00:  // SPECIAL
            switch (d.funct) {
                case 0x00:
                    d.op = (d.rd == 0 && d.rt == 0 && d.sa == 0) ? MipsOp::Nop : MipsOp::Sll;
                    break;
                case 0x02:
                    d.op = (profile == MipsProfile::Allegrex && d.rs == 1) ? MipsOp::Rotr
                                                                           : MipsOp::Srl;
                    break;
                case 0x03:
                    d.op = MipsOp::Sra;
                    break;
                case 0x04:
                    d.op = MipsOp::Sllv;
                    break;
                case 0x06:
                    d.op = (profile == MipsProfile::Allegrex && d.sa == 1) ? MipsOp::Rotrv
                                                                           : MipsOp::Srlv;
                    break;
                case 0x07:
                    d.op = MipsOp::Srav;
                    break;
                case 0x08:
                    d.op = MipsOp::Jr;
                    d.has_delay_slot = true;
                    d.is_jump = true;
                    d.is_indirect_jump = true;
                    d.is_return = (d.rs == 31);
                    break;
                case 0x09:
                    d.op = MipsOp::Jalr;
                    d.has_delay_slot = true;
                    d.is_jump = true;
                    d.is_call = true;
                    d.is_indirect_jump = true;
                    break;
                case 0x0A:
                    d.op = MipsOp::Movz;
                    break;
                case 0x0B:
                    d.op = MipsOp::Movn;
                    break;
                case 0x0C:
                    d.op = MipsOp::Syscall;
                    d.syscall_code = (raw >> 6U) & 0xFFFFFU;
                    break;
                case 0x0D:
                    d.op = MipsOp::Break;
                    d.syscall_code = (raw >> 6U) & 0xFFFFFU;
                    break;
                case 0x0F:
                    d.op = MipsOp::Sync;
                    break;
                case 0x10:
                    d.op = MipsOp::Mfhi;
                    break;
                case 0x11:
                    d.op = MipsOp::Mthi;
                    break;
                case 0x12:
                    d.op = MipsOp::Mflo;
                    break;
                case 0x13:
                    d.op = MipsOp::Mtlo;
                    break;
                case 0x14:
                    if (profile == MipsProfile::R4300i)
                        d.op = MipsOp::Dsllv;
                    break;
                case 0x16:
                    d.op = (profile == MipsProfile::R4300i) ? MipsOp::Dsrlv : MipsOp::Clz;
                    break;
                case 0x17:
                    d.op = (profile == MipsProfile::R4300i) ? MipsOp::Dsrav : MipsOp::Clo;
                    break;
                case 0x18:
                    d.op = MipsOp::Mult;
                    break;
                case 0x19:
                    d.op = MipsOp::Multu;
                    break;
                case 0x1A:
                    d.op = MipsOp::Div;
                    break;
                case 0x1B:
                    d.op = MipsOp::Divu;
                    break;
                case 0x1C:
                    d.op = (profile == MipsProfile::R4300i) ? MipsOp::Dmult : MipsOp::Madd;
                    break;
                case 0x1D:
                    d.op = (profile == MipsProfile::R4300i) ? MipsOp::Dmultu : MipsOp::Maddu;
                    break;
                case 0x1E:
                    if (profile == MipsProfile::R4300i)
                        d.op = MipsOp::Ddiv;
                    break;
                case 0x1F:
                    if (profile == MipsProfile::R4300i)
                        d.op = MipsOp::Ddivu;
                    break;
                case 0x20:
                    d.op = MipsOp::Add;
                    break;
                case 0x21:
                    d.op = MipsOp::Addu;
                    break;
                case 0x22:
                    d.op = MipsOp::Sub;
                    break;
                case 0x23:
                    d.op = MipsOp::Subu;
                    break;
                case 0x24:
                    d.op = MipsOp::And;
                    break;
                case 0x25:
                    d.op = MipsOp::Or;
                    break;
                case 0x26:
                    d.op = MipsOp::Xor;
                    break;
                case 0x27:
                    d.op = MipsOp::Nor;
                    break;
                case 0x2A:
                    d.op = MipsOp::Slt;
                    break;
                case 0x2B:
                    d.op = MipsOp::Sltu;
                    break;
                case 0x2C:
                    d.op = (profile == MipsProfile::R4300i) ? MipsOp::Dadd : MipsOp::Max;
                    break;
                case 0x2D:
                    d.op = (profile == MipsProfile::R4300i) ? MipsOp::Daddu : MipsOp::Min;
                    break;
                case 0x2E:
                    d.op = (profile == MipsProfile::R4300i) ? MipsOp::Dsub : MipsOp::Msub;
                    break;
                case 0x2F:
                    d.op = (profile == MipsProfile::R4300i) ? MipsOp::Dsubu : MipsOp::Msubu;
                    break;
                case 0x38:
                    if (profile == MipsProfile::R4300i)
                        d.op = MipsOp::Dsll;
                    break;
                case 0x3A:
                    if (profile == MipsProfile::R4300i)
                        d.op = MipsOp::Dsrl;
                    break;
                case 0x3B:
                    if (profile == MipsProfile::R4300i)
                        d.op = MipsOp::Dsra;
                    break;
                case 0x3C:
                    if (profile == MipsProfile::R4300i)
                        d.op = MipsOp::Dsll32;
                    break;
                case 0x3E:
                    if (profile == MipsProfile::R4300i)
                        d.op = MipsOp::Dsrl32;
                    break;
                case 0x3F:
                    if (profile == MipsProfile::R4300i)
                        d.op = MipsOp::Dsra32;
                    break;
                default:
                    break;
            }
            break;

        case 0x01:  // REGIMM
            d.target = branch_target(pc, d.simm);
            switch (d.rt) {
                case 0x00:
                    d.op = MipsOp::Bltz;
                    d.is_branch = true;
                    d.has_delay_slot = true;
                    break;
                case 0x01:
                    d.op = MipsOp::Bgez;
                    d.is_branch = true;
                    d.has_delay_slot = true;
                    break;
                case 0x02:
                    d.op = MipsOp::Bltzl;
                    d.is_branch = true;
                    d.is_branch_likely = true;
                    d.has_delay_slot = true;
                    break;
                case 0x03:
                    d.op = MipsOp::Bgezl;
                    d.is_branch = true;
                    d.is_branch_likely = true;
                    d.has_delay_slot = true;
                    break;
                case 0x10:
                    d.op = MipsOp::Bltzal;
                    d.is_branch = true;
                    d.is_call = true;
                    d.has_delay_slot = true;
                    break;
                case 0x11:
                    d.op = MipsOp::Bgezal;
                    d.is_branch = true;
                    d.is_call = true;
                    d.has_delay_slot = true;
                    break;
                case 0x12:
                    d.op = MipsOp::Bltzall;
                    d.is_branch = true;
                    d.is_branch_likely = true;
                    d.is_call = true;
                    d.has_delay_slot = true;
                    break;
                case 0x13:
                    d.op = MipsOp::Bgezall;
                    d.is_branch = true;
                    d.is_branch_likely = true;
                    d.is_call = true;
                    d.has_delay_slot = true;
                    break;
                default:
                    break;
            }
            break;

        case 0x02:
            d.op = MipsOp::J;
            d.target = jump_target(pc, raw);
            d.is_jump = true;
            d.has_delay_slot = true;
            break;
        case 0x03:
            d.op = MipsOp::Jal;
            d.target = jump_target(pc, raw);
            d.is_jump = true;
            d.is_call = true;
            d.has_delay_slot = true;
            break;
        case 0x04:
            d.op = MipsOp::Beq;
            d.target = branch_target(pc, d.simm);
            d.is_branch = true;
            d.has_delay_slot = true;
            break;
        case 0x05:
            d.op = MipsOp::Bne;
            d.target = branch_target(pc, d.simm);
            d.is_branch = true;
            d.has_delay_slot = true;
            break;
        case 0x06:
            d.op = MipsOp::Blez;
            d.target = branch_target(pc, d.simm);
            d.is_branch = true;
            d.has_delay_slot = true;
            break;
        case 0x07:
            d.op = MipsOp::Bgtz;
            d.target = branch_target(pc, d.simm);
            d.is_branch = true;
            d.has_delay_slot = true;
            break;

        case 0x08:
            d.op = MipsOp::Addi;
            break;
        case 0x09:
            d.op = MipsOp::Addiu;
            break;
        case 0x0A:
            d.op = MipsOp::Slti;
            break;
        case 0x0B:
            d.op = MipsOp::Sltiu;
            break;
        case 0x0C:
            d.op = MipsOp::Andi;
            break;
        case 0x0D:
            d.op = MipsOp::Ori;
            break;
        case 0x0E:
            d.op = MipsOp::Xori;
            break;
        case 0x0F:
            d.op = MipsOp::Lui;
            break;

        case 0x10:  // COP0
            if (d.rs == 0x00) {
                d.op = MipsOp::Mfc0;
            } else if (d.rs == 0x04) {
                d.op = MipsOp::Mtc0;
            } else if ((d.rs & 0x10U) != 0U) {
                if (d.funct == 0x18)
                    d.op = MipsOp::Eret;
                else if (d.funct == 0x10)
                    d.op = MipsOp::Rfe;
            }
            break;

        case 0x11:  // COP1 (FPU)
            switch (d.rs) {
                case 0x00:
                    d.op = MipsOp::Mfc1;
                    break;
                case 0x01:
                    if (profile == MipsProfile::R4300i)
                        d.op = MipsOp::Dmfc1;
                    break;
                case 0x02:
                    d.op = MipsOp::Cfc1;
                    break;
                case 0x04:
                    d.op = MipsOp::Mtc1;
                    break;
                case 0x05:
                    if (profile == MipsProfile::R4300i)
                        d.op = MipsOp::Dmtc1;
                    break;
                case 0x06:
                    d.op = MipsOp::Ctc1;
                    break;
                case 0x08:  // BC1
                    d.target = branch_target(pc, d.simm);
                    d.is_branch = true;
                    d.has_delay_slot = true;
                    switch (d.rt & 0x03U) {
                        case 0:
                            d.op = MipsOp::Bc1f;
                            break;
                        case 1:
                            d.op = MipsOp::Bc1t;
                            break;
                        case 2:
                            d.op = MipsOp::Bc1fl;
                            d.is_branch_likely = true;
                            break;
                        case 3:
                            d.op = MipsOp::Bc1tl;
                            d.is_branch_likely = true;
                            break;
                    }
                    break;
                case 0x10:  // Single-precision (.S)
                    switch (d.funct) {
                        case 0x00:
                            d.op = MipsOp::AddS;
                            break;
                        case 0x01:
                            d.op = MipsOp::SubS;
                            break;
                        case 0x02:
                            d.op = MipsOp::MulS;
                            break;
                        case 0x03:
                            d.op = MipsOp::DivS;
                            break;
                        case 0x04:
                            d.op = MipsOp::SqrtS;
                            break;
                        case 0x05:
                            d.op = MipsOp::AbsS;
                            break;
                        case 0x06:
                            d.op = MipsOp::MovS;
                            break;
                        case 0x07:
                            d.op = MipsOp::NegS;
                            break;
                        case 0x0C:
                            d.op = MipsOp::RoundWS;
                            break;
                        case 0x0D:
                            d.op = MipsOp::TruncWS;
                            break;
                        case 0x0E:
                            d.op = MipsOp::CeilWS;
                            break;
                        case 0x0F:
                            d.op = MipsOp::FloorWS;
                            break;
                        case 0x24:
                            d.op = MipsOp::CvtWS;
                            break;
                        default:
                            if ((d.funct & 0x30U) == 0x30U) {
                                d.op = MipsOp::CCondS;
                            }
                            break;
                    }
                    break;
                case 0x14:  // Word (.W)
                    if (d.funct == 0x20)
                        d.op = MipsOp::CvtSW;
                    break;
                default:
                    break;
            }
            break;

        case 0x12:  // COP2 (PS1 GTE / Allegrex VFPU branch/move)
            d.op = MipsOp::Cop2;
            break;

        case 0x14:
            d.op = MipsOp::Beql;
            d.target = branch_target(pc, d.simm);
            d.is_branch = true;
            d.is_branch_likely = true;
            d.has_delay_slot = true;
            break;
        case 0x15:
            d.op = MipsOp::Bnel;
            d.target = branch_target(pc, d.simm);
            d.is_branch = true;
            d.is_branch_likely = true;
            d.has_delay_slot = true;
            break;
        case 0x16:
            d.op = MipsOp::Blezl;
            d.target = branch_target(pc, d.simm);
            d.is_branch = true;
            d.is_branch_likely = true;
            d.has_delay_slot = true;
            break;
        case 0x17:
            d.op = MipsOp::Bgtzl;
            d.target = branch_target(pc, d.simm);
            d.is_branch = true;
            d.is_branch_likely = true;
            d.has_delay_slot = true;
            break;

        case 0x18:
            d.op = (profile == MipsProfile::R4300i) ? MipsOp::Daddi : MipsOp::Vfpu0;
            break;
        case 0x19:
            d.op = (profile == MipsProfile::R4300i) ? MipsOp::Daddiu : MipsOp::Vfpu1;
            break;
        case 0x1A:
            if (profile == MipsProfile::R4300i)
                d.op = MipsOp::Ldl;
            break;
        case 0x1B:
            d.op = (profile == MipsProfile::R4300i) ? MipsOp::Ldr : MipsOp::Vfpu3;
            break;

        case 0x1C:  // SPECIAL2 (Allegrex)
            if (profile == MipsProfile::Allegrex) {
                switch (d.funct) {
                    case 0x00:
                        d.op = MipsOp::Halt;
                        break;
                    case 0x24:
                        d.op = MipsOp::Mfic;
                        break;
                    case 0x26:
                        d.op = MipsOp::Mtic;
                        break;
                    default:
                        break;
                }
            }
            break;

        case 0x1F:  // SPECIAL3 (Allegrex bitfield / sign-ext / byte-swap)
            if (profile == MipsProfile::Allegrex) {
                switch (d.funct) {
                    case 0x00:
                        d.op = MipsOp::Ext;
                        break;
                    case 0x04:
                        d.op = MipsOp::Ins;
                        break;
                    case 0x20:  // ALLEGREX0 sub-decoded by sa (verified against PPSSPP
                                // MIPSTables.cpp)
                        if (d.sa < 16) {
                            d.op = ((d.sa & 1U) == 0U) ? MipsOp::Wsbh : MipsOp::Wsbw;
                        } else if (d.sa < 20) {
                            d.op = MipsOp::Seb;
                        } else if (d.sa < 24) {
                            d.op = MipsOp::Bitrev;
                        } else if (d.sa < 28) {
                            d.op = MipsOp::Seh;
                        } else {
                            d.op = MipsOp::Bitrev;
                        }
                        break;
                    default:
                        break;
                }
            }
            break;

        case 0x20:
            d.op = MipsOp::Lb;
            break;
        case 0x21:
            d.op = MipsOp::Lh;
            break;
        case 0x22:
            d.op = MipsOp::Lwl;
            break;
        case 0x23:
            d.op = MipsOp::Lw;
            break;
        case 0x24:
            d.op = MipsOp::Lbu;
            break;
        case 0x25:
            d.op = MipsOp::Lhu;
            break;
        case 0x26:
            d.op = MipsOp::Lwr;
            break;
        case 0x27:
            if (profile == MipsProfile::R4300i)
                d.op = MipsOp::Lwu;
            break;
        case 0x28:
            d.op = MipsOp::Sb;
            break;
        case 0x29:
            d.op = MipsOp::Sh;
            break;
        case 0x2A:
            d.op = MipsOp::Swl;
            break;
        case 0x2B:
            d.op = MipsOp::Sw;
            break;
        case 0x2C:
            if (profile == MipsProfile::R4300i)
                d.op = MipsOp::Sdl;
            break;
        case 0x2D:
            if (profile == MipsProfile::R4300i)
                d.op = MipsOp::Sdr;
            break;
        case 0x2E:
            d.op = MipsOp::Swr;
            break;
        case 0x2F:
            d.op = MipsOp::Cache;
            break;

        case 0x30:
            d.op = MipsOp::Ll;
            break;
        case 0x31:
            d.op = MipsOp::Lwc1;
            break;
        case 0x32:
            d.op = (profile == MipsProfile::Allegrex) ? MipsOp::Lvs : MipsOp::Lwc2;
            break;
        case 0x34:
            if (profile == MipsProfile::Allegrex)
                d.op = MipsOp::Vfpu4;
            break;
        case 0x35:
            d.op = (profile == MipsProfile::R4300i) ? MipsOp::Ldc1 : MipsOp::Lvlr;
            break;
        case 0x36:
            if (profile == MipsProfile::Allegrex)
                d.op = MipsOp::Lvq;
            break;
        case 0x37:
            d.op = (profile == MipsProfile::R4300i) ? MipsOp::Ld : MipsOp::Vfpu5;
            break;
        case 0x38:
            d.op = MipsOp::Sc;
            break;
        case 0x39:
            d.op = MipsOp::Swc1;
            break;
        case 0x3A:
            d.op = (profile == MipsProfile::Allegrex) ? MipsOp::Svs : MipsOp::Swc2;
            break;
        case 0x3C:
            if (profile == MipsProfile::Allegrex)
                d.op = MipsOp::Vfpu6;
            break;
        case 0x3D:
            d.op = (profile == MipsProfile::R4300i) ? MipsOp::Sdc1 : MipsOp::Svlr;
            break;
        case 0x3E:
            if (profile == MipsProfile::Allegrex)
                d.op = MipsOp::Svq;
            break;
        case 0x3F:
            d.op = (profile == MipsProfile::R4300i) ? MipsOp::Sd : MipsOp::Vflush;
            break;

        default:
            break;
    }

    return d;
}

const char* mips_op_name(MipsOp op) noexcept {
    switch (op) {
        case MipsOp::Unknown:
            return "unknown";
        case MipsOp::Nop:
            return "nop";
        case MipsOp::Sll:
            return "sll";
        case MipsOp::Srl:
            return "srl";
        case MipsOp::Sra:
            return "sra";
        case MipsOp::Sllv:
            return "sllv";
        case MipsOp::Srlv:
            return "srlv";
        case MipsOp::Srav:
            return "srav";
        case MipsOp::Rotr:
            return "rotr";
        case MipsOp::Rotrv:
            return "rotrv";
        case MipsOp::J:
            return "j";
        case MipsOp::Jal:
            return "jal";
        case MipsOp::Jr:
            return "jr";
        case MipsOp::Jalr:
            return "jalr";
        case MipsOp::Beq:
            return "beq";
        case MipsOp::Bne:
            return "bne";
        case MipsOp::Blez:
            return "blez";
        case MipsOp::Bgtz:
            return "bgtz";
        case MipsOp::Bltz:
            return "bltz";
        case MipsOp::Bgez:
            return "bgez";
        case MipsOp::Bltzal:
            return "bltzal";
        case MipsOp::Bgezal:
            return "bgezal";
        case MipsOp::Beql:
            return "beql";
        case MipsOp::Bnel:
            return "bnel";
        case MipsOp::Blezl:
            return "blezl";
        case MipsOp::Bgtzl:
            return "bgtzl";
        case MipsOp::Bltzl:
            return "bltzl";
        case MipsOp::Bgezl:
            return "bgezl";
        case MipsOp::Bltzall:
            return "bltzall";
        case MipsOp::Bgezall:
            return "bgezall";
        case MipsOp::Add:
            return "add";
        case MipsOp::Addu:
            return "addu";
        case MipsOp::Sub:
            return "sub";
        case MipsOp::Subu:
            return "subu";
        case MipsOp::Addi:
            return "addi";
        case MipsOp::Addiu:
            return "addiu";
        case MipsOp::And:
            return "and";
        case MipsOp::Or:
            return "or";
        case MipsOp::Xor:
            return "xor";
        case MipsOp::Nor:
            return "nor";
        case MipsOp::Andi:
            return "andi";
        case MipsOp::Ori:
            return "ori";
        case MipsOp::Xori:
            return "xori";
        case MipsOp::Lui:
            return "lui";
        case MipsOp::Slt:
            return "slt";
        case MipsOp::Sltu:
            return "sltu";
        case MipsOp::Slti:
            return "slti";
        case MipsOp::Sltiu:
            return "sltiu";
        case MipsOp::Movz:
            return "movz";
        case MipsOp::Movn:
            return "movn";
        case MipsOp::Clz:
            return "clz";
        case MipsOp::Clo:
            return "clo";
        case MipsOp::Max:
            return "max";
        case MipsOp::Min:
            return "min";
        case MipsOp::Ext:
            return "ext";
        case MipsOp::Ins:
            return "ins";
        case MipsOp::Wsbh:
            return "wsbh";
        case MipsOp::Wsbw:
            return "wsbw";
        case MipsOp::Seb:
            return "seb";
        case MipsOp::Seh:
            return "seh";
        case MipsOp::Bitrev:
            return "bitrev";
        case MipsOp::Mult:
            return "mult";
        case MipsOp::Multu:
            return "multu";
        case MipsOp::Div:
            return "div";
        case MipsOp::Divu:
            return "divu";
        case MipsOp::Madd:
            return "madd";
        case MipsOp::Maddu:
            return "maddu";
        case MipsOp::Msub:
            return "msub";
        case MipsOp::Msubu:
            return "msubu";
        case MipsOp::Mfhi:
            return "mfhi";
        case MipsOp::Mthi:
            return "mthi";
        case MipsOp::Mflo:
            return "mflo";
        case MipsOp::Mtlo:
            return "mtlo";
        case MipsOp::Lb:
            return "lb";
        case MipsOp::Lbu:
            return "lbu";
        case MipsOp::Lh:
            return "lh";
        case MipsOp::Lhu:
            return "lhu";
        case MipsOp::Lw:
            return "lw";
        case MipsOp::Lwl:
            return "lwl";
        case MipsOp::Lwr:
            return "lwr";
        case MipsOp::Ll:
            return "ll";
        case MipsOp::Sb:
            return "sb";
        case MipsOp::Sh:
            return "sh";
        case MipsOp::Sw:
            return "sw";
        case MipsOp::Swl:
            return "swl";
        case MipsOp::Swr:
            return "swr";
        case MipsOp::Sc:
            return "sc";
        case MipsOp::Cache:
            return "cache";
        case MipsOp::Sync:
            return "sync";
        case MipsOp::Syscall:
            return "syscall";
        case MipsOp::Break:
            return "break";
        case MipsOp::Halt:
            return "halt";
        case MipsOp::Mfic:
            return "mfic";
        case MipsOp::Mtic:
            return "mtic";
        case MipsOp::Mfc0:
            return "mfc0";
        case MipsOp::Mtc0:
            return "mtc0";
        case MipsOp::Eret:
            return "eret";
        case MipsOp::Rfe:
            return "rfe";
        case MipsOp::Mfc1:
            return "mfc1";
        case MipsOp::Cfc1:
            return "cfc1";
        case MipsOp::Mtc1:
            return "mtc1";
        case MipsOp::Ctc1:
            return "ctc1";
        case MipsOp::Lwc1:
            return "lwc1";
        case MipsOp::Swc1:
            return "swc1";
        case MipsOp::Bc1f:
            return "bc1f";
        case MipsOp::Bc1t:
            return "bc1t";
        case MipsOp::Bc1fl:
            return "bc1fl";
        case MipsOp::Bc1tl:
            return "bc1tl";
        case MipsOp::AddS:
            return "add.s";
        case MipsOp::SubS:
            return "sub.s";
        case MipsOp::MulS:
            return "mul.s";
        case MipsOp::DivS:
            return "div.s";
        case MipsOp::SqrtS:
            return "sqrt.s";
        case MipsOp::AbsS:
            return "abs.s";
        case MipsOp::MovS:
            return "mov.s";
        case MipsOp::NegS:
            return "neg.s";
        case MipsOp::RoundWS:
            return "round.w.s";
        case MipsOp::TruncWS:
            return "trunc.w.s";
        case MipsOp::CeilWS:
            return "ceil.w.s";
        case MipsOp::FloorWS:
            return "floor.w.s";
        case MipsOp::CvtWS:
            return "cvt.w.s";
        case MipsOp::CvtSW:
            return "cvt.s.w";
        case MipsOp::CCondS:
            return "c.cond.s";
        case MipsOp::Lvs:
            return "lv.s";
        case MipsOp::Lvlr:
            return "lv.lr";
        case MipsOp::Lvq:
            return "lv.q";
        case MipsOp::Svs:
            return "sv.s";
        case MipsOp::Svlr:
            return "sv.lr";
        case MipsOp::Svq:
            return "sv.q";
        case MipsOp::Vflush:
            return "vflush";
        case MipsOp::Vfpu0:
            return "vfpu0";
        case MipsOp::Vfpu1:
            return "vfpu1";
        case MipsOp::Vfpu3:
            return "vfpu3";
        case MipsOp::Vfpu4:
            return "vfpu4";
        case MipsOp::Vfpu5:
            return "vfpu5";
        case MipsOp::Vfpu6:
            return "vfpu6";
        case MipsOp::Lwc2:
            return "lwc2";
        case MipsOp::Swc2:
            return "swc2";
        case MipsOp::Cop2:
            return "cop2";
        case MipsOp::Dadd:
            return "dadd";
        case MipsOp::Daddu:
            return "daddu";
        case MipsOp::Dsub:
            return "dsub";
        case MipsOp::Dsubu:
            return "dsubu";
        case MipsOp::Daddi:
            return "daddi";
        case MipsOp::Daddiu:
            return "daddiu";
        case MipsOp::Dmult:
            return "dmult";
        case MipsOp::Dmultu:
            return "dmultu";
        case MipsOp::Ddiv:
            return "ddiv";
        case MipsOp::Ddivu:
            return "ddivu";
        case MipsOp::Dsll:
            return "dsll";
        case MipsOp::Dsrl:
            return "dsrl";
        case MipsOp::Dsra:
            return "dsra";
        case MipsOp::Dsll32:
            return "dsll32";
        case MipsOp::Dsrl32:
            return "dsrl32";
        case MipsOp::Dsra32:
            return "dsra32";
        case MipsOp::Dsllv:
            return "dsllv";
        case MipsOp::Dsrlv:
            return "dsrlv";
        case MipsOp::Dsrav:
            return "dsrav";
        case MipsOp::Ld:
            return "ld";
        case MipsOp::Sd:
            return "sd";
        case MipsOp::Lwu:
            return "lwu";
        case MipsOp::Ldl:
            return "ldl";
        case MipsOp::Ldr:
            return "ldr";
        case MipsOp::Sdl:
            return "sdl";
        case MipsOp::Sdr:
            return "sdr";
        case MipsOp::Ldc1:
            return "ldc1";
        case MipsOp::Sdc1:
            return "sdc1";
        case MipsOp::Dmfc1:
            return "dmfc1";
        case MipsOp::Dmtc1:
            return "dmtc1";
    }
    return "unknown";
}

const char* mips_reg_name(uint8_t reg) noexcept {
    static constexpr const char* kNames[32] = {
        "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3", "$t0", "$t1", "$t2",
        "$t3",   "$t4", "$t5", "$t6", "$t7", "$s0", "$s1", "$s2", "$s3", "$s4", "$s5",
        "$s6",   "$s7", "$t8", "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra",
    };
    return kNames[reg & 31U];
}

std::string format_mips(const DecodedMips& d) {
    char buf[128];
    const char* name = mips_op_name(d.op);
    if (d.op == MipsOp::Nop || d.op == MipsOp::Sync || d.op == MipsOp::Eret ||
        d.op == MipsOp::Vflush) {
        return name;
    }
    if (d.op == MipsOp::J || d.op == MipsOp::Jal) {
        std::snprintf(buf, sizeof(buf), "%s 0x%08X", name, d.target);
        return buf;
    }
    if (d.op == MipsOp::Jr) {
        std::snprintf(buf, sizeof(buf), "%s %s", name, mips_reg_name(d.rs));
        return buf;
    }
    if (d.op == MipsOp::Syscall || d.op == MipsOp::Break) {
        std::snprintf(buf, sizeof(buf), "%s 0x%05X", name, d.syscall_code);
        return buf;
    }
    if (d.is_branch) {
        std::snprintf(buf, sizeof(buf), "%s %s, %s, 0x%08X", name, mips_reg_name(d.rs),
                      mips_reg_name(d.rt), d.target);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%s %s, %s, %s (0x%04X)", name, mips_reg_name(d.rd),
                  mips_reg_name(d.rs), mips_reg_name(d.rt), static_cast<unsigned>(d.imm));
    return buf;
}

}  // namespace dragon::mips
