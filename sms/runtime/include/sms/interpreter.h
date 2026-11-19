#pragma once

// Cycle-accurate Zilog Z80 interpreter (reference backend & static-recomp fallback).
// Reference: Zilog Z80 CPU User Manual (UM0080) & docs/SMS_SPEC.md §2.

#include <cstdint>

#include "sms/bus.h"
#include "sms/cpu_state.h"

namespace dragon::sms {

// Check & service pending NMI or maskable VDP IRQ. Returns T-cycles spent (0 if none).
uint32_t check_interrupts(SmsCpuState& cpu, Bus& bus) noexcept;

// Execute a single Z80 instruction (including any prefix bytes) and sync VDP/PSG.
// Returns elapsed T-cycles for this step.
uint32_t step_instruction(SmsCpuState& cpu, Bus& bus) noexcept;

// Run until the VDP finishes the current frame (or `max_cycles` is exceeded).
uint32_t run_frame_interpreter(SmsCpuState& cpu, Bus& bus, uint32_t max_cycles = 120000U) noexcept;

}  // namespace dragon::sms
