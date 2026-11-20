#pragma once

/*
 * C99 / C++20 compatible PC Engine / TurboGrafx-16 HuC6280 CPU state and runtime ABI.
 * Used by both the PCE runtime and pcerecomp-generated C99 code.
 *
 * Reference: docs/PCE_SPEC.md §2, Charles MacDonald pcetech.txt.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__cplusplus) && !defined(inline)
#define inline __inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* HuC6280 Processor Status (P) bit masks: N V T B D I Z C */
#define PCE_FLAG_N 0x80U
#define PCE_FLAG_V 0x40U
#define PCE_FLAG_T 0x20U
#define PCE_FLAG_B 0x10U
#define PCE_FLAG_D 0x08U
#define PCE_FLAG_I 0x04U
#define PCE_FLAG_Z 0x02U
#define PCE_FLAG_C 0x01U

/* Interrupt vector addresses */
#define PCE_VEC_IRQ2  0xFFF6U
#define PCE_VEC_IRQ1  0xFFF8U
#define PCE_VEC_TIQ   0xFFFAU
#define PCE_VEC_NMI   0xFFFCU
#define PCE_VEC_RESET 0xFFFEU

typedef struct PceCpuState {
    uint8_t a, x, y, s, p;
    uint16_t pc;

    uint8_t mpr[8];       /* MMU Page Registers: logical [0x0000..0xFFFF] -> physical bank */
    uint8_t last_mpr;     /* Last value read/written by TMA/TAM (for TMA #$00) */
    uint8_t speed_high;   /* 1 = 7.15909 MHz (CSH), 0 = 1.78977 MHz (CSL) */
    uint8_t nmi_pending;  /* Edge-triggered NMI latch */

    uint64_t cycles;      /* Master CPU cycle counter (high-speed 7.16 MHz units) */
    uint64_t next_event;  /* Next hardware sync cycle threshold */

    void* bus;            /* Opaque pointer to dragon::pce::Bus */
    uint8_t* wram;        /* Direct pointer to 8 KiB Work RAM (physical bank $F8) */
    const uint8_t* rom;   /* Direct pointer to loaded HuCard ROM */
    uint32_t rom_size;    /* Loaded ROM size in bytes */
} PceCpuState;

typedef void (*PceRecompFunc)(PceCpuState* cpu);

typedef struct PceRecompFuncEntry {
    uint16_t addr;
    uint8_t bank;
    PceRecompFunc func;
} PceRecompFuncEntry;

uint8_t pce_bus_read(PceCpuState* cpu, uint16_t addr);
void pce_bus_write(PceCpuState* cpu, uint16_t addr, uint8_t value);
void pce_st0(PceCpuState* cpu, uint8_t value);
void pce_st1(PceCpuState* cpu, uint8_t value);
void pce_st2(PceCpuState* cpu, uint8_t value);
void pce_tam(PceCpuState* cpu, uint8_t mask);
void pce_tma(PceCpuState* cpu, uint8_t mask);
int pce_runtime_poll(PceCpuState* cpu);
int pce_runtime_dispatch(PceCpuState* cpu, uint16_t target, uint16_t from_pc);
int pce_runtime_step(PceCpuState* cpu);

/* Scale CPU cycles by current clock speed (high = 1x, low = 4x) */
static inline void pce_add_cycles(PceCpuState* cpu, uint32_t cyc) {
    cpu->cycles += cpu->speed_high ? (uint64_t)cyc : ((uint64_t)cyc << 2U);
}

/* HuC6280 Zero Page ($2000-$20FF) & Stack ($2100-$21FF) helpers */
static inline uint8_t pce_zp_read(PceCpuState* cpu, uint8_t zp) {
    return pce_bus_read(cpu, (uint16_t)(0x2000U | zp));
}

static inline void pce_zp_write(PceCpuState* cpu, uint8_t zp, uint8_t val) {
    pce_bus_write(cpu, (uint16_t)(0x2000U | zp), val);
}

static inline uint16_t pce_zp_read16(PceCpuState* cpu, uint8_t zp) {
    uint8_t lo = pce_zp_read(cpu, zp);
    uint8_t hi = pce_zp_read(cpu, (uint8_t)(zp + 1U));
    return (uint16_t)(((uint16_t)hi << 8U) | lo);
}

static inline uint16_t pce_bus_read16(PceCpuState* cpu, uint16_t addr) {
    uint8_t lo = pce_bus_read(cpu, addr);
    uint8_t hi = pce_bus_read(cpu, (uint16_t)(addr + 1U));
    return (uint16_t)(((uint16_t)hi << 8U) | lo);
}

static inline void pce_push8(PceCpuState* cpu, uint8_t val) {
    pce_bus_write(cpu, (uint16_t)(0x2100U | cpu->s), val);
    cpu->s = (uint8_t)(cpu->s - 1U);
}

static inline uint8_t pce_pop8(PceCpuState* cpu) {
    cpu->s = (uint8_t)(cpu->s + 1U);
    return pce_bus_read(cpu, (uint16_t)(0x2100U | cpu->s));
}

static inline void pce_push16(PceCpuState* cpu, uint16_t val) {
    pce_push8(cpu, (uint8_t)((val >> 8U) & 0xFFU));
    pce_push8(cpu, (uint8_t)(val & 0xFFU));
}

static inline uint16_t pce_pop16(PceCpuState* cpu) {
    uint8_t lo = pce_pop8(cpu);
    uint8_t hi = pce_pop8(cpu);
    return (uint16_t)(((uint16_t)hi << 8U) | lo);
}

static inline void pce_set_nz(PceCpuState* cpu, uint8_t v) {
    cpu->p = (uint8_t)((cpu->p & (uint8_t)~(PCE_FLAG_N | PCE_FLAG_Z)) |
                       (v & PCE_FLAG_N) | (v == 0U ? PCE_FLAG_Z : 0U));
}

/* Exact HuC6280 ADC (binary & BCD decimal mode) */
static inline uint8_t pce_adc_val(PceCpuState* cpu, uint8_t lhs, uint8_t rhs) {
    uint8_t cy = (cpu->p & PCE_FLAG_C) ? 1U : 0U;
    if (cpu->p & PCE_FLAG_D) {
        uint16_t lo = (uint16_t)((lhs & 0x0FU) + (rhs & 0x0FU) + cy);
        uint16_t hi = (uint16_t)((lhs >> 4U) + (rhs >> 4U));
        if (lo > 9U) {
            lo = (uint16_t)(lo + 6U);
            hi = (uint16_t)(hi + 1U);
        }
        uint8_t bin = (uint8_t)((lhs + rhs + cy) & 0xFFU);
        uint8_t vflag = (((~(lhs ^ rhs)) & (lhs ^ bin)) & 0x80U) ? PCE_FLAG_V : 0U;
        uint8_t cflag = 0U;
        if (hi > 9U) {
            hi = (uint16_t)(hi + 6U);
            cflag = PCE_FLAG_C;
        }
        uint8_t res = (uint8_t)(((hi & 0x0FU) << 4U) | (lo & 0x0FU));
        cpu->p = (uint8_t)((cpu->p & (uint8_t)~(PCE_FLAG_N | PCE_FLAG_V | PCE_FLAG_Z | PCE_FLAG_C)) |
                           (res & PCE_FLAG_N) | vflag | (res == 0U ? PCE_FLAG_Z : 0U) | cflag);
        return res;
    } else {
        uint16_t sum = (uint16_t)lhs + (uint16_t)rhs + (uint16_t)cy;
        uint8_t res = (uint8_t)(sum & 0xFFU);
        uint8_t vflag = (((~(lhs ^ rhs)) & (lhs ^ res)) & 0x80U) ? PCE_FLAG_V : 0U;
        uint8_t cflag = (sum > 0xFFU) ? PCE_FLAG_C : 0U;
        cpu->p = (uint8_t)((cpu->p & (uint8_t)~(PCE_FLAG_N | PCE_FLAG_V | PCE_FLAG_Z | PCE_FLAG_C)) |
                           (res & PCE_FLAG_N) | vflag | (res == 0U ? PCE_FLAG_Z : 0U) | cflag);
        return res;
    }
}

/* Exact HuC6280 SBC (binary & BCD decimal mode) */
static inline uint8_t pce_sbc_val(PceCpuState* cpu, uint8_t lhs, uint8_t rhs) {
    uint8_t borrow = (cpu->p & PCE_FLAG_C) ? 0U : 1U;
    uint16_t diff = (uint16_t)lhs - (uint16_t)rhs - (uint16_t)borrow;
    uint8_t bin = (uint8_t)(diff & 0xFFU);
    uint8_t vflag = (((lhs ^ rhs) & (lhs ^ bin)) & 0x80U) ? PCE_FLAG_V : 0U;
    uint8_t cflag = (diff < 0x100U) ? PCE_FLAG_C : 0U;

    if (cpu->p & PCE_FLAG_D) {
        int lo = (int)(lhs & 0x0FU) - (int)(rhs & 0x0FU) - (int)borrow;
        int hi = (int)(lhs >> 4U) - (int)(rhs >> 4U);
        if (lo < 0) {
            lo -= 6;
            hi -= 1;
        }
        if (hi < 0) {
            hi -= 6;
        }
        uint8_t res = (uint8_t)(((hi & 0x0F) << 4) | (lo & 0x0F));
        cpu->p = (uint8_t)((cpu->p & (uint8_t)~(PCE_FLAG_N | PCE_FLAG_V | PCE_FLAG_Z | PCE_FLAG_C)) |
                           (res & PCE_FLAG_N) | vflag | (res == 0U ? PCE_FLAG_Z : 0U) | cflag);
        return res;
    } else {
        cpu->p = (uint8_t)((cpu->p & (uint8_t)~(PCE_FLAG_N | PCE_FLAG_V | PCE_FLAG_Z | PCE_FLAG_C)) |
                           (bin & PCE_FLAG_N) | vflag | (bin == 0U ? PCE_FLAG_Z : 0U) | cflag);
        return bin;
    }
}

static inline void pce_cmp(PceCpuState* cpu, uint8_t reg, uint8_t val) {
    uint16_t diff = (uint16_t)reg - (uint16_t)val;
    uint8_t res = (uint8_t)(diff & 0xFFU);
    cpu->p = (uint8_t)((cpu->p & (uint8_t)~(PCE_FLAG_N | PCE_FLAG_Z | PCE_FLAG_C)) |
                       (res & PCE_FLAG_N) | (res == 0U ? PCE_FLAG_Z : 0U) |
                       (reg >= val ? PCE_FLAG_C : 0U));
}

static inline uint8_t pce_asl(PceCpuState* cpu, uint8_t v) {
    uint8_t c = (v & 0x80U) ? PCE_FLAG_C : 0U;
    uint8_t r = (uint8_t)(v << 1U);
    cpu->p = (uint8_t)((cpu->p & (uint8_t)~(PCE_FLAG_N | PCE_FLAG_Z | PCE_FLAG_C)) |
                       (r & PCE_FLAG_N) | (r == 0U ? PCE_FLAG_Z : 0U) | c);
    return r;
}

static inline uint8_t pce_lsr(PceCpuState* cpu, uint8_t v) {
    uint8_t c = (v & 0x01U) ? PCE_FLAG_C : 0U;
    uint8_t r = (uint8_t)(v >> 1U);
    cpu->p = (uint8_t)((cpu->p & (uint8_t)~(PCE_FLAG_N | PCE_FLAG_Z | PCE_FLAG_C)) |
                       (r == 0U ? PCE_FLAG_Z : 0U) | c);
    return r;
}

static inline uint8_t pce_rol(PceCpuState* cpu, uint8_t v) {
    uint8_t old_c = (cpu->p & PCE_FLAG_C) ? 1U : 0U;
    uint8_t c = (v & 0x80U) ? PCE_FLAG_C : 0U;
    uint8_t r = (uint8_t)((v << 1U) | old_c);
    cpu->p = (uint8_t)((cpu->p & (uint8_t)~(PCE_FLAG_N | PCE_FLAG_Z | PCE_FLAG_C)) |
                       (r & PCE_FLAG_N) | (r == 0U ? PCE_FLAG_Z : 0U) | c);
    return r;
}

static inline uint8_t pce_ror(PceCpuState* cpu, uint8_t v) {
    uint8_t old_c = (cpu->p & PCE_FLAG_C) ? 0x80U : 0U;
    uint8_t c = (v & 0x01U) ? PCE_FLAG_C : 0U;
    uint8_t r = (uint8_t)((v >> 1U) | old_c);
    cpu->p = (uint8_t)((cpu->p & (uint8_t)~(PCE_FLAG_N | PCE_FLAG_Z | PCE_FLAG_C)) |
                       (r & PCE_FLAG_N) | (r == 0U ? PCE_FLAG_Z : 0U) | c);
    return r;
}

/* HuC6280 T-flag-aware ALU helpers: when T==1, operate on ZP($2000|X) and clear T */
static inline void pce_ora_t(PceCpuState* cpu, uint8_t v) {
    if (cpu->p & PCE_FLAG_T) {
        uint8_t r = (uint8_t)(pce_zp_read(cpu, cpu->x) | v);
        pce_zp_write(cpu, cpu->x, r);
        cpu->p &= (uint8_t)~PCE_FLAG_T;
        pce_set_nz(cpu, r);
        pce_add_cycles(cpu, 3U);
    } else {
        cpu->a |= v;
        pce_set_nz(cpu, cpu->a);
    }
}

static inline void pce_and_t(PceCpuState* cpu, uint8_t v) {
    if (cpu->p & PCE_FLAG_T) {
        uint8_t r = (uint8_t)(pce_zp_read(cpu, cpu->x) & v);
        pce_zp_write(cpu, cpu->x, r);
        cpu->p &= (uint8_t)~PCE_FLAG_T;
        pce_set_nz(cpu, r);
        pce_add_cycles(cpu, 3U);
    } else {
        cpu->a &= v;
        pce_set_nz(cpu, cpu->a);
    }
}

static inline void pce_eor_t(PceCpuState* cpu, uint8_t v) {
    if (cpu->p & PCE_FLAG_T) {
        uint8_t r = (uint8_t)(pce_zp_read(cpu, cpu->x) ^ v);
        pce_zp_write(cpu, cpu->x, r);
        cpu->p &= (uint8_t)~PCE_FLAG_T;
        pce_set_nz(cpu, r);
        pce_add_cycles(cpu, 3U);
    } else {
        cpu->a ^= v;
        pce_set_nz(cpu, cpu->a);
    }
}

static inline void pce_adc_t(PceCpuState* cpu, uint8_t v) {
    if (cpu->p & PCE_FLAG_T) {
        uint8_t r = pce_adc_val(cpu, pce_zp_read(cpu, cpu->x), v);
        pce_zp_write(cpu, cpu->x, r);
        cpu->p &= (uint8_t)~PCE_FLAG_T;
        pce_add_cycles(cpu, 3U);
    } else {
        cpu->a = pce_adc_val(cpu, cpu->a, v);
    }
}

static inline void pce_tst_val(PceCpuState* cpu, uint8_t imm, uint8_t mem) {
    cpu->p = (uint8_t)((cpu->p & (uint8_t)~(PCE_FLAG_N | PCE_FLAG_V | PCE_FLAG_Z)) |
                       (mem & (PCE_FLAG_N | PCE_FLAG_V)) |
                       (((mem & imm) == 0U) ? PCE_FLAG_Z : 0U));
}

#ifdef __cplusplus
}
#endif
