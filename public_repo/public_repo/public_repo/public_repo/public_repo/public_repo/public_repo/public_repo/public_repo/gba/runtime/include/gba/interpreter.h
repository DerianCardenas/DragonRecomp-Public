#pragma once

#include <cstdint>

#include "gba/bus.h"
#include "gba/cpu_state.h"

namespace dragon::gba {

void cpu_init(CpuState& cpu, Bus* bus) noexcept;
uint32_t cpu_step(CpuState& cpu) noexcept;
uint32_t cpu_check_interrupts(CpuState& cpu) noexcept;
void cpu_handle_swi(CpuState& cpu, uint8_t swi_num) noexcept;

}  // namespace dragon::gba
