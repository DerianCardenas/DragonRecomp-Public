#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include "dragon/config.h"
#include "pce/cartridge.h"
#include "pcerecomp/disassembler.h"
#include "pcerecomp/emitter.h"

using namespace dragon::pce;
using namespace dragon::pcerecomp;

namespace {

std::string read_file_str(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void test_disasm_and_c99_emit() {
    std::cout << "[Test] pcerecomp HuC6280 CFG disassembly & C99 emission...\n";

    std::vector<uint8_t> rom(16384, 0xEAU);  // 2 banks of 8 KiB
    // Bank 0 ($E000): SEI; CSH; LDA #$01; TAM #$04 (MPR2=bank 1); ST0 #$05; ST1 #$CC; ST2 #$00;
    // JSR $4000 (cross-bank discovery!); L_loop: BRA L_loop
    const uint8_t reset_code[] = {
        0x78,              // SEI
        0xD4,              // CSH
        0xA9, 0x01,        // LDA #$01
        0x53, 0x04,        // TAM #$04 (MPR2 = 1 -> $4000-$5FFF)
        0x03, 0x05,        // ST0 #$05
        0x13, 0xCC,        // ST1 #$CC
        0x23, 0x00,        // ST2 #$00
        0x20, 0x00, 0x40,  // JSR $4000
        0x80, 0xFE,        // BRA -2 (L_E00F)
    };
    std::memcpy(rom.data(), reset_code, sizeof(reset_code));

    // IRQ1 at $E050: LDA $0000; RTI
    const uint8_t irq1_code[] = {0xAD, 0x00, 0x00, 0x40};
    std::memcpy(rom.data() + 0x0050, irq1_code, sizeof(irq1_code));

    // Bank 1 ($4000): TII $4100 -> $2200, len 16; RTS
    const uint8_t bank1_sub[] = {
        0x73, 0x00, 0x41, 0x00, 0x22, 0x10, 0x00,  // TII
        0x60,                                      // RTS
    };
    std::memcpy(rom.data() + 8192, bank1_sub, sizeof(bank1_sub));

    // Vectors in bank 0 (unused vectors set to $0000)
    rom[0x1FF6] = 0x00; rom[0x1FF7] = 0x00;  // IRQ2 unused
    rom[0x1FF8] = 0x50; rom[0x1FF9] = 0xE0;  // IRQ1 -> $E050
    rom[0x1FFA] = 0x00; rom[0x1FFB] = 0x00;  // TIQ unused
    rom[0x1FFC] = 0x00; rom[0x1FFD] = 0x00;  // NMI unused
    rom[0x1FFE] = 0x00; rom[0x1FFF] = 0xE0;  // RESET -> $E000

    Cartridge cart;
    assert(cart.load_from_bytes(std::move(rom)));

    dragon::GameConfig cfg;
    cfg.name = "SyntheticPCE";
    cfg.platform = "pce";

    const DisassemblyResult disasm = disassemble_rom(cart, cfg);
    // Must discover RESET ($00:$E000), IRQ1 ($00:$E050), AND cross-bank $01:$4000 via LDA #$01; TAM #$04!
    assert(disasm.functions.size() == 3U);

    const auto out_dir = std::filesystem::temp_directory_path() / "dragon_pce_recomp_test";
    std::filesystem::remove_all(out_dir);

    EmitOptions opts;
    opts.output_dir = out_dir;
    opts.single_file = true;
    const EmitResult eres = emit_c99(disasm, opts);
    assert(eres.success);
    assert(eres.functions_emitted == 3U);

    const std::string c_src = read_file_str(out_dir / "recomp_all.c");
    assert(c_src.find("void func_00_E000(PceCpuState* cpu)") != std::string::npos);
    assert(c_src.find("void func_00_E050(PceCpuState* cpu)") != std::string::npos);
    assert(c_src.find("void func_01_4000(PceCpuState* cpu)") != std::string::npos);
    assert(c_src.find("pce_st0(cpu, 0x05U)") != std::string::npos);
    assert(c_src.find("pce_tam(cpu, 0x04U)") != std::string::npos);
    assert(c_src.find("kRecompDispatchTableCount = 3U;") != std::string::npos);

    std::filesystem::remove_all(out_dir);
}

}  // namespace

int main() {
    test_disasm_and_c99_emit();
    std::cout << "All pcerecomp unit tests passed!\n";
    return 0;
}
