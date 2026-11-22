#ifndef DRAGON_PS1_CPU_STATE_H
#define DRAGON_PS1_CPU_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ps1CpuState {
    uint32_t gpr[32];
    uint32_t pc;
    uint32_t npc;
    uint32_t hi;
    uint32_t lo;

    /* COP0 System Control registers (12=SR, 13=CAUSE, 14=EPC, 15=PRID) */
    uint32_t cop0[32];

    /* COP2 (GTE) Data (0..31) & Control (0..31) registers */
    uint32_t gte_data[32];
    uint32_t gte_ctrl[32];

    uint64_t cycles;
    uint8_t halted;
    uint8_t vblank_yield;
    uint8_t in_delay_slot;
    uint8_t reserved_pad;

    /* Opaque pointer to dragon::ps1::Ps1Runtime */
    void* runtime;
} Ps1CpuState;

typedef void (*Ps1RecompFn)(Ps1CpuState* cpu);

typedef struct Ps1RecompFuncEntry {
    uint32_t addr;
    Ps1RecompFn fn;
} Ps1RecompFuncEntry;

/* Memory & unaligned helpers callable from C99 recompiled code */
uint8_t ps1_read8(Ps1CpuState* cpu, uint32_t addr);
uint16_t ps1_read16(Ps1CpuState* cpu, uint32_t addr);
uint32_t ps1_read32(Ps1CpuState* cpu, uint32_t addr);
void ps1_write8(Ps1CpuState* cpu, uint32_t addr, uint8_t val);
void ps1_write16(Ps1CpuState* cpu, uint32_t addr, uint16_t val);
void ps1_write32(Ps1CpuState* cpu, uint32_t addr, uint32_t val);

uint32_t ps1_lwl(Ps1CpuState* cpu, uint32_t addr, uint32_t rt_val);
uint32_t ps1_lwr(Ps1CpuState* cpu, uint32_t addr, uint32_t rt_val);
void ps1_swl(Ps1CpuState* cpu, uint32_t addr, uint32_t rt_val);
void ps1_swr(Ps1CpuState* cpu, uint32_t addr, uint32_t rt_val);

/* COP0 / GTE (COP2) / BIOS HLE / Dynamic Dispatch helpers */
uint32_t ps1_cop0_read(Ps1CpuState* cpu, uint8_t reg);
void ps1_cop0_write(Ps1CpuState* cpu, uint8_t reg, uint32_t val);
void ps1_rfe(Ps1CpuState* cpu);

uint32_t ps1_gte_read_data(Ps1CpuState* cpu, uint8_t reg);
void ps1_gte_write_data(Ps1CpuState* cpu, uint8_t reg, uint32_t val);
uint32_t ps1_gte_read_ctrl(Ps1CpuState* cpu, uint8_t reg);
void ps1_gte_write_ctrl(Ps1CpuState* cpu, uint8_t reg, uint32_t val);
void ps1_gte_cmd(Ps1CpuState* cpu, uint32_t raw_insn);

void ps1_syscall(Ps1CpuState* cpu, uint32_t code);
void ps1_bios_call(Ps1CpuState* cpu, uint32_t vector_addr);
void ps1_runtime_dispatch(Ps1CpuState* cpu, uint32_t target_pc);

#ifdef __cplusplus
}
#endif

#endif /* DRAGON_PS1_CPU_STATE_H */
