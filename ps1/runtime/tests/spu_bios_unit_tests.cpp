#include <cstdio>
#include <vector>

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

void test_spu_adpcm_voice() {
    Ps1Runtime rt;
    auto& spu = rt.bus().spu();

    // Write a 16-byte ADPCM block at SPU RAM offset 0x1000 (reg = 0x0200)
    spu.write16(0x1F801DA6U, 0x0200U);  // Transfer addr = 0x1000
    spu.write16(0x1F801DA8U, 0x0200U);  // shift=0, filter=0, flags=0x02 (loop)
    for (int i = 0; i < 7; ++i) {
        spu.write16(0x1F801DA8U, 0x4444U);  // Positive nibbles -> non-zero audio
    }

    // Configure voice 0
    spu.write16(0x1F801C00U, 0x3FFFU);  // Vol L
    spu.write16(0x1F801C02U, 0x3FFFU);  // Vol R
    spu.write16(0x1F801C04U, 0x1000U);  // Pitch = 44.1 kHz
    spu.write16(0x1F801C06U, 0x0200U);  // Start addr = 0x1000
    spu.write16(0x1F801DAAU, 0xC000U);  // Enable SPU + unmute
    spu.write16(0x1F801D88U, 0x0001U);  // Key-On voice 0

    spu.step_samples(64U);
    CHECK(spu.queued_frames() == 64U);

    std::vector<int16_t> pcm(128U, 0);
    spu.pop_stereo(pcm.data(), 64U);
    bool nonzero = false;
    for (int16_t s : pcm) {
        if (s != 0) {
            nonzero = true;
            break;
        }
    }
    CHECK(nonzero);
}

void test_bios_hle_events_and_pad() {
    Ps1Runtime rt;
    auto& cpu = rt.cpu();
    auto& bios = rt.bios();

    // InitPAD + StartPAD via B(0x12) / B(0x13)
    const uint32_t pad_buf = 0x80050000U;
    cpu.gpr[9] = 0x12U;
    cpu.gpr[4] = pad_buf;
    cpu.gpr[5] = 34U;
    bios.handle_vector(rt, cpu, 0x800000B0U);
    CHECK(cpu.gpr[2] == 1U);

    cpu.gpr[9] = 0x13U;
    bios.handle_vector(rt, cpu, 0x800000B0U);

    rt.bus().set_pad_buttons(kPs1PadCross | kPs1PadStart);
    bios.on_vblank(rt);

    CHECK(rt.bus().read8(pad_buf + 0U) == 0x00U);
    CHECK(rt.bus().read8(pad_buf + 1U) == 0x41U);
    const uint16_t mask_low = static_cast<uint16_t>(rt.bus().read8(pad_buf + 2U)) |
                              (static_cast<uint16_t>(rt.bus().read8(pad_buf + 3U)) << 8U);
    CHECK(static_cast<uint16_t>(~mask_low) == (kPs1PadCross | kPs1PadStart));

    // OpenEvent + EnableEvent + VBlank delivery + TestEvent
    cpu.gpr[9] = 0x08U;
    cpu.gpr[4] = 0xF2000003U;  // VBlank event class
    cpu.gpr[5] = 0x0002U;
    bios.handle_vector(rt, cpu, 0x800000B0U);
    const uint32_t ev = cpu.gpr[2];
    CHECK(ev >= 0xF1000000U);

    cpu.gpr[9] = 0x0CU;
    cpu.gpr[4] = ev;
    bios.handle_vector(rt, cpu, 0x800000B0U);

    bios.on_vblank(rt);
    cpu.gpr[9] = 0x0BU;  // TestEvent
    cpu.gpr[4] = ev;
    bios.handle_vector(rt, cpu, 0x800000B0U);
    CHECK(cpu.gpr[2] == 1U);
}

}  // namespace

int main() {
    test_spu_adpcm_voice();
    test_bios_hle_events_and_pad();
    if (g_failures == 0) {
        std::printf("ps1_spu_bios_unit_tests: ALL PASSED\n");
        return 0;
    }
    return 1;
}
