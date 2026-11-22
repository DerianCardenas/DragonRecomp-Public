#pragma once

#include "nds/bus.h"
#include "nds/cpu_state.h"

namespace dragon::nds {

void run_frame(NdsCpuState& arm9, NdsCpuState& arm7, Bus& bus) noexcept;

}  // namespace dragon::nds
