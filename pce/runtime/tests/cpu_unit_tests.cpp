#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#include "pce/bus.h"
#include "pce/interpreter.h"

using namespace dragon::pce;

namespace {

uint8_t reverse_bits8(uint8_t b) {
    b = static_cast<uint8_t>(((b & 0xF0U) >> 4U) | ((b & 0x0FU) << 4U));
    b = static_cast<uint8_t>(((b & 0xCCU) >> 2U) | ((b & 0x33U) << 2U));
    b = static_cast<uint8_t>(((b & 0xAAU) >> 1U) | ((b & 0x55U) << 1U));
    return b;
}

void test_huc6280_extensions_and_t_flag() {
    std::cout << "[Test] HuC6280 CPU: TAM/TMA, SET+T-flag, BCD, TII, RMB/SMB/BBR/BBS, BSR, swaps...\n";

    std::vector<uint8_t> rom(16384, 0xEAU);  // 2 banks of 8 KiB
    // Boot code at $E000 (bank 0 offset 0):
    const uint8_t prog[] = {
        0x78,                    // SEI
        0xD4,                    // CSH (high speed)
        0xA9, 0xFF,              // LDA #$FF
        0x53, 0x01,              // TAM #$01 (MPR0 = $FF I/O)
        0xA9, 0xF8,              // LDA #$F8
        0x53, 0x02,              // TAM #$02 (MPR1 = $F8 WRAM)
        0xA9, 0x01,              // LDA #$01
        0x53, 0x04,              // TAM #$04 (MPR2 = bank 1 at $4000)
        0x43, 0x04,              // TMA #$04 -> A = $01

        // Test SET + T-flag ADC: ZP[$2010] = $12, X = $10, SET, ADC #$05 -> ZP[$2010] = $17, A stays $01
        0xA2, 0x10,              // LDX #$10
        0xA0, 0x12,              // LDY #$12
        0x84, 0x10,              // STY $10 ($2010 = $12)
        0x18,                    // CLC
        0xF4,                    // SET (T = 1)
        0x69, 0x05,              // ADC #$05 (T=1 -> ZP[$2010] += 5 = $17, A unchanged!)

        // Test BCD ADC & SBC: 0x29 + 0x15 = 0x44, then - 0x19 = 0x25
        0xF8,                    // SED
        0x18,                    // CLC
        0xA9, 0x29,              // LDA #$29
        0x69, 0x15,              // ADC #$15 -> $44
        0x38,                    // SEC
        0xE9, 0x19,              // SBC #$19 -> $25
        0xD8,                    // CLD

        // Test SMB3 / BBS3 / RMB3
        0x64, 0x20,              // STZ $20
        0xB7, 0x20,              // SMB3 $20 -> $2020 = 0x08
        0xBF, 0x20, 0x02,        // BBS3 $20, +2 (skip next 2 bytes!)
        0xA9, 0xEE,              // (skipped)
        0x37, 0x20,              // RMB3 $20 -> $2020 = 0x00

        // Test TII block copy: copy 4 bytes from $4000 (bank 1) -> $2200 (WRAM)
        0x73, 0x00, 0x40, 0x00, 0x22, 0x04, 0x00,

        // Test BSR to $E045 (+4 bytes ahead)
        0x44, 0x04,              // BSR +4
        0x22,                    // SAX (swap A and X)
        0x02,                    // SXY (swap X and Y)
        0xEA, 0xEA,              // padding
        // Subroutine:
        0x62,                    // CLA (A = 0)
        0xA9, 0x77,              // LDA #$77
        0x60,                    // RTS
    };
    std::memcpy(rom.data(), prog, sizeof(prog));

    // Place 4 test bytes at bank 1 offset 0 ($4000 when MPR2=1)
    rom[8192 + 0] = 0xDE;
    rom[8192 + 1] = 0xAD;
    rom[8192 + 2] = 0xBE;
    rom[8192 + 3] = 0xEF;

    // Reset vector -> $E000
    rom[0x1FFE] = 0x00;
    rom[0x1FFF] = 0xE0;

    Bus bus;
    assert(bus.cartridge().load_from_bytes(std::move(rom)));
    PceCpuState cpu{};
    bus.reset(cpu);
    assert(cpu.pc == 0xE000U);

    // Step through until after SXY (33 instructions)
    for (int i = 0; i < 33; ++i) {
        step_instruction(cpu, bus);
    }

    assert(cpu.speed_high == 1U);
    assert(cpu.mpr[2] == 0x01U);
    assert(bus.read(cpu, 0x2010U) == 0x17U);  // SET + ADC verified
    assert(bus.read(cpu, 0x2020U) == 0x00U);  // SMB3 + BBS3 + RMB3 verified
    assert(bus.read(cpu, 0x2200U) == 0xDEU);  // TII verified
    assert(bus.read(cpu, 0x2201U) == 0xADU);
    assert(bus.read(cpu, 0x2202U) == 0xBEU);
    assert(bus.read(cpu, 0x2203U) == 0xEFU);
    // After BSR -> A=0x77, X=0x10, Y=0x12; SAX -> A=0x10, X=0x77; SXY -> X=0x12, Y=0x77
    assert(cpu.a == 0x10U);
    assert(cpu.x == 0x12U);
    assert(cpu.y == 0x77U);
}

void test_timer_and_tiq_interrupt() {
    std::cout << "[Test] HuC6280 Timer & TIQ interrupt...\n";

    std::vector<uint8_t> rom(8192, 0xEAU);
    // $E000: CSH; LDA #$01; STA $0C00 (reload=1); STA $0C01 (enable); CLI; BRA -2
    const uint8_t main_code[] = {
        0xD4,              // CSH
        0xA9, 0x01,        // LDA #$01
        0x8D, 0x00, 0x0C,  // STA $0C00
        0x8D, 0x01, 0x0C,  // STA $0C01
        0x58,              // CLI
        0x80, 0xFE,        // BRA -2
    };
    std::memcpy(rom.data(), main_code, sizeof(main_code));

    // TIQ handler at $E100: INC $00; STA $1403 (ack TIQ); RTI
    const uint8_t tiq_code[] = {
        0xE6, 0x00,        // INC $2000
        0x8D, 0x03, 0x14,  // STA $1403 (ack TIQ)
        0x40,              // RTI
    };
    std::memcpy(rom.data() + 0x0100, tiq_code, sizeof(tiq_code));

    rom[0x1FFA] = 0x00; rom[0x1FFB] = 0xE1;  // TIQ -> $E100
    rom[0x1FFE] = 0x00; rom[0x1FFF] = 0xE0;  // RESET -> $E000

    Bus bus;
    assert(bus.cartridge().load_from_bytes(std::move(rom)));
    PceCpuState cpu{};
    bus.reset(cpu);

    // Run ~10,000 cycles; timer (period 2 * 1024 = 2048 cycles) should fire multiple times
    while (cpu.cycles < 10000U) {
        step_instruction(cpu, bus);
    }
    const uint8_t count = bus.read(cpu, 0x2000U);
    assert(count >= 3U);
}

void test_cartridge_mappers_and_tg16_bitswap() {
    std::cout << "[Test] HuCard 512B header, TG-16 bit-reverse, 384KB mirror, SF2 mapper...\n";

    // 1. 512-byte copier header + bit-reversed US TG-16 ROM
    std::vector<uint8_t> tg_rom(512 + 8192, 0x00U);
    std::vector<uint8_t> raw_bank(8192, 0xEAU);
    raw_bank[0] = 0x78U;  // SEI
    raw_bank[1] = 0xD4U;  // CSH
    raw_bank[2] = 0xD8U;  // CLD
    raw_bank[0x1FF8] = 0x10U; raw_bank[0x1FF9] = 0xE0U;
    raw_bank[0x1FFE] = 0x00U; raw_bank[0x1FFF] = 0xE0U;
    for (size_t i = 0; i < raw_bank.size(); ++i) {
        tg_rom[512 + i] = reverse_bits8(raw_bank[i]);
    }

    Cartridge cart;
    assert(cart.load_from_bytes(std::move(tg_rom)));
    assert(cart.had_copier_header());
    assert(cart.was_bit_reversed());
    assert(cart.is_us_region());
    assert(cart.read_bank(0, 0) == 0x78U);
    assert(cart.read_bank(0, 0x1FFF) == 0xE0U);

    // 1b. Un-reversed US TG-16 ROM with standard NEC USA region-lock stub
    std::vector<uint8_t> us_unrev(8192, 0xEAU);
    us_unrev[0] = 0x78U;  // SEI
    us_unrev[1] = 0xD4U;  // CSH
    us_unrev[2] = 0xADU; us_unrev[3] = 0x00U; us_unrev[4] = 0x10U;  // LDA $1000
    us_unrev[5] = 0x29U; us_unrev[6] = 0x40U;                        // AND #$40
    us_unrev[7] = 0xF0U; us_unrev[8] = 0x03U;                        // BEQ +3
    us_unrev[0x1FFE] = 0x00U; us_unrev[0x1FFF] = 0xE0U;
    assert(cart.load_from_bytes(std::move(us_unrev)));
    assert(!cart.was_bit_reversed());
    assert(cart.is_us_region());

    // 2. 384 KiB mirror test
    std::vector<uint8_t> rom384(384 * 1024, 0x00U);
    rom384[0] = 0x11U;                    // bank 0
    rom384[32 * 8192] = 0x22U;            // bank 32 (upper 128 KiB start)
    assert(cart.load_from_bytes(std::move(rom384)));
    assert(cart.mapper_type() == MapperType::Mirror384K);
    assert(cart.read_bank(0x00U, 0) == 0x11U);
    assert(cart.read_bank(0x20U, 0) == 0x11U);  // mirrored in $00-$3F
    assert(cart.read_bank(0x40U, 0) == 0x22U);  // upper 128 KiB in $40-$7F
    assert(cart.read_bank(0x50U, 0) == 0x22U);

    // 3. Street Fighter II' 2.5 MiB mapper test
    std::vector<uint8_t> sf2(2560 * 1024, 0x00U);
    sf2[0] = 0xAAU;                                // bank 0 fixed
    sf2[0x40U * 8192U] = 0xB0U;                    // SF2 window 0
    sf2[(0x40U + 0x40U) * 8192U] = 0xB1U;          // SF2 window 1
    sf2[(0x40U + 0x80U) * 8192U] = 0xB2U;          // SF2 window 2
    assert(cart.load_from_bytes(std::move(sf2)));
    assert(cart.mapper_type() == MapperType::StreetFighter2);
    assert(cart.read_bank(0x40U, 0) == 0xB0U);
    cart.write_bank(0x00U, 0x1FF1U, 0x00U);
    assert(cart.read_bank(0x40U, 0) == 0xB1U);
    cart.write_bank(0x00U, 0x1FF2U, 0x00U);
    assert(cart.read_bank(0x40U, 0) == 0xB2U);
}

void test_save_state_roundtrip() {
    std::cout << "[Test] PCE Save State (DRST + PCE1) round-trip...\n";

    std::vector<uint8_t> rom(8192, 0xEAU);
    rom[0x1FFE] = 0x00; rom[0x1FFF] = 0xE0;

    Bus bus;
    assert(bus.cartridge().load_from_bytes(std::move(rom)));
    PceCpuState cpu{};
    bus.reset(cpu);
    cpu.a = 0x42U;
    cpu.x = 0x13U;
    cpu.y = 0x37U;
    bus.write(cpu, 0x2055U, 0x99U);

    const auto p = std::filesystem::temp_directory_path() / "dragon_pce_state_test.state";
    assert(bus.save_state(p, cpu));

    cpu.a = 0x00U;
    bus.write(cpu, 0x2055U, 0x00U);
    assert(bus.load_state(p, cpu));
    assert(cpu.a == 0x42U);
    assert(cpu.x == 0x13U);
    assert(cpu.y == 0x37U);
    assert(bus.read(cpu, 0x2055U) == 0x99U);

    std::filesystem::remove(p);
}

}  // namespace

int main() {
    test_huc6280_extensions_and_t_flag();
    test_timer_and_tiq_interrupt();
    test_cartridge_mappers_and_tg16_bitswap();
    test_save_state_roundtrip();
    std::cout << "All PCE CPU unit tests passed!\n";
    return 0;
}
