#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "gb/frontend.h"
#include "gb/runtime.h"

namespace {

using dragon::gb::Bus;
using dragon::gb::cpu_init;
using dragon::gb::cpu_step;

struct TestHarness {
    Bus bus;
    CpuState cpu{};

    TestHarness() {
        bus.enable_flat_ram(true);
        cpu_init(cpu, &bus);
        cpu.pc = 0x8000U;
        cpu.sp = 0xFFFEU;
        cpu.cycles = 0ULL;
    }

    void load(uint16_t addr, std::initializer_list<uint8_t> bytes) {
        std::vector<uint8_t> vec(bytes);
        bus.load_direct(addr, vec);
    }
};

void test_loads_and_register_pairs() {
    TestHarness h;
    // LD BC, $1234  (3 bytes, 12 cyc)
    // LD DE, $5678  (3 bytes, 12 cyc)
    // LD HL, $9ABC  (3 bytes, 12 cyc)
    // LD SP, HL     (1 byte,  8 cyc)
    // PUSH BC       (1 byte, 16 cyc)
    // POP AF        (1 byte, 12 cyc) -> F lower nibble must be 0!
    h.load(0x8000, {0x01, 0x34, 0x12, 0x11, 0x78, 0x56, 0x21, 0xBC, 0x9A, 0xF9, 0xC5, 0xF1});

    assert(cpu_step(h.cpu) == 12);
    assert(h.cpu.b == 0x12 && h.cpu.c == 0x34);
    assert(cpu_bc(&h.cpu) == 0x1234);

    assert(cpu_step(h.cpu) == 12);
    assert(h.cpu.d == 0x56 && h.cpu.e == 0x78);
    assert(cpu_de(&h.cpu) == 0x5678);

    assert(cpu_step(h.cpu) == 12);
    assert(h.cpu.h == 0x9A && h.cpu.l == 0xBC);
    assert(cpu_hl(&h.cpu) == 0x9ABC);

    assert(cpu_step(h.cpu) == 8);
    assert(h.cpu.sp == 0x9ABC);

    assert(cpu_step(h.cpu) == 16);
    assert(h.cpu.sp == 0x9ABA);

    assert(cpu_step(h.cpu) == 12);
    assert(h.cpu.a == 0x12);
    assert(h.cpu.f == 0x30);  // 0x34 masked with 0xF0
    assert(cpu_af(&h.cpu) == 0x1230);
    assert(h.cpu.sp == 0x9ABC);

    std::printf("test_loads_and_register_pairs: PASS\n");
}

void test_alu_8bit_arithmetic() {
    TestHarness h;
    // LD A, $0F
    // ADD A, $01 -> A=$10, Z=0, N=0, H=1, C=0
    // SUB $10    -> A=$00, Z=1, N=1, H=0, C=0
    // DEC A      -> A=$FF, Z=0, N=1, H=1, C=0 (C preserved!)
    h.load(0x8000, {0x3E, 0x0F, 0xC6, 0x01, 0xD6, 0x10, 0x3D});

    cpu_step(h.cpu);  // LD A, $0F
    assert(h.cpu.a == 0x0F);

    cpu_step(h.cpu);  // ADD A, $01
    assert(h.cpu.a == 0x10);
    assert((h.cpu.f & GB_FLAG_Z) == 0);
    assert((h.cpu.f & GB_FLAG_N) == 0);
    assert((h.cpu.f & GB_FLAG_H) != 0);
    assert((h.cpu.f & GB_FLAG_C) == 0);

    cpu_step(h.cpu);  // SUB $10
    assert(h.cpu.a == 0x00);
    assert((h.cpu.f & GB_FLAG_Z) != 0);
    assert((h.cpu.f & GB_FLAG_N) != 0);
    assert((h.cpu.f & GB_FLAG_H) == 0);
    assert((h.cpu.f & GB_FLAG_C) == 0);

    cpu_step(h.cpu);  // DEC A
    assert(h.cpu.a == 0xFF);
    assert((h.cpu.f & GB_FLAG_Z) == 0);
    assert((h.cpu.f & GB_FLAG_N) != 0);
    assert((h.cpu.f & GB_FLAG_H) != 0);
    assert((h.cpu.f & GB_FLAG_C) == 0);

    std::printf("test_alu_8bit_arithmetic: PASS\n");
}

void test_daa_decimal_adjust() {
    TestHarness h;
    // Test 1: $15 + $27 = $3C -> DAA -> $42
    h.load(0x8000, {0x3E, 0x15, 0xC6, 0x27, 0x27});
    cpu_step(h.cpu);  // LD A, $15
    cpu_step(h.cpu);  // ADD A, $27 -> A=$3C
    cpu_step(h.cpu);  // DAA
    assert(h.cpu.a == 0x42);
    assert((h.cpu.f & GB_FLAG_C) == 0);
    assert((h.cpu.f & GB_FLAG_Z) == 0);

    // Test 2: $95 + $15 = $AA -> DAA -> $10 with C=1
    h.cpu.pc = 0x8010;
    h.load(0x8010, {0x3E, 0x95, 0xC6, 0x15, 0x27});
    cpu_step(h.cpu);  // LD A, $95
    cpu_step(h.cpu);  // ADD A, $15 -> A=$AA
    cpu_step(h.cpu);  // DAA
    assert(h.cpu.a == 0x10);
    assert((h.cpu.f & GB_FLAG_C) != 0);
    assert((h.cpu.f & GB_FLAG_Z) == 0);

    // Test 3: Subtraction: $42 - $15 = $2D (with N=1, H=1) -> DAA -> $27
    h.cpu.pc = 0x8020;
    h.load(0x8020, {0x3E, 0x42, 0xD6, 0x15, 0x27});
    cpu_step(h.cpu);  // LD A, $42
    cpu_step(h.cpu);  // SUB $15
    cpu_step(h.cpu);  // DAA
    assert(h.cpu.a == 0x27);
    assert((h.cpu.f & GB_FLAG_C) == 0);
    assert((h.cpu.f & GB_FLAG_N) != 0);

    std::printf("test_daa_decimal_adjust: PASS\n");
}

void test_add_sp_and_ld_hl_sp() {
    TestHarness h;
    // SP starts at $FFFE
    // ADD SP, 2   -> SP = $0000, C=1, H=1, Z=0, N=0
    // LD HL, SP-1 -> HL = $FFFF, C=0, H=0, Z=0, N=0
    h.load(0x8000, {0xE8, 0x02, 0xF8, 0xFF});

    cpu_step(h.cpu);  // ADD SP, 2
    assert(h.cpu.sp == 0x0000);
    assert((h.cpu.f & GB_FLAG_C) != 0);
    assert((h.cpu.f & GB_FLAG_H) != 0);
    assert((h.cpu.f & GB_FLAG_Z) == 0);
    assert((h.cpu.f & GB_FLAG_N) == 0);

    cpu_step(h.cpu);  // LD HL, SP-1
    assert(cpu_hl(&h.cpu) == 0xFFFF);
    assert((h.cpu.f & GB_FLAG_Z) == 0);
    assert((h.cpu.f & GB_FLAG_N) == 0);

    std::printf("test_add_sp_and_ld_hl_sp: PASS\n");
}

void test_cb_bitwise_and_swap() {
    TestHarness h;
    // LD A, $F0
    // SWAP A       -> A=$0F, Z=0
    // BIT 3, A     -> Z=0, N=0, H=1
    // BIT 4, A     -> Z=1, N=0, H=1
    // RES 0, A     -> A=$0E
    // SET 7, A     -> A=$8E
    h.load(0x8000, {0x3E, 0xF0, 0xCB, 0x37, 0xCB, 0x5F, 0xCB, 0x67, 0xCB, 0x87, 0xCB, 0xFF});

    cpu_step(h.cpu);  // LD A, $F0
    cpu_step(h.cpu);  // SWAP A
    assert(h.cpu.a == 0x0F);
    assert((h.cpu.f & GB_FLAG_Z) == 0);

    cpu_step(h.cpu);  // BIT 3, A (bit 3 is 1)
    assert((h.cpu.f & GB_FLAG_Z) == 0);
    assert((h.cpu.f & GB_FLAG_N) == 0);
    assert((h.cpu.f & GB_FLAG_H) != 0);

    cpu_step(h.cpu);  // BIT 4, A (bit 4 is 0)
    assert((h.cpu.f & GB_FLAG_Z) != 0);
    assert((h.cpu.f & GB_FLAG_H) != 0);

    cpu_step(h.cpu);  // RES 0, A
    assert(h.cpu.a == 0x0E);

    cpu_step(h.cpu);  // SET 7, A
    assert(h.cpu.a == 0x8E);

    std::printf("test_cb_bitwise_and_swap: PASS\n");
}

void test_calls_and_returns() {
    TestHarness h;
    // 0x8000: CALL $8006 (3 bytes)
    // 0x8003: NOP
    // 0x8004: HALT
    // 0x8006: LD A, $42
    // 0x8008: RET
    h.load(0x8000, {0xCD, 0x06, 0x80, 0x00, 0x76, 0x00, 0x3E, 0x42, 0xC9});

    assert(cpu_step(h.cpu) == 24);  // CALL
    assert(h.cpu.pc == 0x8006);
    assert(h.cpu.sp == 0xFFFC);

    assert(cpu_step(h.cpu) == 8);  // LD A, $42
    assert(h.cpu.a == 0x42);

    assert(cpu_step(h.cpu) == 16);  // RET
    assert(h.cpu.pc == 0x8003);
    assert(h.cpu.sp == 0xFFFE);

    assert(cpu_step(h.cpu) == 4);  // NOP
    assert(h.cpu.pc == 0x8004);

    std::printf("test_calls_and_returns: PASS\n");
}

void test_ei_delay_and_interrupt() {
    TestHarness h;
    // 0x8000: EI
    // 0x8001: NOP
    // 0x8002: NOP
    h.load(0x8000, {0xFB, 0x00, 0x00});

    // Request VBlank interrupt (bit 0) and enable it in IE ($FFFF)
    h.bus.write(0xFFFF, 0x01);
    h.bus.write(0xFF0F, 0x01);

    cpu_step(h.cpu);  // EI executed -> ime is STILL 0, delay is 1
    assert(h.cpu.ime == 0);
    assert(h.cpu.ime_delay == 1);

    cpu_step(h.cpu);  // NOP at $8001 -> executes, delay becomes 0, activating ime
    assert(h.cpu.ime == 1);
    assert(h.cpu.pc == 0x8002);

    // Next step services interrupt! PC goes to $0040 (VBlank vector)
    uint32_t cyc = cpu_step(h.cpu);
    assert(cyc == 20);
    assert(h.cpu.pc == 0x0040);
    assert(h.cpu.ime == 0);                    // IME cleared upon vectoring
    assert((h.bus.read(0xFF0F) & 0x01) == 0);  // Bit cleared in IF

    std::printf("test_ei_delay_and_interrupt: PASS\n");
}

void test_halt_bug() {
    TestHarness h;
    // IME=0, interrupt pending -> HALT bug triggers: next byte read twice!
    // 0x8000: HALT
    // 0x8001: INC A ($3C)
    // 0x8002: NOP   ($00)
    h.load(0x8000, {0x76, 0x3C, 0x00});
    h.cpu.a = 0x00;
    h.cpu.ime = 0;
    h.bus.write(0xFFFF, 0x01);
    h.bus.write(0xFF0F, 0x01);  // Pending interrupt with IME=0

    cpu_step(h.cpu);  // HALT -> sets halt_bug = 1, does NOT halt
    assert(h.cpu.halted == 0);
    assert(h.cpu.halt_bug == 1);

    cpu_step(h.cpu);  // Reads $3C (INC A), PC NOT incremented!
    assert(h.cpu.a == 0x01);
    assert(h.cpu.pc == 0x8001);  // PC stayed at 0x8001!

    cpu_step(h.cpu);  // Reads $3C (INC A) again! PC now increments to 0x8002
    assert(h.cpu.a == 0x02);
    assert(h.cpu.pc == 0x8002);

    std::printf("test_halt_bug: PASS\n");
}

void test_mbc_banking() {
    // Construct a synthetic 64 KiB ROM with 4 banks of 16 KiB
    std::vector<uint8_t> rom(65536, 0x00);
    // Setup header at $0100
    rom[0x0147] = 0x01;  // MBC1
    rom[0x0148] = 0x01;  // 64 KiB ROM
    rom[0x0149] = 0x00;  // No RAM

    // Mark each bank with distinct identifier byte at offset 0
    rom[0x0000] = 0x10;  // Bank 0
    rom[0x4000] = 0x20;  // Bank 1
    rom[0x8000] = 0x30;  // Bank 2
    rom[0xC000] = 0x40;  // Bank 3

    dragon::gb::Cartridge cart = dragon::gb::parse_gb_rom(rom);
    Bus bus;
    bus.load_cartridge(cart);

    // Initial state: Bank 0 at $0000-$3FFF, Bank 1 at $4000-$7FFF
    assert(bus.read(0x0000) == 0x10);
    assert(bus.read(0x4000) == 0x20);

    // Switch to Bank 2 by writing 0x02 to $2000
    bus.write(0x2000, 0x02);
    assert(bus.read(0x4000) == 0x30);

    // Switch to Bank 3
    bus.write(0x2000, 0x03);
    assert(bus.read(0x4000) == 0x40);

    // Writing 0x00 must map to Bank 1 (MBC1 quirk)
    bus.write(0x2000, 0x00);
    assert(bus.read(0x4000) == 0x20);

    std::printf("test_mbc_banking: PASS\n");
}

void test_gb_battery_ram_persistence() {
    dragon::gb::Bus bus;
    dragon::gb::Cartridge cart;
    cart.mbc = dragon::gb::MbcType::Mbc1;
    cart.has_battery = true;
    cart.ram_size = 8192;
    bus.load_cartridge(cart);
    assert(bus.has_battery());
    assert(bus.sram().size() == 8192);

    // Enable RAM
    bus.write(0x0000, 0x0A);
    bus.write(0xA000, 0x77);
    bus.write(0xA100, 0x88);
    bus.write(0xBFFF, 0x99);

    const std::filesystem::path sav_path =
        std::filesystem::temp_directory_path() / "dragon_gb_test.sav";
    std::filesystem::remove(sav_path);

    assert(bus.save_battery_ram(sav_path));

    // Clear RAM
    std::fill(bus.sram().begin(), bus.sram().end(), 0U);
    assert(bus.read(0xA000) == 0x00);

    // Load back
    assert(bus.load_battery_ram(sav_path));
    assert(bus.read(0xA000) == 0x77);
    assert(bus.read(0xA100) == 0x88);
    assert(bus.read(0xBFFF) == 0x99);

    std::filesystem::remove(sav_path);
    std::printf("test_gb_battery_ram_persistence: PASS\n");
}

void test_gb_save_state() {
    dragon::gb::Bus bus;
    CpuState cpu{};
    dragon::gb::cpu_init(cpu, &bus);

    cpu.a = 0x5A;
    cpu.b = 0x12;
    cpu.c = 0x34;
    cpu.d = 0x56;
    cpu.e = 0x78;
    cpu.h = 0x9A;
    cpu.l = 0xBC;
    cpu.sp = 0xFFFE;
    cpu.pc = 0x0150;
    cpu.cycles = 5555;
    bus.write(0xC000, 0xAA);
    bus.write(0xFF80, 0xBB);

    const std::filesystem::path state_path =
        std::filesystem::temp_directory_path() / "dragon_gb_test.state1";
    std::filesystem::remove(state_path);

    assert(bus.save_state(cpu, state_path));

    // Scramble state
    cpu.a = 0;
    cpu.b = 0;
    cpu.c = 0;
    cpu.d = 0;
    cpu.e = 0;
    cpu.h = 0;
    cpu.l = 0;
    cpu.sp = 0;
    cpu.pc = 0;
    cpu.cycles = 0;
    bus.write(0xC000, 0x00);
    bus.write(0xFF80, 0x00);

    // Load back
    assert(bus.load_state(cpu, state_path));

    assert(cpu.a == 0x5A);
    assert(cpu.b == 0x12);
    assert(cpu.c == 0x34);
    assert(cpu.d == 0x56);
    assert(cpu.e == 0x78);
    assert(cpu.h == 0x9A);
    assert(cpu.l == 0xBC);
    assert(cpu.sp == 0xFFFE);
    assert(cpu.pc == 0x0150);
    assert(cpu.cycles == 5555);
    assert(bus.read(0xC000) == 0xAA);
    assert(bus.read(0xFF80) == 0xBB);

    std::filesystem::remove(state_path);
    std::printf("test_gb_save_state: PASS\n");
}

void test_gb_cli_args() {
    dragon::gb::RunnerOptions opts;
    int exit_code = 0;
    std::array<char*, 9> argv = {const_cast<char*>("gb_runner"),
                                 const_cast<char*>("--state-slot"),
                                 const_cast<char*>("4"),
                                 const_cast<char*>("--load-state"),
                                 const_cast<char*>("/tmp/gb.state4"),
                                 const_cast<char*>("--no-aspect"),
                                 const_cast<char*>("--crt"),
                                 const_cast<char*>("--rom"),
                                 const_cast<char*>("game.gb")};
    bool ok =
        dragon::gb::parse_runner_args(static_cast<int>(argv.size()), argv.data(), opts, exit_code);
    assert(ok);
    assert(opts.state_slot == 4);
    assert(opts.load_state_path == "/tmp/gb.state4");
    assert(opts.aspect_ratio == false);
    assert(opts.crt_filter == true);
    assert(opts.rom_path == "game.gb");
    std::printf("test_gb_cli_args: PASS\n");
}

void test_gb_fallback_logging() {
    dragon::gb::Bus bus;
    CpuState cpu{};
    dragon::gb::cpu_init(cpu, &bus);

    bus.write(0x0200, 0xC9);  // RET opcode
    cpu.sp = 0xFFFE;
    bus.log_fallback(0x0200, 0, 0x0100);
    // Duplicate call should be ignored cleanly
    bus.log_fallback(0x0200, 0, 0x0100);
    std::printf("test_gb_fallback_logging: PASS\n");
}

}  // namespace

int main() {
    std::printf("=== Running Game Boy (SM83) CPU Unit Tests ===\n");
    test_loads_and_register_pairs();
    test_alu_8bit_arithmetic();
    test_daa_decimal_adjust();
    test_add_sp_and_ld_hl_sp();
    test_cb_bitwise_and_swap();
    test_calls_and_returns();
    test_ei_delay_and_interrupt();
    test_halt_bug();
    test_mbc_banking();
    test_gb_battery_ram_persistence();
    test_gb_save_state();
    test_gb_cli_args();
    test_gb_fallback_logging();
    std::printf("=== All Game Boy CPU Unit Tests Passed (13/13) ===\n");
    return 0;
}
