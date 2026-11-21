#include "md/runtime.h"

namespace dragon::md {

const char* runtime_version() noexcept {
    return "0.1.0";
}

}  // namespace dragon::md

extern "C" {

uint8_t md_bus_read8(MdCpuState* cpu, uint32_t addr) {
    auto* bus = static_cast<dragon::md::Bus*>(cpu->bus);
    if (!bus)
        return 0xFFU;
    if ((addr & 0x00FF0000U) == 0x00C00000U) {
        bus->sync_hardware(cpu->cycles);
    }
    return bus->read8(addr);
}

uint16_t md_bus_read16(MdCpuState* cpu, uint32_t addr) {
    auto* bus = static_cast<dragon::md::Bus*>(cpu->bus);
    if (!bus)
        return 0xFFFFU;
    if ((addr & 0x00FF0000U) == 0x00C00000U) {
        bus->sync_hardware(cpu->cycles);
    }
    return bus->read16(addr);
}

uint32_t md_bus_read32(MdCpuState* cpu, uint32_t addr) {
    auto* bus = static_cast<dragon::md::Bus*>(cpu->bus);
    if (!bus)
        return 0xFFFFFFFFU;
    if ((addr & 0x00FF0000U) == 0x00C00000U) {
        bus->sync_hardware(cpu->cycles);
    }
    return bus->read32(addr);
}

void md_bus_write8(MdCpuState* cpu, uint32_t addr, uint8_t value) {
    auto* bus = static_cast<dragon::md::Bus*>(cpu->bus);
    if (!bus)
        return;
    if ((addr & 0x00FF0000U) == 0x00C00000U || (addr & 0x00FF0000U) == 0x00A10000U) {
        bus->sync_hardware(cpu->cycles);
    }
    bus->write8(addr, value);
}

void md_bus_write16(MdCpuState* cpu, uint32_t addr, uint16_t value) {
    auto* bus = static_cast<dragon::md::Bus*>(cpu->bus);
    if (!bus)
        return;
    if ((addr & 0x00FF0000U) == 0x00C00000U || (addr & 0x00FF0000U) == 0x00A10000U) {
        bus->sync_hardware(cpu->cycles);
    }
    bus->write16(addr, value);
}

void md_bus_write32(MdCpuState* cpu, uint32_t addr, uint32_t value) {
    auto* bus = static_cast<dragon::md::Bus*>(cpu->bus);
    if (!bus)
        return;
    if ((addr & 0x00FF0000U) == 0x00C00000U || (addr & 0x00FF0000U) == 0x00A10000U) {
        bus->sync_hardware(cpu->cycles);
    }
    bus->write32(addr, value);
}

void md_runtime_exception(MdCpuState* cpu, uint8_t vector_num) {
    auto* bus = static_cast<dragon::md::Bus*>(cpu->bus);
    if (!bus)
        return;
    dragon::md::cpu_exception(*cpu, *bus, vector_num);
}

int md_runtime_poll(MdCpuState* cpu) {
    auto* bus = static_cast<dragon::md::Bus*>(cpu->bus);
    if (!bus || bus->is_flat_ram_mode()) {
        cpu->next_event = cpu->cycles + 128000ULL;
        return 0;
    }

    bus->sync_hardware(cpu->cycles);

    cpu->next_event = cpu->cycles + dragon::md::Vdp::kCyclesPerScanline;

    if (bus->vdp().consume_frame_complete()) {
        bus->on_frame_complete(cpu);
        if (cpu->stopped == 2U) {
            return 1;
        }
    }

    const uint32_t irq_cyc = dragon::md::cpu_check_interrupts(*cpu, *bus);
    if (irq_cyc > 0U) {
        md_runtime_dispatch(cpu, cpu->pc, cpu->pc);
    }

    return (cpu->stopped == 2U) ? 1 : 0;
}

int md_runtime_stop(MdCpuState* cpu, uint16_t new_sr) {
    auto* bus = static_cast<dragon::md::Bus*>(cpu->bus);
    md_set_sr(cpu, new_sr);
    cpu->stopped = 1U;
    if (!bus)
        return 0;
    while (cpu->stopped == 1U) {
        cpu->cycles += dragon::md::Vdp::kCyclesPerScanline;
        bus->sync_hardware(cpu->cycles);
        if (bus->vdp().consume_frame_complete()) {
            bus->on_frame_complete(cpu);
            if (cpu->stopped == 2U) {
                return 1;
            }
        }
        const uint32_t irq_cyc = dragon::md::cpu_check_interrupts(*cpu, *bus);
        if (irq_cyc > 0U) {
            md_runtime_dispatch(cpu, cpu->pc, cpu->pc);
            break;
        }
    }
    cpu->next_event = cpu->cycles + dragon::md::Vdp::kCyclesPerScanline;
    return (cpu->stopped == 2U) ? 1 : 0;
}

int md_runtime_dispatch(MdCpuState* cpu, uint32_t target, uint32_t from_pc) {
    auto* bus = static_cast<dragon::md::Bus*>(cpu->bus);
    if (!bus)
        return 0;

    cpu->pc = target & 0x00FFFFFFU;
    const uint32_t entry_sp = cpu->a[7];

    if (!bus->lookup_function(cpu->pc)) {
        bus->log_fallback(cpu->pc, from_pc);
    }

    while (cpu->stopped != 2U) {
        if (MdRecompFunc fn = bus->lookup_function(cpu->pc)) {
            fn(cpu);
            if (cpu->stopped == 2U || cpu->a[7] > entry_sp) {
                return (cpu->stopped == 2U) ? 1 : 0;
            }
            continue;
        }

        const uint16_t op = bus->read16(cpu->pc);
        const bool is_ret = (op == 0x4E75U || op == 0x4E73U || op == 0x4E77U);

        dragon::md::step_instruction(*cpu, *bus);

        if (cpu->cycles >= cpu->next_event) {
            if (md_runtime_poll(cpu)) {
                return 1;
            }
        }

        if (is_ret && cpu->a[7] > entry_sp) {
            return 0;
        }
    }
    return 1;
}

int md_runtime_step(MdCpuState* cpu) {
    auto* bus = static_cast<dragon::md::Bus*>(cpu->bus);
    if (!bus)
        return 0;
    return static_cast<int>(dragon::md::step_instruction(*cpu, *bus));
}

}  // extern "C"
