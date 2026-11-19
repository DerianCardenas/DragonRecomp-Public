#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include "dragon/config.h"
#include "sms/cartridge.h"
#include "smsrecomp/disassembler.h"
#include "smsrecomp/emitter.h"

using namespace dragon::sms;
using namespace dragon::smsrecomp;

namespace {

std::string read_file_str(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void test_disasm_and_emit() {
    std::cout << "[Test] smsrecomp Z80 CFG disassembly & C99 emission...\n";

    std::vector<uint8_t> rom(32768, 0x00U);
    // $0000: DI; LD SP, $DFF0; CALL $0100; HALT; JR -2
    const uint8_t reset_code[] = {
        0xF3,                    // DI
        0x31, 0xF0, 0xDF,        // LD SP, $DFF0
        0xCD, 0x00, 0x01,        // CALL $0100
        0x76,                    // HALT
        0x18, 0xFD,              // JR $0007
    };
    std::memcpy(rom.data(), reset_code, sizeof(reset_code));

    // $0038: IN A, ($BF); RETI
    rom[0x0038] = 0xDB; rom[0x0039] = 0xBF;
    rom[0x003A] = 0xED; rom[0x003B] = 0x4D;

    // $0066: RETN
    rom[0x0066] = 0xED; rom[0x0067] = 0x45;

    // $0100: LD B, 4; LD A, 0; L_0104: ADD A, B; DJNZ L_0104; LD ($C000), A; RET
    const uint8_t sub_code[] = {
        0x06, 0x04,              // LD B, 4
        0x3E, 0x00,              // LD A, 0
        0x80,                    // ADD A, B
        0x10, 0xFD,              // DJNZ $0104
        0x32, 0x00, 0xC0,        // LD ($C000), A
        0xC9,                    // RET
    };
    std::memcpy(rom.data() + 0x0100, sub_code, sizeof(sub_code));
    std::memcpy(rom.data() + 0x7FF0, "TMR SEGA", 8);

    Cartridge cart;
    assert(cart.load_from_bytes(std::move(rom)));

    dragon::GameConfig cfg;
    cfg.name = "SyntheticSMS";
    cfg.platform = "sms";

    const DisassemblyResult disasm = disassemble_rom(cart, cfg);
    assert(disasm.functions.size() == 4U);  // $0000, $0038, $0066, $0100
    assert(disasm.total_instructions == 14U);

    const auto out_dir = std::filesystem::temp_directory_path() / "dragon_sms_recomp_test";
    std::filesystem::remove_all(out_dir);

    EmitOptions opts;
    opts.output_dir = out_dir;
    opts.single_file = true;

    const EmitResult eres = emit_c99(disasm, opts);
    assert(eres.success);
    assert(eres.functions_emitted == 4U);
    assert(std::filesystem::exists(out_dir / "recomp_functions.h"));
    assert(std::filesystem::exists(out_dir / "recomp_all.c"));

    const std::string c_src = read_file_str(out_dir / "recomp_all.c");
    assert(c_src.find("void func_00_0000(SmsCpuState* cpu)") != std::string::npos);
    assert(c_src.find("void func_00_0038(SmsCpuState* cpu)") != std::string::npos);
    assert(c_src.find("void func_00_0066(SmsCpuState* cpu)") != std::string::npos);
    assert(c_src.find("void func_00_0100(SmsCpuState* cpu)") != std::string::npos);
    assert(c_src.find("L_0104:") != std::string::npos);
    assert(c_src.find("kRecompDispatchTableCount = 4U;") != std::string::npos);

    std::filesystem::remove_all(out_dir);
}

void test_synthetic_indirect_call_resume() {
    std::cout << "[Test] smsrecomp synthetic indirect call (PUSH DE; JP (HL)) resume...\n";

    std::vector<uint8_t> rom(32768, 0x00U);
    // $0000: DI; RET
    rom[0x0000] = 0xF3;
    rom[0x0001] = 0xC9;

    // $0038 (WWF Wrestlemania-style IRQ handler):
    // PUSH AF; PUSH HL; PUSH DE; LD HL, ($DFEE); LD A, H; OR L; JR Z, $0047;
    // LD DE, $004B; PUSH DE; JP (HL);
    // $0047: IN A, ($BF); IN A, ($C1);
    // $004B: POP DE; POP HL; POP AF; NOP; EI; RET
    const uint8_t irq_code[] = {
        0xF5, 0xE5, 0xD5, 0x2A, 0xEE, 0xDF, 0x7C, 0xB5, 0x28, 0x05,
        0x11, 0x4B, 0x00, 0xD5, 0xE9,
        0xDB, 0xBF, 0xDB, 0xC1,
        0xD1, 0xE1, 0xF1, 0x00, 0xFB, 0xC9,
    };
    std::memcpy(rom.data() + 0x0038, irq_code, sizeof(irq_code));
    rom[0x0066] = 0xED; rom[0x0067] = 0x45;
    std::memcpy(rom.data() + 0x7FF0, "TMR SEGA", 8);

    Cartridge cart;
    assert(cart.load_from_bytes(std::move(rom)));

    dragon::GameConfig cfg;
    cfg.name = "SyntheticIndirectCallSMS";
    cfg.platform = "sms";

    const DisassemblyResult disasm = disassemble_rom(cart, cfg);
    const auto out_dir = std::filesystem::temp_directory_path() / "dragon_sms_indirect_test";
    std::filesystem::remove_all(out_dir);

    EmitOptions opts;
    opts.output_dir = out_dir;
    opts.single_file = true;
    const EmitResult eres = emit_c99(disasm, opts);
    assert(eres.success);

    const std::string c_src = read_file_str(out_dir / "recomp_all.c");
    assert(c_src.find("L_tail_resume:") != std::string::npos);
    assert(c_src.find("case 0x004BU: goto L_004B;") != std::string::npos);
    assert(c_src.find("L_004B:") != std::string::npos);

    std::filesystem::remove_all(out_dir);
}

}  // namespace

int main() {
    test_disasm_and_emit();
    test_synthetic_indirect_call_resume();
    std::cout << "All smsrecomp unit tests passed!\n";
    return 0;
}
