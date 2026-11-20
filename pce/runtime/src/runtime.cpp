#include "pce/runtime.h"

namespace dragon::pce {

const char* runtime_version() {
    return "0.1.0";
}

}  // namespace dragon::pce

extern "C" {

uint8_t pce_bus_read(PceCpuState* cpu, uint16_t addr) {
    auto* bus = static_cast<dragon::pce::Bus*>(cpu->bus);
    if (cpu->mpr[addr >> 13U] == 0xFFU && cpu->cycles >= cpu->next_event) {
        bus->sync_hardware(*cpu);
    }
    return bus->read(*cpu, addr);
}

void pce_bus_write(PceCpuState* cpu, uint16_t addr, uint8_t value) {
    auto* bus = static_cast<dragon::pce::Bus*>(cpu->bus);
    bus->write(*cpu, addr, value);
}

void pce_st0(PceCpuState* cpu, uint8_t value) {
    auto* bus = static_cast<dragon::pce::Bus*>(cpu->bus);
    bus->write_vdc_direct(0U, value);
}

void pce_st1(PceCpuState* cpu, uint8_t value) {
    auto* bus = static_cast<dragon::pce::Bus*>(cpu->bus);
    bus->write_vdc_direct(2U, value);
}

void pce_st2(PceCpuState* cpu, uint8_t value) {
    auto* bus = static_cast<dragon::pce::Bus*>(cpu->bus);
    bus->write_vdc_direct(3U, value);
}

void pce_tam(PceCpuState* cpu, uint8_t mask) {
    if (mask != 0U) {
        cpu->last_mpr = cpu->a;
        for (int i = 0; i < 8; ++i) {
            if (mask & (1U << i)) {
                cpu->mpr[i] = cpu->a;
            }
        }
    }
}

void pce_tma(PceCpuState* cpu, uint8_t mask) {
    if (mask == 0U) {
        cpu->a = cpu->last_mpr;
        return;
    }
    uint8_t val = 0U;
    for (int i = 0; i < 8; ++i) {
        if (mask & (1U << i)) {
            val |= cpu->mpr[i];
        }
    }
    cpu->a = val;
    cpu->last_mpr = val;
}

int pce_runtime_poll(PceCpuState* cpu) {
    auto* bus = static_cast<dragon::pce::Bus*>(cpu->bus);
    bus->sync_hardware(*cpu);
    if (bus->stop_requested()) {
        return 1;
    }
    const uint8_t pre_irq_s = cpu->s;
    const uint32_t irq_cyc = dragon::pce::check_interrupts(*cpu, *bus);
    if (irq_cyc > 0U) {
        while (!bus->stop_requested() && static_cast<int8_t>(cpu->s - pre_irq_s) < 0) {
            if (pce_runtime_dispatch(cpu, cpu->pc, 0xFFFFU) != 0) {
                return 1;
            }
        }
    }
    return bus->stop_requested() ? 1 : 0;
}

int pce_runtime_dispatch(PceCpuState* cpu, uint16_t target, uint16_t from_pc) {
    auto* bus = static_cast<dragon::pce::Bus*>(cpu->bus);
    cpu->pc = target;

    const uint8_t bank = cpu->mpr[target >> 13U];
    if (bank < 0x80U) {
        if (PceRecompFunc fn = bus->lookup_recomp(bank, target)) {
            fn(cpu);
            return bus->stop_requested() ? 1 : 0;
        }
    }
    bus->record_fallback(bank, target, from_pc);

    // Interpreter fallback: run until returning past entry stack pointer or stop requested
    const uint8_t entry_s = cpu->s;
    while (!bus->stop_requested()) {
        const uint32_t irq_c = dragon::pce::check_interrupts(*cpu, *bus);
        if (irq_c > 0U) {
            bus->sync_hardware(*cpu);
            if (bus->stop_requested()) {
                return 1;
            }
        }
        const uint8_t cur_bank = cpu->mpr[cpu->pc >> 13U];
        if (cur_bank < 0x80U) {
            if (PceRecompFunc fn = bus->lookup_recomp(cur_bank, cpu->pc)) {
                fn(cpu);
                if (bus->stop_requested()) {
                    return 1;
                }
                if (static_cast<int8_t>(cpu->s - entry_s) > 0) {
                    return 0;
                }
                continue;
            }
        }
        const uint8_t op = bus->read(*cpu, cpu->pc);
        const bool is_ret = (op == 0x60U || op == 0x40U);  // RTS / RTI
        dragon::pce::step_instruction(*cpu, *bus);
        if (is_ret && static_cast<int8_t>(cpu->s - entry_s) > 0) {
            return 0;
        }
    }
    return 1;
}

int pce_runtime_step(PceCpuState* cpu) {
    auto* bus = static_cast<dragon::pce::Bus*>(cpu->bus);
    const uint16_t start_pc = cpu->pc;
    while (!bus->stop_requested()) {
        const uint32_t irq_c = dragon::pce::check_interrupts(*cpu, *bus);
        if (irq_c > 0U) {
            bus->sync_hardware(*cpu);
            if (bus->stop_requested()) {
                return 1;
            }
            if (pce_runtime_dispatch(cpu, cpu->pc, start_pc) != 0) {
                return 1;
            }
            if (cpu->pc != start_pc) {
                return 0;
            }
            continue;
        }
        dragon::pce::step_instruction(*cpu, *bus);
        break;
    }
    return bus->stop_requested() ? 1 : 0;
}

}  // extern "C"
