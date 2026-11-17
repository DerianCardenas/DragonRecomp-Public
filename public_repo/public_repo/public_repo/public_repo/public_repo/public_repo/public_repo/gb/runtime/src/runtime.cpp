#include "gb/runtime.h"

#include <cstdio>

namespace dragon::gb {

const char* runtime_version() {
    return "0.1.0";
}

}  // namespace dragon::gb

extern "C" {

uint8_t gb_bus_read(CpuState* cpu, uint16_t addr) {
    auto* bus = static_cast<dragon::gb::Bus*>(cpu->bus);
    if (!bus)
        return 0xFF;
    if (!bus->is_flat_ram()) {
        if ((addr >= 0x8000U && addr < 0xFEA0U) || (addr >= 0xFF40U && addr <= 0xFF6BU)) {
            bus->sync_ppu(cpu->cycles);
        } else if (addr >= 0xFF10U && addr <= 0xFF3FU) {
            bus->sync_apu(cpu->cycles);
        }
    }
    return bus->read(addr);
}

void gb_bus_write(CpuState* cpu, uint16_t addr, uint8_t value) {
    auto* bus = static_cast<dragon::gb::Bus*>(cpu->bus);
    if (bus) {
        if (!bus->is_flat_ram()) {
            if ((addr >= 0x8000U && addr < 0xFEA0U) || (addr >= 0xFF40U && addr <= 0xFF6BU)) {
                bus->sync_ppu(cpu->cycles);
            } else if (addr >= 0xFF10U && addr <= 0xFF3FU) {
                bus->sync_apu(cpu->cycles);
            }
        }
        bus->write(addr, value);
    }
}

void gb_runtime_poll(CpuState* cpu) {
    auto* bus = static_cast<dragon::gb::Bus*>(cpu->bus);
    if (!bus || bus->is_flat_ram()) {
        cpu->next_event = cpu->cycles + 70224ULL;
        return;
    }

    bus->sync_ppu(cpu->cycles);
    bus->sync_apu(cpu->cycles);

    if (bus->ppu().consume_frame_complete()) {
        bus->on_frame_complete(cpu);
    }

    uint32_t int_cycles = dragon::gb::cpu_check_interrupts(*cpu);
    if (int_cycles > 0) {
        // Interrupt serviced: cpu->pc redirected to vector ($0040-$0060) and old PC pushed.
        // Dispatch until interrupt routine returns (stack restored).
        gb_runtime_dispatch(cpu, cpu->pc, cpu->pc);
    }

    cpu->next_event = cpu->cycles + 456ULL;
}

void gb_runtime_dispatch(CpuState* cpu, uint16_t target, uint16_t from_pc) {
    auto* bus = static_cast<dragon::gb::Bus*>(cpu->bus);
    if (!bus)
        return;

    cpu->pc = target;
    uint16_t const entry_sp = cpu->sp;

    if (!bus->lookup_function(target, bus->current_rom_bank())) {
        bus->log_fallback(target, bus->current_rom_bank(), from_pc);
    }

    while (!cpu->halted) {
        RecompFunc fn = bus->lookup_function(cpu->pc, bus->current_rom_bank());
        if (fn) {
            fn(cpu);
            if (cpu->halted || cpu->sp > entry_sp) {
                return;
            }
            continue;
        }

        const uint8_t op = bus->read(cpu->pc);
        const bool is_ret =
            (op == 0xC9 || op == 0xD9 || op == 0xC0 || op == 0xC8 || op == 0xD0 || op == 0xD8);

        dragon::gb::cpu_step(*cpu);

        if (bus->ppu().consume_frame_complete()) {
            bus->on_frame_complete(cpu);
            if (cpu->halted) {
                return;
            }
        }

        if (cpu->cycles >= cpu->next_event) {
            gb_runtime_poll(cpu);
            if (cpu->halted || cpu->sp > entry_sp) {
                return;
            }
        }

        if (is_ret && cpu->sp > entry_sp) {
            return;
        }
    }
    (void)from_pc;
}

void gb_runtime_stop(CpuState* cpu) {
    if (!cpu)
        return;
    auto* bus = static_cast<dragon::gb::Bus*>(cpu->bus);
    if (bus && bus->is_cgb() && (bus->read(0xFF4DU) & 0x01U)) {
        bus->toggle_double_speed();
        cpu->double_speed = bus->is_double_speed();
        return;
    }
    cpu->halted = 1;
}

uint32_t gb_runtime_step(CpuState* cpu) {
    if (!cpu)
        return 0;
    return dragon::gb::cpu_step(*cpu);
}
}
