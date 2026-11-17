#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dragon/config.h"
#include "gbrecomp/disassembler.h"
#include "gbrecomp/emitter.h"

namespace {

using dragon::GameConfig;
using dragon::gbrecomp::disassemble_game_boy;
using dragon::gbrecomp::emit_c99;
using dragon::gbrecomp::ProgramCFG;

void test_disassembly_and_cfg() {
    // Construct 32 KiB synthetic Game Boy ROM
    std::vector<uint8_t> rom(32768, 0x00);
    // Header
    rom[0x0147] = 0x00;  // ROM-Only
    rom[0x0148] = 0x00;  // 32 KiB

    // At $0100 (entry point):
    // 0x0100: NOP (0x00)
    // 0x0101: JP $0150 (0xC3, 0x50, 0x01)
    rom[0x0100] = 0x00;
    rom[0x0101] = 0xC3;
    rom[0x0102] = 0x50;
    rom[0x0103] = 0x01;

    // At $0150:
    // 0x0150: LD A, $42 (0x3E, 0x42)
    // 0x0152: CALL $0200 (0xCD, 0x00, 0x02)
    // 0x0155: RET (0xC9)
    rom[0x0150] = 0x3E;
    rom[0x0151] = 0x42;
    rom[0x0152] = 0xCD;
    rom[0x0153] = 0x00;
    rom[0x0154] = 0x02;
    rom[0x0155] = 0xC9;

    // At $0200:
    // 0x0200: INC A (0x3C)
    // 0x0201: RET (0xC9)
    rom[0x0200] = 0x3C;
    rom[0x0201] = 0xC9;

    GameConfig config;
    ProgramCFG cfg = disassemble_game_boy(rom, config);

    assert(cfg.banks.count(0) != 0);
    const auto& bank0 = cfg.banks.at(0);

    // Verify functions discovered
    assert(bank0.functions.count(0x0100) != 0);
    assert(bank0.functions.count(0x0200) != 0);  // Called via CALL $0200

    // Verify 0x0150 is a basic block reachable by func_0100
    assert(bank0.blocks.count(0x0150) != 0);
    const auto& f0100 = bank0.functions.at(0x0100);
    assert(f0100.block_addrs.count(0x0150) != 0);

    std::printf("test_disassembly_and_cfg: PASS\n");
}

void test_c99_emission() {
    std::vector<uint8_t> rom(32768, 0x00);
    rom[0x0100] = 0x00;  // NOP
    rom[0x0101] = 0xC9;  // RET

    GameConfig config;
    ProgramCFG cfg = disassemble_game_boy(rom, config);

    auto files = emit_c99(cfg, config);

    // Header check
    assert(files.header_content.find("void func_0100(CpuState* cpu);") != std::string::npos);
    assert(files.header_content.find("kRecompDispatchTable") != std::string::npos);

    // Source check
    assert(!files.source_chunks.empty());
    const auto& src = files.source_chunks[0];
    assert(src.find("void func_0100(CpuState* cpu)") != std::string::npos);
    assert(src.find("L_0100:") != std::string::npos);
    assert(src.find("cpu->cycles += 4;") != std::string::npos);

    // Dispatch check
    assert(files.dispatch_source.find("{ 0x0100U, 0U, func_0100 }") != std::string::npos);

    std::printf("test_c99_emission: PASS\n");
}

}  // namespace

int main() {
    std::printf("=== Running Game Boy Recompiler Unit Tests ===\n");
    test_disassembly_and_cfg();
    test_c99_emission();
    std::printf("=== All Game Boy Recompiler Unit Tests Passed (2/2) ===\n");
    return 0;
}
