#pragma once

// 65816 (Ricoh 5A22) CPU interpreter — implements all 256 opcodes in both
// Emulation (e=1) and Native (e=0) modes, 8/16-bit M and X widths, and BCD decimal mode.

#include <cstdint>

#include "snes/bus.h"
#include "snes/cpu_state.h"

namespace dragon::snes {

// Initialize CpuState and attach it to `bus`.
void cpu_init(CpuState& cpu, Bus* bus) noexcept;

// Hardware reset sequence (reads vector at $00:FFFC, sets e=1, m=1, xf=1, i=1, d_flag=0, s=$01FF).
void cpu_reset(CpuState& cpu) noexcept;

// Non-maskable interrupt (pushes PB (if e=0), PC, P; sets i=1, d_flag=0, pb=0; jumps to
// $FFEA/$FFFA).
void cpu_nmi(CpuState& cpu) noexcept;

// Maskable interrupt request (if i==0, pushes PB (if e=0), PC, P; sets i=1, d_flag=0, pb=0; jumps
// to $FFEE/$FFFE).
bool cpu_irq(CpuState& cpu) noexcept;

// Execute a single instruction at PB:PC and return the CPU cycle count consumed
// (also adds cpu_cycles * 6 to cpu.master_cycles).
uint32_t cpu_step(CpuState& cpu) noexcept;

// Execute instructions until cpu.master_cycles >= max_master_cycles or cpu.halted/waiting is set.
void cpu_run_until(CpuState& cpu, uint64_t max_master_cycles) noexcept;

}  // namespace dragon::snes
