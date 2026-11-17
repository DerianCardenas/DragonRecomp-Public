#include "gb/interpreter.h"

#include <cassert>
#include <cstdint>

#include "gb/opcodes.h"

namespace dragon::gb {

namespace {

inline uint8_t fetch8(CpuState& cpu, Bus& bus) {
    uint8_t byte = bus.read(cpu.pc);
    if (cpu.halt_bug) {
        cpu.halt_bug = 0;
    } else {
        cpu.pc++;
    }
    return byte;
}

inline uint16_t fetch16(CpuState& cpu, Bus& bus) {
    uint8_t lo = fetch8(cpu, bus);
    uint8_t hi = fetch8(cpu, bus);
    return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
}

inline void alu_add(CpuState& cpu, uint8_t val) {
    uint16_t res = static_cast<uint16_t>(cpu.a) + val;
    cpu.f = 0;
    if ((res & 0xFFU) == 0)
        cpu.f |= GB_FLAG_Z;
    if (((cpu.a & 0x0FU) + (val & 0x0FU)) > 0x0FU)
        cpu.f |= GB_FLAG_H;
    if (res > 0xFFU)
        cpu.f |= GB_FLAG_C;
    cpu.a = static_cast<uint8_t>(res);
}

inline void alu_adc(CpuState& cpu, uint8_t val) {
    uint8_t c = (cpu.f & GB_FLAG_C) ? 1 : 0;
    uint16_t res = static_cast<uint16_t>(cpu.a) + val + c;
    cpu.f = 0;
    if ((res & 0xFFU) == 0)
        cpu.f |= GB_FLAG_Z;
    if (((cpu.a & 0x0FU) + (val & 0x0FU) + c) > 0x0FU)
        cpu.f |= GB_FLAG_H;
    if (res > 0xFFU)
        cpu.f |= GB_FLAG_C;
    cpu.a = static_cast<uint8_t>(res);
}

inline void alu_sub(CpuState& cpu, uint8_t val) {
    uint16_t res = static_cast<uint16_t>(cpu.a) - val;
    cpu.f = GB_FLAG_N;
    if ((res & 0xFFU) == 0)
        cpu.f |= GB_FLAG_Z;
    if ((cpu.a & 0x0FU) < (val & 0x0FU))
        cpu.f |= GB_FLAG_H;
    if (cpu.a < val)
        cpu.f |= GB_FLAG_C;
    cpu.a = static_cast<uint8_t>(res);
}

inline void alu_sbc(CpuState& cpu, uint8_t val) {
    uint8_t c = (cpu.f & GB_FLAG_C) ? 1 : 0;
    int32_t res = static_cast<int32_t>(cpu.a) - val - c;
    cpu.f = GB_FLAG_N;
    if ((res & 0xFF) == 0)
        cpu.f |= GB_FLAG_Z;
    if ((cpu.a & 0x0FU) < ((val & 0x0FU) + c))
        cpu.f |= GB_FLAG_H;
    if (cpu.a < (static_cast<uint16_t>(val) + c))
        cpu.f |= GB_FLAG_C;
    cpu.a = static_cast<uint8_t>(res);
}

inline void alu_and(CpuState& cpu, uint8_t val) {
    cpu.a &= val;
    cpu.f = GB_FLAG_H | (cpu.a == 0 ? GB_FLAG_Z : 0);
}

inline void alu_xor(CpuState& cpu, uint8_t val) {
    cpu.a ^= val;
    cpu.f = (cpu.a == 0 ? GB_FLAG_Z : 0);
}

inline void alu_or(CpuState& cpu, uint8_t val) {
    cpu.a |= val;
    cpu.f = (cpu.a == 0 ? GB_FLAG_Z : 0);
}

inline void alu_cp(CpuState& cpu, uint8_t val) {
    uint8_t orig_a = cpu.a;
    alu_sub(cpu, val);
    cpu.a = orig_a;
}

inline uint8_t alu_inc(CpuState& cpu, uint8_t val) {
    uint8_t res = val + 1U;
    cpu.f = (cpu.f & GB_FLAG_C);  // Preserve C flag
    if (res == 0)
        cpu.f |= GB_FLAG_Z;
    if ((val & 0x0FU) == 0x0FU)
        cpu.f |= GB_FLAG_H;
    return res;
}

inline uint8_t alu_dec(CpuState& cpu, uint8_t val) {
    uint8_t res = val - 1U;
    cpu.f = (cpu.f & GB_FLAG_C) | GB_FLAG_N;  // Preserve C flag, set N
    if (res == 0)
        cpu.f |= GB_FLAG_Z;
    if ((val & 0x0FU) == 0)
        cpu.f |= GB_FLAG_H;
    return res;
}

inline void alu_add_hl(CpuState& cpu, uint16_t val) {
    uint16_t hl = cpu_hl(&cpu);
    uint32_t res = static_cast<uint32_t>(hl) + val;
    cpu.f &= GB_FLAG_Z;  // Preserve Z flag, clear N
    if (((hl & 0x0FFFU) + (val & 0x0FFFU)) > 0x0FFFU)
        cpu.f |= GB_FLAG_H;
    if (res > 0xFFFFU)
        cpu.f |= GB_FLAG_C;
    cpu_set_hl(&cpu, static_cast<uint16_t>(res));
}

inline uint8_t cb_rlc(CpuState& cpu, uint8_t val) {
    uint8_t c = (val >> 7U) & 1U;
    uint8_t res = static_cast<uint8_t>((val << 1U) | c);
    cpu.f = (c ? GB_FLAG_C : 0) | (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

inline uint8_t cb_rrc(CpuState& cpu, uint8_t val) {
    uint8_t c = val & 1U;
    uint8_t res = static_cast<uint8_t>((val >> 1U) | (c << 7U));
    cpu.f = (c ? GB_FLAG_C : 0) | (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

inline uint8_t cb_rl(CpuState& cpu, uint8_t val) {
    uint8_t old_c = (cpu.f & GB_FLAG_C) ? 1U : 0U;
    uint8_t new_c = (val >> 7U) & 1U;
    uint8_t res = static_cast<uint8_t>((val << 1U) | old_c);
    cpu.f = (new_c ? GB_FLAG_C : 0) | (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

inline uint8_t cb_rr(CpuState& cpu, uint8_t val) {
    uint8_t old_c = (cpu.f & GB_FLAG_C) ? 1U : 0U;
    uint8_t new_c = val & 1U;
    uint8_t res = static_cast<uint8_t>((val >> 1U) | (old_c << 7U));
    cpu.f = (new_c ? GB_FLAG_C : 0) | (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

inline uint8_t cb_sla(CpuState& cpu, uint8_t val) {
    uint8_t c = (val >> 7U) & 1U;
    uint8_t res = static_cast<uint8_t>(val << 1U);
    cpu.f = (c ? GB_FLAG_C : 0) | (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

inline uint8_t cb_sra(CpuState& cpu, uint8_t val) {
    uint8_t c = val & 1U;
    uint8_t res = static_cast<uint8_t>((val >> 1U) | (val & 0x80U));
    cpu.f = (c ? GB_FLAG_C : 0) | (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

inline uint8_t cb_swap(CpuState& cpu, uint8_t val) {
    uint8_t res = static_cast<uint8_t>(((val & 0x0FU) << 4U) | ((val & 0xF0U) >> 4U));
    cpu.f = (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

inline uint8_t cb_srl(CpuState& cpu, uint8_t val) {
    uint8_t c = val & 1U;
    uint8_t res = static_cast<uint8_t>(val >> 1U);
    cpu.f = (c ? GB_FLAG_C : 0) | (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

inline void cb_bit(CpuState& cpu, uint8_t bit, uint8_t val) {
    cpu.f = (cpu.f & GB_FLAG_C) | GB_FLAG_H;  // Preserve C, set H
    if ((val & (1U << bit)) == 0)
        cpu.f |= GB_FLAG_Z;
}

uint32_t execute_cb(CpuState& cpu, Bus& bus) {
    uint8_t op = fetch8(cpu, bus);
    uint8_t reg_idx = op & 0x07U;
    uint8_t bit = (op >> 3U) & 0x07U;

    auto get_reg = [&](uint8_t idx) -> uint8_t {
        switch (idx) {
            case 0:
                return cpu.b;
            case 1:
                return cpu.c;
            case 2:
                return cpu.d;
            case 3:
                return cpu.e;
            case 4:
                return cpu.h;
            case 5:
                return cpu.l;
            case 6:
                return bus.read(cpu_hl(&cpu));
            case 7:
                return cpu.a;
            default:
                return 0;
        }
    };

    auto set_reg = [&](uint8_t idx, uint8_t val) {
        switch (idx) {
            case 0:
                cpu.b = val;
                break;
            case 1:
                cpu.c = val;
                break;
            case 2:
                cpu.d = val;
                break;
            case 3:
                cpu.e = val;
                break;
            case 4:
                cpu.h = val;
                break;
            case 5:
                cpu.l = val;
                break;
            case 6:
                bus.write(cpu_hl(&cpu), val);
                break;
            case 7:
                cpu.a = val;
                break;
            default:
                break;
        }
    };

    uint8_t val = get_reg(reg_idx);

    if (op < 0x40) {
        // Shift / Rotate / Swap
        switch ((op >> 3U) & 0x07U) {
            case 0:
                val = cb_rlc(cpu, val);
                break;
            case 1:
                val = cb_rrc(cpu, val);
                break;
            case 2:
                val = cb_rl(cpu, val);
                break;
            case 3:
                val = cb_rr(cpu, val);
                break;
            case 4:
                val = cb_sla(cpu, val);
                break;
            case 5:
                val = cb_sra(cpu, val);
                break;
            case 6:
                val = cb_swap(cpu, val);
                break;
            case 7:
                val = cb_srl(cpu, val);
                break;
        }
        set_reg(reg_idx, val);
    } else if (op < 0x80) {
        // BIT b, r
        cb_bit(cpu, bit, val);
    } else if (op < 0xC0) {
        // RES b, r
        val &= ~(1U << bit);
        set_reg(reg_idx, val);
    } else {
        // SET b, r
        val |= (1U << bit);
        set_reg(reg_idx, val);
    }

    return kGbCbOpcodeTable[op].cycles;
}

}  // namespace

void cpu_init(CpuState& cpu, Bus* bus) noexcept {
    cpu = CpuState{};
    cpu.bus = bus;
    if (bus) {
        cpu.hram = bus->hram_data();
        cpu.rom = bus->rom_data();
        cpu.rom_size = bus->rom_size();
    }
    cpu_reset(cpu);
}

void cpu_reset(CpuState& cpu) noexcept {
    // Official Game Boy post-boot state values
    if (cpu.bus && static_cast<Bus*>(cpu.bus)->is_cgb()) {
        cpu.a = 0x11U;  // CGB boot identifier
        cpu.f = 0x80U;  // Z=1
        cpu.b = 0x00U;
        cpu.c = 0x00U;
        cpu.d = 0xFFU;
        cpu.e = 0x56U;
        cpu.h = 0x00U;
        cpu.l = 0x0DU;
    } else {
        cpu.a = 0x01U;  // DMG boot identifier
        cpu.f = 0xB0U;  // Z=1, H=1, C=1
        cpu.b = 0x00U;
        cpu.c = 0x13U;
        cpu.d = 0x00U;
        cpu.e = 0xD8U;
        cpu.h = 0x01U;
        cpu.l = 0x4DU;
    }
    cpu.sp = 0xFFFEU;
    cpu.pc = 0x0100U;
    cpu.ime = 0;
    cpu.ime_delay = 0;
    cpu.halted = 0;
    cpu.halt_bug = 0;
    cpu.double_speed = 0;
    cpu.cycles = 0;
    cpu.next_event = 0;
}

uint32_t cpu_check_interrupts(CpuState& cpu) noexcept {
    if (!cpu.bus)
        return 0;
    auto* bus = static_cast<Bus*>(cpu.bus);

    uint8_t pending = bus->interrupt_flags() & bus->interrupt_enable() & 0x1FU;
    if (pending == 0)
        return 0;

    // Wake CPU from HALT even if IME is disabled
    if (cpu.halted) {
        cpu.halted = 0;
    }

    if (!cpu.ime)
        return 0;

    // Service highest priority interrupt
    for (uint8_t bit = 0; bit < 5; ++bit) {
        if (pending & (1U << bit)) {
            cpu.ime = 0;
            bus->clear_interrupt(bit);
            cpu_push(&cpu, cpu.pc);
            cpu.pc = static_cast<uint16_t>(0x0040U + (bit * 8U));
            cpu.cycles += 20;
            bus->step_timers(20);
            bus->sync_ppu(cpu.cycles);
            bus->sync_apu(cpu.cycles);
            return 20;
        }
    }
    return 0;
}

uint32_t cpu_step(CpuState& cpu) noexcept {
    auto* bus = static_cast<Bus*>(cpu.bus);
    assert(bus != nullptr);

    // Check for interrupts before instruction fetch
    uint32_t int_cycles = cpu_check_interrupts(cpu);
    if (int_cycles > 0) {
        return int_cycles;
    }

    // Update EI delay after interrupt check so the instruction immediately
    // following EI executes before any pending interrupt is serviced.
    if (cpu.ime_delay > 0) {
        cpu.ime_delay--;
        if (cpu.ime_delay == 0) {
            cpu.ime = 1;
        }
    }

    if (cpu.halted) {
        cpu.cycles += 4;
        bus->step_timers(4);
        bus->sync_ppu(cpu.cycles);
        bus->sync_apu(cpu.cycles);
        return 4;
    }

    uint8_t op = fetch8(cpu, *bus);
    uint32_t cycles = kGbBaseOpcodeTable[op].cycles;

    switch (op) {
        case 0x00:
            break;  // NOP
        case 0x01:
            cpu_set_bc(&cpu, fetch16(cpu, *bus));
            break;
        case 0x02:
            bus->write(cpu_bc(&cpu), cpu.a);
            break;
        case 0x03:
            cpu_set_bc(&cpu, cpu_bc(&cpu) + 1U);
            break;
        case 0x04:
            cpu.b = alu_inc(cpu, cpu.b);
            break;
        case 0x05:
            cpu.b = alu_dec(cpu, cpu.b);
            break;
        case 0x06:
            cpu.b = fetch8(cpu, *bus);
            break;
        case 0x07: {  // RLCA
            uint8_t c = (cpu.a >> 7U) & 1U;
            cpu.a = static_cast<uint8_t>((cpu.a << 1U) | c);
            cpu.f = c ? GB_FLAG_C : 0;
            break;
        }
        case 0x08: {  // LD (a16), SP
            uint16_t addr = fetch16(cpu, *bus);
            bus->write(addr, static_cast<uint8_t>(cpu.sp & 0xFFU));
            bus->write(addr + 1U, static_cast<uint8_t>((cpu.sp >> 8U) & 0xFFU));
            break;
        }
        case 0x09:
            alu_add_hl(cpu, cpu_bc(&cpu));
            break;
        case 0x0A:
            cpu.a = bus->read(cpu_bc(&cpu));
            break;
        case 0x0B:
            cpu_set_bc(&cpu, cpu_bc(&cpu) - 1U);
            break;
        case 0x0C:
            cpu.c = alu_inc(cpu, cpu.c);
            break;
        case 0x0D:
            cpu.c = alu_dec(cpu, cpu.c);
            break;
        case 0x0E:
            cpu.c = fetch8(cpu, *bus);
            break;
        case 0x0F: {  // RRCA
            uint8_t c = cpu.a & 1U;
            cpu.a = static_cast<uint8_t>((cpu.a >> 1U) | (c << 7U));
            cpu.f = c ? GB_FLAG_C : 0;
            break;
        }

        case 0x10: {            // STOP
            fetch8(cpu, *bus);  // consume 0x00
            // If speed switch requested on CGB
            if (bus->is_cgb() && (bus->read(0xFF4DU) & 0x01U)) {
                bus->toggle_double_speed();
                cpu.double_speed = bus->is_double_speed();
            }
            break;
        }
        case 0x11:
            cpu_set_de(&cpu, fetch16(cpu, *bus));
            break;
        case 0x12:
            bus->write(cpu_de(&cpu), cpu.a);
            break;
        case 0x13:
            cpu_set_de(&cpu, cpu_de(&cpu) + 1U);
            break;
        case 0x14:
            cpu.d = alu_inc(cpu, cpu.d);
            break;
        case 0x15:
            cpu.d = alu_dec(cpu, cpu.d);
            break;
        case 0x16:
            cpu.d = fetch8(cpu, *bus);
            break;
        case 0x17: {  // RLA
            uint8_t old_c = (cpu.f & GB_FLAG_C) ? 1U : 0U;
            uint8_t new_c = (cpu.a >> 7U) & 1U;
            cpu.a = static_cast<uint8_t>((cpu.a << 1U) | old_c);
            cpu.f = new_c ? GB_FLAG_C : 0;
            break;
        }
        case 0x18: {  // JR r8
            int8_t offset = static_cast<int8_t>(fetch8(cpu, *bus));
            cpu.pc = static_cast<uint16_t>(cpu.pc + offset);
            break;
        }
        case 0x19:
            alu_add_hl(cpu, cpu_de(&cpu));
            break;
        case 0x1A:
            cpu.a = bus->read(cpu_de(&cpu));
            break;
        case 0x1B:
            cpu_set_de(&cpu, cpu_de(&cpu) - 1U);
            break;
        case 0x1C:
            cpu.e = alu_inc(cpu, cpu.e);
            break;
        case 0x1D:
            cpu.e = alu_dec(cpu, cpu.e);
            break;
        case 0x1E:
            cpu.e = fetch8(cpu, *bus);
            break;
        case 0x1F: {  // RRA
            uint8_t old_c = (cpu.f & GB_FLAG_C) ? 1U : 0U;
            uint8_t new_c = cpu.a & 1U;
            cpu.a = static_cast<uint8_t>((cpu.a >> 1U) | (old_c << 7U));
            cpu.f = new_c ? GB_FLAG_C : 0;
            break;
        }

        case 0x20: {  // JR NZ, r8
            int8_t offset = static_cast<int8_t>(fetch8(cpu, *bus));
            if (!(cpu.f & GB_FLAG_Z)) {
                cpu.pc = static_cast<uint16_t>(cpu.pc + offset);
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0x21:
            cpu_set_hl(&cpu, fetch16(cpu, *bus));
            break;
        case 0x22:
            bus->write(cpu_hl(&cpu), cpu.a);
            cpu_set_hl(&cpu, cpu_hl(&cpu) + 1U);
            break;
        case 0x23:
            cpu_set_hl(&cpu, cpu_hl(&cpu) + 1U);
            break;
        case 0x24:
            cpu.h = alu_inc(cpu, cpu.h);
            break;
        case 0x25:
            cpu.h = alu_dec(cpu, cpu.h);
            break;
        case 0x26:
            cpu.h = fetch8(cpu, *bus);
            break;
        case 0x27: {  // DAA
            if (!(cpu.f & GB_FLAG_N)) {
                if ((cpu.f & GB_FLAG_C) || cpu.a > 0x99U) {
                    cpu.a += 0x60U;
                    cpu.f |= GB_FLAG_C;
                }
                if ((cpu.f & GB_FLAG_H) || (cpu.a & 0x0FU) > 0x09U) {
                    cpu.a += 0x06U;
                }
            } else {
                if (cpu.f & GB_FLAG_C) {
                    cpu.a -= 0x60U;
                }
                if (cpu.f & GB_FLAG_H) {
                    cpu.a -= 0x06U;
                }
            }
            cpu.f = (cpu.f & (GB_FLAG_N | GB_FLAG_C)) | (cpu.a == 0 ? GB_FLAG_Z : 0);
            break;
        }
        case 0x28: {  // JR Z, r8
            int8_t offset = static_cast<int8_t>(fetch8(cpu, *bus));
            if (cpu.f & GB_FLAG_Z) {
                cpu.pc = static_cast<uint16_t>(cpu.pc + offset);
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0x29:
            alu_add_hl(cpu, cpu_hl(&cpu));
            break;
        case 0x2A:
            cpu.a = bus->read(cpu_hl(&cpu));
            cpu_set_hl(&cpu, cpu_hl(&cpu) + 1U);
            break;
        case 0x2B:
            cpu_set_hl(&cpu, cpu_hl(&cpu) - 1U);
            break;
        case 0x2C:
            cpu.l = alu_inc(cpu, cpu.l);
            break;
        case 0x2D:
            cpu.l = alu_dec(cpu, cpu.l);
            break;
        case 0x2E:
            cpu.l = fetch8(cpu, *bus);
            break;
        case 0x2F: {  // CPL
            cpu.a = ~cpu.a;
            cpu.f |= (GB_FLAG_N | GB_FLAG_H);
            break;
        }

        case 0x30: {  // JR NC, r8
            int8_t offset = static_cast<int8_t>(fetch8(cpu, *bus));
            if (!(cpu.f & GB_FLAG_C)) {
                cpu.pc = static_cast<uint16_t>(cpu.pc + offset);
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0x31:
            cpu.sp = fetch16(cpu, *bus);
            break;
        case 0x32:
            bus->write(cpu_hl(&cpu), cpu.a);
            cpu_set_hl(&cpu, cpu_hl(&cpu) - 1U);
            break;
        case 0x33:
            cpu.sp++;
            break;
        case 0x34: {
            uint16_t hl = cpu_hl(&cpu);
            bus->write(hl, alu_inc(cpu, bus->read(hl)));
            break;
        }
        case 0x35: {
            uint16_t hl = cpu_hl(&cpu);
            bus->write(hl, alu_dec(cpu, bus->read(hl)));
            break;
        }
        case 0x36:
            bus->write(cpu_hl(&cpu), fetch8(cpu, *bus));
            break;
        case 0x37:
            cpu.f = (cpu.f & GB_FLAG_Z) | GB_FLAG_C;
            break;    // SCF
        case 0x38: {  // JR C, r8
            int8_t offset = static_cast<int8_t>(fetch8(cpu, *bus));
            if (cpu.f & GB_FLAG_C) {
                cpu.pc = static_cast<uint16_t>(cpu.pc + offset);
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0x39:
            alu_add_hl(cpu, cpu.sp);
            break;
        case 0x3A:
            cpu.a = bus->read(cpu_hl(&cpu));
            cpu_set_hl(&cpu, cpu_hl(&cpu) - 1U);
            break;
        case 0x3B:
            cpu.sp--;
            break;
        case 0x3C:
            cpu.a = alu_inc(cpu, cpu.a);
            break;
        case 0x3D:
            cpu.a = alu_dec(cpu, cpu.a);
            break;
        case 0x3E:
            cpu.a = fetch8(cpu, *bus);
            break;
        case 0x3F:
            cpu.f = (cpu.f & GB_FLAG_Z) | ((cpu.f & GB_FLAG_C) ? 0 : GB_FLAG_C);
            break;  // CCF

        // 0x40 - 0x7F: LD r, r'
        case 0x40:
            break;
        case 0x41:
            cpu.b = cpu.c;
            break;
        case 0x42:
            cpu.b = cpu.d;
            break;
        case 0x43:
            cpu.b = cpu.e;
            break;
        case 0x44:
            cpu.b = cpu.h;
            break;
        case 0x45:
            cpu.b = cpu.l;
            break;
        case 0x46:
            cpu.b = bus->read(cpu_hl(&cpu));
            break;
        case 0x47:
            cpu.b = cpu.a;
            break;

        case 0x48:
            cpu.c = cpu.b;
            break;
        case 0x49:
            break;
        case 0x4A:
            cpu.c = cpu.d;
            break;
        case 0x4B:
            cpu.c = cpu.e;
            break;
        case 0x4C:
            cpu.c = cpu.h;
            break;
        case 0x4D:
            cpu.c = cpu.l;
            break;
        case 0x4E:
            cpu.c = bus->read(cpu_hl(&cpu));
            break;
        case 0x4F:
            cpu.c = cpu.a;
            break;

        case 0x50:
            cpu.d = cpu.b;
            break;
        case 0x51:
            cpu.d = cpu.c;
            break;
        case 0x52:
            break;
        case 0x53:
            cpu.d = cpu.e;
            break;
        case 0x54:
            cpu.d = cpu.h;
            break;
        case 0x55:
            cpu.d = cpu.l;
            break;
        case 0x56:
            cpu.d = bus->read(cpu_hl(&cpu));
            break;
        case 0x57:
            cpu.d = cpu.a;
            break;

        case 0x58:
            cpu.e = cpu.b;
            break;
        case 0x59:
            cpu.e = cpu.c;
            break;
        case 0x5A:
            cpu.e = cpu.d;
            break;
        case 0x5B:
            break;
        case 0x5C:
            cpu.e = cpu.h;
            break;
        case 0x5D:
            cpu.e = cpu.l;
            break;
        case 0x5E:
            cpu.e = bus->read(cpu_hl(&cpu));
            break;
        case 0x5F:
            cpu.e = cpu.a;
            break;

        case 0x60:
            cpu.h = cpu.b;
            break;
        case 0x61:
            cpu.h = cpu.c;
            break;
        case 0x62:
            cpu.h = cpu.d;
            break;
        case 0x63:
            cpu.h = cpu.e;
            break;
        case 0x64:
            break;
        case 0x65:
            cpu.h = cpu.l;
            break;
        case 0x66:
            cpu.h = bus->read(cpu_hl(&cpu));
            break;
        case 0x67:
            cpu.h = cpu.a;
            break;

        case 0x68:
            cpu.l = cpu.b;
            break;
        case 0x69:
            cpu.l = cpu.c;
            break;
        case 0x6A:
            cpu.l = cpu.d;
            break;
        case 0x6B:
            cpu.l = cpu.e;
            break;
        case 0x6C:
            cpu.l = cpu.h;
            break;
        case 0x6D:
            break;
        case 0x6E:
            cpu.l = bus->read(cpu_hl(&cpu));
            break;
        case 0x6F:
            cpu.l = cpu.a;
            break;

        case 0x70:
            bus->write(cpu_hl(&cpu), cpu.b);
            break;
        case 0x71:
            bus->write(cpu_hl(&cpu), cpu.c);
            break;
        case 0x72:
            bus->write(cpu_hl(&cpu), cpu.d);
            break;
        case 0x73:
            bus->write(cpu_hl(&cpu), cpu.e);
            break;
        case 0x74:
            bus->write(cpu_hl(&cpu), cpu.h);
            break;
        case 0x75:
            bus->write(cpu_hl(&cpu), cpu.l);
            break;
        case 0x76: {  // HALT
            uint8_t pending = bus->interrupt_flags() & bus->interrupt_enable() & 0x1FU;
            if (!cpu.ime && pending != 0) {
                cpu.halt_bug = 1;
            } else {
                cpu.halted = 1;
            }
            break;
        }
        case 0x77:
            bus->write(cpu_hl(&cpu), cpu.a);
            break;
        case 0x78:
            cpu.a = cpu.b;
            break;
        case 0x79:
            cpu.a = cpu.c;
            break;
        case 0x7A:
            cpu.a = cpu.d;
            break;
        case 0x7B:
            cpu.a = cpu.e;
            break;
        case 0x7C:
            cpu.a = cpu.h;
            break;
        case 0x7D:
            cpu.a = cpu.l;
            break;
        case 0x7E:
            cpu.a = bus->read(cpu_hl(&cpu));
            break;
        case 0x7F:
            break;

        // 0x80 - 0xBF: ALU A, r
        case 0x80:
            alu_add(cpu, cpu.b);
            break;
        case 0x81:
            alu_add(cpu, cpu.c);
            break;
        case 0x82:
            alu_add(cpu, cpu.d);
            break;
        case 0x83:
            alu_add(cpu, cpu.e);
            break;
        case 0x84:
            alu_add(cpu, cpu.h);
            break;
        case 0x85:
            alu_add(cpu, cpu.l);
            break;
        case 0x86:
            alu_add(cpu, bus->read(cpu_hl(&cpu)));
            break;
        case 0x87:
            alu_add(cpu, cpu.a);
            break;

        case 0x88:
            alu_adc(cpu, cpu.b);
            break;
        case 0x89:
            alu_adc(cpu, cpu.c);
            break;
        case 0x8A:
            alu_adc(cpu, cpu.d);
            break;
        case 0x8B:
            alu_adc(cpu, cpu.e);
            break;
        case 0x8C:
            alu_adc(cpu, cpu.h);
            break;
        case 0x8D:
            alu_adc(cpu, cpu.l);
            break;
        case 0x8E:
            alu_adc(cpu, bus->read(cpu_hl(&cpu)));
            break;
        case 0x8F:
            alu_adc(cpu, cpu.a);
            break;

        case 0x90:
            alu_sub(cpu, cpu.b);
            break;
        case 0x91:
            alu_sub(cpu, cpu.c);
            break;
        case 0x92:
            alu_sub(cpu, cpu.d);
            break;
        case 0x93:
            alu_sub(cpu, cpu.e);
            break;
        case 0x94:
            alu_sub(cpu, cpu.h);
            break;
        case 0x95:
            alu_sub(cpu, cpu.l);
            break;
        case 0x96:
            alu_sub(cpu, bus->read(cpu_hl(&cpu)));
            break;
        case 0x97:
            alu_sub(cpu, cpu.a);
            break;

        case 0x98:
            alu_sbc(cpu, cpu.b);
            break;
        case 0x99:
            alu_sbc(cpu, cpu.c);
            break;
        case 0x9A:
            alu_sbc(cpu, cpu.d);
            break;
        case 0x9B:
            alu_sbc(cpu, cpu.e);
            break;
        case 0x9C:
            alu_sbc(cpu, cpu.h);
            break;
        case 0x9D:
            alu_sbc(cpu, cpu.l);
            break;
        case 0x9E:
            alu_sbc(cpu, bus->read(cpu_hl(&cpu)));
            break;
        case 0x9F:
            alu_sbc(cpu, cpu.a);
            break;

        case 0xA0:
            alu_and(cpu, cpu.b);
            break;
        case 0xA1:
            alu_and(cpu, cpu.c);
            break;
        case 0xA2:
            alu_and(cpu, cpu.d);
            break;
        case 0xA3:
            alu_and(cpu, cpu.e);
            break;
        case 0xA4:
            alu_and(cpu, cpu.h);
            break;
        case 0xA5:
            alu_and(cpu, cpu.l);
            break;
        case 0xA6:
            alu_and(cpu, bus->read(cpu_hl(&cpu)));
            break;
        case 0xA7:
            alu_and(cpu, cpu.a);
            break;

        case 0xA8:
            alu_xor(cpu, cpu.b);
            break;
        case 0xA9:
            alu_xor(cpu, cpu.c);
            break;
        case 0xAA:
            alu_xor(cpu, cpu.d);
            break;
        case 0xAB:
            alu_xor(cpu, cpu.e);
            break;
        case 0xAC:
            alu_xor(cpu, cpu.h);
            break;
        case 0xAD:
            alu_xor(cpu, cpu.l);
            break;
        case 0xAE:
            alu_xor(cpu, bus->read(cpu_hl(&cpu)));
            break;
        case 0xAF:
            alu_xor(cpu, cpu.a);
            break;

        case 0xB0:
            alu_or(cpu, cpu.b);
            break;
        case 0xB1:
            alu_or(cpu, cpu.c);
            break;
        case 0xB2:
            alu_or(cpu, cpu.d);
            break;
        case 0xB3:
            alu_or(cpu, cpu.e);
            break;
        case 0xB4:
            alu_or(cpu, cpu.h);
            break;
        case 0xB5:
            alu_or(cpu, cpu.l);
            break;
        case 0xB6:
            alu_or(cpu, bus->read(cpu_hl(&cpu)));
            break;
        case 0xB7:
            alu_or(cpu, cpu.a);
            break;

        case 0xB8:
            alu_cp(cpu, cpu.b);
            break;
        case 0xB9:
            alu_cp(cpu, cpu.c);
            break;
        case 0xBA:
            alu_cp(cpu, cpu.d);
            break;
        case 0xBB:
            alu_cp(cpu, cpu.e);
            break;
        case 0xBC:
            alu_cp(cpu, cpu.h);
            break;
        case 0xBD:
            alu_cp(cpu, cpu.l);
            break;
        case 0xBE:
            alu_cp(cpu, bus->read(cpu_hl(&cpu)));
            break;
        case 0xBF:
            alu_cp(cpu, cpu.a);
            break;

        case 0xC0: {  // RET NZ
            if (!(cpu.f & GB_FLAG_Z)) {
                cpu.pc = cpu_pop(&cpu);
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0xC1:
            cpu_set_bc(&cpu, cpu_pop(&cpu));
            break;
        case 0xC2: {  // JP NZ, a16
            uint16_t addr = fetch16(cpu, *bus);
            if (!(cpu.f & GB_FLAG_Z)) {
                cpu.pc = addr;
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0xC3:
            cpu.pc = fetch16(cpu, *bus);
            break;
        case 0xC4: {  // CALL NZ, a16
            uint16_t addr = fetch16(cpu, *bus);
            if (!(cpu.f & GB_FLAG_Z)) {
                cpu_push(&cpu, cpu.pc);
                cpu.pc = addr;
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0xC5:
            cpu_push(&cpu, cpu_bc(&cpu));
            break;
        case 0xC6:
            alu_add(cpu, fetch8(cpu, *bus));
            break;
        case 0xC7:
            cpu_push(&cpu, cpu.pc);
            cpu.pc = 0x0000U;
            break;    // RST 00H
        case 0xC8: {  // RET Z
            if (cpu.f & GB_FLAG_Z) {
                cpu.pc = cpu_pop(&cpu);
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0xC9:
            cpu.pc = cpu_pop(&cpu);
            break;    // RET
        case 0xCA: {  // JP Z, a16
            uint16_t addr = fetch16(cpu, *bus);
            if (cpu.f & GB_FLAG_Z) {
                cpu.pc = addr;
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0xCB:
            cycles = execute_cb(cpu, *bus);
            break;
        case 0xCC: {  // CALL Z, a16
            uint16_t addr = fetch16(cpu, *bus);
            if (cpu.f & GB_FLAG_Z) {
                cpu_push(&cpu, cpu.pc);
                cpu.pc = addr;
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0xCD: {  // CALL a16
            uint16_t addr = fetch16(cpu, *bus);
            cpu_push(&cpu, cpu.pc);
            cpu.pc = addr;
            break;
        }
        case 0xCE:
            alu_adc(cpu, fetch8(cpu, *bus));
            break;
        case 0xCF:
            cpu_push(&cpu, cpu.pc);
            cpu.pc = 0x0008U;
            break;  // RST 08H

        case 0xD0: {  // RET NC
            if (!(cpu.f & GB_FLAG_C)) {
                cpu.pc = cpu_pop(&cpu);
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0xD1:
            cpu_set_de(&cpu, cpu_pop(&cpu));
            break;
        case 0xD2: {  // JP NC, a16
            uint16_t addr = fetch16(cpu, *bus);
            if (!(cpu.f & GB_FLAG_C)) {
                cpu.pc = addr;
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0xD4: {  // CALL NC, a16
            uint16_t addr = fetch16(cpu, *bus);
            if (!(cpu.f & GB_FLAG_C)) {
                cpu_push(&cpu, cpu.pc);
                cpu.pc = addr;
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0xD5:
            cpu_push(&cpu, cpu_de(&cpu));
            break;
        case 0xD6:
            alu_sub(cpu, fetch8(cpu, *bus));
            break;
        case 0xD7:
            cpu_push(&cpu, cpu.pc);
            cpu.pc = 0x0010U;
            break;    // RST 10H
        case 0xD8: {  // RET C
            if (cpu.f & GB_FLAG_C) {
                cpu.pc = cpu_pop(&cpu);
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0xD9: {  // RETI
            cpu.pc = cpu_pop(&cpu);
            cpu.ime = 1;
            break;
        }
        case 0xDA: {  // JP C, a16
            uint16_t addr = fetch16(cpu, *bus);
            if (cpu.f & GB_FLAG_C) {
                cpu.pc = addr;
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0xDC: {  // CALL C, a16
            uint16_t addr = fetch16(cpu, *bus);
            if (cpu.f & GB_FLAG_C) {
                cpu_push(&cpu, cpu.pc);
                cpu.pc = addr;
                cycles = kGbBaseOpcodeTable[op].cycles_branch;
            }
            break;
        }
        case 0xDE:
            alu_sbc(cpu, fetch8(cpu, *bus));
            break;
        case 0xDF:
            cpu_push(&cpu, cpu.pc);
            cpu.pc = 0x0018U;
            break;  // RST 18H

        case 0xE0:
            bus->write(static_cast<uint16_t>(0xFF00U + fetch8(cpu, *bus)), cpu.a);
            break;  // LDH (a8), A
        case 0xE1:
            cpu_set_hl(&cpu, cpu_pop(&cpu));
            break;
        case 0xE2:
            bus->write(static_cast<uint16_t>(0xFF00U + cpu.c), cpu.a);
            break;  // LD (C), A
        case 0xE5:
            cpu_push(&cpu, cpu_hl(&cpu));
            break;
        case 0xE6:
            alu_and(cpu, fetch8(cpu, *bus));
            break;
        case 0xE7:
            cpu_push(&cpu, cpu.pc);
            cpu.pc = 0x0020U;
            break;    // RST 20H
        case 0xE8: {  // ADD SP, r8
            uint8_t imm = fetch8(cpu, *bus);
            int8_t offset = static_cast<int8_t>(imm);
            cpu.f = 0;
            if (((cpu.sp & 0x0FU) + (imm & 0x0FU)) > 0x0FU)
                cpu.f |= GB_FLAG_H;
            if (((cpu.sp & 0xFFU) + (imm & 0xFFU)) > 0xFFU)
                cpu.f |= GB_FLAG_C;
            cpu.sp = static_cast<uint16_t>(cpu.sp + offset);
            break;
        }
        case 0xE9:
            cpu.pc = cpu_hl(&cpu);
            break;  // JP HL
        case 0xEA:
            bus->write(fetch16(cpu, *bus), cpu.a);
            break;
        case 0xEE:
            alu_xor(cpu, fetch8(cpu, *bus));
            break;
        case 0xEF:
            cpu_push(&cpu, cpu.pc);
            cpu.pc = 0x0028U;
            break;  // RST 28H

        case 0xF0:
            cpu.a = bus->read(static_cast<uint16_t>(0xFF00U + fetch8(cpu, *bus)));
            break;  // LDH A, (a8)
        case 0xF1:
            cpu_set_af(&cpu, cpu_pop(&cpu));
            break;
        case 0xF2:
            cpu.a = bus->read(static_cast<uint16_t>(0xFF00U + cpu.c));
            break;  // LD A, (C)
        case 0xF3:
            cpu.ime = 0;
            cpu.ime_delay = 0;
            break;  // DI
        case 0xF5:
            cpu_push(&cpu, cpu_af(&cpu));
            break;
        case 0xF6:
            alu_or(cpu, fetch8(cpu, *bus));
            break;
        case 0xF7:
            cpu_push(&cpu, cpu.pc);
            cpu.pc = 0x0030U;
            break;    // RST 30H
        case 0xF8: {  // LD HL, SP+r8
            uint8_t imm = fetch8(cpu, *bus);
            int8_t offset = static_cast<int8_t>(imm);
            cpu.f = 0;
            if (((cpu.sp & 0x0FU) + (imm & 0x0FU)) > 0x0FU)
                cpu.f |= GB_FLAG_H;
            if (((cpu.sp & 0xFFU) + (imm & 0xFFU)) > 0xFFU)
                cpu.f |= GB_FLAG_C;
            cpu_set_hl(&cpu, static_cast<uint16_t>(cpu.sp + offset));
            break;
        }
        case 0xF9:
            cpu.sp = cpu_hl(&cpu);
            break;
        case 0xFA:
            cpu.a = bus->read(fetch16(cpu, *bus));
            break;
        case 0xFB:
            cpu.ime_delay = 1;
            break;  // EI (activates IME after next instruction completes)
        case 0xFE:
            alu_cp(cpu, fetch8(cpu, *bus));
            break;
        case 0xFF:
            cpu_push(&cpu, cpu.pc);
            cpu.pc = 0x0038U;
            break;  // RST 38H

        default:
            // Invalid opcode: halt CPU
            cpu.halted = 1;
            break;
    }

    cpu.cycles += cycles;
    bus->step_timers(cycles);
    bus->sync_ppu(cpu.cycles);
    bus->sync_apu(cpu.cycles);
    return cycles;
}

}  // namespace dragon::gb
