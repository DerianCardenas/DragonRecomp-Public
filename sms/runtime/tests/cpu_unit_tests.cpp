#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#include "sms/bus.h"
#include "sms/interpreter.h"

using namespace dragon::sms;

namespace {

void load_prog(Bus& bus, SmsCpuState& cpu, const std::vector<uint8_t>& prog,
               SystemType sys = SystemType::MasterSystem) {
    std::vector<uint8_t> rom(32768, 0x00U);
    std::memcpy(rom.data(), prog.data(), prog.size());
    assert(bus.cartridge().load_from_bytes(std::move(rom), sys));
    bus.reset(cpu);
}

void test_alu_and_flags() {
    std::cout << "[Test] Z80 ALU, flags, DAA, EX/EXX...\n";
    Bus bus;
    SmsCpuState cpu{};
    // LD A, 0x7F; ADD A, 1 (S=1, PV=1, H=1, C=0); SUB 0x80 (Z=1, N=1);
    // LD A, 0x15; ADD A, 0x27; DAA -> 0x42;
    // EX AF, AF'; LD A, 0xAA; EX AF, AF' -> A == 0x42
    load_prog(bus, cpu, {
        0x3E, 0x7F,        // LD A, 0x7F
        0xC6, 0x01,        // ADD A, 1 -> 0x80 (S, PV, H)
        0xD6, 0x80,        // SUB 0x80 -> 0x00 (Z, N)
        0x3E, 0x15,        // LD A, 0x15
        0xC6, 0x27,        // ADD A, 0x27 -> 0x3C
        0x27,              // DAA -> 0x42
        0x08,              // EX AF, AF'
        0x3E, 0xAA,        // LD A, 0xAA
        0x08,              // EX AF, AF'
    });

    step_instruction(cpu, bus);
    assert(cpu.a == 0x7FU);

    step_instruction(cpu, bus);
    assert(cpu.a == 0x80U);
    assert((cpu.f & SMS_FLAG_S) != 0U);
    assert((cpu.f & SMS_FLAG_PV) != 0U);
    assert((cpu.f & SMS_FLAG_H) != 0U);
    assert((cpu.f & SMS_FLAG_C) == 0U);

    step_instruction(cpu, bus);
    assert(cpu.a == 0x00U);
    assert((cpu.f & SMS_FLAG_Z) != 0U);
    assert((cpu.f & SMS_FLAG_N) != 0U);

    step_instruction(cpu, bus);
    step_instruction(cpu, bus);
    step_instruction(cpu, bus);
    assert(cpu.a == 0x42U);

    step_instruction(cpu, bus);
    step_instruction(cpu, bus);
    assert(cpu.a == 0xAAU);
    step_instruction(cpu, bus);
    assert(cpu.a == 0x42U);
    assert(cpu.a2 == 0xAAU);
}

void test_cb_ed_index_ops() {
    std::cout << "[Test] Z80 CB, ED (LDIR/CPIR/ADC/SBC/NEG), IX/IY & DDCB...\n";
    Bus bus;
    SmsCpuState cpu{};
    // Store 4 bytes at $C000 via IX+d, copy to $C010 via LDIR, set bit 3 at IX+0 via DDCB
    load_prog(bus, cpu, {
        0xDD, 0x21, 0x00, 0xC0,  // LD IX, $C000
        0xDD, 0x36, 0x00, 0x10,  // LD (IX+0), 0x10
        0xDD, 0x36, 0x01, 0x20,  // LD (IX+1), 0x20
        0xDD, 0xCB, 0x00, 0xDE,  // SET 3, (IX+0) -> 0x18
        0x21, 0x00, 0xC0,        // LD HL, $C000
        0x11, 0x10, 0xC0,        // LD DE, $C010
        0x01, 0x02, 0x00,        // LD BC, 2
        0xED, 0xB0,              // LDIR (2 iterations)
        0x3E, 0x05,              // LD A, 5
        0xED, 0x44,              // NEG -> 0xFB
        0xCB, 0x37,              // SLL A (undoc) -> (0xFB<<1)|1 = 0xF7, C=1
    });

    for (int i = 0; i < 12; ++i) {
        step_instruction(cpu, bus);
    }
    assert(bus.read(0xC000U) == 0x18U);
    assert(bus.read(0xC010U) == 0x18U);
    assert(bus.read(0xC011U) == 0x20U);
    assert(sms_bc(&cpu) == 0U);
    assert(cpu.a == 0xF7U);
    assert((cpu.f & SMS_FLAG_C) != 0U);
}

void test_interrupts_ei_delay_and_nmi() {
    std::cout << "[Test] Z80 EI 1-instruction delay, IM1 IRQ ($0038), NMI ($0066)...\n";
    Bus bus;
    SmsCpuState cpu{};
    std::vector<uint8_t> rom(32768, 0x00U);
    // $0000: EI; LD A, 0x55; NOP; NOP
    rom[0x0000] = 0xFB;  // EI
    rom[0x0001] = 0x3E;  // LD A, 0x55
    rom[0x0002] = 0x55;
    rom[0x0003] = 0x00;  // NOP
    // $0038 (IRQ handler): LD B, 0x99; IN A, ($BF); RETI
    rom[0x0038] = 0x06; rom[0x0039] = 0x99;
    rom[0x003A] = 0xDB; rom[0x003B] = 0xBF;
    rom[0x003C] = 0xED; rom[0x003D] = 0x4D;
    // $0066 (NMI handler): LD C, 0x77; RETN
    rom[0x0066] = 0x0E; rom[0x0067] = 0x77;
    rom[0x0068] = 0xED; rom[0x0069] = 0x45;

    assert(bus.cartridge().load_from_bytes(std::move(rom)));
    bus.reset(cpu);

    // Force VDP Frame INT pending & enabled
    bus.vdp().write_control(0xE0U);  // Reg 1 = Display ON + IE0 ON
    bus.vdp().write_control(0x81U);
    // Step VDP to VBlank to assert Frame IRQ
    bus.vdp().step(Vdp::kCyclesPerScanline * 193U);
    assert(bus.vdp().irq_asserted());

    // Execute EI -> iff1 MUST still be 0 until after LD A, 0x55!
    step_instruction(cpu, bus);
    assert(cpu.iff1 == 0U && cpu.ei_delay == 1U);

    // Next step executes LD A, 0x55 (NOT IRQ yet!) then enables iff1
    step_instruction(cpu, bus);
    assert(cpu.a == 0x55U);
    assert(cpu.pc == 0x0003U);
    assert(cpu.iff1 == 1U);

    // Next step services IM1 IRQ at $0038!
    step_instruction(cpu, bus);
    assert(cpu.pc == 0x0038U);
    assert(cpu.iff1 == 0U);

    step_instruction(cpu, bus);  // LD B, 0x99
    step_instruction(cpu, bus);  // IN A, ($BF) clears IRQ
    step_instruction(cpu, bus);  // RETI -> returns to $0003
    assert(cpu.b == 0x99U);
    assert(!bus.vdp().irq_asserted());
    assert(cpu.pc == 0x0003U);

    // Trigger SMS Pause button -> NMI at $0066
    InputState in{};
    in.pause_or_start = true;
    bus.set_input(in, cpu);
    step_instruction(cpu, bus);
    assert(cpu.pc == 0x0066U);
    step_instruction(cpu, bus);  // LD C, 0x77
    step_instruction(cpu, bus);  // RETN
    assert(cpu.c == 0x77U);
    assert(cpu.pc == 0x0003U);
}

void test_sega_and_codemasters_mappers_and_savestate() {
    std::cout << "[Test] Sega Mapper ($FFFC-$FFFF), Codemasters Mapper, & Save State...\n";
    // 64 KiB ROM = 4 banks of 16 KiB, each tagged with bank ID at offset 0x0500 and 0x0010
    std::vector<uint8_t> rom(65536, 0x00U);
    for (int b = 0; b < 4; ++b) {
        rom[static_cast<size_t>(b) * 16384U + 0x0010U] = static_cast<uint8_t>(0xA0 + b);
        rom[static_cast<size_t>(b) * 16384U + 0x0500U] = static_cast<uint8_t>(0xB0 + b);
    }
    std::memcpy(rom.data() + 0x7FF0, "TMR SEGA", 8);

    Bus bus;
    SmsCpuState cpu{};
    assert(bus.cartridge().load_from_bytes(rom));
    bus.reset(cpu);

    // Switch slot 0 to bank 3: $0010 MUST stay fixed to bank 0 (0xA0), $0500 switches to bank 3 (0xB3)
    bus.write(0xFFFDU, 3U);
    assert(bus.read(0x0010U) == 0xA0U);
    assert(bus.read(0x0500U) == 0xB3U);

    // Switch slot 2 to Cart SRAM ($FFFC bit 3)
    bus.write(0xFFFCU, 0x08U);
    bus.write(0x8123U, 0x42U);
    assert(bus.read(0x8123U) == 0x42U);

    // Save state round-trip
    cpu.a = 0xDE;
    cpu.ix = 0xCAFE;
    const auto st_path = std::filesystem::temp_directory_path() / "dragon_sms_test.state";
    assert(bus.save_state(st_path, cpu));

    bus.reset(cpu);
    assert(cpu.a == 0U && bus.read(0x8123U) != 0x42U);
    assert(bus.load_state(st_path, cpu));
    assert(cpu.a == 0xDEU);
    assert(cpu.ix == 0xCAFEU);
    assert(bus.read(0x8123U) == 0x42U);
    std::filesystem::remove(st_path);

    // Codemasters mapper (no fixed 1 KiB)
    Bus cm_bus;
    SmsCpuState cm_cpu{};
    assert(cm_bus.cartridge().load_from_bytes(rom, SystemType::MasterSystem,
                                              MapperType::Codemasters, true));
    cm_bus.reset(cm_cpu);
    cm_bus.write(0x0000U, 3U);
    assert(cm_bus.read(0x0010U) == 0xA3U);
    assert(cm_bus.read(0x0500U) == 0xB3U);
}

}  // namespace

int main() {
    test_alu_and_flags();
    test_cb_ed_index_ops();
    test_interrupts_ei_delay_and_nmi();
    test_sega_and_codemasters_mappers_and_savestate();
    std::cout << "All SMS Z80 CPU unit tests passed!\n";
    return 0;
}
