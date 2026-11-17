#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "dragon/dynlib.h"

#include "dragon/config.h"
#include "nes/runtime.h"
#include "nesrecomp/disassembler.h"
#include "nesrecomp/emitter.h"

namespace {

using dragon::GameConfig;
using dragon::nes::Bus;
using dragon::nes::Cartridge;
using dragon::nes::cpu_init;
using dragon::nes::cpu_reset;
using dragon::nes::cpu_step;
using dragon::nes::cpu_step_with_ppu;
using dragon::nes::Mirroring;
using dragon::nes::Ppu;
using dragon::nesrecomp::analyze_prg_rom;
using dragon::nesrecomp::EmitOptions;
using dragon::nesrecomp::ProgramCFG;
using dragon::nesrecomp::write_generated_output;

// Helper to write a 5-bit value serially into an MMC1 register at `addr`.
void mmc1_write_serial(Bus& bus, uint16_t addr, uint8_t val5) {
    for (uint8_t bit = 0U; bit < 5U; ++bit) {
        bus.write(addr, static_cast<uint8_t>((val5 >> bit) & 0x01U));
    }
}

void test_mapper1_mmc1_serial_prg_chr_and_mirroring() {
    Cartridge cart{};
    cart.mapper = 1U;
    cart.mirroring = Mirroring::Horizontal;
    // 4 x 16 KiB PRG banks (64 KiB total); stamp each bank's first byte with (0xA0 + bank)
    cart.prg_rom.resize(4U * 0x4000U, 0x00U);
    for (uint8_t b = 0U; b < 4U; ++b) {
        cart.prg_rom[static_cast<size_t>(b) * 0x4000U] = static_cast<uint8_t>(0xA0U + b);
    }
    // 4 x 4 KiB CHR banks (16 KiB total); stamp each 4K bank's first byte with (0xC0 + bank)
    cart.chr_rom.resize(4U * 0x1000U, 0x00U);
    for (uint8_t c = 0U; c < 4U; ++c) {
        cart.chr_rom[static_cast<size_t>(c) * 0x1000U] = static_cast<uint8_t>(0xC0U + c);
    }

    Bus bus;
    bus.load_cartridge(cart);

    // Default MMC1 control is $0C (Mode 3: switchable 16K at $8000 = bank 0, fixed last 16K at
    // $C000 = bank 3)
    assert(bus.read(0x8000U) == 0xA0U);
    assert(bus.read(0xC000U) == 0xA3U);
    assert(bus.current_prg_bank(0x8000U) == 0U);
    assert(bus.current_prg_bank(0xC000U) == 3U);

    // Switch $8000 to bank 2 via serial write to $E000
    mmc1_write_serial(bus, 0xE000U, 2U);
    assert(bus.read(0x8000U) == 0xA2U);
    assert(bus.read(0xC000U) == 0xA3U);
    assert(bus.current_prg_bank(0x8000U) == 2U);

    // Test dynamic mirroring via Control register ($8000):
    // 0 = OneScreenLower, 1 = OneScreenUpper, 2 = Vertical, 3 = Horizontal
    mmc1_write_serial(bus, 0x8000U, 0x1CU | 0U);  // 4K CHR mode + Mode 3 PRG + OneScreenLower
    assert(bus.ppu().mirroring() == Mirroring::OneScreenLower);
    bus.ppu().write_vram(0x2000U, 0x55U);
    assert(bus.ppu().read_vram(0x2400U) == 0x55U);
    assert(bus.ppu().read_vram(0x2800U) == 0x55U);
    assert(bus.ppu().read_vram(0x2C00U) == 0x55U);

    mmc1_write_serial(bus, 0x8000U, 0x1CU | 1U);  // OneScreenUpper
    assert(bus.ppu().mirroring() == Mirroring::OneScreenUpper);
    bus.ppu().write_vram(0x2000U, 0x77U);
    assert(bus.ppu().read_vram(0x2C00U) == 0x77U);

    mmc1_write_serial(bus, 0x8000U, 0x1CU | 2U);  // Vertical
    assert(bus.ppu().mirroring() == Mirroring::Vertical);

    mmc1_write_serial(bus, 0x8000U, 0x1CU | 3U);  // Horizontal
    assert(bus.ppu().mirroring() == Mirroring::Horizontal);

    // Test 4K CHR bank switching ($A000 = CHR0 -> bank 1, $C000 = CHR1 -> bank 3)
    mmc1_write_serial(bus, 0xA000U, 1U);
    mmc1_write_serial(bus, 0xC000U, 3U);
    assert(bus.ppu().read_vram(0x0000U) == 0xC1U);
    assert(bus.ppu().read_vram(0x1000U) == 0xC3U);

    // Test PRG Mode 2 (fix bank 0 at $8000, switchable 16K at $C000 -> bank 2)
    mmc1_write_serial(bus, 0x8000U, 0x18U);  // PRG mode = 2
    assert(bus.read(0x8000U) == 0xA0U);
    assert(bus.read(0xC000U) == 0xA2U);

    // Test shift-register reset with bit 7 set ($80) -> forces PRG mode 3 (locks $C000 to bank 3)
    bus.write(0x8000U, 0x01U);  // partial shift
    bus.write(0x8000U, 0x80U);  // reset shift register and lock $C000 to last bank
    assert(bus.read(0xC000U) == 0xA3U);
}

void test_mapper2_uxrom_banking_and_recompiled_dispatch() {
    // Construct a 4-bank (64 KiB) UxROM (Mapper 2) cartridge:
    // - Bank 0 at $8000: LDA #$11; STA $10; RTS  (compiled to func_00_8000)
    // - Bank 1 at $8000: LDA #$22; STA $11; RTS  (compiled to func_01_8000)
    // - Bank 2 at $8000: LDA #$33; STA $12; JSR $C100; RTS  (compiled to func_02_8000)
    // - Bank 3 (fixed at $C000-$FFFF):
    //     $C000 (RESET):
    //       LDA #$00; STA $8000; JSR $8000
    //       LDA #$01; STA $8000; JSR $8000
    //       LDA #$02; STA $8000; JSR $8000
    //       RTS
    //     $C100 (Helper in fixed bank):
    //       LDA #$44; STA $13; RTS
    Cartridge cart{};
    cart.mapper = 2U;
    cart.prg_rom.resize(4U * 0x4000U, 0x02U);

    const std::vector<uint8_t> bank0_code = {0xA9, 0x11, 0x85, 0x10, 0x60};
    const std::vector<uint8_t> bank1_code = {0xA9, 0x22, 0x85, 0x11, 0x60};
    const std::vector<uint8_t> bank2_code = {0xA9, 0x33, 0x85, 0x12, 0x20, 0x00, 0xC1, 0x60};
    const std::vector<uint8_t> fixed_reset = {
        0xA9, 0x00,        // LDA #$00
        0x8D, 0x00, 0x80,  // STA $8000 (select bank 0)
        0x20, 0x00, 0x80,  // JSR $8000 -> calls func_00_8000
        0xA9, 0x01,        // LDA #$01
        0x8D, 0x00, 0x80,  // STA $8000 (select bank 1)
        0x20, 0x00, 0x80,  // JSR $8000 -> calls func_01_8000
        0xA9, 0x02,        // LDA #$02
        0x8D, 0x00, 0x80,  // STA $8000 (select bank 2)
        0x20, 0x00, 0x80,  // JSR $8000 -> calls func_02_8000
        0x60,              // RTS
    };
    const std::vector<uint8_t> fixed_helper = {0xA9, 0x44, 0x85, 0x13, 0x60};

    std::memcpy(cart.prg_rom.data() + 0U * 0x4000U, bank0_code.data(), bank0_code.size());
    std::memcpy(cart.prg_rom.data() + 1U * 0x4000U, bank1_code.data(), bank1_code.size());
    std::memcpy(cart.prg_rom.data() + 2U * 0x4000U, bank2_code.data(), bank2_code.size());
    std::memcpy(cart.prg_rom.data() + 3U * 0x4000U + 0x0000U, fixed_reset.data(),
                fixed_reset.size());
    std::memcpy(cart.prg_rom.data() + 3U * 0x4000U + 0x0100U, fixed_helper.data(),
                fixed_helper.size());

    // Hardware vectors in Bank 3 ($FFFA-$FFFF) -> $C000
    const size_t vec_off = 3U * 0x4000U + 0x3FFAU;
    cart.prg_rom[vec_off + 0] = 0x00U;
    cart.prg_rom[vec_off + 1] = 0xC0U;
    cart.prg_rom[vec_off + 2] = 0x00U;
    cart.prg_rom[vec_off + 3] = 0xC0U;
    cart.prg_rom[vec_off + 4] = 0x00U;
    cart.prg_rom[vec_off + 5] = 0xC0U;

    GameConfig cfg;
    cfg.name = "uxrom_banked_dispatch_test";
    cfg.mapper = 2;

    const ProgramCFG prog_cfg = analyze_prg_rom(cart.prg_rom, cfg);
    assert(prog_cfg.is_banked);
    assert(prog_cfg.functions.count(0xC000U) == 1U);
    assert(prog_cfg.functions.count(0xC100U) == 1U);
    assert(prog_cfg.bank_cfgs.size() >= 3U);

    const std::filesystem::path out_dir =
        std::filesystem::path(DRAGON_BINARY_DIR) / "mapper_test_gen" / "uxrom";
    std::filesystem::remove_all(out_dir);

    EmitOptions opts;
    opts.enable_flag_opt = true;
    opts.split_every = 100;
    write_generated_output(prog_cfg, cfg, out_dir, opts);

    const std::filesystem::path so_path = out_dir / "libuxrom_test.so";
    std::string cmd = std::string(DRAGON_C_COMPILER) +
                      " -std=c99 -Wall -Wextra -Wpedantic -Werror -O1 -fPIC -shared"
                      " -I\"" +
                      std::string(DRAGON_RUNTIME_INCLUDE_DIR) + "\" -I\"" + out_dir.string() +
                      "\" \"" + (out_dir / "recomp_funcs_0.c").string() + "\" \"" +
                      (out_dir / "recomp_dispatch.c").string() + "\" -o \"" +
                      so_path.string() + "\"";
#if defined(_WIN32)
    cmd += " \"" + std::string(DRAGON_BINARY_DIR) + "/nes/runtime/libnes_runtime.a\" \"" +
           std::string(DRAGON_BINARY_DIR) + "/common/libdragon_common.a\" -lstdc++";
#endif
    assert(std::system(cmd.c_str()) == 0);

    void* handle = dlopen(so_path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    assert(handle != nullptr);

    // Verify per-bank symbols func_00_8000, func_01_8000, func_02_8000 and fixed func_C000 exist!
    assert(dlsym(handle, "func_00_8000") != nullptr);
    assert(dlsym(handle, "func_01_8000") != nullptr);
    assert(dlsym(handle, "func_02_8000") != nullptr);
    assert(dlsym(handle, "func_C000") != nullptr);
    assert(dlsym(handle, "func_C100") != nullptr);

    const auto* table = static_cast<const RecompFuncEntry*>(dlsym(handle, "kRecompDispatchTable"));
    const auto* table_count =
        static_cast<const size_t*>(dlsym(handle, "kRecompDispatchTableCount"));
    RecompFunc entry_fn = nullptr;
    void* fn_sym = dlsym(handle, "func_C000");
    std::memcpy(&entry_fn, &fn_sym, sizeof(entry_fn));
    assert(table != nullptr && table_count != nullptr && entry_fn != nullptr);

    Bus recomp_bus;
    recomp_bus.load_cartridge(cart);
    recomp_bus.set_dispatch_table(table, *table_count);
    CpuState recomp_cpu{};
    cpu_init(recomp_cpu, &recomp_bus);
    cpu_reset(recomp_cpu);
    recomp_bus.write(0x01FEU, 0xFEU);
    recomp_bus.write(0x01FFU, 0xFFU);

    entry_fn(&recomp_cpu);

    assert(recomp_bus.fallback_count() == 0U);
    assert(recomp_bus.read(0x0010U) == 0x11U);
    assert(recomp_bus.read(0x0011U) == 0x22U);
    assert(recomp_bus.read(0x0012U) == 0x33U);
    assert(recomp_bus.read(0x0013U) == 0x44U);
    assert(recomp_bus.current_prg_bank(0x8000U) == 2U);
    assert(recomp_bus.current_prg_bank(0xC000U) == 3U);

    dlclose(handle);
}

void test_mapper3_cnrom_chr_switching() {
    Cartridge cart{};
    cart.mapper = 3U;
    cart.prg_rom.resize(0x8000U, 0xEA);  // 32 KiB fixed PRG
    // 4 x 8 KiB CHR banks (32 KiB total); stamp byte $0010 of each bank with (0xD0 + bank)
    cart.chr_rom.resize(4U * 0x2000U, 0x00U);
    for (uint8_t b = 0U; b < 4U; ++b) {
        cart.chr_rom[static_cast<size_t>(b) * 0x2000U + 0x0010U] = static_cast<uint8_t>(0xD0U + b);
    }

    Bus bus;
    bus.load_cartridge(cart);

    for (uint8_t b = 0U; b < 4U; ++b) {
        bus.write(0x8000U, b);
        assert(bus.ppu().read_vram(0x0010U) == static_cast<uint8_t>(0xD0U + b));
    }
}

void test_mapper4_mmc3_banking_and_scanline_irq() {
    Cartridge cart{};
    cart.mapper = 4U;
    cart.mirroring = Mirroring::Vertical;
    // 8 x 8 KiB PRG banks (64 KiB total); stamp first byte of each 8K bank with (0xB0 + bank)
    cart.prg_rom.resize(8U * 0x2000U, 0x00U);
    for (uint8_t b = 0U; b < 8U; ++b) {
        cart.prg_rom[static_cast<size_t>(b) * 0x2000U] = static_cast<uint8_t>(0xB0U + b);
    }
    // 16 x 1 KiB CHR banks (16 KiB total); stamp first byte of each 1K bank with (0xE0 + bank)
    cart.chr_rom.resize(16U * 0x0400U, 0x00U);
    for (uint8_t c = 0U; c < 16U; ++c) {
        cart.chr_rom[static_cast<size_t>(c) * 0x0400U] = static_cast<uint8_t>(0xE0U + c);
    }

    Bus bus;
    bus.load_cartridge(cart);

    // Select R6 = bank 3, R7 = bank 4 in PRG mode 0 ($8000 = R6, $A000 = R7, $C000 = 6, $E000 = 7)
    bus.write(0x8000U, 6U);
    bus.write(0x8001U, 3U);
    bus.write(0x8000U, 7U);
    bus.write(0x8001U, 4U);
    assert(bus.read(0x8000U) == 0xB3U);
    assert(bus.read(0xA000U) == 0xB4U);
    assert(bus.read(0xC000U) == 0xB6U);
    assert(bus.read(0xE000U) == 0xB7U);

    // Switch to PRG mode 1 ($8000 bit 6 = 1): $8000 fixed to second-last (6), $C000 = R6 (3)
    bus.write(0x8000U, 0x46U);
    assert(bus.read(0x8000U) == 0xB6U);
    assert(bus.read(0xA000U) == 0xB4U);
    assert(bus.read(0xC000U) == 0xB3U);
    assert(bus.read(0xE000U) == 0xB7U);

    // Test CHR banking in mode 0 (R0=2K at $0000, R1=2K at $0800, R2..R5=1K at $1000..$1C00)
    bus.write(0x8000U, 0U);
    bus.write(0x8001U, 4U);  // R0 -> 1K banks 4, 5
    bus.write(0x8000U, 2U);
    bus.write(0x8001U, 9U);  // R2 -> 1K bank 9 at $1000
    assert(bus.ppu().read_vram(0x0000U) == 0xE4U);
    assert(bus.ppu().read_vram(0x0400U) == 0xE5U);
    assert(bus.ppu().read_vram(0x1000U) == 0xE9U);

    // Test CHR A12 inversion ($8000 bit 7 = 1): R2 moves to $0000, R0 moves to $1000
    bus.write(0x8000U, 0x80U);
    assert(bus.ppu().read_vram(0x0000U) == 0xE9U);
    assert(bus.ppu().read_vram(0x1000U) == 0xE4U);
    assert(bus.ppu().read_vram(0x1400U) == 0xE5U);

    // Test $A000 mirroring toggle (0 = Vertical, 1 = Horizontal)
    bus.write(0xA000U, 0x01U);
    assert(bus.ppu().mirroring() == Mirroring::Horizontal);
    bus.write(0xA000U, 0x00U);
    assert(bus.ppu().mirroring() == Mirroring::Vertical);

    // Test MMC3 scanline IRQ ($C000 latch = 4, $C001 reload, $E001 enable):
    // Enable PPU rendering ($2001 = $18) and BG pattern table at $1000 ($2000 = $10).
    bus.ppu().reset();
    bus.write(0x2000U, 0x10U);
    bus.write(0x2001U, 0x18U);
    bus.write(0xE000U, 0x00U);  // Disable & clear IRQ
    bus.write(0xC000U, 4U);     // Latch = 4
    bus.write(0xC001U, 0x00U);  // Request reload on next A12 clock
    bus.write(0xE001U, 0x00U);  // Enable IRQ
    assert(!bus.mapper_irq_pending());

    // Scanline 0 dot 260 -> reloads counter to 4
    // Scanline 1 dot 260 -> counter = 3
    // Scanline 2 dot 260 -> counter = 2
    // Scanline 3 dot 260 -> counter = 1
    // Scanline 4 dot 260 -> counter = 0 -> IRQ fires!
    bus.ppu().step_dots(4ULL * Ppu::kDotsPerScanline + 259ULL);
    assert(bus.ppu().scanline() == 4U && bus.ppu().dot() == 259U);
    assert(!bus.mapper_irq_pending());

    // Step 1 dot to (scanline 4, dot 260) -> MMC3 IRQ must assert on this exact dot!
    bus.ppu().step_dot();
    assert(bus.ppu().scanline() == 4U && bus.ppu().dot() == 260U);
    assert(bus.mapper_irq_pending());
    assert(bus.irq_pending());

    // Writing to $E000 acknowledges and clears the MMC3 IRQ
    bus.write(0xE000U, 0x00U);
    assert(!bus.mapper_irq_pending());
}

void test_mapper189_prg32k_and_mmc3_chr_irq() {
    // Mapper 189 (TXC/MMC3 variant, e.g. Street Fighter II: The World Warrior):
    // a single 32 KiB PRG bank at $8000-$FFFF selected via $4100-$5FFF, with
    // pure MMC3 CHR banking and scanline IRQ. https://www.nesdev.org/wiki/INES_Mapper_189
    Cartridge cart{};
    cart.mapper = 189U;
    cart.mirroring = Mirroring::Vertical;
    // 4 x 32 KiB PRG banks (128 KiB total); stamp first byte of each 32K bank
    // with (0x90 + bank) and the $C000 half's first byte with (0xA0 + bank).
    cart.prg_rom.resize(4U * 0x8000U, 0x00U);
    for (uint8_t b = 0U; b < 4U; ++b) {
        cart.prg_rom[static_cast<size_t>(b) * 0x8000U] = static_cast<uint8_t>(0x90U + b);
        cart.prg_rom[static_cast<size_t>(b) * 0x8000U + 0x4000U] = static_cast<uint8_t>(0xA0U + b);
    }
    // 16 x 1 KiB CHR banks; stamp first byte of each 1K bank with (0xE0 + bank).
    cart.chr_rom.resize(16U * 0x0400U, 0x00U);
    for (uint8_t c = 0U; c < 16U; ++c) {
        cart.chr_rom[static_cast<size_t>(c) * 0x0400U] = static_cast<uint8_t>(0xE0U + c);
    }

    Bus bus;
    bus.load_cartridge(cart);

    // Reset defaults to 32K bank 0.
    assert(bus.read(0x8000U) == 0x90U);
    assert(bus.read(0xC000U) == 0xA0U);

    // The bank number is the OR of the written value's two nibbles. $11, $22, $33
    // select banks 1, 2, 3 (both nibbles agree); $30 and $03 both select bank 3.
    for (uint8_t b = 0U; b < 4U; ++b) {
        const uint8_t written = static_cast<uint8_t>((b << 4U) | b);
        bus.write(0x4120U, written);  // register lives in $4100-$5FFF
        assert(bus.read(0x8000U) == static_cast<uint8_t>(0x90U + b));
        assert(bus.read(0xC000U) == static_cast<uint8_t>(0xA0U + b));
    }
    bus.write(0x4120U, 0x30U);
    assert(bus.read(0x8000U) == 0x93U);
    bus.write(0x4120U, 0x03U);
    assert(bus.read(0x8000U) == 0x93U);

    // MMC3 PRG register writes ($8000/$8001) must NOT disturb the 32K PRG bank.
    bus.write(0x4120U, 0x00U);  // bank 0
    bus.write(0x8000U, 6U);
    bus.write(0x8001U, 2U);  // would move R6 under MMC3; ignored for PRG here
    assert(bus.read(0x8000U) == 0x90U);
    assert(bus.read(0xC000U) == 0xA0U);

    // CHR banking is pure MMC3 (R2 = 1K bank at $1000 in mode 0).
    bus.write(0x8000U, 0U);
    bus.write(0x8001U, 4U);  // R0 -> 2K at $0000 (1K banks 4,5)
    bus.write(0x8000U, 2U);
    bus.write(0x8001U, 9U);  // R2 -> 1K bank 9 at $1000
    assert(bus.ppu().read_vram(0x0000U) == 0xE4U);
    assert(bus.ppu().read_vram(0x1000U) == 0xE9U);

    // Scanline IRQ works like MMC3 (needed by SF2 for its status-bar split).
    bus.ppu().reset();
    bus.write(0x2000U, 0x10U);
    bus.write(0x2001U, 0x18U);
    bus.write(0xE000U, 0x00U);
    bus.write(0xC000U, 4U);
    bus.write(0xC001U, 0x00U);
    bus.write(0xE001U, 0x00U);
    bus.ppu().step_dots(4ULL * Ppu::kDotsPerScanline + 259ULL);
    assert(!bus.mapper_irq_pending());
    bus.ppu().step_dot();
    assert(bus.mapper_irq_pending());
    bus.write(0xE000U, 0x00U);
    assert(!bus.mapper_irq_pending());
}

void test_mapper40_banking_and_irq() {
    // Mapper 40 (NTDEC 2722 / SMB2J pirate):
    // 8 x 8 KiB PRG banks (64 KiB total).
    // Bank 6 at $6000, Bank 4 at $8000, Bank 5 at $A000, switchable bank at $C000, Bank 7 at $E000.
    Cartridge cart{};
    cart.mapper = 40U;
    cart.mirroring = Mirroring::Vertical;
    cart.prg_rom.resize(8U * 0x2000U, 0x00U);
    for (uint8_t b = 0U; b < 8U; ++b) {
        cart.prg_rom[static_cast<size_t>(b) * 0x2000U] = static_cast<uint8_t>(0x40U + b);
    }
    cart.chr_rom.resize(0x2000U, 0xCCU);

    Bus bus;
    bus.load_cartridge(cart);

    // Verify initial bank layout:
    // $6000: Bank 6
    assert(bus.read(0x6000U) == 0x46U);
    assert(bus.peek(0x6000U) == 0x46U);
    // Writes to $6000 are ignored (ROM)
    bus.write(0x6000U, 0x00U);
    assert(bus.read(0x6000U) == 0x46U);

    // $8000: Bank 4
    assert(bus.read(0x8000U) == 0x44U);
    // $A000: Bank 5
    assert(bus.read(0xA000U) == 0x45U);
    // $C000: Bank 0 initially
    assert(bus.read(0xC000U) == 0x40U);
    // $E000: Bank 7
    assert(bus.read(0xE000U) == 0x47U);

    // Switch $C000 bank via write to $E000
    bus.write(0xE000U, 2U);
    assert(bus.read(0xC000U) == 0x42U);
    bus.write(0xFFFFU, 3U);
    assert(bus.read(0xC000U) == 0x43U);

    // Test IRQ timer:
    assert(!bus.mapper_irq_pending());
    // Write $A000 enables IRQ and starts 4096-cycle timer
    bus.write(0xA000U, 0x00U);
    assert(!bus.mapper_irq_pending());

    // Advance 4000 cycles -> still not fired
    bus.sync_ppu(4000ULL);
    assert(!bus.mapper_irq_pending());

    // Advance to 4096 cycles -> IRQ fired!
    bus.sync_ppu(4096ULL);
    assert(bus.mapper_irq_pending());
    assert(bus.irq_pending());

    // Write to $C000 acknowledges IRQ
    bus.write(0xC000U, 0x00U);
    assert(!bus.mapper_irq_pending());

    // Re-enable and advance past 4096 to check $8000 disable
    bus.write(0xA000U, 0x00U);
    bus.sync_ppu(4096ULL + 4096ULL);
    assert(bus.mapper_irq_pending());
    bus.write(0x8000U, 0x00U);
    assert(!bus.mapper_irq_pending());
}

void test_interpreter_only_emits_no_functions() {
    // A minimal NROM image with a real routine at RESET so the recompiler would
    // normally emit at least one function.
    std::vector<uint8_t> prg(0x8000, 0x02U);  // KIL filler
    // RESET handler at $8000: LDA #$00; STA $00; RTI
    const std::array<uint8_t, 6> code{0xA9U, 0x00U, 0x85U, 0x00U, 0x40U, 0x00U};
    std::memcpy(prg.data(), code.data(), code.size());
    prg[0x7FFA] = 0x00;
    prg[0x7FFB] = 0x80;  // NMI -> $8000
    prg[0x7FFC] = 0x00;
    prg[0x7FFD] = 0x80;  // RESET -> $8000
    prg[0x7FFE] = 0x00;
    prg[0x7FFF] = 0x80;  // IRQ -> $8000

    GameConfig cfg;
    cfg.mapper = 0;
    // Default (recompiled) emits at least one function.
    assert(!analyze_prg_rom(prg, cfg).functions.empty());
    // interpreter_only emits none, so the native build runs the interpreter.
    cfg.interpreter_only = true;
    const ProgramCFG io_cfg = analyze_prg_rom(prg, cfg);
    assert(io_cfg.functions.empty());
    assert(io_cfg.bank_cfgs.empty());
}

}  // namespace

int main() {
    test_mapper1_mmc1_serial_prg_chr_and_mirroring();
    test_mapper2_uxrom_banking_and_recompiled_dispatch();
    test_mapper3_cnrom_chr_switching();
    test_mapper4_mmc3_banking_and_scanline_irq();
    test_mapper189_prg32k_and_mmc3_chr_irq();
    test_mapper40_banking_and_irq();
    test_interpreter_only_emits_no_functions();
    std::printf("nes_mapper_unit_tests: all Mapper 1/2/3/4/40/189 + interpreter_only tests passed\n");
    return 0;
}
