#pragma once

#include "gb/bus.h"
#include "gb/cpu_state.h"

namespace dragon::gb {

void cpu_init(CpuState& cpu, Bus* bus) noexcept;
void cpu_reset(CpuState& cpu) noexcept;

// Execute one instruction. Returns T-cycles elapsed.
uint32_t cpu_step(CpuState& cpu) noexcept;

// Service pending interrupts. Returns T-cycles elapsed (20 if serviced, 0 otherwise).
uint32_t cpu_check_interrupts(CpuState& cpu) noexcept;

}  // namespace dragon::gb
