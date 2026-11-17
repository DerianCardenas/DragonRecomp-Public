#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "nes/frontend.h"
#include "nes/runtime.h"

namespace {

using dragon::nes::Bus;
using dragon::nes::cpu_init;
using dragon::nes::cpu_irq;
using dragon::nes::cpu_nmi;
using dragon::nes::cpu_step;

struct TestHarness {
    Bus bus;
    CpuState cpu{};

    TestHarness() {
        bus.enable_flat_ram(true);
        cpu_init(cpu, &bus);
        cpu.pc = 0x8000U;
        cpu.cycles = 0ULL;
    }

    void load(uint16_t addr, std::initializer_list<uint8_t> bytes) {
        std::vector<uint8_t> vec(bytes);
        bus.load_direct(addr, vec);
    }
};

void test_load_store_and_flags() {
    TestHarness h;
    // LDA #$00 -> Z=1, N=0 (2 cyc)
    // LDA #$80 -> Z=0, N=1 (2 cyc)
    // TAX      -> X=$80    (2 cyc)
    // STX $10  -> [$10]=$80 (3 cyc)
    // LDY $10  -> Y=$80    (3 cyc)
    h.load(0x8000, {0xA9, 0x00, 0xA9, 0x80, 0xAA, 0x86, 0x10, 0xA4, 0x10});

    assert(cpu_step(h.cpu) == 2);
    assert(h.cpu.a == 0x00 && h.cpu.z == 0 && (h.cpu.n & 0x80) == 0);

    assert(cpu_step(h.cpu) == 2);
    assert(h.cpu.a == 0x80 && h.cpu.z != 0 && (h.cpu.n & 0x80) != 0);

    assert(cpu_step(h.cpu) == 2);
    assert(h.cpu.x == 0x80);

    assert(cpu_step(h.cpu) == 3);
    assert(h.bus.read(0x0010) == 0x80);

    assert(cpu_step(h.cpu) == 3);
    assert(h.cpu.y == 0x80);
    assert(h.cpu.cycles == 12);
}

void test_adc_sbc_overflow_and_decimal_ignored() {
    TestHarness h;
    // SED (decimal flag set, must be ignored for arithmetic on 2A03)
    // CLC; LDA #$50; ADC #$50 -> A=$A0, V=1, C=0, N=1, Z=0
    // CLC; LDA #$D0; ADC #$90 -> A=$60, V=1, C=1, N=0, Z=0
    // SEC; LDA #$50; SBC #$B0 -> A=$A0, V=1, C=0, N=1, Z=0
    h.load(0x8000, {
                       0xF8,  // SED
                       0x18,  // CLC
                       0xA9,
                       0x50,  // LDA #$50
                       0x69,
                       0x50,  // ADC #$50
                       0x18,  // CLC
                       0xA9,
                       0xD0,  // LDA #$D0
                       0x69,
                       0x90,  // ADC #$90
                       0x38,  // SEC
                       0xA9,
                       0x50,  // LDA #$50
                       0xE9,
                       0xB0,  // SBC #$B0
                   });

    cpu_step(h.cpu);  // SED
    assert(h.cpu.d == 1);
    cpu_step(h.cpu);  // CLC
    cpu_step(h.cpu);  // LDA #$50
    cpu_step(h.cpu);  // ADC #$50
    assert(h.cpu.a == 0xA0);
    assert(h.cpu.v == 1);
    assert(h.cpu.c == 0);
    assert((h.cpu.n & 0x80) != 0);
    assert(h.cpu.z != 0);

    cpu_step(h.cpu);  // CLC
    cpu_step(h.cpu);  // LDA #$D0
    cpu_step(h.cpu);  // ADC #$90
    assert(h.cpu.a == 0x60);
    assert(h.cpu.v == 1);
    assert(h.cpu.c == 1);
    assert((h.cpu.n & 0x80) == 0);

    cpu_step(h.cpu);  // SEC
    cpu_step(h.cpu);  // LDA #$50
    cpu_step(h.cpu);  // SBC #$B0
    assert(h.cpu.a == 0xA0);
    assert(h.cpu.v == 1);
    assert(h.cpu.c == 0);
    assert((h.cpu.n & 0x80) != 0);
}

void test_bit_instruction() {
    TestHarness h;
    // [$0020] = $C0 (bits 7 and 6 set)
    // LDA #$0F -> A & [$20] == 0 -> Z=1, N=1, V=1!
    h.bus.write(0x0020, 0xC0);
    h.load(0x8000, {0xA9, 0x0F, 0x24, 0x20});
    cpu_step(h.cpu);
    assert(cpu_step(h.cpu) == 3);
    assert((h.cpu.n & 0x80) != 0);
    assert(h.cpu.v == 1);
    assert(h.cpu.z == 0);  // Z flag is set!
    assert((cpu_pack_flags(&h.cpu, 0) & 0xC2U) == 0xC2U);
}

void test_page_crossing_cycles() {
    TestHarness h;
    h.bus.write(0x10FF, 0x42);
    h.bus.write(0x1100, 0x99);
    // LDX #$00; LDA $10FF,X (4 cycles, no cross)
    // LDX #$01; LDA $10FF,X (5 cycles, page cross)
    // STA $10FF,X           (always 5 cycles)
    h.load(0x8000, {
                       0xA2,
                       0x00,  // LDX #$00
                       0xBD,
                       0xFF,
                       0x10,  // LDA $10FF,X
                       0xA2,
                       0x01,  // LDX #$01
                       0xBD,
                       0xFF,
                       0x10,  // LDA $10FF,X
                       0x9D,
                       0xFF,
                       0x10,  // STA $10FF,X
                   });
    cpu_step(h.cpu);
    assert(cpu_step(h.cpu) == 4);
    assert(h.cpu.a == 0x42);

    cpu_step(h.cpu);
    assert(cpu_step(h.cpu) == 5);
    assert(h.cpu.a == 0x99);

    assert(cpu_step(h.cpu) == 5);
}

void test_branch_timing() {
    TestHarness h;
    // At $80F0:
    // SEC
    // BCC +2 (not taken -> 2 cycles)
    // BCS +2 (taken, same page -> 3 cycles, lands at $80F7)
    // At $80F7:
    // BCS +10 (taken, crosses to $8103 -> 4 cycles)
    h.cpu.pc = 0x80F0;
    h.load(0x80F0, {
                       0x38,  // SEC ($80F0)
                       0x90,
                       0x02,  // BCC +2 ($80F1)
                       0xB0,
                       0x02,  // BCS +2 ($80F3 -> $80F7)
                       0xEA,
                       0xEA,  // NOP, NOP ($80F5..$80F6)
                       0xB0,
                       0x0A,  // BCS +10 ($80F7 -> $8103)
                   });
    cpu_step(h.cpu);               // SEC
    assert(cpu_step(h.cpu) == 2);  // BCC not taken
    assert(cpu_step(h.cpu) == 3);  // BCS same page
    assert(h.cpu.pc == 0x80F7);
    assert(cpu_step(h.cpu) == 4);  // BCS page cross
    assert(h.cpu.pc == 0x8103);
}

void test_stack_and_jmp_indirect_bug() {
    TestHarness h;
    // Test PHA/PLA, PHP/PLP, JSR/RTS, and JMP ($10FF) page-wrap bug.
    h.bus.write(0x10FF, 0x34);
    h.bus.write(0x1000, 0x92);  // wrapped high byte
    h.bus.write(0x1100, 0xFF);  // wrong high byte if not wrapped

    h.load(0x8000, {
                       0x20,
                       0x10,
                       0x80,  // JSR $8010 (6 cyc)
                       0x6C,
                       0xFF,
                       0x10,  // JMP ($10FF) (5 cyc -> $9234)
                   });
    h.load(0x8010, {
                       0xA9,
                       0x77,  // LDA #$77
                       0x48,  // PHA
                       0xA9,
                       0x00,  // LDA #$00
                       0x68,  // PLA
                       0x60,  // RTS
                   });

    assert(cpu_step(h.cpu) == 6);  // JSR $8010
    assert(h.cpu.pc == 0x8010);
    cpu_step(h.cpu);               // LDA #$77
    assert(cpu_step(h.cpu) == 3);  // PHA
    cpu_step(h.cpu);               // LDA #$00
    assert(cpu_step(h.cpu) == 4);  // PLA
    assert(h.cpu.a == 0x77);
    assert(cpu_step(h.cpu) == 6);  // RTS
    assert(h.cpu.pc == 0x8003);

    assert(cpu_step(h.cpu) == 5);  // JMP ($10FF)
    assert(h.cpu.pc == 0x9234);
}

void test_interrupts_nmi_irq_rti() {
    TestHarness h;
    h.bus.write(0xFFFA, 0x00);
    h.bus.write(0xFFFB, 0x90);  // NMI -> $9000
    h.bus.write(0xFFFE, 0x00);
    h.bus.write(0xFFFF, 0xA0);  // IRQ -> $A000
    h.load(0x9000, {0x40});     // RTI
    h.load(0xA000, {0x40});     // RTI

    h.cpu.pc = 0x8000;
    h.cpu.i = 1;  // IRQ masked
    assert(!cpu_irq(h.cpu));

    h.cpu.i = 0;  // IRQ enabled
    assert(cpu_irq(h.cpu));
    assert(h.cpu.pc == 0xA000);
    assert(h.cpu.i == 1);
    assert(cpu_step(h.cpu) == 6);  // RTI
    assert(h.cpu.pc == 0x8000);
    assert(h.cpu.i == 0);

    cpu_nmi(h.cpu);
    assert(h.cpu.pc == 0x9000);
    assert(cpu_step(h.cpu) == 6);  // RTI
    assert(h.cpu.pc == 0x8000);
}

void test_battery_ram_persistence() {
    dragon::nes::Bus bus;
    dragon::nes::Cartridge cart;
    cart.has_battery = true;
    bus.load_cartridge(cart);
    assert(bus.has_battery());

    // Write some data to PRG RAM ($6000-$7FFF)
    bus.write(0x6000, 0x55);
    bus.write(0x6100, 0xAA);
    bus.write(0x7FFF, 0x33);

    const std::filesystem::path sav_path =
        std::filesystem::temp_directory_path() / "dragon_nes_test.sav";
    std::filesystem::remove(sav_path);

    assert(bus.save_battery_ram(sav_path));

    // Clear PRG RAM
    bus.write(0x6000, 0x00);
    bus.write(0x6100, 0x00);
    bus.write(0x7FFF, 0x00);
    assert(bus.read(0x6000) == 0x00);

    // Load back
    assert(bus.load_battery_ram(sav_path));
    assert(bus.read(0x6000) == 0x55);
    assert(bus.read(0x6100) == 0xAA);
    assert(bus.read(0x7FFF) == 0x33);

    std::filesystem::remove(sav_path);
}

void test_nes_save_state() {
    dragon::nes::Bus bus;
    CpuState cpu{};
    dragon::nes::cpu_init(cpu, &bus);

    cpu.a = 0x42;
    cpu.x = 0x13;
    cpu.y = 0x37;
    cpu.s = 0xFD;
    cpu.pc = 0x8123;
    cpu.c = 1;
    cpu.v = 0;
    cpu.cycles = 12345;
    bus.write(0x0200, 0xAB);
    bus.write(0x6000, 0xCD);

    const std::filesystem::path state_path =
        std::filesystem::temp_directory_path() / "dragon_nes_test.state1";
    std::filesystem::remove(state_path);

    assert(bus.save_state(cpu, state_path));

    // Scramble state
    cpu.a = 0;
    cpu.x = 0;
    cpu.y = 0;
    cpu.s = 0xFF;
    cpu.pc = 0x8000;
    cpu.cycles = 0;
    bus.write(0x0200, 0);
    bus.write(0x6000, 0);

    // Load state
    assert(bus.load_state(cpu, state_path));

    assert(cpu.a == 0x42);
    assert(cpu.x == 0x13);
    assert(cpu.y == 0x37);
    assert(cpu.s == 0xFD);
    assert(cpu.pc == 0x8123);
    assert(cpu.c == 1);
    assert(cpu.cycles == 12345);
    assert(bus.read(0x0200) == 0xAB);
    assert(bus.read(0x6000) == 0xCD);

    std::filesystem::remove(state_path);
}

void test_nes_cli_args() {
    dragon::nes::RunnerOptions opts;
    int exit_code = 0;
    std::array<char*, 9> argv = {const_cast<char*>("nes_runner"),
                                 const_cast<char*>("--state-slot"),
                                 const_cast<char*>("3"),
                                 const_cast<char*>("--load-state"),
                                 const_cast<char*>("/tmp/save.state3"),
                                 const_cast<char*>("--no-aspect"),
                                 const_cast<char*>("--crt"),
                                 const_cast<char*>("--rom"),
                                 const_cast<char*>("game.nes")};
    bool ok =
        dragon::nes::parse_runner_args(static_cast<int>(argv.size()), argv.data(), opts, exit_code);
    assert(ok);
    assert(opts.state_slot == 3);
    assert(opts.load_state_path == "/tmp/save.state3");
    assert(opts.aspect_ratio == false);
    assert(opts.crt_filter == true);
    assert(opts.rom_path == "game.nes");
}

}  // namespace

int main() {
    test_load_store_and_flags();
    test_adc_sbc_overflow_and_decimal_ignored();
    test_bit_instruction();
    test_page_crossing_cycles();
    test_branch_timing();
    test_stack_and_jmp_indirect_bug();
    test_interrupts_nmi_irq_rti();
    test_battery_ram_persistence();
    test_nes_save_state();
    test_nes_cli_args();
    std::printf("nes_cpu_unit_tests: all tests passed\n");
    return 0;
}
