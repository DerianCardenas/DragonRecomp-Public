#include <cstdio>
#include <cstring>
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

inline uint32_t ge_cmd(uint8_t op, uint32_t arg) {
    return (static_cast<uint32_t>(op) << 24U) | (arg & 0x00FFFFFFU);
}

void write_f32(PspBus& bus, uint32_t addr, float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    bus.write32(addr, u);
}

void test_ge_3d_triangle_and_present() {
    PspRuntime rt;
    PspBus& bus = rt.bus();
    PspGe& ge = rt.ge();

    // Put 3 float vertices (color RGBA8888 + float XYZ) at 0x08900000
    // VTYPE: col_fmt=7 (bits 2..4 = 7 -> 0x1C), pos_fmt=3 (bits 7..8 = 3 -> 0x180), through=1 (bit
    // 23)
    const uint32_t vtype = (7U << 2U) | (3U << 7U) | (1U << 23U);
    const uint32_t vaddr = 0x08900000U;

    auto write_vert = [&](uint32_t idx, uint32_t abgr, float x, float y, float z) {
        const uint32_t base = vaddr + idx * 16U;
        bus.write32(base + 0U, abgr);
        write_f32(bus, base + 4U, x);
        write_f32(bus, base + 8U, y);
        write_f32(bus, base + 12U, z);
    };

    // Large green triangle covering center of 480x272 screen
    write_vert(0, 0xFF00FF00U, 100.0f, 50.0f, 0.0f);
    write_vert(1, 0xFF00FF00U, 380.0f, 50.0f, 0.0f);
    write_vert(2, 0xFF00FF00U, 240.0f, 220.0f, 0.0f);

    // Build GE display list at 0x08901000
    const uint32_t list_addr = 0x08901000U;
    const std::vector<uint32_t> dl = {
        ge_cmd(0x10, (vaddr >> 8U) & 0x000F0000U),  // BASE = 0x08000000
        ge_cmd(0x9C, 0x000000U),                    // FRAMEBUF PTR low
        ge_cmd(0x9D, 0x040200U),                    // FRAMEBUF PTR high=0x04, width=512
        ge_cmd(0xD2, 3U),                           // PIXEL FORMAT = RGBA8888
        ge_cmd(0x12, vtype),                        // VERTEXTYPE
        ge_cmd(0x01, vaddr & 0x00FFFFFFU),          // VADDR
        ge_cmd(0x04, (3U << 16U) | 3U),             // PRIM = GU_TRIANGLES, 3 verts
        ge_cmd(0x0F, 0U),                           // FINISH
        ge_cmd(0x0C, 0U),                           // END
    };
    for (size_t i = 0; i < dl.size(); ++i) {
        bus.write32(list_addr + static_cast<uint32_t>(i) * 4U, dl[i]);
    }

    ge.set_display_framebuf(0x04000000U, 512U, 3U);
    const int qid = ge.enqueue_list(bus, list_addr, 0U, -1);
    CHECK(qid > 0);
    CHECK(ge.triangles_submitted() == 1U);

    std::vector<uint32_t> fb(PspGe::kScreenWidth * PspGe::kScreenHeight, 0U);
    ge.present_frame(bus, fb.data());

    // Center pixel (240, 120) must be bright green (ABGR = 0xFF00FF00)
    const uint32_t center = fb[120U * PspGe::kScreenWidth + 240U];
    CHECK((center & 0x00FFFFFFU) == 0x0000FF00U);
    // Corner pixel (10, 10) must remain black
    const uint32_t corner = fb[10U * PspGe::kScreenWidth + 10U];
    CHECK((corner & 0x00FFFFFFU) == 0x00000000U);
}

}  // namespace

int main() {
    test_ge_3d_triangle_and_present();
    if (g_failures == 0) {
        std::printf("psp_ge_unit_tests: ALL PASSED\n");
        return 0;
    }
    return 1;
}
