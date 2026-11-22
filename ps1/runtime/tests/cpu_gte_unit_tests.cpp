#include <cstdio>
#include <filesystem>
#include <vector>

#include "ps1/gte.h"
#include "ps1/interpreter.h"
#include "ps1/runtime.h"

namespace {

using namespace dragon::ps1;

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

void test_r3000a_and_dma_otc() {
    Ps1Runtime rt;
    const uint32_t base = 0x80010000U;
    const std::vector<uint32_t> code = {
        enc_i(0x09, 0, 4, 15),           // addiu $a0, $zero, 15
        enc_i(0x09, 0, 5, 27),           // addiu $a1, $zero, 27
        enc_r(0x00, 4, 5, 2, 0, 0x21),   // addu $v0, $a0, $a1 -> 42
        enc_r(0x00, 4, 5, 0, 0, 0x18),   // mult $a0, $a1 -> 405
        enc_r(0x00, 0, 0, 8, 0, 0x12),   // mflo $t0 -> 405
        enc_r(0x00, 31, 0, 0, 0, 0x08),  // jr $ra -> halt at 0xBFC00000
        0U,                              // nop
    };
    for (size_t i = 0; i < code.size(); ++i) {
        rt.bus().write32(base + static_cast<uint32_t>(i) * 4U, code[i]);
    }
    rt.cpu().pc = base;
    rt.cpu().npc = base + 4U;
    rt.cpu().gpr[31] = 0xBFC00000U;

    Ps1Interpreter::run_slice(rt, rt.cpu(), 64);
    CHECK(rt.cpu().gpr[2] == 42U);
    CHECK(rt.cpu().gpr[8] == 405U);
    CHECK(rt.cpu().halted == 1U);

    // DMA Channel 6 OTC reverse linked list (4 entries starting at 0x8002000C)
    rt.bus().write32(0x1F8010E0U, 0x0002000CU);  // MADR
    rt.bus().write32(0x1F8010E4U, 4U);           // BCR = 4 words
    rt.bus().write32(0x1F8010E8U, 0x11000002U);  // CHCR trigger OTC
    CHECK(rt.bus().read32(0x8002000CU) == 0x00020008U);
    CHECK(rt.bus().read32(0x80020008U) == 0x00020004U);
    CHECK(rt.bus().read32(0x80020004U) == 0x00020000U);
    CHECK(rt.bus().read32(0x80020000U) == 0x00FFFFFFU);
}

void test_gte_rtps_nclip_avsz3() {
    Ps1Runtime rt;
    auto& cpu = rt.cpu();

    // Identity rotation matrix (4096 = 1.0 in 1.3.12 fixed point)
    Ps1Gte::write_ctrl(cpu, 0, 0x00001000U);  // R11=4096, R12=0
    Ps1Gte::write_ctrl(cpu, 1, 0x00000000U);  // R13=0, R21=0
    Ps1Gte::write_ctrl(cpu, 2, 0x00001000U);  // R22=4096, R23=0
    Ps1Gte::write_ctrl(cpu, 3, 0x00000000U);  // R31=0, R32=0
    Ps1Gte::write_ctrl(cpu, 4, 0x00001000U);  // R33=4096
    // Translation: TRX=0, TRY=0, TRZ=200
    Ps1Gte::write_ctrl(cpu, 5, 0U);
    Ps1Gte::write_ctrl(cpu, 6, 0U);
    Ps1Gte::write_ctrl(cpu, 7, 200U);
    // Screen center (160, 120) in 16.16 fixed point, H=200
    Ps1Gte::write_ctrl(cpu, 24, 160U << 16U);  // OFX
    Ps1Gte::write_ctrl(cpu, 25, 120U << 16U);  // OFY
    Ps1Gte::write_ctrl(cpu, 26, 200U);         // H
    Ps1Gte::write_ctrl(cpu, 29, 0x0555U);      // ZSF3 = 1/3 * 4096

    // Vertex V0 = (40, -20, 0) -> camera Z=200 -> SX = 160 + 40*(200/200) = 200, SY = 120 - 20 =
    // 100
    Ps1Gte::write_data(
        cpu, 0,
        static_cast<uint32_t>(40) | (static_cast<uint32_t>(static_cast<uint16_t>(-20)) << 16U));
    Ps1Gte::write_data(cpu, 1, 0U);
    Ps1Gte::execute_command(cpu, 0x4A180001U);  // RTPS (sf=1, lm=0)

    const uint32_t sxy2 = Ps1Gte::read_data(cpu, 14);
    const int16_t sx = static_cast<int16_t>(sxy2 & 0xFFFFU);
    const int16_t sy = static_cast<int16_t>(sxy2 >> 16U);
    CHECK(sx >= 199 && sx <= 201);
    CHECK(sy >= 99 && sy <= 101);
    CHECK(Ps1Gte::read_data(cpu, 19) == 200U);

    // NCLIP test: CW triangle (0,0), (10,0), (0,10) -> area = +100
    Ps1Gte::write_data(cpu, 12, 0x00000000U);   // SXY0 = (0,0)
    Ps1Gte::write_data(cpu, 13, 0x0000000AU);   // SXY1 = (10,0)
    Ps1Gte::write_data(cpu, 14, 0x000A0000U);   // SXY2 = (0,10)
    Ps1Gte::execute_command(cpu, 0x4A000006U);  // NCLIP
    CHECK(static_cast<int32_t>(Ps1Gte::read_data(cpu, 24)) == 100);

    // AVSZ3 test
    Ps1Gte::write_data(cpu, 17, 300U);
    Ps1Gte::write_data(cpu, 18, 300U);
    Ps1Gte::write_data(cpu, 19, 300U);
    Ps1Gte::execute_command(cpu, 0x4A00002DU);  // AVSZ3
    CHECK(Ps1Gte::read_data(cpu, 7) >= 299U && Ps1Gte::read_data(cpu, 7) <= 300U);

    // LZCS/LZCR test
    Ps1Gte::write_data(cpu, 30, 0x000000FFU);
    CHECK(Ps1Gte::read_data(cpu, 31) == 24U);
}

void test_save_state_roundtrip() {
    Ps1Runtime rt;
    rt.cpu().gpr[5] = 0x13572468U;
    rt.bus().write32(0x80030000U, 0xDEADBEEFU);
    const auto path = std::filesystem::temp_directory_path() / "dragon_ps1_state.drst";
    CHECK(rt.save_state(path));

    Ps1Runtime rt2;
    CHECK(rt2.load_state(path));
    CHECK(rt2.cpu().gpr[5] == 0x13572468U);
    CHECK(rt2.bus().read32(0x80030000U) == 0xDEADBEEFU);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace

int main() {
    test_r3000a_and_dma_otc();
    test_gte_rtps_nclip_avsz3();
    test_save_state_roundtrip();
    if (g_failures == 0) {
        std::printf("ps1_cpu_gte_unit_tests: ALL PASSED\n");
        return 0;
    }
    return 1;
}
