#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "dragon/config.h"
#include "ps1/interpreter.h"
#include "ps1/loader.h"
#include "ps1/runtime.h"
#include "psxrecomp/disassembler.h"
#include "psxrecomp/emitter.h"

namespace {

using namespace dragon::ps1;
using namespace dragon::psxrecomp;

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

void put_u32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    b[off + 0U] = static_cast<uint8_t>(v & 0xFFU);
    b[off + 1U] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
    b[off + 2U] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
    b[off + 3U] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
}

std::vector<uint8_t> build_synthetic_psx_exe(uint32_t load_va, uint32_t entry_va,
                                             const std::vector<uint32_t>& code) {
    const size_t body_sz = code.size() * 4U;
    std::vector<uint8_t> exe(2048U + body_sz, 0U);
    std::memcpy(exe.data(), "PS-X EXE", 8);
    put_u32(exe, 0x10U, entry_va);
    put_u32(exe, 0x14U, 0x80018000U);  // GP
    put_u32(exe, 0x18U, load_va);
    put_u32(exe, 0x1CU, static_cast<uint32_t>(body_sz));
    put_u32(exe, 0x30U, 0x801FFFF0U);
    for (size_t i = 0; i < code.size(); ++i) {
        put_u32(exe, 2048U + i * 4U, code[i]);
    }
    return exe;
}

void test_psx_exe_disasm_and_c99_emit() {
    const uint32_t base = 0x80010000U;
    const uint32_t helper = base + 0x20U;
    // main (0x80010000):
    //   addiu $sp, $sp, -16
    //   sw $ra, 12($sp)
    //   addiu $a0, $zero, 19
    //   jal helper
    //   addiu $a1, $zero, 23  (delay slot)
    //   lw $ra, 12($sp)
    //   jr $ra
    //   addiu $sp, $sp, 16    (delay slot)
    // helper (0x80010020):
    //   addu $v0, $a0, $a1
    //   jr $ra
    //   nop
    const std::vector<uint32_t> code = {
        enc_i(0x09, 29, 29, static_cast<uint16_t>(-16)),  // +0x00
        enc_i(0x2B, 29, 31, 12),                          // +0x04
        enc_i(0x09, 0, 4, 19),                            // +0x08
        enc_j(0x03, helper),                              // +0x0C
        enc_i(0x09, 0, 5, 23),                            // +0x10
        enc_i(0x23, 29, 31, 12),                          // +0x14
        enc_r(0x00, 31, 0, 0, 0, 0x08),                   // +0x18
        enc_i(0x09, 29, 29, 16),                          // +0x1C
        enc_r(0x00, 4, 5, 2, 0, 0x21),                    // +0x20
        enc_r(0x00, 31, 0, 0, 0, 0x08),                   // +0x24
        0U,                                               // +0x28
    };

    const auto exe = build_synthetic_psx_exe(base, base, code);
    LoadedPs1Image img;
    std::string err;
    CHECK(load_ps1_exe_buffer(exe.data(), exe.size(), img, err));
    CHECK(img.initial_pc == base);

    // Run in interpreter first -> $v0 == 42
    Ps1Runtime rt;
    CHECK(rt.load_exe_memory(exe.data(), exe.size(), err));
    Ps1Interpreter::run_slice(rt, rt.cpu(), 64);
    CHECK(rt.cpu().gpr[2] == 42U);

    // Disassemble & emit C99
    dragon::GameConfig cfg_in{};
    const auto cfg = disassemble_ps1(img, cfg_in);
    CHECK(cfg.functions.count(base) == 1U);
    CHECK(cfg.functions.count(helper) == 1U);

    const auto out_dir = std::filesystem::temp_directory_path() / "dragon_ps1_recomp_test_out";
    std::error_code ec;
    std::filesystem::remove_all(out_dir, ec);
    const auto res = emit_ps1_c99(cfg, cfg_in, out_dir, img.sha1_hex, "test.psexe");
    CHECK(res.functions_emitted == 2U);
    CHECK(res.files_written == 3U);
    CHECK(std::filesystem::exists(out_dir / "ps1_recomp.h"));
    CHECK(std::filesystem::exists(out_dir / "ps1_funcs_00.c"));
    CHECK(std::filesystem::exists(out_dir / "ps1_dispatch.c"));

    std::ifstream f(out_dir / "ps1_funcs_00.c");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(text.find("void func_80010000(Ps1CpuState* cpu)") != std::string::npos);
    CHECK(text.find("void func_80010020(Ps1CpuState* cpu)") != std::string::npos);
    CHECK(text.find("func_80010020(cpu);") != std::string::npos);

    std::filesystem::remove_all(out_dir, ec);
}

}  // namespace

int main() {
    test_psx_exe_disasm_and_c99_emit();
    if (g_failures == 0) {
        std::printf("ps1_recomp_unit_tests: ALL PASSED\n");
        return 0;
    }
    return 1;
}
