#pragma once

#include <cstdint>

#include "ps1/cpu_state.h"

namespace dragon::ps1 {

class Ps1Runtime;

class Ps1Interpreter {
public:
    // Execute one R3000A instruction at cpu.pc (handles delay slots, COP0, GTE, BIOS vectors).
    static void step(Ps1Runtime& rt, Ps1CpuState& cpu) noexcept;

    // Execute up to max_instructions or until vblank_yield / halted.
    static uint64_t run_slice(Ps1Runtime& rt, Ps1CpuState& cpu, uint64_t max_instructions) noexcept;

    // Execute from target_pc until returning to return_pc (dynamic fallback from recompiled code).
    static void run_until_return(Ps1Runtime& rt, Ps1CpuState& cpu, uint32_t target_pc,
                                 uint32_t return_pc,
                                 uint64_t max_instructions = 2000000ULL) noexcept;
};

}  // namespace dragon::ps1
