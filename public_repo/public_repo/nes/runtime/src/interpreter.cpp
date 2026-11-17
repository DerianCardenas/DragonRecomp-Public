#include "nes/interpreter.h"

#include <cstring>

#include "nes/opcodes.h"

namespace dragon::nes {

void cpu_init(CpuState& cpu, Bus* bus) noexcept {
    std::memset(&cpu, 0, sizeof(CpuState));
    cpu.s = 0xFDU;
    cpu.i = 1U;
    cpu.z = 1U; /* Z=0 initially (P=$24 -> I=1, bit5=1, Z=0) */
    cpu.cycles = 7ULL;
    cpu.next_event = 29780ULL;
    cpu.bus = bus;
    if (bus) {
        cpu.ram = bus->ram_data();
        cpu.prg_rom = bus->prg_rom_data();
        cpu.prg_rom_size = bus->prg_rom_size();
        bus->attach_cpu(&cpu);
    }
}

void cpu_reset(CpuState& cpu) noexcept {
    cpu.s = 0xFDU;
    cpu.i = 1U;
    cpu.halted = 0U;
    cpu.cycles = 7ULL;
    if (auto* bus = static_cast<Bus*>(cpu.bus)) {
        cpu.ram = bus->ram_data();
        cpu.prg_rom = bus->prg_rom_data();
        cpu.prg_rom_size = bus->prg_rom_size();
        bus->attach_cpu(&cpu);
    }
    cpu.pc = bus_read16(&cpu, 0xFFFCU);
}

void cpu_nmi(CpuState& cpu) noexcept {
    cpu_push16(&cpu, cpu.pc);
    cpu_push8(&cpu, cpu_pack_flags(&cpu, 0U));
    cpu.i = 1U;
    cpu.pc = bus_read16(&cpu, 0xFFFAU);
    cpu.cycles += 7ULL;
    if (auto* bus = static_cast<Bus*>(cpu.bus)) {
        bus->sync_ppu(cpu.cycles);
        bus->update_next_event(&cpu);
    }
}

bool cpu_irq(CpuState& cpu) noexcept {
    if (cpu.i != 0U) {
        return false;
    }
    cpu_push16(&cpu, cpu.pc);
    cpu_push8(&cpu, cpu_pack_flags(&cpu, 0U));
    cpu.i = 1U;
    cpu.pc = bus_read16(&cpu, 0xFFFEU);
    cpu.cycles += 7ULL;
    if (auto* bus = static_cast<Bus*>(cpu.bus)) {
        bus->sync_ppu(cpu.cycles);
        bus->update_next_event(&cpu);
    }
    return true;
}

uint32_t cpu_step(CpuState& cpu) noexcept {
    if (cpu.halted) {
        return 0U;
    }

    const uint64_t start_cycles = cpu.cycles;
    const uint16_t instr_pc = cpu.pc;
    const uint8_t op = bus_read(&cpu, instr_pc);
    const OpcodeInfo& info = get_opcode_info(op);

    uint16_t addr = 0U;
    bool page_crossed = false;

    switch (info.mode) {
        case AddrMode::Implied:
        case AddrMode::Accumulator:
            break;
        case AddrMode::Immediate:
            addr = static_cast<uint16_t>(instr_pc + 1U);
            break;
        case AddrMode::ZeroPage:
            addr = bus_read(&cpu, static_cast<uint16_t>(instr_pc + 1U));
            break;
        case AddrMode::ZeroPageX:
            addr =
                static_cast<uint8_t>(bus_read(&cpu, static_cast<uint16_t>(instr_pc + 1U)) + cpu.x);
            break;
        case AddrMode::ZeroPageY:
            addr =
                static_cast<uint8_t>(bus_read(&cpu, static_cast<uint16_t>(instr_pc + 1U)) + cpu.y);
            break;
        case AddrMode::Relative: {
            const auto offset =
                static_cast<int8_t>(bus_read(&cpu, static_cast<uint16_t>(instr_pc + 1U)));
            addr = static_cast<uint16_t>(static_cast<int32_t>(instr_pc + 2U) + offset);
            break;
        }
        case AddrMode::Absolute:
            addr = bus_read16(&cpu, static_cast<uint16_t>(instr_pc + 1U));
            break;
        case AddrMode::AbsoluteX: {
            const uint16_t base = bus_read16(&cpu, static_cast<uint16_t>(instr_pc + 1U));
            addr = static_cast<uint16_t>(base + cpu.x);
            page_crossed = ((base & 0xFF00U) != (addr & 0xFF00U));
            break;
        }
        case AddrMode::AbsoluteY: {
            const uint16_t base = bus_read16(&cpu, static_cast<uint16_t>(instr_pc + 1U));
            addr = static_cast<uint16_t>(base + cpu.y);
            page_crossed = ((base & 0xFF00U) != (addr & 0xFF00U));
            break;
        }
        case AddrMode::Indirect: {
            const uint16_t ptr = bus_read16(&cpu, static_cast<uint16_t>(instr_pc + 1U));
            addr = bus_read16_wrap(&cpu, ptr);
            break;
        }
        case AddrMode::IndexedIndirectX: {
            const uint8_t zp =
                static_cast<uint8_t>(bus_read(&cpu, static_cast<uint16_t>(instr_pc + 1U)) + cpu.x);
            addr = bus_read16_zp(&cpu, zp);
            break;
        }
        case AddrMode::IndirectIndexedY: {
            const uint8_t zp = bus_read(&cpu, static_cast<uint16_t>(instr_pc + 1U));
            const uint16_t base = bus_read16_zp(&cpu, zp);
            addr = static_cast<uint16_t>(base + cpu.y);
            page_crossed = ((base & 0xFF00U) != (addr & 0xFF00U));
            break;
        }
    }

    cpu.pc = static_cast<uint16_t>(instr_pc + info.length);
    uint32_t cycles = info.base_cycles;
    if (info.page_cross_penalty && page_crossed) {
        cycles += 1U;
    }
    cpu.cycles += cycles;

    auto take_branch = [&](bool cond) {
        if (cond) {
            uint32_t extra = 1U;
            if ((cpu.pc & 0xFF00U) != (addr & 0xFF00U)) {
                extra += 1U;
            }
            cpu.cycles += extra;
            cpu.pc = addr;
        }
    };

    auto rmw_write = [&](uint16_t target_addr, uint8_t old_val, uint8_t new_val) {
        if (target_addr >= 0x2000U && target_addr < 0x4020U) {
            bus_write(&cpu, target_addr, old_val);
        }
        bus_write(&cpu, target_addr, new_val);
    };

    switch (info.mnemonic) {
        case Mnemonic::LDA:
            cpu.a = bus_read(&cpu, addr);
            cpu_set_nz(&cpu, cpu.a);
            break;
        case Mnemonic::LDX:
            cpu.x = bus_read(&cpu, addr);
            cpu_set_nz(&cpu, cpu.x);
            break;
        case Mnemonic::LDY:
            cpu.y = bus_read(&cpu, addr);
            cpu_set_nz(&cpu, cpu.y);
            break;
        case Mnemonic::STA:
            bus_write(&cpu, addr, cpu.a);
            break;
        case Mnemonic::STX:
            bus_write(&cpu, addr, cpu.x);
            break;
        case Mnemonic::STY:
            bus_write(&cpu, addr, cpu.y);
            break;
        case Mnemonic::TAX:
            cpu.x = cpu.a;
            cpu_set_nz(&cpu, cpu.x);
            break;
        case Mnemonic::TAY:
            cpu.y = cpu.a;
            cpu_set_nz(&cpu, cpu.y);
            break;
        case Mnemonic::TXA:
            cpu.a = cpu.x;
            cpu_set_nz(&cpu, cpu.a);
            break;
        case Mnemonic::TYA:
            cpu.a = cpu.y;
            cpu_set_nz(&cpu, cpu.a);
            break;
        case Mnemonic::TSX:
            cpu.x = cpu.s;
            cpu_set_nz(&cpu, cpu.x);
            break;
        case Mnemonic::TXS:
            cpu.s = cpu.x;
            break;
        case Mnemonic::PHA:
            cpu_push8(&cpu, cpu.a);
            break;
        case Mnemonic::PHP:
            cpu_push8(&cpu, cpu_pack_flags(&cpu, 1U));
            break;
        case Mnemonic::PLA:
            cpu.a = cpu_pop8(&cpu);
            cpu_set_nz(&cpu, cpu.a);
            break;
        case Mnemonic::PLP:
            cpu_unpack_flags(&cpu, cpu_pop8(&cpu));
            break;
        case Mnemonic::AND:
            cpu.a &= bus_read(&cpu, addr);
            cpu_set_nz(&cpu, cpu.a);
            break;
        case Mnemonic::EOR:
            cpu.a ^= bus_read(&cpu, addr);
            cpu_set_nz(&cpu, cpu.a);
            break;
        case Mnemonic::ORA:
            cpu.a |= bus_read(&cpu, addr);
            cpu_set_nz(&cpu, cpu.a);
            break;
        case Mnemonic::BIT:
            cpu_bit(&cpu, bus_read(&cpu, addr));
            break;
        case Mnemonic::ADC:
            cpu_adc(&cpu, bus_read(&cpu, addr));
            break;
        case Mnemonic::SBC:
            cpu_sbc(&cpu, bus_read(&cpu, addr));
            break;
        case Mnemonic::CMP:
            cpu_cmp(&cpu, cpu.a, bus_read(&cpu, addr));
            break;
        case Mnemonic::CPX:
            cpu_cmp(&cpu, cpu.x, bus_read(&cpu, addr));
            break;
        case Mnemonic::CPY:
            cpu_cmp(&cpu, cpu.y, bus_read(&cpu, addr));
            break;
        case Mnemonic::INC: {
            const uint8_t old_val = bus_read(&cpu, addr);
            const uint8_t res = static_cast<uint8_t>(old_val + 1U);
            rmw_write(addr, old_val, res);
            cpu_set_nz(&cpu, res);
            break;
        }
        case Mnemonic::DEC: {
            const uint8_t old_val = bus_read(&cpu, addr);
            const uint8_t res = static_cast<uint8_t>(old_val - 1U);
            rmw_write(addr, old_val, res);
            cpu_set_nz(&cpu, res);
            break;
        }
        case Mnemonic::INX:
            cpu.x = static_cast<uint8_t>(cpu.x + 1U);
            cpu_set_nz(&cpu, cpu.x);
            break;
        case Mnemonic::DEX:
            cpu.x = static_cast<uint8_t>(cpu.x - 1U);
            cpu_set_nz(&cpu, cpu.x);
            break;
        case Mnemonic::INY:
            cpu.y = static_cast<uint8_t>(cpu.y + 1U);
            cpu_set_nz(&cpu, cpu.y);
            break;
        case Mnemonic::DEY:
            cpu.y = static_cast<uint8_t>(cpu.y - 1U);
            cpu_set_nz(&cpu, cpu.y);
            break;
        case Mnemonic::ASL:
            if (info.mode == AddrMode::Accumulator) {
                cpu.a = cpu_asl(&cpu, cpu.a);
            } else {
                const uint8_t old_val = bus_read(&cpu, addr);
                const uint8_t res = cpu_asl(&cpu, old_val);
                rmw_write(addr, old_val, res);
            }
            break;
        case Mnemonic::LSR:
            if (info.mode == AddrMode::Accumulator) {
                cpu.a = cpu_lsr(&cpu, cpu.a);
            } else {
                const uint8_t old_val = bus_read(&cpu, addr);
                const uint8_t res = cpu_lsr(&cpu, old_val);
                rmw_write(addr, old_val, res);
            }
            break;
        case Mnemonic::ROL:
            if (info.mode == AddrMode::Accumulator) {
                cpu.a = cpu_rol(&cpu, cpu.a);
            } else {
                const uint8_t old_val = bus_read(&cpu, addr);
                const uint8_t res = cpu_rol(&cpu, old_val);
                rmw_write(addr, old_val, res);
            }
            break;
        case Mnemonic::ROR:
            if (info.mode == AddrMode::Accumulator) {
                cpu.a = cpu_ror(&cpu, cpu.a);
            } else {
                const uint8_t old_val = bus_read(&cpu, addr);
                const uint8_t res = cpu_ror(&cpu, old_val);
                rmw_write(addr, old_val, res);
            }
            break;
        case Mnemonic::JMP:
            cpu.pc = addr;
            break;
        case Mnemonic::JSR:
            cpu_push16(&cpu, static_cast<uint16_t>(cpu.pc - 1U));
            cpu.pc = addr;
            break;
        case Mnemonic::RTS:
            cpu.pc = static_cast<uint16_t>(cpu_pop16(&cpu) + 1U);
            break;
        case Mnemonic::RTI:
            cpu_unpack_flags(&cpu, cpu_pop8(&cpu));
            cpu.pc = cpu_pop16(&cpu);
            break;
        case Mnemonic::BRK:
            cpu_push16(&cpu, static_cast<uint16_t>(cpu.pc + 1U));
            cpu_push8(&cpu, cpu_pack_flags(&cpu, 1U));
            cpu.i = 1U;
            cpu.pc = bus_read16(&cpu, 0xFFFEU);
            break;
        case Mnemonic::BCC:
            take_branch(cpu.c == 0U);
            break;
        case Mnemonic::BCS:
            take_branch(cpu.c != 0U);
            break;
        case Mnemonic::BEQ:
            take_branch(cpu.z == 0U);
            break;
        case Mnemonic::BNE:
            take_branch(cpu.z != 0U);
            break;
        case Mnemonic::BMI:
            take_branch((cpu.n & 0x80U) != 0U);
            break;
        case Mnemonic::BPL:
            take_branch((cpu.n & 0x80U) == 0U);
            break;
        case Mnemonic::BVC:
            take_branch(cpu.v == 0U);
            break;
        case Mnemonic::BVS:
            take_branch(cpu.v != 0U);
            break;
        case Mnemonic::CLC:
            cpu.c = 0U;
            break;
        case Mnemonic::CLD:
            cpu.d = 0U;
            break;
        case Mnemonic::CLI:
            cpu.i = 0U;
            break;
        case Mnemonic::CLV:
            cpu.v = 0U;
            break;
        case Mnemonic::SEC:
            cpu.c = 1U;
            break;
        case Mnemonic::SED:
            cpu.d = 1U;
            break;
        case Mnemonic::SEI:
            cpu.i = 1U;
            break;
        case Mnemonic::NOP:
            if (info.mode != AddrMode::Implied) {
                (void)bus_read(&cpu, addr);
            }
            break;

        // Stable unofficial opcodes
        case Mnemonic::LAX: {
            const uint8_t val = bus_read(&cpu, addr);
            cpu.a = val;
            cpu.x = val;
            cpu_set_nz(&cpu, val);
            break;
        }
        case Mnemonic::SAX:
            bus_write(&cpu, addr, static_cast<uint8_t>(cpu.a & cpu.x));
            break;
        case Mnemonic::DCP: {
            const uint8_t old_val = bus_read(&cpu, addr);
            const uint8_t dec_val = static_cast<uint8_t>(old_val - 1U);
            rmw_write(addr, old_val, dec_val);
            cpu_cmp(&cpu, cpu.a, dec_val);
            break;
        }
        case Mnemonic::ISC: {
            const uint8_t old_val = bus_read(&cpu, addr);
            const uint8_t inc_val = static_cast<uint8_t>(old_val + 1U);
            rmw_write(addr, old_val, inc_val);
            cpu_sbc(&cpu, inc_val);
            break;
        }
        case Mnemonic::SLO: {
            const uint8_t old_val = bus_read(&cpu, addr);
            const uint8_t res = cpu_asl(&cpu, old_val);
            rmw_write(addr, old_val, res);
            cpu.a |= res;
            cpu_set_nz(&cpu, cpu.a);
            break;
        }
        case Mnemonic::RLA: {
            const uint8_t old_val = bus_read(&cpu, addr);
            const uint8_t res = cpu_rol(&cpu, old_val);
            rmw_write(addr, old_val, res);
            cpu.a &= res;
            cpu_set_nz(&cpu, cpu.a);
            break;
        }
        case Mnemonic::SRE: {
            const uint8_t old_val = bus_read(&cpu, addr);
            const uint8_t res = cpu_lsr(&cpu, old_val);
            rmw_write(addr, old_val, res);
            cpu.a ^= res;
            cpu_set_nz(&cpu, cpu.a);
            break;
        }
        case Mnemonic::RRA: {
            const uint8_t old_val = bus_read(&cpu, addr);
            const uint8_t res = cpu_ror(&cpu, old_val);
            rmw_write(addr, old_val, res);
            cpu_adc(&cpu, res);
            break;
        }
        case Mnemonic::ANC:
            cpu.a &= bus_read(&cpu, addr);
            cpu_set_nz(&cpu, cpu.a);
            cpu.c = static_cast<uint8_t>((cpu.a >> 7U) & 1U);
            break;
        case Mnemonic::ALR:
            cpu.a &= bus_read(&cpu, addr);
            cpu.a = cpu_lsr(&cpu, cpu.a);
            break;
        case Mnemonic::ARR: {
            cpu.a &= bus_read(&cpu, addr);
            const uint8_t old_c = static_cast<uint8_t>(cpu.c ? 0x80U : 0U);
            cpu.a = static_cast<uint8_t>((cpu.a >> 1U) | old_c);
            cpu_set_nz(&cpu, cpu.a);
            cpu.c = static_cast<uint8_t>((cpu.a >> 6U) & 1U);
            cpu.v = static_cast<uint8_t>(((cpu.a >> 6U) ^ (cpu.a >> 5U)) & 1U);
            break;
        }
        case Mnemonic::AXS: {
            const uint8_t imm = bus_read(&cpu, addr);
            const uint8_t ax = static_cast<uint8_t>(cpu.a & cpu.x);
            cpu.c = static_cast<uint8_t>(ax >= imm);
            cpu.x = static_cast<uint8_t>(ax - imm);
            cpu_set_nz(&cpu, cpu.x);
            break;
        }

        // Unstable / halt opcodes
        case Mnemonic::KIL:
        default:
            cpu.halted = 1U;
            break;
    }

    return static_cast<uint32_t>(cpu.cycles - start_cycles);
}

uint32_t cpu_step_with_ppu(CpuState& cpu) noexcept {
    const uint32_t cyc = cpu_step(cpu);
    auto* bus = static_cast<Bus*>(cpu.bus);
    if (!bus || bus->is_flat_ram()) {
        return cyc;
    }
    bus->sync_ppu(cpu.cycles);
    if (bus->ppu().consume_frame_complete()) {
        bus->on_frame_complete(&cpu);
        if (cpu.halted) {
            return cyc;
        }
    }
    if (bus->ppu().consume_nmi()) {
        cpu_nmi(cpu);
        if (bus->ppu().consume_frame_complete()) {
            bus->on_frame_complete(&cpu);
        }
    } else if (cpu.i == 0U && bus->irq_pending()) {
        cpu_irq(cpu);
        if (bus->ppu().consume_frame_complete()) {
            bus->on_frame_complete(&cpu);
        }
    }
    bus->update_next_event(&cpu);
    return cyc;
}

void cpu_run_until(CpuState& cpu, uint64_t max_cycles) noexcept {
    while (!cpu.halted && cpu.cycles < max_cycles) {
        cpu_step(cpu);
    }
}

void cpu_run_frames_interpreter(CpuState& cpu, uint64_t num_frames) noexcept {
    auto* bus = static_cast<Bus*>(cpu.bus);
    if (!bus) {
        return;
    }
    const uint64_t target = bus->completed_frames() + num_frames;
    while (!cpu.halted && bus->completed_frames() < target) {
        cpu_step_with_ppu(cpu);
    }
}

}  // namespace dragon::nes
