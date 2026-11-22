#pragma once

#include <cstdint>

#include "ps1/cpu_state.h"

namespace dragon::ps1 {

/*
 * PlayStation 1 Geometry Transformation Engine (COP2 / GTE).
 * Operates directly on cpu.gte_data[32] and cpu.gte_ctrl[32] for zero-overhead
 * sharing between the R3000A interpreter, C99 recompiled code, and save states.
 */
class Ps1Gte {
public:
    static uint32_t read_data(const Ps1CpuState& cpu, uint8_t reg) noexcept;
    static void write_data(Ps1CpuState& cpu, uint8_t reg, uint32_t val) noexcept;

    static uint32_t read_ctrl(const Ps1CpuState& cpu, uint8_t reg) noexcept;
    static void write_ctrl(Ps1CpuState& cpu, uint8_t reg, uint32_t val) noexcept;

    static void execute_command(Ps1CpuState& cpu, uint32_t raw_insn) noexcept;

    // Exact PS1 hardware UNR division: min(0x1FFFF, (h * 0x20000 / sz3))
    static uint32_t unr_divide(uint16_t h, uint16_t sz3) noexcept;
};

}  // namespace dragon::ps1
