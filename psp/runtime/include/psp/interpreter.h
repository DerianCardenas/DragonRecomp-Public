#pragma once

#include <cstdint>

#include "psp/cpu_state.h"

namespace dragon::psp {

class PspRuntime;

class PspInterpreter {
public:
    // Execute a single instruction at cpu.pc (handles MIPS32, Allegrex, FPU, VFPU, delay slots).
    static void step(PspRuntime& rt, PspCpuState& cpu) noexcept;

    // Execute a single raw instruction without touching pc/npc (used for C99 fallback ops like
    // VFPU).
    static void exec_single_non_branch(PspRuntime& rt, PspCpuState& cpu, uint32_t raw,
                                       uint32_t pc) noexcept;

    // Execute up to max_instructions or until vblank_yield / halted.
    static uint64_t run_slice(PspRuntime& rt, PspCpuState& cpu, uint64_t max_instructions) noexcept;

    // Run until returning to return_pc (used when recompiled code calls an uncompiled function).
    static void run_until_return(PspRuntime& rt, PspCpuState& cpu, uint32_t target_pc,
                                 uint32_t return_pc,
                                 uint64_t max_instructions = 2000000ULL) noexcept;
};

}  // namespace dragon::psp
