#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "gba/bus.h"
#include "gba/cartridge.h"
#include "gba/cpu_state.h"
#include "gba/interpreter.h"
#include "gba/runtime.h"

namespace {

using dragon::gba::Bus;
using dragon::gba::Cartridge;
using dragon::gba::SaveType;

std::vector<uint8_t> make_test_rom(size_t size = 0x4000) {
    std::vector<uint8_t> rom(size, 0);
    // Entry point: B 0x080000C0 (offset = (0xC0 - 8) / 4 = 0x2E -> EA00002E)
    const uint32_t branch = 0xEA00002EU;
    std::memcpy(&rom[0], &branch, 4);
    const char* title = "DRAGONTEST";
    std::memcpy(&rom[0xA0], title, std::strlen(title));
    const char* code = "ADTE";
    std::memcpy(&rom[0xAC], code, 4);
    rom[0xB2] = 0x96;
    uint8_t chk = 0;
    for (size_t i = 0xA0; i <= 0xBC; ++i) {
        chk = static_cast<uint8_t>(chk - rom[i]);
    }
    rom[0xBD] = static_cast<uint8_t>(chk - 0x19U);
    return rom;
}

void write32_le(std::vector<uint8_t>& rom, uint32_t offset, uint32_t val) {
    rom[offset + 0] = static_cast<uint8_t>(val & 0xFFU);
    rom[offset + 1] = static_cast<uint8_t>((val >> 8) & 0xFFU);
    rom[offset + 2] = static_cast<uint8_t>((val >> 16) & 0xFFU);
    rom[offset + 3] = static_cast<uint8_t>((val >> 24) & 0xFFU);
}

void write16_le(std::vector<uint8_t>& rom, uint32_t offset, uint16_t val) {
    rom[offset + 0] = static_cast<uint8_t>(val & 0xFFU);
    rom[offset + 1] = static_cast<uint8_t>((val >> 8) & 0xFFU);
}

void test_cartridge_detection() {
    auto raw = make_test_rom(0x4000);
    const char* sig = "FLASH1M_V103";
    std::memcpy(&raw[0x1000], sig, std::strlen(sig));

    Cartridge cart = dragon::gba::parse_gba_rom_bytes(std::move(raw));
    assert(cart.title == "DRAGONTEST");
    assert(cart.game_code == "ADTE");
    assert(cart.checksum_valid);
    assert(cart.save_type == SaveType::Flash128K);
}

void test_arm_alu_and_unaligned_loads() {
    auto raw = make_test_rom();
    // 0x080000C0:
    // MOV R0, #25             (E3A00019)
    // MOV R1, #17             (E3A01011)
    // ADD R2, R0, R1, LSL #1  (E0802081) -> 25 + 34 = 59
    // SUBS R3, R1, R0         (E0513000) -> 17 - 25 = -8 (N=1, C=0)
    // MUL R4, R0, R1          (E0040190) -> 25 * 17 = 425
    write32_le(raw, 0xC0, 0xE3A00019U);
    write32_le(raw, 0xC4, 0xE3A01011U);
    write32_le(raw, 0xC8, 0xE0802081U);
    write32_le(raw, 0xCC, 0xE0513000U);
    write32_le(raw, 0xD0, 0xE0040190U);

    Cartridge cart = dragon::gba::parse_gba_rom_bytes(std::move(raw));
    Bus bus;
    bus.load_cartridge(cart);
    bus.reset();
    CpuState cpu{};
    dragon::gba::cpu_init(cpu, &bus);

    // Step B 0x080000C0 + 5 instructions
    for (int i = 0; i < 6; ++i) {
        dragon::gba::cpu_step(cpu);
    }
    assert(cpu.r[0] == 25U);
    assert(cpu.r[1] == 17U);
    assert(cpu.r[2] == 59U);
    assert(cpu.r[3] == static_cast<uint32_t>(-8));
    assert(cpu.n != 0U);
    assert(cpu.c == 0U);
    assert(cpu.r[4] == 425U);

    // Test ARM7TDMI unaligned LDR, LDRH, LDRSH
    bus.write32(0x02000000U, 0x11223344U);
    assert(gba_ldr32(&cpu, 0x02000000U) == 0x11223344U);
    assert(gba_ldr32(&cpu, 0x02000001U) == 0x44112233U);
    assert(gba_ldr32(&cpu, 0x02000002U) == 0x33441122U);
    assert(gba_ldr32(&cpu, 0x02000003U) == 0x22334411U);

    bus.write16(0x02000010U, 0x80F0U);
    assert(gba_ldrh16(&cpu, 0x02000010U) == 0x000080F0U);
    assert(gba_ldrh16(&cpu, 0x02000011U) == 0xF0000080U);
    assert(gba_ldrsh16(&cpu, 0x02000010U) == 0xFFFF80F0U);
    // Unaligned LDRSH sign-extends only the odd byte!
    assert(gba_ldrsh16(&cpu, 0x02000011U) == 0xFFFFFF80U);
}

void test_interworking_and_thumb_execution() {
    auto raw = make_test_rom();
    // 0x080000C0 (ARM):
    // LDR R0, [PC, #4]  (E59F0004) -> loads from 0xC0 + 8 + 4 = 0xCC (0x08000101)
    // BX R0             (E12FFF10) -> switches to Thumb at 0x08000100
    // B .               (EAFFFFFE)
    // .word 0x08000101  (at 0xCC)
    write32_le(raw, 0xC0, 0xE59F0004U);
    write32_le(raw, 0xC4, 0xE12FFF10U);
    write32_le(raw, 0xC8, 0xEAFFFFFEU);
    write32_le(raw, 0xCC, 0x08000101U);

    // 0x08000100 (Thumb):
    // PUSH {R4, LR}     (B510)
    // MOV R4, #42       (242A)
    // BL 0x08000110     (F000 F804: PC=0x104+4=0x108 + 8 = 0x110)
    // ADD R0, R0, R4    (1900: Rd=0, Rs=0, Rn=4 -> R0 = R0 + R4)
    // POP {R4, PC}      (BD10)
    write16_le(raw, 0x100, 0xB510U);
    write16_le(raw, 0x102, 0x242AU);
    write16_le(raw, 0x104, 0xF000U);
    write16_le(raw, 0x106, 0xF804U);
    write16_le(raw, 0x108, 0x1900U);
    write16_le(raw, 0x10A, 0xBD10U);

    // 0x08000110 (Thumb leaf):
    // MOV R0, #58       (203A)
    // BX LR             (4770)
    write16_le(raw, 0x110, 0x203AU);
    write16_le(raw, 0x112, 0x4770U);

    Cartridge cart = dragon::gba::parse_gba_rom_bytes(std::move(raw));
    Bus bus;
    bus.load_cartridge(cart);
    bus.reset();
    CpuState cpu{};
    dragon::gba::cpu_init(cpu, &bus);
    cpu.r[14] = 0x080000C8U;  // ARM return address

    // Step B 0xC0, LDR R0, BX R0
    dragon::gba::cpu_step(cpu);
    dragon::gba::cpu_step(cpu);
    dragon::gba::cpu_step(cpu);
    assert(cpu.thumb != 0U);
    assert(cpu.r[15] == 0x08000100U);

    // Step PUSH, MOV, BL (2 steps), MOV, BX LR, ADD, POP {R4, PC}
    for (int i = 0; i < 8; ++i) {
        dragon::gba::cpu_step(cpu);
    }
    assert(cpu.r[0] == 100U);
    assert(cpu.r[15] == 0x080000C8U);
}

void test_bios_swi_hle() {
    auto raw = make_test_rom();
    Cartridge cart = dragon::gba::parse_gba_rom_bytes(std::move(raw));
    Bus bus;
    bus.load_cartridge(cart);
    bus.reset();
    CpuState cpu{};
    dragon::gba::cpu_init(cpu, &bus);

    // SWI 0x06: Div (1000 / -7 = -142, rem 6, abs 142)
    cpu.r[0] = 1000U;
    cpu.r[1] = static_cast<uint32_t>(-7);
    dragon::gba::cpu_handle_swi(cpu, 0x06U);
    assert(static_cast<int32_t>(cpu.r[0]) == -142);
    assert(static_cast<int32_t>(cpu.r[1]) == 6);
    assert(cpu.r[3] == 142U);

    // SWI 0x08: Sqrt (144 -> 12)
    cpu.r[0] = 144U;
    dragon::gba::cpu_handle_swi(cpu, 0x08U);
    assert(cpu.r[0] == 12U);

    // SWI 0x0B: CpuSet (32-bit fill 4 words)
    bus.write32(0x02000100U, 0xCAFEBABEU);
    cpu.r[0] = 0x02000100U;
    cpu.r[1] = 0x02000200U;
    cpu.r[2] = 4U | (1U << 24) | (1U << 26);  // fill + 32-bit
    dragon::gba::cpu_handle_swi(cpu, 0x0BU);
    for (uint32_t i = 0; i < 4U; ++i) {
        assert(bus.read32(0x02000200U + i * 4U) == 0xCAFEBABEU);
    }

    // SWI 0x11: LZ77UnCompWram ("AAAAAAAA" compressed)
    const uint8_t lz_data[] = {0x10, 0x08, 0x00, 0x00, 0x40, 'A', 0x40, 0x00};
    for (size_t i = 0; i < sizeof(lz_data); ++i) {
        bus.write8(0x02001000U + static_cast<uint32_t>(i), lz_data[i]);
    }
    cpu.r[0] = 0x02001000U;
    cpu.r[1] = 0x02002000U;
    dragon::gba::cpu_handle_swi(cpu, 0x11U);
    for (uint32_t i = 0; i < 8U; ++i) {
        assert(bus.read8(0x02002000U + i) == 'A');
    }

    // SWI 0x0D: GetBiosChecksum
    dragon::gba::cpu_handle_swi(cpu, 0x0DU);
    assert(cpu.r[0] == 0xBAAE187FU);

    // SWI 0x13: HuffUnComp (8-bit Huffman tree with 2 leaves: 'X' on bit 0, 'Y' on bit 1 -> "XYXY")
    // Header at 0x02003000: 0x00000428 (type=2, bit_depth=8, size=4 bytes)
    bus.write32(0x02003000U, 0x00000428U);
    bus.write8(0x02003004U, 0x01U);  // tree_size_half = 1 -> tree bytes = 4 (at 0x3004..0x3007)
    bus.write8(0x02003005U,
               0xC0U);  // root node: both child 0 (0x80) and child 1 (0x40) are leaves, offset=0
    bus.write8(0x02003006U, 'X');  // child 0 leaf
    bus.write8(0x02003007U, 'Y');  // child 1 leaf
    // Bitstream at 0x02003008 (MSB first: bits 31..28 = 0, 1, 0, 1 -> 'X', 'Y', 'X', 'Y')
    bus.write32(0x02003008U, 0x50000000U);
    cpu.r[0] = 0x02003000U;
    cpu.r[1] = 0x02003100U;
    dragon::gba::cpu_handle_swi(cpu, 0x13U);
    assert(bus.read8(0x02003100U) == 'X');
    assert(bus.read8(0x02003101U) == 'Y');
    assert(bus.read8(0x02003102U) == 'X');
    assert(bus.read8(0x02003103U) == 'Y');

    // Verify gba_runtime_dispatch sets cpu.thumb from target bit 0 for interworking fallback
    write16_le(cart.rom, 0x200, 0x2055U);  // MOV R0, #0x55
    write16_le(cart.rom, 0x202, 0x4770U);  // BX LR
    bus.load_cartridge(cart);
    dragon::gba::cpu_init(cpu, &bus);
    cpu.thumb = 0U;
    cpu.r[14] = 0x080000C8U;
    gba_runtime_dispatch(&cpu, 0x08000201U, 0x080000C0U);
    assert(cpu.r[0] == 0x55U);
    assert(cpu.thumb == 0U);
}

void test_dma_timers_and_saves() {
    auto raw = make_test_rom();
    Cartridge cart = dragon::gba::parse_gba_rom_bytes(std::move(raw));
    cart.save_type = SaveType::Flash128K;
    Bus bus;
    bus.load_cartridge(cart);
    bus.reset();
    CpuState cpu{};
    dragon::gba::cpu_init(cpu, &bus);

    // 1. DMA3 immediate 32-bit copy
    bus.write32(0x02000000U, 0xDEADBEEFU);
    bus.write32(0x02000004U, 0x12345678U);
    bus.write32(0x040000D4U, 0x02000000U);  // DMA3SAD
    bus.write32(0x040000D8U, 0x03000000U);  // DMA3DAD
    bus.write16(0x040000DCU, 2U);           // DMA3CNT_L = 2 words
    bus.write16(0x040000DEU, 0x8400U);      // DMA3CNT_H = Enable + 32-bit + Immediate
    assert(bus.read32(0x03000000U) == 0xDEADBEEFU);
    assert(bus.read32(0x03000004U) == 0x12345678U);

    // 2. Timer 0 overflow cascading into Timer 1
    bus.write16(0x04000100U, 0xFFF0U);  // TM0CNT_L reload = 0xFFF0 (overflows every 16 cycles)
    bus.write16(0x04000104U, 0x0000U);  // TM1CNT_L reload = 0
    bus.write16(0x04000106U, 0x0084U);  // TM1CNT_H = Enable + Count-up (cascade)
    bus.write16(0x04000102U, 0x0080U);  // TM0CNT_H = Enable + Prescaler 1
    cpu.cycles += 64ULL;
    bus.sync_hardware(cpu.cycles);
    assert(bus.read16(0x04000104U) == 4U);

    // 3. Flash 128K Chip ID, Bank Switch, and Byte Program
    bus.write8(0x0E005555U, 0xAAU);
    bus.write8(0x0E002AAAU, 0x55U);
    bus.write8(0x0E005555U, 0x90U);  // Enter ID mode
    assert(bus.read8(0x0E000000U) == 0x62U);
    assert(bus.read8(0x0E000001U) == 0x13U);
    bus.write8(0x0E005555U, 0xAAU);
    bus.write8(0x0E002AAAU, 0x55U);
    bus.write8(0x0E005555U, 0xF0U);  // Exit ID mode

    // Switch to bank 1 and program byte 0x42 at 0x0E000123
    bus.write8(0x0E005555U, 0xAAU);
    bus.write8(0x0E002AAAU, 0x55U);
    bus.write8(0x0E005555U, 0xB0U);
    bus.write8(0x0E000000U, 0x01U);  // Bank 1

    bus.write8(0x0E005555U, 0xAAU);
    bus.write8(0x0E002AAAU, 0x55U);
    bus.write8(0x0E005555U, 0xA0U);
    bus.write8(0x0E000123U, 0x42U);
    assert(bus.read8(0x0E000123U) == 0x42U);

    // 4. Save State v2 round-trip (verifying registers I/O and timers persist)
    const auto state_path = std::filesystem::temp_directory_path() / "gba_test_state.drst";
    cpu.r[7] = 0x778899AAU;
    bus.write16(0x04000200U, 0x1234U);  // IE
    bus.write16(0x04000208U, 0x0001U);  // IME
    assert(bus.save_state(cpu, state_path));
    cpu.r[7] = 0U;
    bus.write32(0x03000000U, 0U);
    bus.write16(0x04000200U, 0U);
    bus.write16(0x04000208U, 0U);
    assert(bus.load_state(cpu, state_path));
    assert(cpu.r[7] == 0x778899AAU);
    assert(bus.read32(0x03000000U) == 0xDEADBEEFU);
    assert(bus.read16(0x04000200U) == 0x1234U);
    assert(bus.read16(0x04000208U) == 0x0001U);
    std::filesystem::remove(state_path);
}

}  // namespace

int main() {
    test_cartridge_detection();
    test_arm_alu_and_unaligned_loads();
    test_interworking_and_thumb_execution();
    test_bios_swi_hle();
    test_dma_timers_and_saves();
    std::printf("All GBA CPU/Bus unit tests passed.\n");
    return 0;
}
