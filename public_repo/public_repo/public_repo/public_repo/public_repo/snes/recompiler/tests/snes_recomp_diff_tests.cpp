#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>
#include "dragon/dynlib.h"

#include "dragon/config.h"
#include "snes/runtime.h"
#include "snesrecomp/disassembler.h"
#include "snesrecomp/emitter.h"

namespace {

using dragon::GameConfig;
using dragon::snes::Bus;
using dragon::snes::Cartridge;
using dragon::snes::cpu_init;
using dragon::snes::cpu_reset;
using dragon::snes::cpu_step;
using dragon::snes::MapMode;
using dragon::snes::parse_snes_rom;
using dragon::snesrecomp::analyze_snes_rom;
using dragon::snesrecomp::EmitOptions;
using dragon::snesrecomp::ProgramCFG;
using dragon::snesrecomp::write_generated_output;

struct Snippet {
    uint8_t bank;
    uint16_t addr;
    std::vector<uint8_t> bytes;
};

void run_snes_differential_test(const char* test_name, std::initializer_list<Snippet> snippets,
                                const GameConfig& cfg) {
    // 1. Construct a 64 KiB LoROM image (2 banks: $00 and $01, mapped at $8000-$FFFF each).
    std::vector<uint8_t> rom(0x10000, 0xDBU);  // Fill unused bytes with STP ($DB)
    for (const auto& s : snippets) {
        assert(s.addr >= 0x8000U);
        const size_t rom_off =
            (static_cast<size_t>(s.bank & 0x7FU) * 0x8000U) + static_cast<size_t>(s.addr - 0x8000U);
        assert(rom_off + s.bytes.size() <= rom.size());
        std::memcpy(rom.data() + rom_off, s.bytes.data(), s.bytes.size());
    }

    // Internal LoROM header at $7FC0 with valid checksum and RESET=$8000
    const char* title = "SNES DIFF TEST       ";
    std::memcpy(rom.data() + 0x7FC0, title, 21);
    rom[0x7FD5] = 0x20U;  // LoROM
    rom[0x7FD7] = 0x06U;  // 64 KiB
    rom[0x7FDC] = 0x00U;
    rom[0x7FDD] = 0xFFU;
    rom[0x7FDE] = 0xFFU;
    rom[0x7FDF] = 0x00U;
    rom[0x7FFC] = 0x00U;
    rom[0x7FFD] = 0x80U;

    const Cartridge cart = parse_snes_rom(rom, {}, "lorom");
    const ProgramCFG prog_cfg = analyze_snes_rom(cart, cfg);
    assert(!prog_cfg.functions.empty());

    const std::filesystem::path out_dir =
        std::filesystem::path(DRAGON_BINARY_DIR) / "snes_diff_test_gen" / test_name;
    std::filesystem::remove_all(out_dir);

    EmitOptions opts;
    opts.enable_flag_opt = true;
    opts.split_every = 100;
    write_generated_output(prog_cfg, cfg, out_dir, opts);

    // 2. Compile generated C99 into a shared library with -std=c99 -Wall -Wextra -Wpedantic -Werror
    const std::filesystem::path so_path = out_dir / "libtest_snes_recomp.so";
    std::string cmd = std::string(DRAGON_C_COMPILER) +
                      " -std=c99 -Wall -Wextra -Wpedantic -Werror -O1 -fPIC -shared"
                      " -I\"" +
                      std::string(DRAGON_RUNTIME_INCLUDE_DIR) + "\" -I\"" + out_dir.string() +
                      "\" \"" + (out_dir / "recomp_funcs_0.c").string() + "\" \"" +
                      (out_dir / "recomp_dispatch.c").string() + "\" -o \"" + so_path.string() +
                      "\"";
#if defined(_WIN32)
    cmd += " \"" + std::string(DRAGON_BINARY_DIR) + "/snes/runtime/libsnes_runtime.a\" \"" +
           std::string(DRAGON_BINARY_DIR) + "/common/libdragon_common.a\" -lstdc++";
#endif
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::fprintf(stderr, "Failed to compile generated C99 for SNES test '%s':\n  %s\n",
                     test_name, cmd.c_str());
    }
    assert(rc == 0);

    // 3. Load shared library and resolve kRecompDispatchTable + func_00_8000_e1
    void* handle = dlopen(so_path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
    }
    assert(handle != nullptr);

    const auto* table = static_cast<const RecompFuncEntry*>(dlsym(handle, "kRecompDispatchTable"));
    const auto* table_count =
        static_cast<const size_t*>(dlsym(handle, "kRecompDispatchTableCount"));
    RecompFunc entry_fn = nullptr;
    void* fn_sym = dlsym(handle, "func_00_8000_e1");
    std::memcpy(&entry_fn, &fn_sym, sizeof(entry_fn));
    assert(table != nullptr && table_count != nullptr && entry_fn != nullptr);

    // 4. Run 65816 interpreter from reset until STP (cpu.halted != 0)
    Bus interp_bus;
    interp_bus.load_cartridge(cart);
    CpuState interp_cpu{};
    cpu_init(interp_cpu, &interp_bus);
    cpu_reset(interp_cpu);

    while (!interp_cpu.halted && !interp_cpu.waiting) {
        cpu_step(interp_cpu);
    }

    // 5. Run recompiled C99 function func_00_8000_e1 from identical reset state
    const auto* stack_tricks =
        static_cast<const uint32_t*>(dlsym(handle, "kRecompStackTrickTargets"));
    const auto* stack_trick_count =
        static_cast<const size_t*>(dlsym(handle, "kRecompStackTrickTargetCount"));
    Bus recomp_bus;
    recomp_bus.load_cartridge(cart);
    recomp_bus.set_dispatch_table(table, *table_count);
    if (stack_tricks != nullptr && stack_trick_count != nullptr) {
        recomp_bus.set_stack_trick_targets(stack_tricks, *stack_trick_count);
    }
    CpuState recomp_cpu{};
    cpu_init(recomp_cpu, &recomp_bus);
    cpu_reset(recomp_cpu);

    entry_fn(&recomp_cpu);

    assert(recomp_bus.fallback_count() == 0U);

    // 6. Compare all registers, flags, master_cycles, and all 128 KiB of WRAM!
    const uint8_t interp_p = cpu_pack_flags(&interp_cpu, 0U);
    const uint8_t recomp_p = cpu_pack_flags(&recomp_cpu, 0U);

    if (interp_cpu.a != recomp_cpu.a || interp_cpu.x != recomp_cpu.x ||
        interp_cpu.y != recomp_cpu.y || interp_cpu.s != recomp_cpu.s ||
        interp_cpu.d != recomp_cpu.d || interp_cpu.db != recomp_cpu.db ||
        interp_cpu.pb != recomp_cpu.pb || interp_cpu.pc != recomp_cpu.pc ||
        interp_cpu.e != recomp_cpu.e || interp_p != recomp_p ||
        interp_cpu.master_cycles != recomp_cpu.master_cycles) {
        std::fprintf(stderr,
                     "SNES Differential mismatch in '%s':\n"
                     "  interp: A=%04X X=%04X Y=%04X S=%04X D=%04X DB=%02X PB=%02X PC=%04X E=%u "
                     "P=%02X CYC=%llu\n"
                     "  recomp: A=%04X X=%04X Y=%04X S=%04X D=%04X DB=%02X PB=%02X PC=%04X E=%u "
                     "P=%02X CYC=%llu\n",
                     test_name, interp_cpu.a, interp_cpu.x, interp_cpu.y, interp_cpu.s,
                     interp_cpu.d, interp_cpu.db, interp_cpu.pb, interp_cpu.pc, interp_cpu.e,
                     interp_p, static_cast<unsigned long long>(interp_cpu.master_cycles),
                     recomp_cpu.a, recomp_cpu.x, recomp_cpu.y, recomp_cpu.s, recomp_cpu.d,
                     recomp_cpu.db, recomp_cpu.pb, recomp_cpu.pc, recomp_cpu.e, recomp_p,
                     static_cast<unsigned long long>(recomp_cpu.master_cycles));
        assert(false);
    }

    if (std::memcmp(interp_bus.wram_data(), recomp_bus.wram_data(), Bus::kWramSize) != 0) {
        for (size_t i = 0; i < Bus::kWramSize; ++i) {
            if (interp_bus.wram_data()[i] != recomp_bus.wram_data()[i]) {
                std::fprintf(stderr,
                             "WRAM mismatch in '%s' at offset $%05zX: interp=%02X recomp=%02X\n",
                             test_name, i, interp_bus.wram_data()[i], recomp_bus.wram_data()[i]);
                break;
            }
        }
        assert(false);
    }

    dlclose(handle);
}

void test_mode_transitions_and_16bit_loops() {
    GameConfig cfg;
    cfg.name = "mode_transitions_16bit_loops";

    // Switch to native mode (CLC; XCE; REP #$30), run a 16-bit loop summing 1..16,
    // test 16-bit shifts/RMW, test SEP #$10 X/Y truncation, test XBA/TCD/TCS.
    Snippet s_main{
        0x00,
        0x8000,
        {
            0x18,
            0xFB,  // CLC; XCE
            0xC2,
            0x30,  // REP #$30 (m=0, xf=0)
            0xA9,
            0xFF,
            0x1F,
            0x1B,  // LDA #$1FFF; TCS
            0xA2,
            0x10,
            0x00,  // LDX #$0010
            0xA9,
            0x00,
            0x00,  // LDA #$0000
            0x85,
            0x10,  // STA $10
            // loop ($8010):
            0x86,
            0x12,  // STX $12
            0x18,  // CLC
            0xA5,
            0x10,  // LDA $10
            0x65,
            0x12,  // ADC $12
            0x85,
            0x10,  // STA $10
            0xCA,  // DEX
            0xD0,
            0xF4,  // BNE $8010
            // 16-bit RMW & shifts:
            0x06,
            0x10,  // ASL $10 (136 * 2 = 272)
            0xE6,
            0x10,  // INC $10 (273)
            0xA2,
            0x34,
            0x12,  // LDX #$1234
            0xA0,
            0x78,
            0x56,  // LDY #$5678
            0xE2,
            0x10,  // SEP #$10 (xf=1 -> truncates X=$0034, Y=$0078)
            0x86,
            0x14,  // STX $14
            0x84,
            0x15,  // STY $15
            0xA9,
            0xCD,
            0xAB,  // LDA #$ABCD (still 16-bit M!)
            0xEB,  // XBA -> $CDAB
            0x85,
            0x16,  // STA $16
            0xDB,  // STP
        },
    };

    run_snes_differential_test("mode_transitions_16bit_loops", {s_main}, cfg);
}

void test_bcd_decimal_diff() {
    GameConfig cfg;
    cfg.name = "bcd_decimal_diff";

    Snippet s_main{
        0x00,
        0x8000,
        {
            0x18,
            0xFB,  // CLC; XCE
            0xF8,  // SED
            // 8-bit BCD additions & subtractions
            0x18,
            0xA9,
            0x78,  // CLC; LDA #$78
            0x69,
            0x45,  // ADC #$45 -> $23, C=1
            0x85,
            0x20,  // STA $20
            0xA9,
            0x32,
            0x18,  // LDA #$32; CLC (borrow)
            0xE9,
            0x15,  // SBC #$15 -> 32 - 15 - 1 = 16, C=1
            0x85,
            0x21,  // STA $21
            // 16-bit BCD additions & subtractions
            0xC2,
            0x20,  // REP #$20
            0x18,
            0xA9,
            0x89,
            0x67,  // CLC; LDA #$6789
            0x69,
            0x34,
            0x45,  // ADC #$4534 -> $1323, C=1
            0x85,
            0x22,  // STA $22
            0x38,
            0xA9,
            0x00,
            0x50,  // SEC; LDA #$5000
            0xE9,
            0x99,
            0x19,  // SBC #$1999 -> $3001, C=1
            0x85,
            0x24,  // STA $24
            0xD8,  // CLD
            0xDB,  // STP
        },
    };

    run_snes_differential_test("bcd_decimal_diff", {s_main}, cfg);
}

void test_jsl_rtl_and_mx_variants_at_same_address() {
    GameConfig cfg;
    cfg.name = "jsl_rtl_mx_variants";

    // Bank $00:8000 calls Bank $01:8000 FIRST in 16-bit mode (m=0, x=0 -> func_01_8000_m0x0),
    // then switches to 8-bit mode (SEP #$30) and calls the EXACT SAME address $01:8000
    // (m=1, x=1 -> func_01_8000_m1x1)!
    Snippet s_bank0{
        0x00,
        0x8000,
        {
            0x18, 0xFB,              // CLC; XCE
            0xC2, 0x30,              // REP #$30 (m=0, x=0)
            0xA9, 0xF0, 0x1F, 0x1B,  // LDA #$1FF0; TCS
            0xA9, 0x00, 0x10,        // LDA #$1000
            0x22, 0x00, 0x80, 0x01,  // JSL $018000 (calls func_01_8000_m0x0!)
            0x85, 0x30,              // STA $30 (16-bit store)
            0xE2, 0x30,              // SEP #$30 (m=1, x=1)
            0xA9, 0x20,              // LDA #$20 (8-bit immediate)
            0x22, 0x00, 0x80, 0x01,  // JSL $018000 (calls func_01_8000_m1x1!)
            0x85, 0x32,              // STA $32 (8-bit store)
            0x20, 0x00, 0x81,        // JSR $8100 (intra-bank JSR)
            0xDB,                    // STP
        },
    };
    Snippet s_bank0_sub{
        0x00,
        0x8100,
        {
            0xA2,
            0x77,  // LDX #$77
            0x86,
            0x33,  // STX $33
            0x60,  // RTS
        },
    };
    // Bank $01:8000:
    //   CLC
    //   ADC #imm (in m=0 this reads $05, $00 = #$0005, then 0xEA is NOP;
    //             in m=1 this reads $05 = #$05, then 0x00 would be BRK unless we use 0xEA, 0xEA!)
    //   Look: 18 (CLC), 69 EA EA (ADC #$EAEA in 16-bit, or ADC #$EA; NOP in 8-bit!), 6B (RTL)!
    Snippet s_bank1{
        0x01,
        0x8000,
        {
            0x18,  // CLC
            0x69,
            0xEA,
            0xEA,  // m=0: ADC #$EAEA; m=1: ADC #$EA followed by NOP ($EA)!
            0x6B,  // RTL
        },
    };

    run_snes_differential_test("jsl_rtl_mx_variants", {s_bank0, s_bank0_sub, s_bank1}, cfg);
}

void test_addressing_mvn_mul_div_and_dma_diff() {
    GameConfig cfg;
    cfg.name = "addr_mvn_mul_div_dma";

    // Test MVN block move, hardware multiply/divide, and DMA channel 0 to $2180 WRAM port
    Snippet s_main{
        0x00,
        0x8000,
        {
            0x18,
            0xFB,  // CLC; XCE
            // 1. Hardware multiply: 12 * 34 = 408 ($0198)
            0xA9,
            12,  // LDA #12
            0x8D,
            0x02,
            0x42,  // STA $4202
            0xA9,
            34,  // LDA #34
            0x8D,
            0x03,
            0x42,  // STA $4203
            0xC2,
            0x30,  // REP #$30
            0xAD,
            0x16,
            0x42,  // LDA $4216
            0x85,
            0x40,  // STA $40
            // 2. MVN 8 bytes from ROM $00:8500 to WRAM $7E:1200
            0xA9,
            0x07,
            0x00,  // LDA #$0007 (8 bytes)
            0xA2,
            0x00,
            0x85,  // LDX #$8500
            0xA0,
            0x00,
            0x12,  // LDY #$1200
            0x54,
            0x7E,
            0x00,  // MVN dst=$7E, src=$00 (also sets DB=$7E)
            0x4B,
            0xAB,  // PHK; PLB (restore DB=$00)
            // 3. DMA channel 0: copy 8 bytes from ROM $00:8500 to WRAM $7F:0020 via $2180
            0xE2,
            0x20,  // SEP #$20
            0xA9,
            0x20,  // LDA #$20
            0x8D,
            0x81,
            0x21,  // STA $2181 (WMADDL)
            0x9C,
            0x82,
            0x21,  // STZ $2182 (WMADDM)
            0xA9,
            0x01,  // LDA #$01
            0x8D,
            0x83,
            0x21,  // STA $2183 (WMADDH -> bank $7F)
            0x9C,
            0x00,
            0x43,  // STZ $4300 (DMA0 mode 0, A->B, increment)
            0xA9,
            0x80,  // LDA #$80
            0x8D,
            0x01,
            0x43,  // STA $4301 ($2180 WMDATA)
            0xA9,
            0x00,  // LDA #$00
            0x8D,
            0x02,
            0x43,  // STA $4302 (src lo = $00)
            0xA9,
            0x85,  // LDA #$85
            0x8D,
            0x03,
            0x43,  // STA $4303 (src hi = $85)
            0x9C,
            0x04,
            0x43,  // STZ $4304 (src bank = $00)
            0xA9,
            0x08,  // LDA #$08
            0x8D,
            0x05,
            0x43,  // STA $4305 (size lo = 8)
            0x9C,
            0x06,
            0x43,  // STZ $4306 (size hi = 0)
            0xA9,
            0x01,  // LDA #$01
            0x8D,
            0x0B,
            0x42,  // STA $420B (start DMA0!)
            0xDB,  // STP
        },
    };
    Snippet s_rom_data{
        0x00,
        0x8500,
        {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
    };

    run_snes_differential_test("addr_mvn_mul_div_dma", {s_main, s_rom_data}, cfg);
}

void test_interprocedural_mx_mode_and_php_plp_diff() {
    GameConfig cfg;
    cfg.name = "interproc_mx_php_plp";

    // Tests:
    // 1. Callee at $00:8100 switches from m=1,x=1 to m=0,x=0 (REP #$30) and returns via RTS.
    //    Caller at $00:8000 must continue disassembling after JSR $8100 with m=0,x=0 (16-bit
    //    immediates).
    // 2. Callee at $00:8200 saves mode via PHP, switches to m=1,x=1 (SEP #$30), then restores
    //    m=0,x=0 via PLP before RTS. Caller must remain in m=0,x=0.
    Snippet s_main{
        0x00,
        0x8000,
        {
            0x18, 0xFB,        // CLC; XCE (m=1, x=1)
            0x20, 0x00, 0x81,  // JSR $8100 (callee sets REP #$30 -> returns with m=0, x=0!)
            0xA9, 0x34, 0x12,  // LDA #$1234 (16-bit immediate because m=0!)
            0x85, 0x50,        // STA $50
            0x20, 0x00, 0x82,  // JSR $8200 (PHP; SEP #$30; LDA #$99; STA $52; PLP; RTS)
            0xA2, 0x78, 0x56,  // LDX #$5678 (16-bit immediate because PLP restored x=0!)
            0x86, 0x54,        // STX $54
            0xDB,              // STP
        },
    };
    Snippet s_set_16bit{
        0x00,
        0x8100,
        {
            0xC2,
            0x30,  // REP #$30 (m=0, x=0)
            0x60,  // RTS
        },
    };
    Snippet s_php_plp{
        0x00,
        0x8200,
        {
            0x08,  // PHP (saves m=0, x=0)
            0xE2,
            0x30,  // SEP #$30 (m=1, x=1)
            0xA9,
            0x99,  // LDA #$99 (8-bit immediate)
            0x85,
            0x52,  // STA $52
            0x28,  // PLP (restores m=0, x=0)
            0x60,  // RTS
        },
    };

    run_snes_differential_test("interproc_mx_php_plp", {s_main, s_set_16bit, s_php_plp}, cfg);
}

}  // namespace

int main() {
    test_mode_transitions_and_16bit_loops();
    test_bcd_decimal_diff();
    test_jsl_rtl_and_mx_variants_at_same_address();
    test_addressing_mvn_mul_div_and_dma_diff();
    test_interprocedural_mx_mode_and_php_plp_diff();
    std::printf("snes_recomp_diff_tests: all differential tests passed\n");
    return 0;
}
