#include <cassert>
#include <cstdio>
#include <vector>

#include "nds/bus.h"
#include "nds/interpreter.h"

using namespace dragon::nds;

namespace {

void test_armv5te_clz_qadd_dsp_and_cp15() {
    std::printf("Running test_armv5te_clz_qadd_dsp_and_cp15...\n");
    Bus bus;
    NdsCpuState arm9{}, arm7{};
    bus.reset(arm9, arm7);

    // Place ARM9 code at 0x02000000:
    // 0: MOV R0, #0x00100000  (E3A00601)
    // 4: CLZ R1, R0           (E16F1F10) -> R1 = 11
    // 8: MOV R2, #0x7F000000  (E3A0247F)
    // C: QADD R3, R2, R2      (E1023052) -> R3 = 0x7FFFFFFF, Q=1
    // 10: SMULBB R4, R5, R6   (E1640685) -> R4 = (int16)R5 * (int16)R6
    // 14: MRC p15, 0, R7, c0, c0, 0 (EE107F10) -> R7 = 0x41059461
    const uint32_t code[] = {
        0xE3A00601U,  // MOV R0, #0x00100000
        0xE16F1F10U,  // CLZ R1, R0
        0xE3A0247FU,  // MOV R2, #0x7F000000
        0xE1023052U,  // QADD R3, R2, R2
        0xE1640685U,  // SMULBB R4, R5, R6
        0xEE107F10U,  // MRC p15, 0, R7, c0, c0, 0
    };
    for (size_t i = 0; i < sizeof(code) / 4U; ++i) {
        bus.arm9_write32(0x02000000U + static_cast<uint32_t>(i * 4U), code[i], &arm9);
    }
    arm9.r[5] = 0x0000FFF6U;  // -10 in low halfword
    arm9.r[6] = 0x00000007U;  // +7 in low halfword
    arm9.r[15] = 0x02000000U;

    for (int i = 0; i < 6; ++i) {
        cpu_step(arm9, bus);
    }

    assert(arm9.r[0] == 0x00100000U);
    assert(arm9.r[1] == 11U);
    assert(arm9.r[3] == 0x7FFFFFFFU);
    assert(arm9.q == 1U);
    assert(static_cast<int32_t>(arm9.r[4]) == -70);
    assert(arm9.r[7] == 0x41059461U);
    std::printf("  passed!\n");
}

void test_ipc_fifo_and_math_hw() {
    std::printf("Running test_ipc_fifo_and_math_hw...\n");
    Bus bus;
    NdsCpuState arm9{}, arm7{};
    bus.reset(arm9, arm7);

    // Enable IPC FIFO on both ARM9 and ARM7
    bus.arm9_write16(0x04000184U, 0x8400U, &arm9);
    bus.arm7_write16(0x04000184U, 0x8400U);

    // ARM9 -> ARM7
    bus.arm9_write32(0x04000188U, 0xDEADBEEFU, &arm9);
    assert((bus.arm7_if() & kIrqIpcRecvNotEmpty) != 0U);
    assert(bus.arm7_read32(0x04100000U) == 0xDEADBEEFU);

    // ARM7 -> ARM9
    bus.arm7_write32(0x04000188U, 0xCAFEBABEU);
    assert((bus.arm9_if() & kIrqIpcRecvNotEmpty) != 0U);
    assert(bus.arm9_read32(0x04100000U, &arm9) == 0xCAFEBABEU);

    // Hardware Divider (32/32): -100 / 7 = -14 rem -2
    bus.arm9_write32(0x04000280U, 0U, &arm9);
    bus.arm9_write32(0x04000290U, static_cast<uint32_t>(-100), &arm9);
    bus.arm9_write32(0x04000298U, 7U, &arm9);
    assert(static_cast<int32_t>(bus.arm9_read32(0x040002A0U, &arm9)) == -14);
    assert(static_cast<int32_t>(bus.arm9_read32(0x040002A8U, &arm9)) == -2);

    // Hardware Sqrt (32-bit): sqrt(144) == 12
    bus.arm9_write32(0x040002B0U, 0U, &arm9);
    bus.arm9_write32(0x040002B8U, 144U, &arm9);
    assert(bus.arm9_read32(0x040002B4U, &arm9) == 12U);
    std::printf("  passed!\n");
}

void test_touchscreen_spi_and_swi_hle() {
    std::printf("Running test_touchscreen_spi_and_swi_hle...\n");
    Bus bus;
    NdsCpuState arm9{}, arm7{};
    bus.reset(arm9, arm7);

    // Touch at (128, 96)
    bus.set_touch(true, 128U, 96U);
    const uint16_t extkey = static_cast<uint16_t>(bus.arm7_read32(0x04000134U) >> 16U);
    assert((extkey & 0x0040U) == 0U);  // Pen down active-low

    // Select TSC2046 (dev = 2, enable = bit 15, hold = bit 11)
    bus.arm7_write16(0x040001C0U, 0x8A00U);
    bus.arm7_write8(0x040001C2U, 0xD0U);  // Channel 5 = X
    bus.arm7_write8(0x040001C2U, 0x00U);
    const uint8_t hi = static_cast<uint8_t>(bus.arm7_read16(0x040001C2U));
    bus.arm7_write8(0x040001C2U, 0x00U);
    const uint8_t lo = static_cast<uint8_t>(bus.arm7_read16(0x040001C2U));
    const uint16_t adc_x = static_cast<uint16_t>(((hi << 8U) | lo) >> 3U);
    assert(adc_x == (128U << 4U));

    // SWI Div & Sqrt
    arm9.r[0] = 1000U;
    arm9.r[1] = 30U;
    cpu_handle_swi(arm9, bus, 0x09U);
    assert(arm9.r[0] == 33U && arm9.r[1] == 10U);
    arm9.r[0] = 625U;
    cpu_handle_swi(arm9, bus, 0x0DU);
    assert(arm9.r[0] == 25U);
    std::printf("  passed!\n");
}

}  // namespace

int main() {
    test_armv5te_clz_qadd_dsp_and_cp15();
    test_ipc_fifo_and_math_hw();
    test_touchscreen_spi_and_swi_hle();
    std::printf("All NDS CPU/Bus/IPC unit tests passed!\n");
    return 0;
}
