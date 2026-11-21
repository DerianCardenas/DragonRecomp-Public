#pragma once

// Cycle-accurate Motorola 68000 interpreter (reference backend & static-recomp fallback).
// Reference: Motorola M68000 Programmer's Reference Manual, docs/GENESIS_SPEC.md §2.

#include <cstdint>

#include "md/bus.h"
#include "md/cpu_state.h"

namespace dragon::md {

// Initialize M68000 from Genesis vector table ($000000 = SSP, $000004 = PC).
void cpu_reset(MdCpuState& cpu, Bus& bus) noexcept;

// Service an M68000 exception (pushes PC & SR onto SSP, enters Supervisor mode, loads vector).
void cpu_exception(MdCpuState& cpu, Bus& bus, uint8_t vector_num, uint8_t new_ipl = 0xFFU) noexcept;

// Check and service pending VDP V-Int (L6) or H-Int (L4). Returns cycles spent (0 if none).
uint32_t cpu_check_interrupts(MdCpuState& cpu, Bus& bus) noexcept;

// Execute a single M68000 instruction and return elapsed 68K cycles.
uint32_t step_instruction(MdCpuState& cpu, Bus& bus) noexcept;

// Run until VDP completes one frame (or `max_cycles` exceeded).
uint32_t run_frame_interpreter(MdCpuState& cpu, Bus& bus, uint32_t max_cycles = 200000U) noexcept;

}  // namespace dragon::md
