#include <cassert>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "md/cartridge.h"
#include "mdrecomp/disassembler.h"
#include "mdrecomp/emitter.h"

using namespace dragon::md;
using namespace dragon::mdrecomp;

namespace {

void put_be16(std::vector<uint8_t>& rom, uint32_t addr, uint16_t v) {
    rom[addr] = static_cast<uint8_t>(v >> 8U);
    rom[addr + 1U] = static_cast<uint8_t>(v & 0xFFU);
}

void put_be32(std::vector<uint8_t>& rom, uint32_t addr, uint32_t v) {
    put_be16(rom, addr, static_cast<uint16_t>(v >> 16U));
    put_be16(rom, addr + 2U, static_cast<uint16_t>(v & 0xFFFFU));
}

void test_mdrecomp_cfg_and_emit() {
    std::printf("[Test] mdrecomp CFG builder + C99 emitter...\n");
    std::vector<uint8_t> rom(65536, 0);
    // Vector table: SSP = $FFFE00, RESET = $000200, V-Int = $000220
    put_be32(rom, 0x0000U, 0x00FFFE00U);
    put_be32(rom, 0x0004U, 0x00000200U);
    put_be32(rom, 0x0078U, 0x00000220U);

    // $0200: MOVEQ #5, D0; BSR.S $0210; NOP; BRA.S $0204 (loop)
    put_be16(rom, 0x0200U, 0x7005U);
    put_be16(rom, 0x0202U, 0x610CU);  // BSR.S +12 -> $0210
    put_be16(rom, 0x0204U, 0x4E71U);
    put_be16(rom, 0x0206U, 0x60FCU);  // BRA.S -4 -> $0204

    // $0210: ADDQ.L #1, D0; RTS
    put_be16(rom, 0x0210U, 0x5280U);
    put_be16(rom, 0x0212U, 0x4E75U);

    // $0220 (V-Int): ADDQ.L #1, D1; RTE
    put_be16(rom, 0x0220U, 0x5281U);
    put_be16(rom, 0x0222U, 0x4E73U);

    Cartridge cart;
    assert(cart.load_from_bytes(rom));

    const ProgramCfg cfg = build_cfg(cart);
    assert(cfg.functions.count(0x000200U) == 1U);
    assert(cfg.functions.count(0x000210U) == 1U);
    assert(cfg.functions.count(0x000220U) == 1U);
    assert(cfg.total_instructions >= 8U);

    const auto out_dir = std::filesystem::temp_directory_path() / "dragon_test_mdrecomp";
    std::filesystem::remove_all(out_dir);

    EmitOptions opts;
    opts.output_dir = out_dir;
    opts.rom_path = "synthetic.md";
    opts.game_name = "Synthetic Genesis";
    const EmitResult res = emit_c99(cart, cfg, opts);
    assert(res.success);
    assert(std::filesystem::exists(out_dir / "recomp_decls.h"));
    assert(std::filesystem::exists(out_dir / "recomp_dispatch.c"));
    assert(std::filesystem::exists(out_dir / "recomp_funcs_0.c"));
    assert(std::filesystem::exists(out_dir / "report.toml"));

    std::filesystem::remove_all(out_dir);
    std::printf("  passed!\n");
}

}  // namespace

int main() {
    test_mdrecomp_cfg_and_emit();
    std::printf("All mdrecomp unit tests passed!\n");
    return 0;
}
