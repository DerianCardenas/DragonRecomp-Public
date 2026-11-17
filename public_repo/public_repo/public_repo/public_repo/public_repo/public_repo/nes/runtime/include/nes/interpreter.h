#pragma once

// 6502 (Ricoh 2A03) CPU interpreter — implements all 151 official opcodes and
// all stable unofficial opcodes, sharing kOpcodeTable with nesrecomp.

#include <cstdint>

#include "nes/bus.h"
#include "nes/cpu_state.h"

namespace dragon::nes {

// Initialize CpuState and attach it to `bus`.
void cpu_init(CpuState& cpu, Bus* bus) noexcept;

// Hardware reset sequence (reads vector at $FFFC, sets S=$FD, I=1, cycles=7).
void cpu_reset(CpuState& cpu) noexcept;

// Non-maskable interrupt (pushes PC and P with B=0, sets I=1, jumps to $FFFA, +7 cycles).
void cpu_nmi(CpuState& cpu) noexcept;

// Maskable interrupt request (if I==0, pushes PC and P with B=0, sets I=1, jumps to $FFFE, +7
// cycles).
bool cpu_irq(CpuState& cpu) noexcept;

// Execute a single instruction at cpu.pc and return the cycle count consumed.
uint32_t cpu_step(CpuState& cpu) noexcept;

// Execute a single instruction, advance PPU, and deliver pending NMI / frame completion.
uint32_t cpu_step_with_ppu(CpuState& cpu) noexcept;

// Execute instructions until cpu.cycles >= max_cycles or cpu.halted is set.
void cpu_run_until(CpuState& cpu, uint64_t max_cycles) noexcept;

// Run interpreter with PPU/NMI stepping until `num_frames` frames complete or `cpu.halted` is set.
void cpu_run_frames_interpreter(CpuState& cpu, uint64_t num_frames) noexcept;

}  // namespace dragon::nes
