#include <cassert>
#include <cstdio>
#include <vector>

#include "md/bus.h"
#include "md/interpreter.h"

using namespace dragon::md;

namespace {

void write_be16(Bus& bus, uint32_t addr, uint16_t v) {
    bus.write16(addr, v);
}

void write_be32(Bus& bus, uint32_t addr, uint32_t v) {
    bus.write32(addr, v);
}

void test_m68k_move_add_sub_flags() {
    std::printf("[Test] M68K MOVEQ / ADD / SUB / CMP / flags...\n");
    Bus bus;
    bus.set_flat_ram_mode(true);
    MdCpuState cpu{};
    cpu.bus = &bus;
    cpu.sr = 0x2700U;
    cpu.a[7] = 0x1000U;
    cpu.pc = 0x0200U;

    // MOVEQ #$10, D0 (0x7010)
    // MOVEQ #$20, D1 (0x7220)
    // ADD.L D1, D0   (0xD081)
    // SUBI.L #$30,D0 (0x0480, 0x0000, 0x0030)
    write_be16(bus, 0x0200U, 0x7010U);
    write_be16(bus, 0x0202U, 0x7220U);
    write_be16(bus, 0x0204U, 0xD081U);
    write_be16(bus, 0x0206U, 0x0480U);
    write_be32(bus, 0x0208U, 0x00000030U);

    step_instruction(cpu, bus);
    assert(cpu.d[0] == 0x10U);
    step_instruction(cpu, bus);
    assert(cpu.d[1] == 0x20U);
    step_instruction(cpu, bus);
    assert(cpu.d[0] == 0x30U);
    assert((cpu.sr & MD_FLAG_Z) == 0U);
    step_instruction(cpu, bus);
    assert(cpu.d[0] == 0x00U);
    assert((cpu.sr & MD_FLAG_Z) != 0U);
    std::printf("  passed!\n");
}

void test_m68k_mul_div_shifts_dbcc() {
    std::printf("[Test] M68K MULU / DIVU / LSL / DBRA loop...\n");
    Bus bus;
    bus.set_flat_ram_mode(true);
    MdCpuState cpu{};
    cpu.bus = &bus;
    cpu.sr = 0x2700U;
    cpu.a[7] = 0x1000U;
    cpu.pc = 0x0300U;

    // D0 = 7, D1 = 6; MULU D1, D0 -> D0 = 42
    // DIVU #5, D0 -> Q=8, R=2 -> D0 = 0x00020008
    // DBRA loop: sum 1..5 in D2 using D3=4
    cpu.d[0] = 7U;
    cpu.d[1] = 6U;
    cpu.d[2] = 0U;
    cpu.d[3] = 4U;

    write_be16(bus, 0x0300U, 0xC0C1U);  // MULU.W D1, D0
    write_be16(bus, 0x0302U, 0x80FCU);  // DIVU.W #5, D0
    write_be16(bus, 0x0304U, 0x0005U);
    write_be16(bus, 0x0306U, 0x5282U);  // ADDQ.L #1, D2
    write_be16(bus, 0x0308U, 0x51CBU);  // DBRA D3, -4 (back to 0x0306)
    write_be16(bus, 0x030AU, 0xFFFCU);

    step_instruction(cpu, bus);
    assert(cpu.d[0] == 42U);
    step_instruction(cpu, bus);
    assert((cpu.d[0] & 0xFFFFU) == 8U);
    assert((cpu.d[0] >> 16U) == 2U);

    for (int i = 0; i < 10; ++i) {
        step_instruction(cpu, bus);
    }
    assert(cpu.d[2] == 5U);
    assert(cpu.pc == 0x030CU);
    std::printf("  passed!\n");
}

void test_m68k_tas_genesis_quirk() {
    std::printf("[Test] M68K TAS Genesis memory write-inhibit quirk...\n");
    Bus bus;
    bus.set_flat_ram_mode(true);
    MdCpuState cpu{};
    cpu.bus = &bus;
    cpu.sr = 0x2700U;
    cpu.a[0] = 0x0500U;
    cpu.d[0] = 0x00U;
    bus.write8(0x0500U, 0x00U);
    cpu.pc = 0x0400U;

    // TAS D0 (0x4AC0) -> sets bit 7 of D0
    // TAS (A0) (0x4AD0) -> sets Z flag, does NOT write bit 7 to memory on Genesis!
    write_be16(bus, 0x0400U, 0x4AC0U);
    write_be16(bus, 0x0402U, 0x4AD0U);

    step_instruction(cpu, bus);
    assert((cpu.d[0] & 0xFFU) == 0x80U);
    assert((cpu.sr & MD_FLAG_Z) != 0U);

    step_instruction(cpu, bus);
    assert((cpu.sr & MD_FLAG_Z) != 0U);
    assert(bus.read8(0x0500U) == 0x00U);  // Real Genesis hardware quirk!
    std::printf("  passed!\n");
}

void test_m68k_bsr_rts_movem_and_trap() {
    std::printf("[Test] M68K BSR / RTS / MOVEM / TRAP / RTE...\n");
    Bus bus;
    bus.set_flat_ram_mode(true);
    MdCpuState cpu{};
    cpu.bus = &bus;
    cpu.sr = 0x2700U;
    cpu.ssp = 0x2000U;
    cpu.a[7] = 0x2000U;
    cpu.d[0] = 0x11223344U;
    cpu.d[1] = 0x55667788U;

    // Vector TRAP #0 ($80) -> $0600
    write_be32(bus, 0x0080U, 0x00000600U);
    // $0600: MOVEQ #$7F, D7; RTE
    write_be16(bus, 0x0600U, 0x7E7FU);
    write_be16(bus, 0x0602U, 0x4E73U);

    // $0500: MOVEM.L D0-D1,-(A7) (0x48E7, 0xC000)
    // $0504: BSR.S +6 ($050C)     (0x6106)
    // $0506: MOVEM.L (A7)+,D2-D3  (0x4CDF, 0x000C)
    // $050A: TRAP #0              (0x4E40)
    // $050C: MOVEQ #$42, D4; RTS
    cpu.pc = 0x0500U;
    write_be16(bus, 0x0500U, 0x48E7U);
    write_be16(bus, 0x0502U, 0xC000U);
    write_be16(bus, 0x0504U, 0x6106U);
    write_be16(bus, 0x0506U, 0x4CDFU);
    write_be16(bus, 0x0508U, 0x000CU);
    write_be16(bus, 0x050AU, 0x4E40U);
    write_be16(bus, 0x050CU, 0x7842U);
    write_be16(bus, 0x050EU, 0x4E75U);

    step_instruction(cpu, bus);  // MOVEM push
    assert(cpu.a[7] == 0x1FF8U);
    step_instruction(cpu, bus);  // BSR
    assert(cpu.pc == 0x050CU);
    step_instruction(cpu, bus);  // MOVEQ #$42, D4
    assert(cpu.d[4] == 0x42U);
    step_instruction(cpu, bus);  // RTS
    assert(cpu.pc == 0x0506U);
    step_instruction(cpu, bus);  // MOVEM pop
    assert(cpu.d[2] == 0x11223344U && cpu.d[3] == 0x55667788U);
    assert(cpu.a[7] == 0x2000U);
    step_instruction(cpu, bus);  // TRAP #0
    assert(cpu.pc == 0x0600U);
    step_instruction(cpu, bus);  // MOVEQ #$7F, D7
    step_instruction(cpu, bus);  // RTE
    assert(cpu.d[7] == 0x7FU);
    assert(cpu.pc == 0x050CU);
    std::printf("  passed!\n");
}

void test_z80_busreq_and_bank() {
    std::printf("[Test] Genesis Z80 BUSREQ / RESET / 9-bit bank register...\n");
    Bus bus;
    std::vector<uint8_t> rom(65536, 0);
    // Initial SSP/PC
    rom[0] = 0x00;
    rom[1] = 0xFF;
    rom[2] = 0xFE;
    rom[3] = 0x00;
    rom[4] = 0x00;
    rom[5] = 0x00;
    rom[6] = 0x02;
    rom[7] = 0x00;
    assert(bus.load_rom_bytes(rom));
    MdCpuState cpu{};
    bus.reset(cpu);

    // Z80 starts in reset -> BUSACK is granted (0)
    assert(bus.z80().is_bus_granted());
    // Request bus, release reset, write simple Z80 program to Sound RAM: LD A, 0x5A; LD (0x0100),
    // A; HALT
    bus.write16(0xA11100U, 0x0100U);
    bus.write16(0xA11200U, 0x0100U);
    bus.write8(0xA00000U, 0x3EU);
    bus.write8(0xA00001U, 0x5AU);
    bus.write8(0xA00002U, 0x32U);
    bus.write8(0xA00003U, 0x00U);
    bus.write8(0xA00004U, 0x01U);
    bus.write8(0xA00005U, 0x76U);
    // Release BUSREQ -> Z80 executes!
    bus.write16(0xA11100U, 0x0000U);
    assert(bus.z80().is_running());
    bus.z80().step(100U, &bus);
    assert(bus.z80().read_ram(0x0100U) == 0x5AU);
    std::printf("  passed!\n");
}

void test_controller_3btn_and_6btn_protocol() {
    std::printf("Running test_controller_3btn_and_6btn_protocol...\n");
    Bus bus;
    MdCpuState cpu{};
    bus.reset(cpu);

    // Before ctrl is configured as output, TH is pulled HIGH (returns Up/Down/Left/Right/B/C)
    bus.set_controller_state(0, kBtnRight | kBtnB);
    uint8_t v = bus.read8(0xA10003U);
    assert((v & 0x40U) == 0x40U);  // TH high
    assert((v & 0x08U) == 0x00U);  // Right pressed (active low)
    assert((v & 0x04U) == 0x04U);  // Left released
    assert((v & 0x10U) == 0x00U);  // B pressed
    assert((v & 0x20U) == 0x20U);  // C released

    // Configure TH as output (ctrl = 0x40) and set TH = 1
    bus.write8(0xA10009U, 0x40U);
    bus.write8(0xA10003U, 0x40U);

    // Full 6-button polling sequence with Up + Left + A + X + Mode pressed
    bus.set_controller_state(0, kBtnUp | kBtnLeft | kBtnA | kBtnX | kBtnMode);

    // Phase 1 (1st TH=0): 0 S A 0 0 D U
    bus.write8(0xA10003U, 0x00U);
    v = bus.read8(0xA10003U);
    assert((v & 0x7FU) == 0x22U);  // TH=0, Start=1(0x20), A=0, 0, 0, D=1(0x02), U=0

    // Phase 2 (1st TH=1): 1 C B R L D U
    bus.write8(0xA10003U, 0x40U);
    v = bus.read8(0xA10003U);
    assert((v & 0x7FU) == 0x7AU);  // TH=1, C=1, B=1, R=1, L=0, D=1, U=0

    // Phase 3 (2nd TH=0): 0 S A 0 0 D U
    bus.write8(0xA10003U, 0x00U);
    v = bus.read8(0xA10003U);
    assert((v & 0x7FU) == 0x22U);

    // Phase 4 (2nd TH=1): 1 C B R L D U
    bus.write8(0xA10003U, 0x40U);
    v = bus.read8(0xA10003U);
    assert((v & 0x7FU) == 0x7AU);

    // Phase 5 (3rd TH=0): 6-button ID (bits 0..3 = 0)
    bus.write8(0xA10003U, 0x00U);
    v = bus.read8(0xA10003U);
    assert((v & 0x0FU) == 0x00U);
    assert((v & 0x30U) == 0x20U);  // Start=1, A=0

    // Phase 6 (3rd TH=1): 1 C B Mode X Y Z (X and Mode pressed -> bits 2 & 3 = 0)
    bus.write8(0xA10003U, 0x40U);
    v = bus.read8(0xA10003U);
    assert((v & 0x7FU) == 0x73U);  // TH=1, C=1, B=1, Mode=0, X=0, Y=1, Z=1

    // Phase 7 (4th TH=0): bits 0..3 = 1
    bus.write8(0xA10003U, 0x00U);
    v = bus.read8(0xA10003U);
    assert((v & 0x0FU) == 0x0FU);

    // Rising edge after 4th TH=0 wraps back to normal TH=1
    bus.write8(0xA10003U, 0x40U);
    v = bus.read8(0xA10003U);
    assert((v & 0x7FU) == 0x7AU);

    // Verify timeout resets th_step back to 0 mid-sequence
    bus.write8(0xA10003U, 0x00U);  // step 1
    bus.write8(0xA10003U, 0x40U);
    bus.write8(0xA10003U, 0x00U);  // step 2
    bus.write8(0xA10003U, 0x40U);
    bus.sync_hardware(20000U);     // > 12000 cycles -> resets step to 0
    bus.write8(0xA10003U, 0x00U);  // step 1 (NOT step 3!)
    v = bus.read8(0xA10003U);
    assert((v & 0x7FU) == 0x22U);  // Down=1, Up=0 (not 6-button 0000 ID)

    std::printf("  passed!\n");
}

}  // namespace

int main() {
    test_m68k_move_add_sub_flags();
    test_m68k_mul_div_shifts_dbcc();
    test_m68k_tas_genesis_quirk();
    test_m68k_bsr_rts_movem_and_trap();
    test_z80_busreq_and_bank();
    test_controller_3btn_and_6btn_protocol();
    std::printf("All MD M68K/Z80 CPU unit tests passed!\n");
    return 0;
}
