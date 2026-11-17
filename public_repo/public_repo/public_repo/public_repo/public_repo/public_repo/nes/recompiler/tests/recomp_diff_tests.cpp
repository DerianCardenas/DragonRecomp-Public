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
#include "nes/runtime.h"
#include "nesrecomp/disassembler.h"
#include "nesrecomp/emitter.h"

namespace {

using dragon::GameConfig;
using dragon::nes::Bus;
using dragon::nes::cpu_init;
using dragon::nes::cpu_step;
using dragon::nesrecomp::analyze_prg_rom;
using dragon::nesrecomp::EmitOptions;
using dragon::nesrecomp::ProgramCFG;
using dragon::nesrecomp::write_generated_output;

struct Snippet {
    uint16_t addr;
    std::vector<uint8_t> bytes;
};

void run_differential_test(const char* test_name, std::initializer_list<Snippet> snippets,
                           const GameConfig& cfg, bool enable_flag_opt = true) {
    // 1. Construct a 32 KiB PRG ROM image ($8000-$FFFF) with RESET=$8000, NMI=$8000, IRQ=$8000.
    std::vector<uint8_t> prg(0x8000, 0x02U);  // Fill unused bytes with KIL ($02)
    for (const auto& s : snippets) {
        assert(s.addr >= 0x8000U);
        const size_t off = s.addr - 0x8000U;
        assert(off + s.bytes.size() <= prg.size());
        std::memcpy(prg.data() + off, s.bytes.data(), s.bytes.size());
    }
    // Vectors at $FFFA-$FFFF -> $8000
    prg[0x7FFA] = 0x00;
    prg[0x7FFB] = 0x80;
    prg[0x7FFC] = 0x00;
    prg[0x7FFD] = 0x80;
    prg[0x7FFE] = 0x00;
    prg[0x7FFF] = 0x80;

    // 2. Disassemble and emit C99 files into a temporary directory under build/.
    const ProgramCFG prog_cfg = analyze_prg_rom(prg, cfg);
    assert(!prog_cfg.functions.empty());

    const std::filesystem::path out_dir =
        std::filesystem::path(DRAGON_BINARY_DIR) / "diff_test_gen" / test_name;
    std::filesystem::remove_all(out_dir);

    EmitOptions opts;
    opts.enable_flag_opt = enable_flag_opt;
    opts.split_every = 50;
    write_generated_output(prog_cfg, cfg, out_dir, opts);

    // 3. Compile the generated C99 sources into a shared library with -std=c99 -Wall -Wextra
    // -Werror.
    const std::filesystem::path so_path = out_dir / "libtest_recomp.so";
    std::string cmd = std::string(DRAGON_C_COMPILER) +
                      " -std=c99 -Wall -Wextra -Wpedantic -Werror -O1 -fPIC -shared"
                      " -I\"" +
                      std::string(DRAGON_RUNTIME_INCLUDE_DIR) + "\" -I\"" + out_dir.string() +
                      "\" \"" + (out_dir / "recomp_funcs_0.c").string() + "\" \"" +
                      (out_dir / "recomp_dispatch.c").string() + "\" -o \"" + so_path.string() +
                      "\"";
#if defined(_WIN32)
    cmd += " \"" + std::string(DRAGON_BINARY_DIR) + "/nes/runtime/libnes_runtime.a\" \"" +
           std::string(DRAGON_BINARY_DIR) + "/common/libdragon_common.a\" -lstdc++";
#endif
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::fprintf(stderr, "Failed to compile generated C99 for test '%s':\n  %s\n", test_name,
                     cmd.c_str());
    }
    assert(rc == 0);

    // 4. Load the compiled C99 shared library and resolve kRecompDispatchTable + func_8000.
    void* handle = dlopen(so_path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
    }
    assert(handle != nullptr);

    const auto* table = static_cast<const RecompFuncEntry*>(dlsym(handle, "kRecompDispatchTable"));
    const auto* table_count =
        static_cast<const size_t*>(dlsym(handle, "kRecompDispatchTableCount"));
    RecompFunc entry_fn = nullptr;
    void* fn_sym = dlsym(handle, "func_8000");
    std::memcpy(&entry_fn, &fn_sym, sizeof(entry_fn));
    assert(table != nullptr && table_count != nullptr && entry_fn != nullptr);

    // 5. Run the interpreter from $8000 until it returns from top-level RTS (S > $FD).
    Bus interp_bus;
    interp_bus.load_prg_rom(prg);
    CpuState interp_cpu{};
    cpu_init(interp_cpu, &interp_bus);
    interp_cpu.pc = 0x8000U;
    interp_cpu.s = 0xFDU;
    // Place return sentinel $FFFF - 1 ($FFFE) on the stack at $01FE-$01FF
    interp_bus.write(0x01FE, 0xFE);
    interp_bus.write(0x01FF, 0xFF);
    const uint8_t init_s = interp_cpu.s;

    while (!interp_cpu.halted) {
        const uint8_t op = interp_bus.read(interp_cpu.pc);
        const auto mnem = dragon::nes::get_opcode_info(op).mnemonic;
        cpu_step(interp_cpu);
        if (mnem == dragon::nes::Mnemonic::RTS && static_cast<int8_t>(interp_cpu.s - init_s) > 0) {
            break;
        }
    }

    // 6. Run the recompiled C99 function func_8000 on an identical initial state.
    Bus recomp_bus;
    recomp_bus.load_prg_rom(prg);
    recomp_bus.set_dispatch_table(table, *table_count);
    CpuState recomp_cpu{};
    cpu_init(recomp_cpu, &recomp_bus);
    recomp_cpu.pc = 0x8000U;
    recomp_cpu.s = 0xFDU;
    recomp_bus.write(0x01FE, 0xFE);
    recomp_bus.write(0x01FF, 0xFF);

    entry_fn(&recomp_cpu);

    // 7. Compare registers, packed flags, cycles, and all 2 KiB of internal RAM!
    const uint8_t interp_p = cpu_pack_flags(&interp_cpu, 0U);
    const uint8_t recomp_p = cpu_pack_flags(&recomp_cpu, 0U);

    if (interp_cpu.a != recomp_cpu.a || interp_cpu.x != recomp_cpu.x ||
        interp_cpu.y != recomp_cpu.y || interp_cpu.s != recomp_cpu.s || interp_p != recomp_p ||
        interp_cpu.cycles != recomp_cpu.cycles) {
        std::fprintf(stderr,
                     "Differential mismatch in '%s':\n"
                     "  interp: A=%02X X=%02X Y=%02X S=%02X P=%02X CYC=%llu\n"
                     "  recomp: A=%02X X=%02X Y=%02X S=%02X P=%02X CYC=%llu\n",
                     test_name, interp_cpu.a, interp_cpu.x, interp_cpu.y, interp_cpu.s, interp_p,
                     static_cast<unsigned long long>(interp_cpu.cycles), recomp_cpu.a, recomp_cpu.x,
                     recomp_cpu.y, recomp_cpu.s, recomp_p,
                     static_cast<unsigned long long>(recomp_cpu.cycles));
        assert(false);
    }

    for (uint16_t addr = 0; addr < 0x0800U; ++addr) {
        if (interp_bus.read(addr) != recomp_bus.read(addr)) {
            std::fprintf(stderr, "RAM mismatch in '%s' at $%04X: interp=%02X recomp=%02X\n",
                         test_name, addr, interp_bus.read(addr), recomp_bus.read(addr));
            assert(false);
        }
    }

    dlclose(handle);
}

void test_alu_shifts_and_loops() {
    GameConfig cfg;
    cfg.name = "alu_loops";

    // Compute sum of 1..10, shift results, test ADC/SBC/BIT/CMP/RMW in RAM, and store to ZP.
    Snippet main_code{
        0x8000,
        {
            0x18,  // CLC
            0xA2,
            0x0A,  // LDX #$0A
            0xA9,
            0x00,  // LDA #$00
            0x85,
            0x10,  // STA $10
            // loop ($8007):
            0x86,
            0x11,  // STX $11
            0xA5,
            0x10,  // LDA $10
            0x18,  // CLC
            0x65,
            0x11,  // ADC $11
            0x85,
            0x10,  // STA $10
            0xCA,  // DEX
            0xD0,
            0xF4,  // BNE $8007
            // RMW & shifts:
            0x06,
            0x10,  // ASL $10 (55 * 2 = 110)
            0xE6,
            0x10,  // INC $10 (111)
            0xC6,
            0x11,  // DEC $11 (1 - 1 = 0)
            0xA5,
            0x10,  // LDA $10
            0x4A,  // LSR A   (55, C=1)
            0x6A,  // ROR A   (0x80 | 27 = 155)
            0x85,
            0x12,  // STA $12
            0x24,
            0x12,  // BIT $12
            0x60,  // RTS
        },
    };

    run_differential_test("alu_loops_opt", {main_code}, cfg, true);
    run_differential_test("alu_loops_no_opt", {main_code}, cfg, false);
}

void test_jsr_rts_and_rts_trick() {
    GameConfig cfg;
    cfg.name = "jsr_and_rts_trick";
    cfg.entry_points.push_back({0x8200, 0, "RtsTrickTarget"});

    // $8000 calls $8100 via normal JSR, then calls $8150 which uses PHA; PHA; RTS to jump to $8200!
    Snippet s_main{
        0x8000,
        {
            0xA9,
            0x10,  // LDA #$10
            0x20,
            0x00,
            0x81,  // JSR $8100
            0x20,
            0x50,
            0x81,  // JSR $8150
            0x85,
            0x20,  // STA $20
            0x60,  // RTS
        },
    };
    Snippet s_sub1{
        0x8100,
        {
            0x18,  // CLC
            0x69,
            0x22,  // ADC #$22
            0x60,  // RTS
        },
    };
    // $8150 pushes ($8200 - 1) = $81FF and executes RTS!
    Snippet s_rts_trick{
        0x8150,
        {
            0x48,  // PHA (save A)
            0xA9,
            0x81,  // LDA #$81 (hi of $81FF)
            0x48,  // PHA
            0xA9,
            0xFF,  // LDA #$FF (lo of $81FF)
            0x48,  // PHA
            0x60,  // RTS -> jumps to $8200!
        },
    };
    Snippet s_target{
        0x8200,
        {
            0x68,  // PLA (restore A)
            0x18,  // CLC
            0x69,
            0x05,  // ADC #$05
            0x60,  // RTS -> returns to $8008!
        },
    };

    run_differential_test("jsr_and_rts_trick", {s_main, s_sub1, s_rts_trick, s_target}, cfg);
}

void test_smb_style_jump_engine() {
    GameConfig cfg;
    cfg.name = "jump_engine_test";
    cfg.jump_engine.push_back(0x8E04);

    // $8000 calls $8020 with A=0,1,2; $8020 uses JSR $8E04 followed by inline table!
    Snippet s_main{
        0x8000,
        {
            0xA9,
            0x00,  // LDA #$00
            0x20,
            0x20,
            0x80,  // JSR $8020
            0xA9,
            0x01,  // LDA #$01
            0x20,
            0x20,
            0x80,  // JSR $8020
            0xA9,
            0x02,  // LDA #$02
            0x20,
            0x20,
            0x80,  // JSR $8020
            0x60,  // RTS
        },
    };
    Snippet s_dispatch_caller{
        0x8020,
        {
            0x20,
            0x04,
            0x8E,  // JSR $8E04 (JumpEngine)
            0x30,
            0x80,  // .word $8030
            0x40,
            0x80,  // .word $8040
            0x50,
            0x80,  // .word $8050
        },
    };
    Snippet s_case0{0x8030, {0xA2, 0x11, 0x86, 0x30, 0x60}};  // LDX #$11; STX $30; RTS
    Snippet s_case1{0x8040, {0xA2, 0x22, 0x86, 0x31, 0x60}};  // LDX #$22; STX $31; RTS
    Snippet s_case2{0x8050, {0xA2, 0x33, 0x86, 0x32, 0x60}};  // LDX #$33; STX $32; RTS

    // Exact SMB JumpEngine implementation at $8E04:
    Snippet s_jump_engine{
        0x8E04,
        {
            0x0A,              // ASL A
            0xA8,              // TAY
            0x68,              // PLA
            0x85, 0x04,        // STA $04
            0x68,              // PLA
            0x85, 0x05,        // STA $05
            0xC8,              // INY
            0xB1, 0x04,        // LDA ($04),Y
            0x85, 0x06,        // STA $06
            0xC8,              // INY
            0xB1, 0x04,        // LDA ($04),Y
            0x85, 0x07,        // STA $07
            0x6C, 0x06, 0x00,  // JMP ($0006)
        },
    };

    run_differential_test("smb_jump_engine",
                          {s_main, s_dispatch_caller, s_case0, s_case1, s_case2, s_jump_engine},
                          cfg);
}

void test_page_crossing_and_indexed_modes() {
    GameConfig cfg;
    cfg.name = "page_cross_test";

    // Test (zp,X), (zp),Y with and without page crossing, and abs,X / abs,Y.
    Snippet s_main{
        0x8000,
        {
            // Store pointer $83F0 at $0010-$0011
            0xA9, 0xF0,        // LDA #$F0
            0x85, 0x10,        // STA $10
            0xA9, 0x83,        // LDA #$83
            0x85, 0x11,        // STA $11
            0xA0, 0x05,        // LDY #$05 (no page cross: $83F5)
            0xB1, 0x10,        // LDA ($10),Y
            0x85, 0x40,        // STA $40
            0xA0, 0x15,        // LDY #$15 (page cross: $8405)
            0xB1, 0x10,        // LDA ($10),Y
            0x85, 0x41,        // STA $41
            0xA2, 0x15,        // LDX #$15
            0xBD, 0xF0, 0x83,  // LDA $83F0,X (page cross: $8405)
            0x85, 0x42,        // STA $42
            0x60,              // RTS
        },
    };
    Snippet s_data1{0x83F5, {0xAB}};
    Snippet s_data2{0x8405, {0xCD}};

    run_differential_test("page_cross_modes", {s_main, s_data1, s_data2}, cfg);
}

void test_nmi_and_rti_in_recompiled_loop() {
    GameConfig cfg;
    cfg.name = "nmi_rti_loop_test";

    std::vector<uint8_t> prg(0x8000, 0x02U);
    // $8000 (RESET):
    //   LDA #$80; STA $2000 (enable VBlank NMI)
    //   LDA #$00; STA $10   (NMI counter = 0)
    //   LDX #$42; LDY #$99
    // loop ($800D):
    //   LDA $10; CMP #$03; BCC $800D
    //   STX $11; STY $12; RTS
    const std::vector<uint8_t> main_bytes = {
        0xA9,
        0x80,  // LDA #$80
        0x8D,
        0x00,
        0x20,  // STA $2000
        0xA9,
        0x00,  // LDA #$00
        0x85,
        0x10,  // STA $10
        0xA2,
        0x42,  // LDX #$42
        0xA0,
        0x99,  // LDY #$99
        // $800D:
        0xA5,
        0x10,  // LDA $10
        0xC9,
        0x03,  // CMP #$03
        0x90,
        0xFA,  // BCC $800D
        0x86,
        0x11,  // STX $11
        0x84,
        0x12,  // STY $12
        0x60,  // RTS
    };
    // $8100 (NMI):
    //   PHA; TXA; PHA; TYA; PHA
    //   INC $10
    //   LDA $2002
    //   LDX #$FF; LDY #$FE; CLC
    //   PLA; TAY; PLA; TAX; PLA
    //   RTI
    const std::vector<uint8_t> nmi_bytes = {
        0x48,              // PHA
        0x8A,              // TXA
        0x48,              // PHA
        0x98,              // TYA
        0x48,              // PHA
        0xE6, 0x10,        // INC $10
        0xAD, 0x02, 0x20,  // LDA $2002
        0xA2, 0xFF,        // LDX #$FF
        0xA0, 0xFE,        // LDY #$FE
        0x18,              // CLC
        0x68,              // PLA
        0xA8,              // TAY
        0x68,              // PLA
        0xAA,              // TAX
        0x68,              // PLA
        0x40,              // RTI
    };
    std::memcpy(prg.data() + 0x0000U, main_bytes.data(), main_bytes.size());
    std::memcpy(prg.data() + 0x0100U, nmi_bytes.data(), nmi_bytes.size());
    // Vectors: NMI=$8100, RESET=$8000, IRQ=$8000
    prg[0x7FFA] = 0x00;
    prg[0x7FFB] = 0x81;
    prg[0x7FFC] = 0x00;
    prg[0x7FFD] = 0x80;
    prg[0x7FFE] = 0x00;
    prg[0x7FFF] = 0x80;

    const ProgramCFG prog_cfg = analyze_prg_rom(prg, cfg);
    const std::filesystem::path out_dir =
        std::filesystem::path(DRAGON_BINARY_DIR) / "diff_test_gen" / "nmi_rti_loop";
    std::filesystem::remove_all(out_dir);

    EmitOptions opts;
    opts.enable_flag_opt = true;
    opts.split_every = 50;
    write_generated_output(prog_cfg, cfg, out_dir, opts);

    const std::filesystem::path so_path = out_dir / "libtest_nmi.so";
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
    const auto* table = static_cast<const RecompFuncEntry*>(dlsym(handle, "kRecompDispatchTable"));
    const auto* table_count =
        static_cast<const size_t*>(dlsym(handle, "kRecompDispatchTableCount"));
    RecompFunc entry_fn = nullptr;
    void* fn_sym = dlsym(handle, "func_8000");
    std::memcpy(&entry_fn, &fn_sym, sizeof(entry_fn));
    assert(table != nullptr && table_count != nullptr && entry_fn != nullptr);

    dragon::nes::Cartridge cart{};
    cart.prg_rom = prg;
    cart.chr_rom.resize(0x2000, 0U);

    // Run recompiled code with PPU enabled
    Bus recomp_bus;
    recomp_bus.load_cartridge(cart);
    recomp_bus.set_dispatch_table(table, *table_count);
    CpuState recomp_cpu{};
    cpu_init(recomp_cpu, &recomp_bus);
    recomp_cpu.pc = 0x8000U;
    recomp_cpu.s = 0xFDU;
    recomp_bus.write(0x01FE, 0xFE);
    recomp_bus.write(0x01FF, 0xFF);

    entry_fn(&recomp_cpu);

    assert(recomp_bus.fallback_count() == 0);
    assert(recomp_bus.read(0x0010) == 0x03U);
    assert(recomp_bus.read(0x0011) == 0x42U);
    assert(recomp_bus.read(0x0012) == 0x99U);
    assert(recomp_cpu.s == 0xFFU);
    assert(recomp_bus.completed_frames() == 3ULL);

    // Run interpreter on the same cartridge and compare state
    Bus interp_bus;
    interp_bus.load_cartridge(cart);
    CpuState interp_cpu{};
    cpu_init(interp_cpu, &interp_bus);
    interp_cpu.pc = 0x8000U;
    interp_cpu.s = 0xFDU;
    interp_bus.write(0x01FE, 0xFE);
    interp_bus.write(0x01FF, 0xFF);
    const uint8_t init_s = interp_cpu.s;

    while (!interp_cpu.halted) {
        const uint8_t op = interp_bus.read(interp_cpu.pc);
        const auto mnem = dragon::nes::get_opcode_info(op).mnemonic;
        dragon::nes::cpu_step_with_ppu(interp_cpu);
        if (mnem == dragon::nes::Mnemonic::RTS && static_cast<int8_t>(interp_cpu.s - init_s) > 0) {
            break;
        }
    }

    assert(interp_bus.read(0x0010) == 0x03U);
    assert(interp_bus.read(0x0011) == 0x42U);
    assert(interp_bus.read(0x0012) == 0x99U);
    assert(interp_cpu.s == recomp_cpu.s);
    assert(interp_cpu.a == recomp_cpu.a);
    assert(interp_cpu.x == recomp_cpu.x);
    assert(interp_cpu.y == recomp_cpu.y);
    assert(cpu_pack_flags(&interp_cpu, 0U) == cpu_pack_flags(&recomp_cpu, 0U));

    for (uint16_t addr = 0; addr < 0x0800U; ++addr) {
        // Dead stack bytes below S ($0100..$0100+S) hold the return PC of the last NMI, which
        // may be $800D, $800F, or $8011 in the interpreter vs $800D at the backward branch poll.
        if (addr >= 0x0100U && addr <= static_cast<uint16_t>(0x0100U + recomp_cpu.s)) {
            continue;
        }
        assert(interp_bus.read(addr) == recomp_bus.read(addr));
    }

    dlclose(handle);
}

}  // namespace

int main() {
    test_alu_shifts_and_loops();
    test_jsr_rts_and_rts_trick();
    test_smb_style_jump_engine();
    test_page_crossing_and_indexed_modes();
    test_nmi_and_rti_in_recompiled_loop();
    std::printf("nes_recomp_diff_tests: all differential tests passed\n");
    return 0;
}
