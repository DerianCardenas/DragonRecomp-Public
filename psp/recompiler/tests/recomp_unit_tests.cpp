#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "dragon/config.h"
#include "psp/interpreter.h"
#include "psp/loader.h"
#include "psp/runtime.h"
#include "psprecomp/disassembler.h"
#include "psprecomp/emitter.h"

namespace {

using namespace dragon::psp;
using namespace dragon::psprecomp;

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

uint32_t enc_r(uint8_t op, uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct) {
    return (static_cast<uint32_t>(op) << 26U) | (static_cast<uint32_t>(rs) << 21U) |
           (static_cast<uint32_t>(rt) << 16U) | (static_cast<uint32_t>(rd) << 11U) |
           (static_cast<uint32_t>(sa) << 6U) | static_cast<uint32_t>(funct);
}

uint32_t enc_i(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (static_cast<uint32_t>(op) << 26U) | (static_cast<uint32_t>(rs) << 21U) |
           (static_cast<uint32_t>(rt) << 16U) | static_cast<uint32_t>(imm);
}

uint32_t enc_j(uint8_t op, uint32_t target) {
    return (static_cast<uint32_t>(op) << 26U) | ((target >> 2U) & 0x03FFFFFFU);
}

void put_u16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
    b[off] = static_cast<uint8_t>(v & 0xFFU);
    b[off + 1U] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
}

void put_u32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    b[off] = static_cast<uint8_t>(v & 0xFFU);
    b[off + 1U] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
    b[off + 2U] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
    b[off + 3U] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
}

// Build a minimal valid ELF32 MIPS binary with 1 PT_LOAD segment at 0x08804000
std::vector<uint8_t> build_synthetic_elf(uint32_t entry_va, const std::vector<uint32_t>& code) {
    const size_t code_off = 0x100U;
    const size_t code_bytes = code.size() * 4U;
    std::vector<uint8_t> elf(code_off + code_bytes, 0U);

    // ELF32 header (52 bytes)
    elf[0] = 0x7F;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 1;           // 32-bit
    elf[5] = 1;           // little-endian
    elf[6] = 1;           // version
    put_u16(elf, 16, 2);  // ET_EXEC
    put_u16(elf, 18, 8);  // EM_MIPS
    put_u32(elf, 20, 1);  // EV_CURRENT
    put_u32(elf, 24, entry_va);
    put_u32(elf, 28, 52);  // e_phoff
    put_u16(elf, 40, 52);  // e_ehsize
    put_u16(elf, 42, 32);  // e_phentsize
    put_u16(elf, 44, 1);   // e_phnum

    // Program header PT_LOAD (32 bytes at offset 52)
    put_u32(elf, 52 + 0, 1);  // PT_LOAD
    put_u32(elf, 52 + 4, static_cast<uint32_t>(code_off));
    put_u32(elf, 52 + 8, entry_va);
    put_u32(elf, 52 + 12, entry_va);
    put_u32(elf, 52 + 16, static_cast<uint32_t>(code_bytes));
    put_u32(elf, 52 + 20, static_cast<uint32_t>(code_bytes));
    put_u32(elf, 52 + 24, 5);  // PF_R | PF_X

    for (size_t i = 0; i < code.size(); ++i) {
        put_u32(elf, code_off + i * 4U, code[i]);
    }
    return elf;
}

void test_elf_disasm_and_c99_emit() {
    const uint32_t base = 0x08804000U;
    const uint32_t helper = base + 0x20U;
    // main:
    //   addiu $a0, $zero, 10
    //   jal helper
    //   addiu $a1, $zero, 32   (delay slot)
    //   halt
    //   nop x 4
    // helper (0x08804020):
    //   addu $v0, $a0, $a1
    //   jr $ra
    //   nop
    const std::vector<uint32_t> code = {
        enc_i(0x09, 0, 4, 10),          // +0x00: addiu $a0, $zero, 10
        enc_j(0x03, helper),            // +0x04: jal helper
        enc_i(0x09, 0, 5, 32),          // +0x08: addiu $a1, $zero, 32
        enc_r(0x1C, 0, 0, 0, 0, 0x00),  // +0x0C: halt
        0,
        0,
        0,
        0,                               // +0x10..0x1C padding
        enc_r(0x00, 4, 5, 2, 0, 0x21),   // +0x20: addu $v0, $a0, $a1
        enc_r(0x00, 31, 0, 0, 0, 0x08),  // +0x24: jr $ra
        0,                               // +0x28: nop
    };

    const auto elf = build_synthetic_elf(base, code);
    LoadedPspImage img;
    std::string err;
    CHECK(load_psp_elf_buffer(elf.data(), elf.size(), img, err));
    CHECK(img.entry_point == base);

    // Run in interpreter first to check reference answer ($v0 == 42)
    PspRuntime rt;
    CHECK(rt.load_elf_memory(elf.data(), elf.size(), err));
    PspInterpreter::run_slice(rt, rt.cpu(), 64);
    CHECK(rt.cpu().gpr[2] == 42U);

    // Disassemble CFG & emit C99
    dragon::GameConfig cfg_in{};
    const auto cfg = disassemble_psp(img, cfg_in);
    CHECK(cfg.functions.count(base) == 1U);
    CHECK(cfg.functions.count(helper) == 1U);

    const auto out_dir = std::filesystem::temp_directory_path() / "dragon_psp_recomp_test_out";
    std::error_code ec;
    std::filesystem::remove_all(out_dir, ec);
    const auto res = emit_psp_c99(cfg, cfg_in, out_dir, img.sha1_hex, "test.elf");
    CHECK(res.functions_emitted == 2U);
    CHECK(res.files_written == 3U);
    CHECK(std::filesystem::exists(out_dir / "psp_recomp.h"));
    CHECK(std::filesystem::exists(out_dir / "psp_funcs_00.c"));
    CHECK(std::filesystem::exists(out_dir / "psp_dispatch.c"));

    // Verify generated C99 contains both function definitions
    std::ifstream f(out_dir / "psp_funcs_00.c");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(text.find("void func_08804000(PspCpuState* cpu)") != std::string::npos);
    CHECK(text.find("void func_08804020(PspCpuState* cpu)") != std::string::npos);
    CHECK(text.find("func_08804020(cpu);") != std::string::npos);

    std::filesystem::remove_all(out_dir, ec);
}

}  // namespace

int main() {
    test_elf_disasm_and_c99_emit();
    if (g_failures == 0) {
        std::printf("psp_recomp_unit_tests: ALL PASSED\n");
        return 0;
    }
    return 1;
}
