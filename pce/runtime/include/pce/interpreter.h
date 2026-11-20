#pragma once

// Cycle-accurate HuC6280 CPU interpreter for PC Engine / TurboGrafx-16.
// Reference: docs/PCE_SPEC.md §2.

#include <cstdint>

#include "pce/bus.h"
#include "pce/cpu_state.h"

namespace dragon::pce {

// Check and service NMI / TIQ / IRQ1 / IRQ2. Returns cycles consumed (0 if none).
uint32_t check_interrupts(PceCpuState& cpu, Bus& bus) noexcept;

// Execute a single HuC6280 instruction and synchronize hardware. Returns cycles consumed.
uint32_t step_instruction(PceCpuState& cpu, Bus& bus) noexcept;

// Run interpreter until VBlank frame completes (or max_cycles reached).
uint32_t run_frame_interpreter(PceCpuState& cpu, Bus& bus, uint32_t max_cycles = 160000U) noexcept;

}  // namespace dragon::pce
