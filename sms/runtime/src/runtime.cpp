#include "sms/runtime.h"

namespace dragon::sms {

const char* runtime_version() {
    return "0.1.0";
}

}  // namespace dragon::sms

extern "C" {

uint8_t sms_bus_read(SmsCpuState* cpu, uint16_t addr) {
    return static_cast<dragon::sms::Bus*>(cpu->bus)->read(addr);
}

void sms_bus_write(SmsCpuState* cpu, uint16_t addr, uint8_t value) {
    static_cast<dragon::sms::Bus*>(cpu->bus)->write(addr, value);
}

uint8_t sms_io_read(SmsCpuState* cpu, uint8_t port) {
    auto* bus = static_cast<dragon::sms::Bus*>(cpu->bus);
    bus->sync_hardware(*cpu);
    return bus->io_read(port);
}

void sms_io_write(SmsCpuState* cpu, uint8_t port, uint8_t value) {
    auto* bus = static_cast<dragon::sms::Bus*>(cpu->bus);
    bus->sync_hardware(*cpu);
    bus->io_write(port, value);
}

int sms_runtime_poll(SmsCpuState* cpu) {
    auto* bus = static_cast<dragon::sms::Bus*>(cpu->bus);
    bus->sync_hardware(*cpu);
    if (bus->stop_requested()) {
        return 1;
    }
    const uint32_t irq_cyc = dragon::sms::check_interrupts(*cpu, *bus);
    if (irq_cyc > 0U) {
        const uint16_t vec_pc = cpu->pc;
        if (sms_runtime_dispatch(cpu, vec_pc, 0xFFFFU) != 0) {
            return 1;
        }
    }
    return bus->stop_requested() ? 1 : 0;
}

int sms_runtime_halt(SmsCpuState* cpu) {
    auto* bus = static_cast<dragon::sms::Bus*>(cpu->bus);
    cpu->halted = 1U;
    while (cpu->halted != 0U && !bus->stop_requested()) {
        const uint64_t step =
            (cpu->next_event > cpu->cycles) ? (cpu->next_event - cpu->cycles) : 4ULL;
        const uint64_t aligned = ((step + 3ULL) / 4ULL) * 4ULL;
        cpu->cycles += aligned;
        if (sms_runtime_poll(cpu) != 0) {
            return 1;
        }
    }
    return bus->stop_requested() ? 1 : 0;
}

int sms_runtime_dispatch(SmsCpuState* cpu, uint16_t target, uint16_t from_pc) {
    auto* bus = static_cast<dragon::sms::Bus*>(cpu->bus);
    cpu->pc = target;

    if (target < 0xC000U) {
        const uint8_t bank = bus->cartridge().current_bank(target);
        if (SmsRecompFunc fn = bus->lookup_recomp(bank, target)) {
            fn(cpu);
            return bus->stop_requested() ? 1 : 0;
        }
        bus->record_fallback(bank, target, from_pc);
    } else {
        bus->record_fallback(0xFFU, target, from_pc);
    }

    // Interpreter fallback: run until returning past entry stack pointer or stop requested.
    const uint16_t entry_sp = cpu->sp;
    while (!bus->stop_requested()) {
        const uint32_t irq_c = dragon::sms::check_interrupts(*cpu, *bus);
        if (irq_c > 0U) {
            bus->sync_hardware(*cpu);
            if (bus->stop_requested()) {
                return 1;
            }
        }
        if (cpu->halted != 0U) {
            if (sms_runtime_halt(cpu) != 0) {
                return 1;
            }
            continue;
        }
        if (cpu->pc < 0xC000U) {
            const uint8_t bank = bus->cartridge().current_bank(cpu->pc);
            if (SmsRecompFunc fn = bus->lookup_recomp(bank, cpu->pc)) {
                fn(cpu);
                if (bus->stop_requested()) {
                    return 1;
                }
                if (static_cast<int16_t>(cpu->sp - entry_sp) > 0) {
                    return 0;
                }
                continue;
            }
        }

        const uint8_t op = bus->read(cpu->pc);
        const uint8_t op2 =
            (op == 0xEDU) ? bus->read(static_cast<uint16_t>(cpu->pc + 1U)) : 0U;
        const bool is_ret =
            (op == 0xC9U || op == 0xC0U || op == 0xC8U || op == 0xD0U || op == 0xD8U ||
             op == 0xE0U || op == 0xE8U || op == 0xF0U || op == 0xF8U ||
             (op == 0xEDU && (op2 == 0x45U || op2 == 0x4DU || op2 == 0x55U || op2 == 0x5DU ||
                              op2 == 0x65U || op2 == 0x6DU || op2 == 0x75U || op2 == 0x7DU)));

        dragon::sms::step_instruction(*cpu, *bus);
        if (is_ret && static_cast<int16_t>(cpu->sp - entry_sp) > 0) {
            return 0;
        }
    }
    return 1;
}

int sms_runtime_step(SmsCpuState* cpu) {
    auto* bus = static_cast<dragon::sms::Bus*>(cpu->bus);
    const uint16_t start_pc = cpu->pc;
    while (!bus->stop_requested()) {
        const uint32_t irq_c = dragon::sms::check_interrupts(*cpu, *bus);
        if (irq_c > 0U) {
            bus->sync_hardware(*cpu);
            if (bus->stop_requested()) {
                return 1;
            }
            if (sms_runtime_dispatch(cpu, cpu->pc, start_pc) != 0) {
                return 1;
            }
            if (cpu->pc != start_pc) {
                return 0;
            }
            continue;
        }
        dragon::sms::step_instruction(*cpu, *bus);
        if (bus->stop_requested()) {
            return 1;
        }
        if (cpu->pc != start_pc || cpu->halted != 0U) {
            break;
        }
    }
    return bus->stop_requested() ? 1 : 0;
}

}  // extern "C"
