#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "nes/runtime.h"

int main() {
    const std::filesystem::path rom_path = "roms/test/nestest.nes";
    const std::filesystem::path log_path = "roms/test/nestest.log";

    if (!std::filesystem::exists(rom_path) || !std::filesystem::exists(log_path)) {
        std::printf("nes_nestest: roms/test/nestest.{nes,log} not present; skipping.\n");
        return 77;
    }

    dragon::nes::Cartridge cart = dragon::nes::load_ines_rom(rom_path);
    dragon::nes::Bus bus;
    bus.load_cartridge(cart);

    CpuState cpu{};
    dragon::nes::cpu_init(cpu, &bus);
    cpu.pc = 0xC000U;  // nestest automated mode entry point
    cpu.s = 0xFDU;
    cpu_unpack_flags(&cpu, 0x24U);
    cpu.cycles = 7ULL;

    std::ifstream log_file(log_path);
    if (!log_file) {
        std::fprintf(stderr, "Failed to open %s\n", log_path.string().c_str());
        return 1;
    }

    std::string line;
    int line_num = 0;
    while (std::getline(log_file, line)) {
        if (line.size() < 73) {
            continue;
        }
        ++line_num;

        const auto exp_pc =
            static_cast<uint16_t>(std::strtoul(line.substr(0, 4).c_str(), nullptr, 16));
        const size_t a_pos = line.find("A:");
        const size_t cyc_pos = line.find("CYC:");
        if (a_pos == std::string::npos || cyc_pos == std::string::npos) {
            std::fprintf(stderr, "Malformed nestest.log line %d\n", line_num);
            return 1;
        }

        const auto exp_a =
            static_cast<uint8_t>(std::strtoul(line.substr(a_pos + 2, 2).c_str(), nullptr, 16));
        const auto exp_x =
            static_cast<uint8_t>(std::strtoul(line.substr(a_pos + 7, 2).c_str(), nullptr, 16));
        const auto exp_y =
            static_cast<uint8_t>(std::strtoul(line.substr(a_pos + 12, 2).c_str(), nullptr, 16));
        const auto exp_p =
            static_cast<uint8_t>(std::strtoul(line.substr(a_pos + 17, 2).c_str(), nullptr, 16));
        const auto exp_sp =
            static_cast<uint8_t>(std::strtoul(line.substr(a_pos + 23, 2).c_str(), nullptr, 16));
        const auto exp_cyc =
            static_cast<uint64_t>(std::strtoull(line.substr(cyc_pos + 4).c_str(), nullptr, 10));

        const uint8_t got_p = cpu_pack_flags(&cpu, 0U);

        if (cpu.pc != exp_pc || cpu.a != exp_a || cpu.x != exp_x || cpu.y != exp_y ||
            got_p != exp_p || cpu.s != exp_sp || cpu.cycles != exp_cyc) {
            std::fprintf(stderr,
                         "nestest mismatch at line %d:\n"
                         "  expected: PC=%04X A=%02X X=%02X Y=%02X P=%02X SP=%02X CYC=%llu\n"
                         "  got:      PC=%04X A=%02X X=%02X Y=%02X P=%02X SP=%02X CYC=%llu\n"
                         "  log line: %s\n",
                         line_num, exp_pc, exp_a, exp_x, exp_y, exp_p, exp_sp,
                         static_cast<unsigned long long>(exp_cyc), cpu.pc, cpu.a, cpu.x, cpu.y,
                         got_p, cpu.s, static_cast<unsigned long long>(cpu.cycles), line.c_str());
            return 1;
        }

        dragon::nes::cpu_step(cpu);
    }

    const uint8_t err_official = bus.read(0x0002U);
    const uint8_t err_unofficial = bus.read(0x0003U);
    if (err_official != 0U || err_unofficial != 0U) {
        std::fprintf(stderr, "nestest reported error bytes: [$02]=%02X [$03]=%02X\n", err_official,
                     err_unofficial);
        return 1;
    }

    std::printf("nes_nestest: matched all %d instructions in nestest.log ([$02]=00 [$03]=00)\n",
                line_num);
    return 0;
}
