#ifndef DRAGON_PSP_CPU_STATE_H
#define DRAGON_PSP_CPU_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PspCpuState {
    uint32_t gpr[32];
    uint32_t pc;
    uint32_t npc;
    uint32_t hi;
    uint32_t lo;

    /* Single-precision FPU (COP1) */
    float f[32];
    uint32_t fcr31; /* bit 23 = FCC condition flag */

    /* Allegrex VFPU: 8 matrices x 4x4 = 128 single-precision registers */
    float vfpu[128];
    uint32_t vfpu_cc[6];
    uint32_t vfpu_pfx_s;
    uint32_t vfpu_pfx_t;
    uint32_t vfpu_pfx_d;
    uint8_t vfpu_pfx_s_enabled;
    uint8_t vfpu_pfx_t_enabled;
    uint8_t vfpu_pfx_d_enabled;
    uint8_t in_delay_slot;

    uint64_t cycles;
    uint32_t branch_target;
    uint8_t branch_pending;
    uint8_t halted;
    uint8_t vblank_yield;
    uint8_t reserved_pad;

    /* Opaque pointer to dragon::psp::PspRuntime (accessed by C99 helpers) */
    void* runtime;
} PspCpuState;

typedef void (*PspRecompFn)(PspCpuState* cpu);

typedef struct PspRecompFuncEntry {
    uint32_t addr;
    PspRecompFn fn;
} PspRecompFuncEntry;

/* Bus & memory helpers callable from C99 recompiled code */
uint8_t psp_read8(PspCpuState* cpu, uint32_t addr);
uint16_t psp_read16(PspCpuState* cpu, uint32_t addr);
uint32_t psp_read32(PspCpuState* cpu, uint32_t addr);
void psp_write8(PspCpuState* cpu, uint32_t addr, uint8_t val);
void psp_write16(PspCpuState* cpu, uint32_t addr, uint16_t val);
void psp_write32(PspCpuState* cpu, uint32_t addr, uint32_t val);

uint32_t psp_lwl(PspCpuState* cpu, uint32_t addr, uint32_t rt_val);
uint32_t psp_lwr(PspCpuState* cpu, uint32_t addr, uint32_t rt_val);
void psp_swl(PspCpuState* cpu, uint32_t addr, uint32_t rt_val);
void psp_swr(PspCpuState* cpu, uint32_t addr, uint32_t rt_val);

/* HLE syscall dispatch */
void psp_hle_syscall(PspCpuState* cpu, uint32_t code);

/* Execute a single VFPU or fallback instruction at pc */
void psp_exec_fallback_insn(PspCpuState* cpu, uint32_t raw, uint32_t pc);

/* Dynamic indirect call/jump dispatch (recompiled table or interpreter fallback) */
void psp_runtime_dispatch(PspCpuState* cpu, uint32_t target_pc);

#ifdef __cplusplus
}
#endif

#endif /* DRAGON_PSP_CPU_STATE_H */
