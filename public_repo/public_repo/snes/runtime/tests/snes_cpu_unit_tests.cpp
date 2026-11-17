#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "dragon/sha1.h"
#include "snes/frontend.h"
#include "snes/runtime.h"

namespace {

using dragon::snes::AddrMode;
using dragon::snes::Bus;
using dragon::snes::Cartridge;
using dragon::snes::cpu_init;
using dragon::snes::cpu_irq;
using dragon::snes::cpu_nmi;
using dragon::snes::cpu_reset;
using dragon::snes::cpu_step;
using dragon::snes::get_opcode_info;
using dragon::snes::instruction_length;
using dragon::snes::kOpcodeTable;
using dragon::snes::MapMode;
using dragon::snes::Mnemonic;
using dragon::snes::parse_snes_rom;

void test_opcode_table() {
    for (size_t i = 0; i < 256; ++i) {
        const auto& info = kOpcodeTable[i];
        assert(info.opcode == static_cast<uint8_t>(i));
        assert(info.name != nullptr && info.name[0] != '\0');
        assert(info.base_cycles >= 2 && info.base_cycles <= 8);
    }

    // Immediate length depends on M, X, and E flags:
    // LDA #imm ($A9 = ImmM): 2 bytes when m=1 or e=1, 3 bytes when m=0 and e=0
    assert(instruction_length(0xA9, true, true, false) == 2);
    assert(instruction_length(0xA9, false, true, false) == 3);
    assert(instruction_length(0xA9, false, false, true) == 2);

    // LDX #imm ($A2 = ImmX): 2 bytes when x=1 or e=1, 3 bytes when x=0 and e=0
    assert(instruction_length(0xA2, false, true, false) == 2);
    assert(instruction_length(0xA2, true, false, false) == 3);
    assert(instruction_length(0xA2, false, false, true) == 2);

    // SEP/REP ($E2/$C2 = Imm8): always 2 bytes
    assert(instruction_length(0xE2, false, false, false) == 2);
    assert(instruction_length(0xC2, false, false, false) == 2);

    // PEA ($F4 = Imm16), BRL ($82 = PCRel16), MVN ($54 = BlockMove): always 3 bytes
    assert(instruction_length(0xF4, true, true, true) == 3);
    assert(instruction_length(0x82, true, true, true) == 3);
    assert(instruction_length(0x54, false, false, false) == 3);

    // JSL ($22 = AbsLong), LDA long,X ($BF = AbsLongX): always 4 bytes
    assert(instruction_length(0x22, true, true, true) == 4);
    assert(instruction_length(0xBF, false, false, false) == 4);
}

void test_xce_sep_rep_and_register_truncation() {
    Bus bus;
    std::vector<uint8_t> rom(0x10000, 0xEAU);
    bus.load_rom(rom, MapMode::LoROM);

    // Program at $00:8000:
    //   CLC          ; 18
    //   XCE          ; FB (enter native mode: e=0, m=1, xf=1, c=1)
    //   REP #$30     ; C2 30 (16-bit M and X: m=0, xf=0)
    //   LDA #$1234   ; A9 34 12
    //   LDX #$5678   ; A2 78 56
    //   LDY #$9ABC   ; A0 BC 9A
    //   SEP #$10     ; E2 10 (8-bit X/Y: xf=1 -> must truncate X and Y high bytes!)
    //   XBA          ; EB (swap A <-> B: A becomes $3412, N=0, Z=0 based on $12)
    //   SEC          ; 38
    //   XCE          ; FB (enter emulation mode: e=1, m=1, xf=1, S forced to $01xx)
    //   STP          ; DB
    const std::vector<uint8_t> code = {
        0x18,              // CLC
        0xFB,              // XCE
        0xC2, 0x30,        // REP #$30
        0xA9, 0x34, 0x12,  // LDA #$1234
        0xA2, 0x78, 0x56,  // LDX #$5678
        0xA0, 0xBC, 0x9A,  // LDY #$9ABC
        0xE2, 0x10,        // SEP #$10
        0xEB,              // XBA
        0x38,              // SEC
        0xFB,              // XCE
        0xDB,              // STP
    };
    bus.load_direct(0x008000U, code);
    bus.write_direct(0x00FFFCU, 0x00U);
    bus.write_direct(0x00FFFDU, 0x80U);

    CpuState cpu{};
    cpu_init(cpu, &bus);
    cpu_reset(cpu);
    cpu.s = 0x1FF0U;

    cpu_step(cpu);  // CLC
    cpu_step(cpu);  // XCE
    assert(cpu.e == 0 && cpu.c == 1 && cpu.m == 1 && cpu.xf == 1);

    cpu_step(cpu);  // REP #$30
    assert(cpu.m == 0 && cpu.xf == 0);

    cpu_step(cpu);  // LDA #$1234
    cpu_step(cpu);  // LDX #$5678
    cpu_step(cpu);  // LDY #$9ABC
    assert(cpu.a == 0x1234U && cpu.x == 0x5678U && cpu.y == 0x9ABCU);

    cpu_step(cpu);  // SEP #$10 -> truncates X and Y high bytes immediately
    assert(cpu.xf == 1 && cpu.m == 0);
    assert(cpu.x == 0x0078U && cpu.y == 0x00BCU);

    cpu_step(cpu);  // XBA
    assert(cpu.a == 0x3412U);
    assert(cpu_is_negative(&cpu) == 0 && cpu_is_zero(&cpu) == 0);

    cpu.s = 0x1FF4U;
    cpu_step(cpu);  // SEC
    cpu_step(cpu);  // XCE -> back to emulation mode
    assert(cpu.e == 1 && cpu.c == 0 && cpu.m == 1 && cpu.xf == 1);
    assert(cpu.s == 0x01F4U);
}

void test_bcd_decimal_8bit_and_16bit() {
    Bus bus;
    std::vector<uint8_t> rom(0x10000, 0xEAU);
    bus.load_rom(rom, MapMode::LoROM);

    // Program at $00:8000:
    //   CLC; XCE           ; native mode, 8-bit M/X
    //   SED                ; decimal mode on
    //   CLC; LDA #$45; ADC #$55 ; 45 + 55 = 00 with C=1, Z=1
    //   STA $0010
    //   LDA #$50; SEC; SBC #$01 ; 50 - 01 = 49 with C=1
    //   STA $0011
    //   LDA #$00; SEC; SBC #$01 ; 00 - 01 = 99 with C=0
    //   STA $0012
    //   REP #$20           ; 16-bit M
    //   CLC; LDA #$1999; ADC #$0001 ; 1999 + 1 = 2000, C=0
    //   STA $0014
    //   CLC; LDA #$9999; ADC #$0001 ; 9999 + 1 = 0000, C=1, Z=1
    //   STA $0016
    //   SEC; LDA #$1000; SBC #$0001 ; 1000 - 1 = 0999, C=1
    //   STA $0018
    //   CLD
    //   STP
    const std::vector<uint8_t> code = {
        0x18, 0xFB,              // CLC; XCE
        0xF8,                    // SED
        0x18, 0xA9, 0x45,        // CLC; LDA #$45
        0x69, 0x55,              // ADC #$55 -> $00, C=1
        0x85, 0x10,              // STA $10
        0xA9, 0x50, 0x38,        // LDA #$50; SEC
        0xE9, 0x01,              // SBC #$01 -> $49, C=1
        0x85, 0x11,              // STA $11
        0xA9, 0x00, 0x38,        // LDA #$00; SEC
        0xE9, 0x01,              // SBC #$01 -> $99, C=0
        0x85, 0x12,              // STA $12
        0xC2, 0x20,              // REP #$20 (16-bit M)
        0x18, 0xA9, 0x99, 0x19,  // CLC; LDA #$1999
        0x69, 0x01, 0x00,        // ADC #$0001 -> $2000
        0x85, 0x14,              // STA $14
        0x18, 0xA9, 0x99, 0x99,  // CLC; LDA #$9999
        0x69, 0x01, 0x00,        // ADC #$0001 -> $0000, C=1
        0x85, 0x16,              // STA $16
        0x38, 0xA9, 0x00, 0x10,  // SEC; LDA #$1000
        0xE9, 0x01, 0x00,        // SBC #$0001 -> $0999, C=1
        0x85, 0x18,              // STA $18
        0xD8,                    // CLD
        0xDB,                    // STP
    };
    bus.load_direct(0x008000U, code);

    CpuState cpu{};
    cpu_init(cpu, &bus);
    cpu.pc = 0x8000U;
    while (!cpu.halted) {
        cpu_step(cpu);
    }

    assert(bus.read(0x000010U) == 0x00U);
    assert(bus.read(0x000011U) == 0x49U);
    assert(bus.read(0x000012U) == 0x99U);
    assert(bus.read16(0x000014U) == 0x2000U);
    assert(bus.read16(0x000016U) == 0x0000U);
    assert(bus.read16(0x000018U) == 0x0999U);
    assert(cpu.c == 1U);
    assert(cpu.d_flag == 0U);
}

void test_24bit_and_stack_relative_addressing_modes() {
    Bus bus;
    std::vector<uint8_t> rom(0x20000, 0xEAU);
    bus.load_rom(rom, MapMode::LoROM);

    // Place test values in WRAM bank $7E and $7F
    bus.write16(0x7E2000U, 0xBEEFU);
    bus.write16(0x7E2010U, 0xCAFEU);
    bus.write16(0x7F1004U, 0x1337U);

    // Set 24-bit pointer at $00:0020 -> $7F:1000
    bus.write(0x000020U, 0x00U);
    bus.write(0x000021U, 0x10U);
    bus.write(0x000022U, 0x7FU);

    // Program at $00:8000:
    //   CLC; XCE; REP #$30
    //   LDA #$1FF0; TCS
    //   LDA $7E2000         ; AF 00 20 7E (AbsLong) -> $BEEF
    //   STA $0030
    //   LDX #$0010
    //   LDA $7E2000,X       ; BF 00 20 7E (AbsLongX) -> $CAFE
    //   STA $0032
    //   LDY #$0004
    //   LDA [$20],Y         ; B7 20 (DPIndLongY) -> $1337
    //   STA $0034
    //   PEA $2000           ; F4 00 20 (pushes $2000 at S-1, S-2)
    //   LDA #$7E7E; PHA     ; pushes $7E7E
    //   LDA $03,S           ; A3 03 (StackRel: reads $2000 from stack!)
    //   STA $0036
    //   LDA #$007E; SEP #$20; PHA; PLB; REP #$20 ; DB = $7E
    //   LDY #$0010
    //   LDA ($03,S),Y       ; B3 03 (StackRelIndY: ($2000) in DB=$7E + Y=$10 -> $7E:2010 = $CAFE!)
    //   STA $0038
    //   STP
    const std::vector<uint8_t> code = {
        0x18, 0xFB,              // CLC; XCE
        0xC2, 0x30,              // REP #$30
        0xA9, 0xF0, 0x1F, 0x1B,  // LDA #$1FF0; TCS
        0xAF, 0x00, 0x20, 0x7E,  // LDA $7E2000
        0x85, 0x30,              // STA $30
        0xA2, 0x10, 0x00,        // LDX #$0010
        0xBF, 0x00, 0x20, 0x7E,  // LDA $7E2000,X
        0x85, 0x32,              // STA $32
        0xA0, 0x04, 0x00,        // LDY #$0004
        0xB7, 0x20,              // LDA [$20],Y
        0x85, 0x34,              // STA $34
        0xF4, 0x00, 0x20,        // PEA $2000
        0xA9, 0x7E, 0x7E, 0x48,  // LDA #$7E7E; PHA
        0xA3, 0x03,              // LDA $03,S
        0x85, 0x36,              // STA $36
        0xE2, 0x20,              // SEP #$20
        0xA9, 0x7E, 0x48, 0xAB,  // LDA #$7E; PHA; PLB (DB = $7E)
        0xC2, 0x20,              // REP #$20
        0xA0, 0x10, 0x00,        // LDY #$0010
        0xB3, 0x03,              // LDA ($03,S),Y
        0x85, 0x38,              // STA $38
        0xDB,                    // STP
    };
    bus.load_direct(0x008000U, code);

    CpuState cpu{};
    cpu_init(cpu, &bus);
    cpu.pc = 0x8000U;
    while (!cpu.halted) {
        cpu_step(cpu);
    }

    assert(bus.read16(0x000030U) == 0xBEEFU);
    assert(bus.read16(0x000032U) == 0xCAFEU);
    assert(bus.read16(0x000034U) == 0x1337U);
    assert(bus.read16(0x000036U) == 0x2000U);
    assert(bus.read16(0x000038U) == 0xCAFEU);
}

void test_block_moves_mvn_and_mvp() {
    Bus bus;
    std::vector<uint8_t> rom(0x10000, 0xEAU);
    bus.load_rom(rom, MapMode::LoROM);

    for (uint16_t i = 0; i < 8; ++i) {
        bus.write(0x7E1000U + i, static_cast<uint8_t>(0xA0U + i));
    }

    // Program at $00:8000:
    //   CLC; XCE; REP #$30
    //   LDA #$0007          ; 8 bytes (A+1)
    //   LDX #$1000          ; src $7E:1000
    //   LDY #$2000          ; dst $7F:2000
    //   MVN $7E, $7F        ; 54 7F 7E (opcode 54, dst=$7F, src=$7E)
    //   LDA #$0003          ; 4 bytes (A+1)
    //   LDX #$2007          ; src $7F:2007
    //   LDY #$3003          ; dst $7E:3003
    //   MVP $7F, $7E        ; 44 7E 7F (opcode 44, dst=$7E, src=$7F)
    //   STP
    const std::vector<uint8_t> code = {
        0x18, 0xFB,        // CLC; XCE
        0xC2, 0x30,        // REP #$30
        0xA9, 0x07, 0x00,  // LDA #$0007
        0xA2, 0x00, 0x10,  // LDX #$1000
        0xA0, 0x00, 0x20,  // LDY #$2000
        0x54, 0x7F, 0x7E,  // MVN dst=$7F, src=$7E
        0xA9, 0x03, 0x00,  // LDA #$0003
        0xA2, 0x07, 0x20,  // LDX #$2007
        0xA0, 0x03, 0x30,  // LDY #$3003
        0x44, 0x7E, 0x7F,  // MVP dst=$7E, src=$7F
        0xDB,              // STP
    };
    bus.load_direct(0x008000U, code);

    CpuState cpu{};
    cpu_init(cpu, &bus);
    cpu.pc = 0x8000U;
    while (!cpu.halted) {
        cpu_step(cpu);
    }

    assert(cpu.a == 0xFFFFU);
    assert(cpu.db == 0x7EU);
    for (uint16_t i = 0; i < 8; ++i) {
        assert(bus.read(0x7F2000U + i) == static_cast<uint8_t>(0xA0U + i));
    }
    for (uint16_t i = 0; i < 4; ++i) {
        assert(bus.read(0x7E3000U + i) == static_cast<uint8_t>(0xA4U + i));
    }
}

void test_hardware_mul_div_and_dma() {
    Bus bus;
    std::vector<uint8_t> rom(0x10000, 0xEAU);
    bus.load_rom(rom, MapMode::LoROM);

    CpuState cpu{};
    cpu_init(cpu, &bus);

    // 1. Hardware 8x8 -> 16 unsigned multiplication ($4202 * $4203 -> $4216-$4217)
    bus.write(0x004202U, 200U);
    bus.write(0x004203U, 150U);
    assert(bus.read16(0x004216U) == 30000U);

    // 2. Hardware 16/8 -> 16 unsigned division ($4204-$4205 / $4206 -> $4214-$4215, rem
    // $4216-$4217)
    bus.write16(0x004204U, 12345U);
    bus.write(0x004206U, 100U);
    assert(bus.read16(0x004214U) == 123U);
    assert(bus.read16(0x004216U) == 45U);

    // Divide by zero edge case: quotient = $FFFF, remainder = dividend
    bus.write16(0x004204U, 0x4321U);
    bus.write(0x004206U, 0U);
    assert(bus.read16(0x004214U) == 0xFFFFU);
    assert(bus.read16(0x004216U) == 0x4321U);

    // 3. 8-channel DMA: copy 16 bytes from ROM $00:9000 to WRAM $7F:0100 via $2180 (WMDATA)
    for (uint8_t i = 0; i < 16; ++i) {
        bus.write_direct(0x009000U + i, static_cast<uint8_t>(0x50U + i));
    }
    bus.write(0x002181U, 0x00U);  // WMADDL = $00
    bus.write(0x002182U, 0x01U);  // WMADDM = $01
    bus.write(0x002183U, 0x01U);  // WMADDH = $01 ($7F:0100)

    bus.write(0x004300U, 0x00U);  // Mode 0, A->B, increment
    bus.write(0x004301U, 0x80U);  // B-bus = $2180 (WMDATA)
    bus.write16(0x004302U, 0x9000U);
    bus.write(0x004304U, 0x00U);  // A-bus bank = $00
    bus.write16(0x004305U, 16U);  // 16 bytes

    const uint64_t cyc_before = cpu.master_cycles;
    bus.write(0x00420BU, 0x01U);  // Trigger DMA channel 0
    assert(cpu.master_cycles - cyc_before == 8ULL * 16ULL);

    for (uint8_t i = 0; i < 16; ++i) {
        assert(bus.read(0x7F0100U + i) == static_cast<uint8_t>(0x50U + i));
    }
}

void test_lorom_and_hirom_cartridge_detection() {
    // 1. Construct synthetic 64 KiB LoROM with 512-byte SMC header
    std::vector<uint8_t> lorom_img(512 + 0x10000, 0U);
    const char* lo_title = "TEST LOROM GAME      ";
    std::memcpy(lorom_img.data() + 512 + 0x7FC0, lo_title, 21);
    lorom_img[512 + 0x7FD5] = 0x20U;  // LoROM
    lorom_img[512 + 0x7FD7] = 0x06U;  // 64 KiB
    lorom_img[512 + 0x7FD8] = 0x03U;  // 8 KiB SRAM
    lorom_img[512 + 0x7FDC] = 0xAAU;
    lorom_img[512 + 0x7FDD] = 0x55U;
    lorom_img[512 + 0x7FDE] = 0x55U;
    lorom_img[512 + 0x7FDF] = 0xAAU;
    lorom_img[512 + 0x7FFC] = 0x00U;
    lorom_img[512 + 0x7FFD] = 0x80U;
    lorom_img[512 + 0x0000] = 0x78U;  // SEI at $00:8000
    lorom_img[512 + 0x8000] = 0x42U;  // Bank $01:$8000

    const Cartridge lo_cart = parse_snes_rom(lorom_img);
    assert(lo_cart.has_smc_header);
    assert(lo_cart.map_mode == MapMode::LoROM);
    assert(lo_cart.title == "TEST LOROM GAME");
    assert(lo_cart.emu_vectors.reset == 0x8000U);
    assert(lo_cart.sram_size_bytes == 8192U);

    Bus lo_bus;
    lo_bus.load_cartridge(lo_cart);
    assert(lo_bus.read(0x008000U) == 0x78U);
    assert(lo_bus.read(0x808000U) == 0x78U);  // Mirror bank $80
    assert(lo_bus.read(0x018000U) == 0x42U);
    // LoROM SRAM at $70:0000
    lo_bus.write(0x700123U, 0x99U);
    assert(lo_bus.read(0x700123U) == 0x99U);

    // 1b. Construct synthetic 64 KiB LoROM with 1024-byte double SMC header and 0 KiB SRAM
    std::vector<uint8_t> double_smc_img(1024 + 0x10000, 0U);
    std::memcpy(double_smc_img.data() + 1024 + 0x7FC0, lo_title, 21);
    double_smc_img[1024 + 0x7FD5] = 0x20U;  // LoROM
    double_smc_img[1024 + 0x7FD7] = 0x06U;  // 64 KiB
    double_smc_img[1024 + 0x7FD8] = 0x00U;  // 0 KiB SRAM (no SRAM, no battery)
    double_smc_img[1024 + 0x7FDC] = 0xAAU;
    double_smc_img[1024 + 0x7FDD] = 0x55U;
    double_smc_img[1024 + 0x7FDE] = 0x55U;
    double_smc_img[1024 + 0x7FDF] = 0xAAU;
    double_smc_img[1024 + 0x7FFC] = 0x00U;
    double_smc_img[1024 + 0x7FFD] = 0x80U;
    double_smc_img[1024 + 0x0000] = 0x78U;  // SEI at $00:8000

    const Cartridge double_smc_cart = parse_snes_rom(double_smc_img);
    assert(double_smc_cart.has_smc_header);
    assert(double_smc_cart.map_mode == MapMode::LoROM);
    assert(double_smc_cart.rom.size() == 0x10000U);
    assert(double_smc_cart.sram_size_bytes == 0U);

    Bus no_sram_bus;
    no_sram_bus.load_cartridge(double_smc_cart);
    assert(no_sram_bus.sram_size() == 0U);
    // Writing to unmapped SRAM at $70:0000 on a 0-SRAM cartridge must not read back the written
    // value
    no_sram_bus.write(0x700000U, 0x00U);
    assert(no_sram_bus.read(0x700000U) != 0x00U);

    // 2. Construct synthetic 64 KiB HiROM (unheadered)
    std::vector<uint8_t> hirom_img(0x10000, 0U);
    const char* hi_title = "TEST HIROM GAME      ";
    std::memcpy(hirom_img.data() + 0xFFC0, hi_title, 21);
    hirom_img[0xFFD5] = 0x31U;  // HiROM + FastROM
    hirom_img[0xFFD7] = 0x06U;
    hirom_img[0xFFD8] = 0x03U;
    hirom_img[0xFFDC] = 0x00U;
    hirom_img[0xFFDD] = 0xFFU;
    hirom_img[0xFFDE] = 0xFFU;
    hirom_img[0xFFDF] = 0x00U;
    hirom_img[0xFFFC] = 0x00U;
    hirom_img[0xFFFD] = 0x80U;
    hirom_img[0x0000] = 0x11U;  // $C0:0000
    hirom_img[0x8000] = 0x78U;  // $00:8000 and $C0:8000

    const Cartridge hi_cart = parse_snes_rom(hirom_img);
    assert(!hi_cart.has_smc_header);
    assert(hi_cart.map_mode == MapMode::HiROM);
    assert(hi_cart.has_fastrom);
    assert(hi_cart.title == "TEST HIROM GAME");

    Bus hi_bus;
    hi_bus.load_cartridge(hi_cart);
    assert(hi_bus.read(0xC00000U) == 0x11U);
    assert(hi_bus.read(0xC08000U) == 0x78U);
    assert(hi_bus.read(0x008000U) == 0x78U);  // Upper half mirror in bank $00
    assert(hi_bus.read(0x808000U) == 0x78U);  // Upper half mirror in bank $80
    // HiROM SRAM at $20:6000 and mirror $A0:6000
    hi_bus.write(0x206010U, 0x77U);
    assert(hi_bus.read(0xA06010U) == 0x77U);
}

void test_interrupts_and_long_calls() {
    Bus bus;
    std::vector<uint8_t> rom(0x20000, 0xEAU);
    bus.load_rom(rom, MapMode::LoROM);

    // Bank $00:8000:
    //   CLC; XCE; REP #$30
    //   LDA #$1FF0; TCS
    //   CLI
    //   JSL $018000
    //   STP
    const std::vector<uint8_t> bank0_code = {
        0x18, 0xFB,              // CLC; XCE
        0xC2, 0x30,              // REP #$30
        0xA9, 0xF0, 0x1F, 0x1B,  // LDA #$1FF0; TCS
        0x58,                    // CLI
        0x22, 0x00, 0x80, 0x01,  // JSL $018000
        0xDB,                    // STP
    };
    // Bank $01:8000:
    //   LDA #$4455
    //   STA $0040
    //   RTL
    const std::vector<uint8_t> bank1_code = {
        0xA9, 0x55, 0x44,  // LDA #$4455
        0x85, 0x40,        // STA $40
        0x6B,              // RTL
    };
    // Native NMI handler at $00:9000:
    //   INC $0042
    //   RTI
    const std::vector<uint8_t> nmi_code = {
        0xE6,
        0x42,  // INC $42
        0x40,  // RTI
    };

    bus.load_direct(0x008000U, bank0_code);
    bus.load_direct(0x018000U, bank1_code);
    bus.load_direct(0x009000U, nmi_code);
    bus.write_direct(0x00FFEAU, 0x00U);
    bus.write_direct(0x00FFEBU, 0x90U);

    CpuState cpu{};
    cpu_init(cpu, &bus);
    cpu.pc = 0x8000U;

    // Step until JSL $018000 enters bank $01
    while (cpu.pb != 0x01U) {
        cpu_step(cpu);
    }
    // Deliver a native NMI while executing in bank $01!
    cpu_nmi(cpu);
    assert(cpu.pb == 0x00U && cpu.pc == 0x9000U);

    while (!cpu.halted) {
        cpu_step(cpu);
    }

    assert(bus.read16(0x000040U) == 0x4455U);
    assert(bus.read16(0x000042U) == 0x0001U);
    assert(cpu.pb == 0x00U);
    assert(cpu.s == 0x1FF0U);
}

void test_snes_battery_ram_persistence() {
    Bus bus;
    Cartridge cart;
    cart.has_battery = true;
    cart.sram_size_bytes = 8192;
    bus.load_cartridge(cart);
    assert(bus.has_battery());
    assert(bus.sram_size() == 8192);

    bus.write(0x700000U, 0x12);
    bus.write(0x700100U, 0x34);
    bus.write(0x701FFFU, 0x56);

    const std::filesystem::path sav_path =
        std::filesystem::temp_directory_path() / "dragon_snes_test.sav";
    std::filesystem::remove(sav_path);

    assert(bus.save_battery_ram(sav_path));

    // Clear SRAM
    std::fill(bus.sram_data(), bus.sram_data() + bus.sram_size(), 0U);
    assert(bus.read(0x700000U) == 0x00);

    // Load back
    assert(bus.load_battery_ram(sav_path));
    assert(bus.read(0x700000U) == 0x12);
    assert(bus.read(0x700100U) == 0x34);
    assert(bus.read(0x701FFFU) == 0x56);

    std::filesystem::remove(sav_path);
}

void test_snes_save_state() {
    Bus bus;
    CpuState cpu{};
    cpu_init(cpu, &bus);

    cpu.a = 0x1234;
    cpu.x = 0x5678;
    cpu.y = 0x9ABC;
    cpu.s = 0x01FF;
    cpu.pc = 0x8050;
    cpu.pb = 0x01;
    cpu.c = 1;
    cpu.m = 0;
    cpu.xf = 1;
    cpu.master_cycles = 98765;
    bus.write(0x7E0100U, 0xEE);
    bus.write(0x700020U, 0xDD);

    const std::filesystem::path state_path =
        std::filesystem::temp_directory_path() / "dragon_snes_test.state1";
    std::filesystem::remove(state_path);

    assert(bus.save_state(cpu, state_path));

    // Scramble state
    cpu.a = 0;
    cpu.x = 0;
    cpu.y = 0;
    cpu.s = 0;
    cpu.pc = 0;
    cpu.pb = 0;
    cpu.c = 0;
    cpu.master_cycles = 0;
    bus.write(0x7E0100U, 0x00);
    bus.write(0x700020U, 0x00);

    // Load back
    assert(bus.load_state(cpu, state_path));

    assert(cpu.a == 0x1234);
    assert(cpu.x == 0x5678);
    assert(cpu.y == 0x9ABC);
    assert(cpu.s == 0x01FF);
    assert(cpu.pc == 0x8050);
    assert(cpu.pb == 0x01);
    assert(cpu.c == 1);
    assert(cpu.m == 0);
    assert(cpu.xf == 1);
    assert(cpu.master_cycles == 98765);
    assert(bus.read(0x7E0100U) == 0xEE);
    assert(bus.read(0x700020U) == 0xDD);

    std::filesystem::remove(state_path);
}

void test_snes_cli_args() {
    dragon::snes::RunnerOptions opts;
    int exit_code = 0;
    std::array<char*, 9> argv = {const_cast<char*>("snes_runner"),
                                 const_cast<char*>("--state-slot"),
                                 const_cast<char*>("2"),
                                 const_cast<char*>("--load-state"),
                                 const_cast<char*>("/tmp/snes.state2"),
                                 const_cast<char*>("--no-aspect"),
                                 const_cast<char*>("--crt"),
                                 const_cast<char*>("--rom"),
                                 const_cast<char*>("game.sfc")};
    bool ok = dragon::snes::parse_runner_args(static_cast<int>(argv.size()), argv.data(), opts,
                                              exit_code);
    assert(ok);
    assert(opts.state_slot == 2);
    assert(opts.load_state_path == "/tmp/snes.state2");
    assert(opts.aspect_ratio == false);
    assert(opts.crt_filter == true);
    assert(opts.rom_path == "game.sfc");
}

}  // namespace

int main() {
    test_opcode_table();
    test_xce_sep_rep_and_register_truncation();
    test_bcd_decimal_8bit_and_16bit();
    test_24bit_and_stack_relative_addressing_modes();
    test_block_moves_mvn_and_mvp();
    test_hardware_mul_div_and_dma();
    test_lorom_and_hirom_cartridge_detection();
    test_interrupts_and_long_calls();
    test_snes_battery_ram_persistence();
    test_snes_save_state();
    test_snes_cli_args();
    std::printf("snes_cpu_unit_tests: all unit tests passed\n");
    return 0;
}
