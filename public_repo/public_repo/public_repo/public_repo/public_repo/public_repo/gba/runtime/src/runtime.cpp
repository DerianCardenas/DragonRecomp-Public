#include "gba/runtime.h"

namespace dragon::gba {

const char* runtime_version() {
    return "0.1.0";
}

}  // namespace dragon::gba

extern "C" {

uint8_t gba_bus_read8(CpuState* cpu, uint32_t addr) {
    auto* bus = static_cast<dragon::gba::Bus*>(cpu->bus);
    if (!bus) {
        return 0U;
    }
    if ((addr >> 24U) == 0x04U) {
        bus->sync_hardware(cpu->cycles);
    }
    return bus->read8(addr);
}

uint16_t gba_bus_read16(CpuState* cpu, uint32_t addr) {
    auto* bus = static_cast<dragon::gba::Bus*>(cpu->bus);
    if (!bus) {
        return 0U;
    }
    if ((addr >> 24U) == 0x04U) {
        bus->sync_hardware(cpu->cycles);
    }
    return bus->read16(addr);
}

uint32_t gba_bus_read32(CpuState* cpu, uint32_t addr) {
    auto* bus = static_cast<dragon::gba::Bus*>(cpu->bus);
    if (!bus) {
        return 0U;
    }
    if ((addr >> 24U) == 0x04U) {
        bus->sync_hardware(cpu->cycles);
    }
    return bus->read32(addr);
}

void gba_bus_write8(CpuState* cpu, uint32_t addr, uint8_t value) {
    auto* bus = static_cast<dragon::gba::Bus*>(cpu->bus);
    if (!bus) {
        return;
    }
    if ((addr >> 24U) == 0x04U) {
        bus->sync_hardware(cpu->cycles);
    }
    bus->write8(addr, value);
}

void gba_bus_write16(CpuState* cpu, uint32_t addr, uint16_t value) {
    auto* bus = static_cast<dragon::gba::Bus*>(cpu->bus);
    if (!bus) {
        return;
    }
    if ((addr >> 24U) == 0x04U) {
        bus->sync_hardware(cpu->cycles);
    }
    bus->write16(addr, value);
}

void gba_bus_write32(CpuState* cpu, uint32_t addr, uint32_t value) {
    auto* bus = static_cast<dragon::gba::Bus*>(cpu->bus);
    if (!bus) {
        return;
    }
    if ((addr >> 24U) == 0x04U) {
        bus->sync_hardware(cpu->cycles);
    }
    bus->write32(addr, value);
}

void gba_swi(CpuState* cpu, uint8_t swi_num) {
    if (!cpu) {
        return;
    }
    dragon::gba::cpu_handle_swi(*cpu, swi_num);
}

void gba_runtime_poll(CpuState* cpu) {
    auto* bus = static_cast<dragon::gba::Bus*>(cpu->bus);
    if (!bus) {
        cpu->next_event = cpu->cycles + 280896ULL;
        return;
    }

    // Fast path: generated code polls on every branch; only sync hardware when the next event is
    // due or an enabled IRQ is already pending.
    if (cpu->cycles < cpu->next_event && (cpu->i_disable != 0U || !bus->has_pending_irq())) {
        return;
    }

    bus->sync_hardware(cpu->cycles);

    if (bus->ppu().consume_frame_complete()) {
        bus->on_frame_complete(cpu);
        // on_frame_complete may request a stop (e.g. headless frame budget reached)
        // by halting the CPU. Return immediately so the generated code unwinds; do
        // not fall through to IRQ delivery, which would clear the halt via
        // cpu_check_interrupts and run on forever.
        if (cpu->halted) {
            return;
        }
    }

    if (bus->has_pending_irq() && cpu->i_disable == 0U) {
        gba_dispatch_irq(cpu);
    }

    cpu->next_event = cpu->cycles + 128ULL;
}

void gba_dispatch_irq(CpuState* cpu) {
    auto* bus = static_cast<dragon::gba::Bus*>(cpu->bus);
    if (!bus) {
        return;
    }

    // Guard against runaway nesting. Real handlers acknowledge IF before they
    // re-enable interrupts, so a well-behaved game nests at most a couple of
    // levels; cap the depth so a misbehaving (or mistranslated) handler that
    // leaves its own IRQ line asserted cannot recurse without bound.
    static int irq_depth = 0;
    if (irq_depth >= 8) {
        return;
    }

    // Snapshot the interrupted context. The BIOS IRQ wrapper returns here via
    // SUBS PC, LR, #4 once the user handler is done, restoring CPSR (and thus
    // the original processor mode and I flag) from SPSR_irq.
    const uint32_t resume_pc = cpu->r[15];
    const uint8_t resume_thumb = cpu->thumb;
    const uint8_t resume_mode = static_cast<uint8_t>(cpu->cpsr & GBA_CPSR_MODE_MASK);

    // Enter IRQ mode and jump to the BIOS IRQ vector ($00000128).
    if (dragon::gba::cpu_check_interrupts(*cpu) == 0U) {
        return;
    }

    ++irq_depth;

    // Run the handler to completion. Unlike gba_runtime_dispatch, this loop does
    // NOT bail when the mode changes: real handlers switch IRQ -> System to run
    // their body (and to allow nesting) and only return to the interrupted code
    // at the very end. Completion is reached when execution lands back on the
    // interrupted instruction with the interrupted mode restored. A generous
    // cycle budget guards against a runaway handler.
    const uint64_t budget_end = cpu->cycles + 2000000ULL;
    while (!cpu->halted) {
        const uint32_t pc = cpu->r[15];
        const uint8_t mode = static_cast<uint8_t>(cpu->cpsr & GBA_CPSR_MODE_MASK);
        if (pc == resume_pc && mode == resume_mode) {
            break;
        }

        const uint32_t region = (pc >> 24U) & 0xFFU;
        if (region >= 0x08U && region <= 0x0DU) {
            RecompFunc fn = bus->lookup_function(pc, cpu->thumb);
            if (fn) {
                fn(cpu);
                if (cpu->halted) {
                    break;
                }
                continue;
            }
        }

        dragon::gba::cpu_step(*cpu);

        if (cpu->cycles >= budget_end) {
            break;
        }
    }

    --irq_depth;

    // Resume exactly where we were interrupted. The handler's BIOS return already
    // restored mode/I/thumb from SPSR_irq, but pin the resume state to be safe in
    // case the budget guard tripped mid-handler.
    cpu->r[15] = resume_pc;
    cpu->thumb = resume_thumb;
    if (resume_thumb) {
        cpu->cpsr |= GBA_CPSR_T;
    } else {
        cpu->cpsr &= ~GBA_CPSR_T;
    }
}

void gba_runtime_dispatch(CpuState* cpu, uint32_t target, uint32_t from_pc) {
    auto* bus = static_cast<dragon::gba::Bus*>(cpu->bus);
    if (!bus) {
        return;
    }

    if ((from_pc >> 24U) != 0x00U) {
        cpu->thumb = static_cast<uint8_t>(target & 1U);
        if (cpu->thumb) {
            cpu->cpsr |= GBA_CPSR_T;
        } else {
            cpu->cpsr &= ~GBA_CPSR_T;
        }
    }
    cpu->r[15] = target & (cpu->thumb ? ~1U : ~3U);

    const uint32_t target_addr = cpu->r[15];
    const uint32_t target_region = (target_addr >> 24U) & 0xFFU;
    if (target_region >= 0x08U && target_region <= 0x0DU) {
        RecompFunc fn = bus->lookup_function(target_addr, cpu->thumb);
        if (fn) {
            fn(cpu);
            return;
        }
        // Dispatch target is in ROM but has no compiled function: log fallback entry once.
        bus->log_fallback(target_addr, cpu->thumb, from_pc);
    }

    const uint32_t entry_sp = cpu->r[13];
    const uint32_t entry_lr_addr = cpu->r[14] & ~1U;
    const uint8_t entry_mode = static_cast<uint8_t>(cpu->cpsr & GBA_CPSR_MODE_MASK);

    while (!cpu->halted) {
        const uint32_t pc = cpu->r[15];
        const uint32_t region = (pc >> 24U) & 0xFFU;

        if (region >= 0x08U && region <= 0x0DU) {
            RecompFunc fn = bus->lookup_function(pc, cpu->thumb);
            if (fn) {
                fn(cpu);
                if (cpu->halted) {
                    return;
                }
                const uint8_t cur_mode = static_cast<uint8_t>(cpu->cpsr & GBA_CPSR_MODE_MASK);
                if (cur_mode != entry_mode) {
                    return;
                }
                if (cpu->r[13] >= entry_sp && cpu->r[15] == entry_lr_addr) {
                    return;
                }
                continue;
            }
            // Sequential interpreter steps or return addresses are NEVER logged as fallbacks;
            // doing so records mid-function return addresses and shatters functions on
            // recompilation.
        }

        dragon::gba::cpu_step(*cpu);

        if (bus->ppu().consume_frame_complete()) {
            bus->on_frame_complete(cpu);
            if (cpu->halted) {
                return;
            }
        }

        const uint8_t cur_mode = static_cast<uint8_t>(cpu->cpsr & GBA_CPSR_MODE_MASK);
        if (cur_mode != entry_mode) {
            return;
        }
        if (cpu->r[13] >= entry_sp && cpu->r[15] == entry_lr_addr && cpu->r[15] != target) {
            return;
        }
    }
}

uint32_t gba_runtime_step(CpuState* cpu) {
    if (!cpu) {
        return 0U;
    }
    return dragon::gba::cpu_step(*cpu);
}

}  // extern "C"
