#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

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

void test_display_ctrl_mem_audio_hle() {
    PspRuntime rt;
    auto& cpu = rt.cpu();
    auto& hle = rt.hle();

    // 1. sceKernelAllocPartitionMemory + sceKernelGetBlockHeadAddr
    cpu.gpr[4] = 2U;     // user partition
    cpu.gpr[5] = 0U;     // name
    cpu.gpr[6] = 0U;     // low
    cpu.gpr[7] = 4096U;  // size
    CHECK(hle.dispatch_nid(rt, cpu, 0x237DBD4FU));
    const uint32_t uid = cpu.gpr[2];
    CHECK(uid >= 0x100U);

    cpu.gpr[4] = uid;
    CHECK(hle.dispatch_nid(rt, cpu, 0x9D9A5BA1U));
    const uint32_t blk_addr = cpu.gpr[2];
    CHECK(blk_addr >= 0x08900000U);

    // 2. sceCtrlReadBufferPositive
    PspInputState inp{};
    inp.buttons = kPspCtrlCross | kPspCtrlRight;
    inp.lx = 220;
    inp.ly = 64;
    hle.set_input(inp);
    cpu.gpr[4] = blk_addr;
    cpu.gpr[5] = 1U;
    CHECK(hle.dispatch_nid(rt, cpu, 0x1F803938U));
    CHECK(rt.bus().read32(blk_addr + 4U) == (kPspCtrlCross | kPspCtrlRight));
    CHECK(rt.bus().read8(blk_addr + 8U) == 220U);
    CHECK(rt.bus().read8(blk_addr + 9U) == 64U);

    // 3. sceDisplaySetFrameBuf + sceDisplayWaitVblankStart
    cpu.gpr[4] = 0x04000000U;
    cpu.gpr[5] = 512U;
    cpu.gpr[6] = 3U;  // RGBA8888
    CHECK(hle.dispatch_nid(rt, cpu, 0x289D82FEU));
    CHECK(rt.ge().display_top_addr() == 0x04000000U);
    CHECK(hle.dispatch_nid(rt, cpu, 0x984C27E7U));
    CHECK(cpu.vblank_yield == 1U);

    // 4. sceAudioChReserve + sceAudioOutput
    cpu.gpr[4] = 0xFFFFFFFFU;  // auto channel
    cpu.gpr[5] = 64U;          // 64 samples
    cpu.gpr[6] = 0U;           // stereo
    CHECK(hle.dispatch_nid(rt, cpu, 0x5EC81C55U));
    const int ch = static_cast<int>(cpu.gpr[2]);
    CHECK(ch >= 0);

    for (uint32_t i = 0; i < 128U; ++i) {
        rt.bus().write16(blk_addr + 256U + i * 2U, 10000U);
    }
    cpu.gpr[4] = static_cast<uint32_t>(ch);
    cpu.gpr[5] = 0x8000U;
    cpu.gpr[6] = blk_addr + 256U;
    CHECK(hle.dispatch_nid(rt, cpu, 0x8C1009B2U));
    CHECK(rt.audio().queued_frames() == 64U);
}

void test_ms0_file_io_hle() {
    PspRuntime rt;
    auto& cpu = rt.cpu();
    auto& hle = rt.hle();

    const auto tmp_dir = std::filesystem::temp_directory_path() / "dragon_psp_hle_io_test";
    std::error_code ec;
    std::filesystem::remove_all(tmp_dir, ec);
    hle.set_save_dir(tmp_dir);

    const uint32_t path_addr = 0x08901000U;
    const uint32_t data_addr = 0x08901100U;
    const uint32_t read_addr = 0x08901200U;
    const char* guest_path = "ms0:/SAVEDATA/test.bin";
    const char* payload = "DRAGON_PSP_OK";
    rt.bus().write_bytes(path_addr, guest_path, std::strlen(guest_path) + 1U);
    rt.bus().write_bytes(data_addr, payload, std::strlen(payload));

    // sceIoOpen (write | create | trunc)
    cpu.gpr[4] = path_addr;
    cpu.gpr[5] = 0x0602U;
    cpu.gpr[6] = 0777U;
    CHECK(hle.dispatch_nid(rt, cpu, 0x109F50BCU));
    const uint32_t fd = cpu.gpr[2];
    CHECK(static_cast<int32_t>(fd) >= 3);

    // sceIoWrite
    cpu.gpr[4] = fd;
    cpu.gpr[5] = data_addr;
    cpu.gpr[6] = static_cast<uint32_t>(std::strlen(payload));
    CHECK(hle.dispatch_nid(rt, cpu, 0x42EC03ACU));
    CHECK(cpu.gpr[2] == std::strlen(payload));

    // sceIoClose
    cpu.gpr[4] = fd;
    CHECK(hle.dispatch_nid(rt, cpu, 0x810C4BC3U));

    // Reopen read-only and verify content
    cpu.gpr[4] = path_addr;
    cpu.gpr[5] = 0x0001U;
    CHECK(hle.dispatch_nid(rt, cpu, 0x109F50BCU));
    const uint32_t fd2 = cpu.gpr[2];
    CHECK(static_cast<int32_t>(fd2) >= 3);

    cpu.gpr[4] = fd2;
    cpu.gpr[5] = read_addr;
    cpu.gpr[6] = static_cast<uint32_t>(std::strlen(payload));
    CHECK(hle.dispatch_nid(rt, cpu, 0x6A638D83U));
    CHECK(cpu.gpr[2] == std::strlen(payload));

    char out_buf[32] = {};
    rt.bus().read_bytes(read_addr, out_buf, std::strlen(payload));
    CHECK(std::strcmp(out_buf, payload) == 0);

    cpu.gpr[4] = fd2;
    hle.dispatch_nid(rt, cpu, 0x810C4BC3U);
    std::filesystem::remove_all(tmp_dir, ec);
}

}  // namespace

int main() {
    test_display_ctrl_mem_audio_hle();
    test_ms0_file_io_hle();
    if (g_failures == 0) {
        std::printf("psp_hle_unit_tests: ALL PASSED\n");
        return 0;
    }
    return 1;
}
