#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "nds/cartridge.h"
#include "ndsrecomp/disassembler.h"
#include "ndsrecomp/emitter.h"

namespace {

void wr32(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
    buf[off + 0] = static_cast<uint8_t>(v & 0xFFU);
    buf[off + 1] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
    buf[off + 2] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
    buf[off + 3] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
}

void test_nds_disassemble_and_emit() {
    std::printf("Running test_nds_disassemble_and_emit...\n");
    std::vector<uint8_t> rom(0x8000U, 0U);
    std::memcpy(&rom[0x000], "DRAGON_NDS", 10);
    std::memcpy(&rom[0x00C], "DRGE", 4);
    std::memcpy(&rom[0x010], "01", 2);
    // ARM9 binary at ROM 0x4000, RAM 0x02000000, entry 0x02000000, size 0x40
    wr32(rom, 0x020, 0x4000U);
    wr32(rom, 0x024, 0x02000000U);
    wr32(rom, 0x028, 0x02000000U);
    wr32(rom, 0x02C, 0x40U);
    // ARM7 binary at ROM 0x5000, RAM 0x02380000, entry 0x02380000, size 0x20
    wr32(rom, 0x030, 0x5000U);
    wr32(rom, 0x034, 0x02380000U);
    wr32(rom, 0x038, 0x02380000U);
    wr32(rom, 0x03C, 0x20U);

    // ARM9 code:
    // 0x02000000: BL 0x02000010 (EB000002)
    // 0x02000004: CLZ R0, R0    (E16F0F10)
    // 0x02000008: BX LR         (E12FFF1E)
    // 0x02000010: MOV R0, #42   (E3A0002A)
    // 0x02000014: BX LR         (E12FFF1E)
    wr32(rom, 0x4000, 0xEB000002U);
    wr32(rom, 0x4004, 0xE16F0F10U);
    wr32(rom, 0x4008, 0xE12FFF1EU);
    wr32(rom, 0x4010, 0xE3A0002AU);
    wr32(rom, 0x4014, 0xE12FFF1EU);

    dragon::nds::Cartridge cart;
    std::string err;
    assert(cart.load_from_bytes(rom, &err));
    assert(cart.header().title == "DRAGON_NDS");

    dragon::GameConfig config{};
    const auto cfg = dragon::ndsrecomp::disassemble_nds(cart, config);
    assert(cfg.functions.size() == 2U);
    assert(cfg.find_function(0x02000000U, false) != nullptr);
    assert(cfg.find_function(0x02000010U, false) != nullptr);

    const auto out_dir = std::filesystem::temp_directory_path() / "dragon_ndsrecomp_test";
    std::filesystem::remove_all(out_dir);
    const auto res = dragon::ndsrecomp::emit_nds_c99(cfg, config, out_dir, "0123456789abcdef", "");
    assert(res.functions_emitted == 2U);
    assert(res.files_written == 3U);
    assert(std::filesystem::exists(out_dir / "recomp_funcs.h"));
    assert(std::filesystem::exists(out_dir / "recomp_funcs_0.c"));
    assert(std::filesystem::exists(out_dir / "recomp_dispatch.c"));
    std::filesystem::remove_all(out_dir);
    std::printf("  passed!\n");
}

}  // namespace

int main() {
    test_nds_disassemble_and_emit();
    std::printf("All ndsrecomp unit tests passed!\n");
    return 0;
}
