#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "dragon/config.h"
#include "gba/bus.h"
#include "gba/cartridge.h"
#include "gba/cpu_state.h"
#include "gba/interpreter.h"
#include "gba/runtime.h"
#include "gbarecomp/disassembler.h"
#include "gbarecomp/emitter.h"

namespace {

void write32_le(std::vector<uint8_t>& rom, uint32_t offset, uint32_t val) {
    rom[offset + 0] = static_cast<uint8_t>(val & 0xFFU);
    rom[offset + 1] = static_cast<uint8_t>((val >> 8) & 0xFFU);
    rom[offset + 2] = static_cast<uint8_t>((val >> 16) & 0xFFU);
    rom[offset + 3] = static_cast<uint8_t>((val >> 24) & 0xFFU);
}

void write16_le(std::vector<uint8_t>& rom, uint32_t offset, uint16_t val) {
    rom[offset + 0] = static_cast<uint8_t>(val & 0xFFU);
    rom[offset + 1] = static_cast<uint8_t>((val >> 8) & 0xFFU);
}

void test_multicase_disassembly_and_c99_emission() {
    std::vector<uint8_t> rom(0x4000, 0);

    // 0x08000000: B 0x080000C0 (ARM entry point)
    write32_le(rom, 0x00, 0xEA00002EU);

    // 0x080000C0 (ARM startup):
    // LDR R0, [PC, #16]   (E59F0010 -> loads from 0xC0+8+16 = 0xD8: IRQ handler 0x08000300)
    // LDR R1, [PC, #16]   (E59F1010 -> loads from 0xC4+8+16 = 0xDC: 0x03007FFC)
    // STR R0, [R1]        (E5810000 -> registers IRQ handler at [0x03007FFC])
    // LDR R0, [PC, #12]   (E59F000C -> loads from 0xCC+8+12 = 0xE0: Thumb main 0x08000101)
    // BX R0               (E12FFF10 -> interworking jump to Thumb AgbMain at 0x08000100)
    // B .                 (EAFFFFFE)
    write32_le(rom, 0xC0, 0xE59F0010U);
    write32_le(rom, 0xC4, 0xE59F1010U);
    write32_le(rom, 0xC8, 0xE5810000U);
    write32_le(rom, 0xCC, 0xE59F000CU);
    write32_le(rom, 0xD0, 0xE12FFF10U);
    write32_le(rom, 0xD4, 0xEAFFFFFEU);
    // Literal pool:
    write32_le(rom, 0xD8, 0x08000300U);  // ARM IRQ handler at 0x08000300
    write32_le(rom, 0xDC, 0x03007FFCU);  // BIOS IRQ vector pointer
    write32_le(rom, 0xE0, 0x08000101U);  // Thumb AgbMain at 0x08000100 (bit 0 = 1)

    // 0x08000100 (Thumb AgbMain):
    // PUSH {R4, LR}       (B510)
    // LDR R0, [PC, #16]   (4804 -> loads from (0x102+4)&~2 + 16 = 0x114: 0x08000221)
    // BL 0x08000200       (F000 F87C -> calls _call_via_r0 at 0x08000200)
    // BL 0x08000210       (F000 F882 -> calls agbcc pop_r3_bx_r3 leaf at 0x08000210)
    // POP {R4, PC}        (BD10)
    write16_le(rom, 0x100, 0xB510U);
    write16_le(rom, 0x102, 0x4804U);
    write16_le(rom, 0x104, 0xF000U);
    write16_le(rom, 0x106, 0xF87CU);
    write16_le(rom, 0x108, 0xF000U);
    write16_le(rom, 0x10A, 0xF882U);
    write16_le(rom, 0x10C, 0xBD10U);
    write32_le(rom, 0x114, 0x08000221U);

    // 0x08000200 (Thumb _call_via_r0 veneer):
    // BX R0               (4700)
    write16_le(rom, 0x200, 0x4700U);

    // 0x08000210 (Thumb agbcc return-via-POP-{R3}-BX-R3 function):
    // PUSH {LR}           (B500)
    // MOV R0, #77         (204D)
    // POP {R3}            (BC08)
    // BX R3               (4718)
    write16_le(rom, 0x210, 0xB500U);
    write16_le(rom, 0x212, 0x204DU);
    write16_le(rom, 0x214, 0xBC08U);
    write16_le(rom, 0x216, 0x4718U);

    // 0x08000220 (Thumb switch function using __gnu_thumb1_case_uqi):
    // CMP R0, #2          (2802)
    // BHI 0x08000234      (D806: PC=0x222+4=0x226 + 12 = 0x232)
    // BL 0x08000240       (F000 F80B: PC=0x224+4=0x228 + 22 = 0x23E -> wait: (0x240 - 0x228)/2 = 12
    // = 0x0C -> F80C)
    write16_le(rom, 0x220, 0x2802U);
    write16_le(rom, 0x222, 0xD805U);  // target: 0x226 + 10 = 0x230
    write16_le(rom, 0x224, 0xF000U);
    write16_le(rom, 0x226, 0xF80CU);  // BL 0x08000240 (__gnu_thumb1_case_uqi)
    // Inline 3-byte offset table at 0x08000228:
    // base = 0x08000228; offsets in halfwords:
    // [0] -> +2 halfwords (0x0800022C)
    // [1] -> +3 halfwords (0x0800022E)
    // [2] -> +4 halfwords (0x08000230)
    rom[0x228] = 2U;
    rom[0x229] = 3U;
    rom[0x22A] = 4U;
    rom[0x22B] = 0U;                  // padding
    write16_le(rom, 0x22C, 0x200AU);  // Case 0: MOV R0, #10
    write16_le(rom, 0x22E, 0x2014U);  // Case 1: MOV R0, #20
    write16_le(rom, 0x230, 0x4770U);  // Case 2 / default: BX LR

    // 0x08000240 (__gnu_thumb1_case_uqi signature):
    // PUSH {R1}           (B402)
    // MOV R1, LR          (4671)
    // SUB R1, #1          (3901)
    // LDRB R0, [R1, R0]   (5C08)
    // LSL R0, R0, #1      (0040)
    // ADD LR, R1, R0      (4486)
    // POP {R1}            (BC02)
    // BX LR               (4770)
    write16_le(rom, 0x240, 0xB402U);
    write16_le(rom, 0x242, 0x4671U);
    write16_le(rom, 0x244, 0x3901U);
    write16_le(rom, 0x246, 0x5C08U);
    write16_le(rom, 0x248, 0x0040U);
    write16_le(rom, 0x24A, 0x4486U);
    write16_le(rom, 0x24C, 0xBC02U);
    write16_le(rom, 0x24E, 0x4770U);

    // 0x08000300 (ARM IRQ handler with ARM jump table):
    // CMP R0, #1                  (E3500001)
    // LDRLS PC, [PC, R0, LSL #2]  (979FF100)
    // BX LR                       (E12FFF1E)
    // .word 0x08000314            (at 0x30C)
    // .word 0x08000318            (at 0x310)
    // 0x08000314: MOV R0, #1; BX LR
    write32_le(rom, 0x300, 0xE3500001U);
    write32_le(rom, 0x304, 0x979FF100U);
    write32_le(rom, 0x308, 0xE12FFF1EU);
    write32_le(rom, 0x30C, 0x08000314U);
    write32_le(rom, 0x310, 0x08000318U);
    write32_le(rom, 0x314, 0xE3A00001U);
    write32_le(rom, 0x318, 0xE12FFF1EU);

    dragon::GameConfig config;
    config.name = "GbaMultiCaseTest";
    config.platform = "gba";

    const auto cfg = dragon::gbarecomp::disassemble_gba(rom, config);

    // Verify all multi-case entry points were discovered automatically:
    // 1. ARM entry point at 0x08000000
    assert(cfg.find_function(0x08000000U, false) != nullptr);
    // 2. Thumb AgbMain at 0x08000100 (discovered via literal pool + BX R0)
    assert(cfg.find_function(0x08000100U, true) != nullptr);
    // 3. Thumb _call_via_r0 at 0x08000200 (discovered via BL)
    assert(cfg.find_function(0x08000200U, true) != nullptr);
    // 4. Thumb agbcc leaf at 0x08000210 (discovered via BL)
    assert(cfg.find_function(0x08000210U, true) != nullptr);
    // 5. Thumb switch function at 0x08000220 (discovered via literal pool 0x08000221)
    const auto* switch_fn = cfg.find_function(0x08000220U, true);
    assert(switch_fn != nullptr);
    // Verify __gnu_thumb1_case_uqi inline table cases (0x0800022C, 0x0800022E, 0x08000230)
    // were decoded as instructions inside switch_fn!
    assert(switch_fn->instructions.contains(0x0800022CU));
    assert(switch_fn->instructions.contains(0x0800022EU));
    assert(switch_fn->instructions.contains(0x08000230U));
    // 6. ARM IRQ handler at 0x08000300 (discovered via STR to 0x03007FFC)
    const auto* irq_fn = cfg.find_function(0x08000300U, false);
    assert(irq_fn != nullptr);
    // Verify ARM jump table targets 0x08000314 and 0x08000318 were decoded inside irq_fn!
    assert(irq_fn->instructions.contains(0x08000314U));
    assert(irq_fn->instructions.contains(0x08000318U));

    // Emit C99 (chunked mode) and write to a temporary directory
    config.split_every = 2;
    const auto out_dir = std::filesystem::temp_directory_path() / "gba_recomp_unit_test_out";
    std::filesystem::remove_all(out_dir);
    const auto files = dragon::gbarecomp::write_generated_output(cfg, config, out_dir);
    assert(files.size() >= 2U);

    // Verify the generated C99 code contains expected symbols and dispatch table
    const auto gen = dragon::gbarecomp::emit_c99(cfg, config);
    assert(gen.header_content.find("func_08000000_arm") != std::string::npos);
    assert(gen.header_content.find("func_08000100_thumb") != std::string::npos);
    assert(gen.header_content.find("func_08000300_arm") != std::string::npos);
    assert(gen.dispatch_source.find("kRecompDispatchTable") != std::string::npos);
    // Verify BL __gnu_thumb1_case_uqi emits unconditional goto dispatch_pc because inline data
    // follows at 0x08000228 instead of a Thumb instruction!
    bool found_switch_dispatch = false;
    for (const auto& chunk : gen.source_chunks) {
        if (chunk.find("func_08000220_thumb") != std::string::npos) {
            assert(chunk.find("|| 1) { goto dispatch_pc; }") != std::string::npos);
            found_switch_dispatch = true;
        }
    }
    assert(found_switch_dispatch);

#ifdef DRAGON_SOURCE_DIR
    // Verify the emitted C99 code compiles cleanly under strict -std=c99 -Wall -Wextra -Werror
    const std::string inc_dir = std::string(DRAGON_SOURCE_DIR) + "/gba/runtime/include";
    std::string cmd = "cc -std=c99 -Wall -Wextra -Werror -fsyntax-only -I\"" + inc_dir + "\" -I\"" +
                      out_dir.string() + "\"";
    for (const auto& f : files) {
        if (f.extension() == ".c") {
            cmd += " \"" + f.string() + "\"";
        }
    }
    const int rc = std::system(cmd.c_str());
    assert(rc == 0);
#endif

    std::filesystem::remove_all(out_dir);
}

void test_no_runaway_disassembly_on_data_and_padding() {
    // Fill a 64 KB ROM with 0xFF padding to simulate unused Flash/ROM space
    std::vector<uint8_t> rom(0x10000, 0xFFU);

    // 0x08000000: B 0x080000C0
    write32_le(rom, 0x00, 0xEA00002EU);

    // 0x080000C0:
    // LDR R0, [PC, #12]   (E59F000C -> loads ROM data pointer 0x08001000 from 0xD4)
    // LDR R1, [PC, #12]   (E59F100C -> loads DMA3SAD 0x040000D4 from 0xD8)
    // STR R0, [R1]        (E5810000 -> stores ROM data pointer to DMA3SAD, NOT 0x03007FFC!)
    // BX LR               (E12FFF1E)
    write32_le(rom, 0xC0, 0xE59F000CU);
    write32_le(rom, 0xC4, 0xE59F100CU);
    write32_le(rom, 0xC8, 0xE5810000U);
    write32_le(rom, 0xCC, 0xE12FFF1EU);
    write32_le(rom, 0xD4, 0x08001000U);  // Graphics/audio data address
    write32_le(rom, 0xD8, 0x040000D4U);  // DMA3SAD register

    // Place fake ARM branch-looking words at 0x08001000 (simulating graphics/sound data)
    write32_le(rom, 0x1000, 0x0A000100U);
    write32_le(rom, 0x1004, 0xEB000200U);

    dragon::GameConfig config;
    config.name = "GbaDataGuardTest";
    config.platform = "gba";

    const auto cfg = dragon::gbarecomp::disassemble_gba(rom, config);

    // Data pointer stored to DMA3SAD must NOT be seeded as a function
    assert(cfg.find_function(0x08001000U, false) == nullptr);
    assert(cfg.find_function(0x08001000U, true) == nullptr);
    assert(cfg.functions.size() == 1U);

    // Total instructions across all functions must never exceed decoded blocks
    size_t total_insts = 0;
    for (const auto& [k, fn] : cfg.functions) {
        (void)k;
        total_insts += fn.instructions.size();
    }
    assert(total_insts == 5U);
}

void test_demote_consecutive_learned_entry_points() {
    std::vector<uint8_t> rom(0x1000, 0x00U);

    // 0x08000000: B 0x08000080 (ARM)
    write32_le(rom, 0x00, 0xEA00001EU);

    // 0x08000080: BX PC (switch to Thumb at 0x88)
    write32_le(rom, 0x80, 0xE12FFF1FU);
    write32_le(rom, 0x84, 0xE1A00000U);  // NOP / padding

    // 0x08000088: Thumb function
    // 0x088: PUSH {R4, LR} (0xB510)
    // 0x08A: MOV R0, #1    (0x2001)
    // 0x08C: MOV R1, #2    (0x2102)
    // 0x08E: POP {R4, PC}  (0xBD10)
    write16_le(rom, 0x88, 0xB510U);
    write16_le(rom, 0x8A, 0x2001U);
    write16_le(rom, 0x8C, 0x2102U);
    write16_le(rom, 0x8E, 0xBD10U);

    dragon::GameConfig config;
    config.name = "DemoteLearnedTest";
    config.platform = "gba";

    // Simulate entry points polluted by sequential fallback logging
    dragon::EntryPoint ep1;
    ep1.addr = 0x08000088U;
    ep1.thumb = true;
    ep1.has_thumb = true;
    config.entry_points.push_back(ep1);

    dragon::EntryPoint ep2;
    ep2.addr = 0x0800008AU;
    ep2.thumb = true;
    ep2.has_thumb = true;
    config.entry_points.push_back(ep2);

    dragon::EntryPoint ep3;
    ep3.addr = 0x0800008CU;
    ep3.thumb = true;
    ep3.has_thumb = true;
    config.entry_points.push_back(ep3);

    const auto cfg = dragon::gbarecomp::disassemble_gba(rom, config);

    // Mid-block entries 0x0800008A and 0x0800008C must be demoted so the function remains unified
    assert(cfg.find_function(0x08000088U, true) != nullptr);
    assert(cfg.find_function(0x0800008AU, true) == nullptr);
    assert(cfg.find_function(0x0800008CU, true) == nullptr);
    const auto* fn = cfg.find_function(0x08000088U, true);
    assert(fn->instructions.size() == 4U);
}

}  // namespace

int main() {
    test_multicase_disassembly_and_c99_emission();
    test_no_runaway_disassembly_on_data_and_padding();
    test_demote_consecutive_learned_entry_points();
    std::printf("All GBA recompiler unit tests passed.\n");
    return 0;
}
