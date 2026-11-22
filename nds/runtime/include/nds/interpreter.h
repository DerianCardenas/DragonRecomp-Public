#pragma once

#include <cstdint>

#include "nds/bus.h"
#include "nds/cpu_state.h"

namespace dragon::nds {

void cpu_reset_arm9(NdsCpuState& cpu, Bus& bus, uint32_t entry_pc) noexcept;
void cpu_reset_arm7(NdsCpuState& cpu, Bus& bus, uint32_t entry_pc) noexcept;
uint32_t cpu_step(NdsCpuState& cpu, Bus& bus) noexcept;
void cpu_dispatch_irq(NdsCpuState& cpu, Bus& bus) noexcept;
void cpu_handle_swi(NdsCpuState& cpu, Bus& bus, uint8_t swi_num) noexcept;

}  // namespace dragon::nds
