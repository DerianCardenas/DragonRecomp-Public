#include "nds/runtime.h"

#include <cstdio>

#include "nds/interpreter.h"

extern "C" {

uint8_t nds_bus_read8(NdsCpuState* cpu, uint32_t addr) {
    auto* bus = static_cast<dragon::nds::Bus*>(cpu->bus);
    return cpu->is_arm9 ? bus->arm9_read8(addr, cpu) : bus->arm7_read8(addr);
}

uint16_t nds_bus_read16(NdsCpuState* cpu, uint32_t addr) {
    auto* bus = static_cast<dragon::nds::Bus*>(cpu->bus);
    return cpu->is_arm9 ? bus->arm9_read16(addr, cpu) : bus->arm7_read16(addr);
}

uint32_t nds_bus_read32(NdsCpuState* cpu, uint32_t addr) {
    auto* bus = static_cast<dragon::nds::Bus*>(cpu->bus);
    return cpu->is_arm9 ? bus->arm9_read32(addr, cpu) : bus->arm7_read32(addr);
}

void nds_bus_write8(NdsCpuState* cpu, uint32_t addr, uint8_t value) {
    auto* bus = static_cast<dragon::nds::Bus*>(cpu->bus);
    if (cpu->is_arm9)
        bus->arm9_write8(addr, value, cpu);
    else
        bus->arm7_write8(addr, value);
}

void nds_bus_write16(NdsCpuState* cpu, uint32_t addr, uint16_t value) {
    auto* bus = static_cast<dragon::nds::Bus*>(cpu->bus);
    if (cpu->is_arm9)
        bus->arm9_write16(addr, value, cpu);
    else
        bus->arm7_write16(addr, value);
}

void nds_bus_write32(NdsCpuState* cpu, uint32_t addr, uint32_t value) {
    auto* bus = static_cast<dragon::nds::Bus*>(cpu->bus);
    if (cpu->is_arm9)
        bus->arm9_write32(addr, value, cpu);
    else
        bus->arm7_write32(addr, value);
}

uint32_t nds_cp15_read(NdsCpuState* cpu, uint32_t cn, uint32_t cm, uint32_t cp) {
    if (cn == 0U && cm == 0U && cp == 0U)
        return 0x41059461U;  // ARM946E-S Main ID
    if (cn == 0U && cm == 0U && cp == 1U)
        return 0x0F0D2112U;  // Cache Type
    if (cn == 0U && cm == 0U && cp == 2U)
        return 0x00140180U;  // TCM Size (32 KB ITCM, 16 KB DTCM)
    if (cn == 1U && cm == 0U && cp == 0U)
        return cpu->cp15_ctrl;
    if (cn == 9U && cm == 1U && cp == 0U)
        return cpu->cp15_dtcm_reg;
    if (cn == 9U && cm == 1U && cp == 1U)
        return cpu->cp15_itcm_reg;
    return 0U;
}

void nds_cp15_write(NdsCpuState* cpu, uint32_t cn, uint32_t cm, uint32_t cp, uint32_t value) {
    if (cn == 1U && cm == 0U && cp == 0U) {
        cpu->cp15_ctrl = (cpu->cp15_ctrl & ~0x000FF085U) | (value & 0x000FF085U);
    } else if (cn == 7U && cm == 0U && cp == 4U) {
        // Wait For Interrupt
        cpu->halted = 1U;
    } else if (cn == 9U && cm == 1U && cp == 0U) {
        cpu->cp15_dtcm_reg = value;
        cpu->dtcm_base = value & 0xFFFFF000U;
        const uint32_t shift = (value >> 1U) & 0x1FU;
        cpu->dtcm_size = (shift >= 3U) ? (512U << shift) : 0x4000U;
    } else if (cn == 9U && cm == 1U && cp == 1U) {
        cpu->cp15_itcm_reg = value;
        const uint32_t shift = (value >> 1U) & 0x1FU;
        cpu->itcm_size = (shift >= 3U) ? (512U << shift) : 0x8000U;
    }
}

void nds_swi(NdsCpuState* cpu, uint8_t swi_num) {
    auto* bus = static_cast<dragon::nds::Bus*>(cpu->bus);
    dragon::nds::cpu_handle_swi(*cpu, *bus, swi_num);
}

void nds_dispatch_irq(NdsCpuState* cpu) {
    auto* bus = static_cast<dragon::nds::Bus*>(cpu->bus);
    dragon::nds::cpu_dispatch_irq(*cpu, *bus);
}

void nds_runtime_poll(NdsCpuState* cpu) {
    auto* bus = static_cast<dragon::nds::Bus*>(cpu->bus);
    if (!bus || !cpu->is_arm9)
        return;
    cpu->next_event = cpu->cycles + dragon::nds::Bus::kArm9CyclesPerLine;
    bus->sync_arm9_cycles(cpu->cycles, *cpu);
    if (bus->arm9_irq_pending() && !cpu->i_disable) {
        const uint32_t saved_pc = cpu->r[15];
        const uint8_t saved_thumb = cpu->thumb;
        dragon::nds::cpu_dispatch_irq(*cpu, *bus);
        // Run IRQ handler to completion (until trampoline 0xFFFF0020 restores interrupted PC)
        if (cpu->r[15] != saved_pc || cpu->i_disable != 0U) {
            for (int steps = 0; steps < 262144 && cpu->halted != 2U; ++steps) {
                dragon::nds::cpu_step(*cpu, *bus);
                if (cpu->r[15] == saved_pc && cpu->thumb == saved_thumb &&
                    (cpu->cpsr & NDS_CPSR_MODE_MASK) != NDS_MODE_IRQ) {
                    break;
                }
            }
        }
    }
}

uint32_t nds_runtime_step(NdsCpuState* cpu) {
    auto* bus = static_cast<dragon::nds::Bus*>(cpu->bus);
    return dragon::nds::cpu_step(*cpu, *bus);
}

void nds_runtime_dispatch(NdsCpuState* cpu, uint32_t target, uint32_t from_pc) {
    (void)from_pc;
    auto* bus = static_cast<dragon::nds::Bus*>(cpu->bus);
    cpu->thumb = static_cast<uint8_t>(target & 1U);
    cpu->r[15] = cpu->thumb ? (target & ~1U) : (target & ~3U);

    if (bus->recomp_mode()) {
        if (NdsRecompFunc fn = bus->lookup_recomp_function(cpu->r[15], cpu->thumb != 0U)) {
            fn(cpu);
            return;
        }
    }
    // Target is in dynamically modified RAM (BLZ decompressed / overlay / ITCM):
    // leave cpu->r[15] at target and return so run_frame() executes it cleanly.
}

}  // extern "C"

namespace dragon::nds {

void run_frame(NdsCpuState& arm9, NdsCpuState& arm7, Bus& bus) noexcept {
    const uint64_t start_frame = bus.frame_count();
    while (bus.frame_count() == start_frame && arm9.halted != 2U) {
        if (arm9.halted == 1U) {
            arm9.cycles += Bus::kArm9CyclesPerLine;
            bus.sync_arm9_cycles(arm9.cycles, arm9);
            if (bus.arm9_irq_pending() && !arm9.i_disable) {
                cpu_dispatch_irq(arm9, bus);
            }
            continue;
        }

        if (bus.recomp_mode()) {
            if (NdsRecompFunc fn = bus.lookup_recomp_function(arm9.r[15], arm9.thumb != 0U)) {
                fn(&arm9);
                bus.sync_arm9_cycles(arm9.cycles, arm9);
                continue;
            }
        }

        if (bus.arm9_irq_pending() && !arm9.i_disable) {
            cpu_dispatch_irq(arm9, bus);
        }

        // Run a burst of ARM9 instructions then sync ARM7 + scanline
        constexpr int kBurst = 64;
        for (int i = 0; i < kBurst && arm9.halted == 0U; ++i) {
            cpu_step(arm9, bus);
        }
        bus.sync_arm9_cycles(arm9.cycles, arm9);
    }
    (void)arm7;
}

}  // namespace dragon::nds
