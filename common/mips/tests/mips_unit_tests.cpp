#include "dragon/mips.h"

#include <cstdio>
#include <cstdlib>

namespace {

using namespace dragon::mips;

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

uint32_t enc_r(uint8_t op, uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct) {
    return (static_cast<uint32_t>(op) << 26U) | (static_cast<uint32_t>(rs) << 21U) |
           (static_cast<uint32_t>(rt) << 16U) | (static_cast<uint32_t>(rd) << 11U) |
           (static_cast<uint32_t>(sa) << 6U) | static_cast<uint32_t>(funct);
}

uint32_t enc_i(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (static_cast<uint32_t>(op) << 26U) | (static_cast<uint32_t>(rs) << 21U) |
           (static_cast<uint32_t>(rt) << 16U) | static_cast<uint32_t>(imm);
}

uint32_t enc_j(uint8_t op, uint32_t target) {
    return (static_cast<uint32_t>(op) << 26U) | ((target >> 2U) & 0x03FFFFFFU);
}

void test_basic_mips32() {
    auto nop = decode_mips(0x00000000U, 0x08804000U);
    CHECK(nop.op == MipsOp::Nop);

    auto addiu = decode_mips(enc_i(0x09, 29, 29, static_cast<uint16_t>(-32)), 0x08804000U);
    CHECK(addiu.op == MipsOp::Addiu);
    CHECK(addiu.rs == 29 && addiu.rt == 29 && addiu.simm == -32);

    auto jal = decode_mips(enc_j(0x03, 0x08805100U), 0x08804004U);
    CHECK(jal.op == MipsOp::Jal);
    CHECK(jal.target == 0x08805100U);
    CHECK(jal.is_call && jal.is_jump && jal.has_delay_slot);

    auto jr_ra = decode_mips(enc_r(0x00, 31, 0, 0, 0, 0x08), 0x08804008U);
    CHECK(jr_ra.op == MipsOp::Jr);
    CHECK(jr_ra.is_return && jr_ra.is_indirect_jump && jr_ra.has_delay_slot);

    auto bne = decode_mips(enc_i(0x05, 4, 5, 4), 0x08804000U);
    CHECK(bne.op == MipsOp::Bne);
    CHECK(bne.is_branch && !bne.is_branch_likely && bne.has_delay_slot);
    CHECK(bne.target == 0x08804014U);

    auto beql = decode_mips(enc_i(0x14, 4, 0, static_cast<uint16_t>(-2)), 0x08804010U);
    CHECK(beql.op == MipsOp::Beql);
    CHECK(beql.is_branch && beql.is_branch_likely && beql.target == 0x0880400CU);

    auto syscall = decode_mips(enc_r(0x00, 0, 0, 0, 0, 0x0C) | (0x2005U << 6U), 0x08804020U);
    CHECK(syscall.op == MipsOp::Syscall);
    CHECK(syscall.syscall_code == 0x2005U);
}

void test_allegrex_extensions() {
    // Verified Allegrex encodings against PPSSPP MIPSTables.cpp
    CHECK(decode_mips(enc_r(0x00, 1, 5, 4, 8, 0x02), 0).op == MipsOp::Rotr);
    CHECK(decode_mips(enc_r(0x00, 0, 5, 4, 8, 0x02), 0).op == MipsOp::Srl);
    CHECK(decode_mips(enc_r(0x00, 6, 5, 4, 1, 0x06), 0).op == MipsOp::Rotrv);
    CHECK(decode_mips(enc_r(0x00, 6, 5, 4, 0, 0x06), 0).op == MipsOp::Srlv);

    CHECK(decode_mips(enc_r(0x00, 5, 0, 4, 0, 0x16), 0).op == MipsOp::Clz);
    CHECK(decode_mips(enc_r(0x00, 5, 0, 4, 0, 0x17), 0).op == MipsOp::Clo);
    CHECK(decode_mips(enc_r(0x00, 5, 6, 0, 0, 0x1C), 0).op == MipsOp::Madd);
    CHECK(decode_mips(enc_r(0x00, 5, 6, 0, 0, 0x1D), 0).op == MipsOp::Maddu);
    CHECK(decode_mips(enc_r(0x00, 5, 6, 4, 0, 0x2C), 0).op == MipsOp::Max);
    CHECK(decode_mips(enc_r(0x00, 5, 6, 4, 0, 0x2D), 0).op == MipsOp::Min);
    CHECK(decode_mips(enc_r(0x00, 5, 6, 0, 0, 0x2E), 0).op == MipsOp::Msub);
    CHECK(decode_mips(enc_r(0x00, 5, 6, 0, 0, 0x2F), 0).op == MipsOp::Msubu);

    CHECK(decode_mips(enc_r(0x1F, 5, 4, 7, 3, 0x00), 0).op == MipsOp::Ext);
    CHECK(decode_mips(enc_r(0x1F, 5, 4, 10, 3, 0x04), 0).op == MipsOp::Ins);
    CHECK(decode_mips(enc_r(0x1F, 0, 5, 4, 2, 0x20), 0).op == MipsOp::Wsbh);
    CHECK(decode_mips(enc_r(0x1F, 0, 5, 4, 3, 0x20), 0).op == MipsOp::Wsbw);
    CHECK(decode_mips(enc_r(0x1F, 0, 5, 4, 16, 0x20), 0).op == MipsOp::Seb);
    CHECK(decode_mips(enc_r(0x1F, 0, 5, 4, 20, 0x20), 0).op == MipsOp::Bitrev);
    CHECK(decode_mips(enc_r(0x1F, 0, 5, 4, 24, 0x20), 0).op == MipsOp::Seh);
}

void test_fpu_and_r4300i() {
    CHECK(decode_mips(enc_r(0x11, 0x10, 2, 1, 0, 0x00), 0).op == MipsOp::AddS);
    CHECK(decode_mips(enc_r(0x11, 0x10, 2, 1, 0, 0x02), 0).op == MipsOp::MulS);
    CHECK(decode_mips(enc_r(0x11, 0x10, 2, 1, 0, 0x32), 0).op == MipsOp::CCondS);
    CHECK(decode_mips(enc_r(0x11, 0x14, 0, 1, 0, 0x20), 0).op == MipsOp::CvtSW);
    CHECK(decode_mips(enc_i(0x11, 0x08, 0x01, 2), 0x08804000U).op == MipsOp::Bc1t);

    // R4300i profile disambiguates 64-bit instructions from Allegrex extensions
    CHECK(decode_mips(enc_r(0x00, 5, 6, 4, 0, 0x2C), 0, MipsProfile::R4300i).op == MipsOp::Dadd);
    CHECK(decode_mips(enc_r(0x00, 5, 6, 4, 0, 0x2D), 0, MipsProfile::R4300i).op == MipsOp::Daddu);
    CHECK(decode_mips(enc_i(0x37, 29, 4, 8), 0, MipsProfile::R4300i).op == MipsOp::Ld);
    CHECK(decode_mips(enc_i(0x3F, 29, 4, 8), 0, MipsProfile::R4300i).op == MipsOp::Sd);
    // Same opcodes under Allegrex are VFPU
    CHECK(decode_mips(enc_i(0x37, 29, 4, 8), 0, MipsProfile::Allegrex).op == MipsOp::Vfpu5);
    CHECK(decode_mips(enc_i(0x3F, 0, 0, 0), 0, MipsProfile::Allegrex).op == MipsOp::Vflush);
}

}  // namespace

int main() {
    test_basic_mips32();
    test_allegrex_extensions();
    test_fpu_and_r4300i();
    if (g_failures == 0) {
        std::printf("dragon_mips_tests: ALL PASSED\n");
        return 0;
    }
    return 1;
}
