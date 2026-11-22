#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "psp/interpreter.h"
#include "psp/runtime.h"

namespace {

using namespace dragon::psp;

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

void write_prog(PspRuntime& rt, uint32_t base, const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i) {
        rt.bus().write32(base + static_cast<uint32_t>(i) * 4U, words[i]);
    }
    rt.cpu().pc = base;
    rt.cpu().npc = base + 4U;
    rt.cpu().halted = 0;
}

void test_allegrex_alu_and_ext() {
    PspRuntime rt;
    rt.cpu().gpr[4] = 0x00123456U;
    rt.cpu().gpr[5] = static_cast<uint32_t>(-10);
    rt.cpu().gpr[6] = 25U;

    const uint32_t base = 0x08804000U;
    write_prog(rt, base,
               {
                   enc_r(0x00, 4, 0, 8, 0, 0x16),    // clz $t0, $a0 -> 11
                   enc_r(0x00, 5, 6, 9, 0, 0x2C),    // max $t1, $a1(-10), $a2(25) -> 25
                   enc_r(0x00, 5, 6, 10, 0, 0x2D),   // min $t2, $a1(-10), $a2(25) -> -10
                   enc_r(0x1F, 4, 11, 7, 8, 0x00),   // ext $t3, $a0, 8, 8 -> 0x34
                   enc_r(0x1F, 0, 4, 12, 2, 0x20),   // wsbh $t4, $a0 -> 0x12005634
                   enc_r(0x1F, 0, 4, 13, 3, 0x20),   // wsbw $t5, $a0 -> 0x56341200
                   enc_r(0x1F, 0, 5, 14, 16, 0x20),  // seb $t6, $a1 -> -10
                   enc_r(0x00, 1, 4, 15, 4, 0x02),   // rotr $t7, $a0, 4 -> 0x60012345
                   enc_r(0x1C, 0, 0, 0, 0, 0x00),    // halt
               });

    PspInterpreter::run_slice(rt, rt.cpu(), 64);
    CHECK(rt.cpu().gpr[8] == 11U);
    CHECK(rt.cpu().gpr[9] == 25U);
    CHECK(static_cast<int32_t>(rt.cpu().gpr[10]) == -10);
    CHECK(rt.cpu().gpr[11] == 0x34U);
    CHECK(rt.cpu().gpr[12] == 0x12005634U);
    CHECK(rt.cpu().gpr[13] == 0x56341200U);
    CHECK(static_cast<int32_t>(rt.cpu().gpr[14]) == -10);
    CHECK(rt.cpu().gpr[15] == 0x60012345U);
}

void test_delay_slot_and_branch_likely() {
    PspRuntime rt;
    const uint32_t base = 0x08804100U;
    write_prog(rt, base,
               {
                   enc_i(0x09, 0, 8, 5),   // addiu $t0, $zero, 5
                   enc_i(0x14, 8, 0, 2),   // beql $t0, $zero, +2 (NOT taken -> nullify delay slot!)
                   enc_i(0x09, 0, 9, 99),  // (nullified) addiu $t1, $zero, 99
                   enc_i(0x05, 8, 0, 2),   // bne $t0, $zero, +2 (TAKEN -> execute delay slot!)
                   enc_i(0x09, 0, 10, 42),         // (executed) addiu $t2, $zero, 42
                   enc_i(0x09, 0, 11, 77),         // skipped
                   enc_r(0x1C, 0, 0, 0, 0, 0x00),  // halt
               });

    PspInterpreter::run_slice(rt, rt.cpu(), 32);
    CHECK(rt.cpu().gpr[9] == 0U);    // delay slot was nullified
    CHECK(rt.cpu().gpr[10] == 42U);  // delay slot executed
    CHECK(rt.cpu().gpr[11] == 0U);   // target skipped +1
}

void test_unaligned_and_fpu() {
    PspRuntime rt;
    const uint32_t data_addr = 0x08900001U;
    rt.bus().write32(0x08900000U, 0x44332211U);
    rt.bus().write32(0x08900004U, 0x88776655U);

    // Load unaligned 32-bit word at 0x08900001 using lwl +3 / lwr +0 -> 0x55443322
    uint32_t v = 0;
    v = psp_lwl(&rt.cpu(), data_addr + 3U, v);
    v = psp_lwr(&rt.cpu(), data_addr, v);
    CHECK(v == 0x55443322U);

    // FPU add.s + c.lt.s + bc1t
    rt.cpu().f[1] = 1.5f;
    rt.cpu().f[2] = 2.25f;
    const uint32_t base = 0x08804200U;
    write_prog(rt, base,
               {
                   enc_r(0x11, 0x10, 2, 1, 3, 0x00),  // add.s $f3, $f1, $f2 -> 3.75
                   enc_r(0x11, 0x10, 3, 1, 0, 0x3C),  // c.lt.s $f1, $f3 -> FCC=1
                   enc_i(0x11, 0x08, 0x01, 2),        // bc1t +2
                   enc_i(0x09, 0, 8, 1),              // delay slot: $t0 = 1
                   enc_i(0x09, 0, 9, 2),              // skipped
                   enc_r(0x1C, 0, 0, 0, 0, 0x00),     // halt
               });
    PspInterpreter::run_slice(rt, rt.cpu(), 32);
    CHECK(std::fabs(rt.cpu().f[3] - 3.75f) < 1e-5f);
    CHECK(rt.cpu().gpr[8] == 1U);
    CHECK(rt.cpu().gpr[9] == 0U);
}

void test_save_state_roundtrip() {
    PspRuntime rt;
    rt.cpu().gpr[4] = 0xCAFEBABEU;
    rt.bus().write32(0x08810000U, 0x12345678U);
    const auto path = std::filesystem::temp_directory_path() / "dragon_psp_test.drst";
    CHECK(rt.save_state(path));

    PspRuntime rt2;
    CHECK(rt2.load_state(path));
    CHECK(rt2.cpu().gpr[4] == 0xCAFEBABEU);
    CHECK(rt2.bus().read32(0x08810000U) == 0x12345678U);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace

int main() {
    test_allegrex_alu_and_ext();
    test_delay_slot_and_branch_likely();
    test_unaligned_and_fpu();
    test_save_state_roundtrip();
    if (g_failures == 0) {
        std::printf("psp_cpu_unit_tests: ALL PASSED\n");
        return 0;
    }
    return 1;
}
