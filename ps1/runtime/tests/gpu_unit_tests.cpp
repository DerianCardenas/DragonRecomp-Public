#include <cstdio>
#include <vector>

#include "ps1/gpu.h"
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

void test_gpu_polygon_and_dma_linked_list() {
    Ps1Runtime rt;
    auto& bus = rt.bus();
    auto& gpu = bus.gpu();

    // Configure display 320x240 at (0,0), enabled
    gpu.write_gp1(0x03000000U);  // Display enable
    gpu.write_gp1(0x05000000U);  // Display start (0,0)
    gpu.write_gp1(0x08000001U);  // 320x240 15-bit

    // Build GPU linked list in Main RAM at 0x80040000:
    // Packet 1 (3 words): FillRect black 320x240 -> next = 0x00040020
    bus.write32(0x80040000U, (3U << 24U) | 0x00040020U);
    bus.write32(0x80040004U, 0x02000000U);  // FillRect black
    bus.write32(0x80040008U, 0x00000000U);  // (0,0)
    bus.write32(0x8004000CU, (240U << 16U) | 320U);

    // Packet 2 (4 words): Flat red triangle (0x200000FF) covering center -> terminator 0x00FFFFFF
    bus.write32(0x80040020U, (4U << 24U) | 0x00FFFFFFU);
    bus.write32(0x80040024U, 0x200000FFU);  // Flat red (R=255)
    bus.write32(0x80040028U, (40U << 16U) | 80U);
    bus.write32(0x8004002CU, (40U << 16U) | 240U);
    bus.write32(0x80040030U, (200U << 16U) | 160U);

    // Kick DMA Channel 2 linked-list mode
    bus.write32(0x1F8010A0U, 0x00040000U);  // MADR
    bus.write32(0x1F8010A4U, 0U);           // BCR
    bus.write32(0x1F8010A8U, 0x01000401U);  // Start, Linked-List, RAM->GPU

    CHECK(gpu.primitives_drawn() == 2U);

    std::vector<uint32_t> fb;
    uint32_t w = 0, h = 0;
    gpu.present_frame(fb, w, h);
    CHECK(w == 320U && h == 240U);

    // Center pixel (160, 100) must be bright red (ABGR = 0xFF0000FF)
    const uint32_t center = fb[100U * w + 160U];
    CHECK((center & 0x00FFFFFFU) == 0x000000FFU);
    // Corner pixel (5, 5) must be black
    const uint32_t corner = fb[5U * w + 5U];
    CHECK((corner & 0x00FFFFFFU) == 0x00000000U);
}

}  // namespace

int main() {
    test_gpu_polygon_and_dma_linked_list();
    if (g_failures == 0) {
        std::printf("ps1_gpu_unit_tests: ALL PASSED\n");
        return 0;
    }
    return 1;
}
