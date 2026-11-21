#pragma once

/*
 * C99 / C++20 compatible Sega Genesis / Mega Drive Motorola 68000 CPU state & runtime interface.
 * Used by both the MD runtime and mdrecomp-generated C99 code.
 *
 * Reference: Motorola M68000 Programmer's Reference Manual, docs/GENESIS_SPEC.md §2.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__cplusplus) && !defined(inline)
#define inline __inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Status Register (SR) bit masks: T . S . . I2 I1 I0 . . . X N Z V C */
#define MD_FLAG_C 0x0001U
#define MD_FLAG_V 0x0002U
#define MD_FLAG_Z 0x0004U
#define MD_FLAG_N 0x0008U
#define MD_FLAG_X 0x0010U

#define MD_SR_CCR_MASK 0x001FU
#define MD_SR_I_MASK 0x0700U
#define MD_SR_S 0x2000U
#define MD_SR_T 0x8000U
#define MD_SR_VALID 0xA71FU

/* M68000 Exception Vector numbers */
#define MD_VEC_BUS_ERROR 2U
#define MD_VEC_ADDR_ERROR 3U
#define MD_VEC_ILLEGAL 4U
#define MD_VEC_ZERO_DIV 5U
#define MD_VEC_CHK 6U
#define MD_VEC_TRAPV 7U
#define MD_VEC_PRIVILEGE 8U
#define MD_VEC_TRACE 9U
#define MD_VEC_LINE_A 10U
#define MD_VEC_LINE_F 11U
#define MD_VEC_SPURIOUS 24U
#define MD_VEC_IRQ_L1 25U
#define MD_VEC_IRQ_L2 26U /* External / Controller TH IRQ */
#define MD_VEC_IRQ_L3 27U
#define MD_VEC_IRQ_L4 28U /* VDP H-Int */
#define MD_VEC_IRQ_L5 29U
#define MD_VEC_IRQ_L6 30U /* VDP V-Int */
#define MD_VEC_IRQ_L7 31U
#define MD_VEC_TRAP_BASE 32U /* TRAP #0..#15 -> vectors 32..47 */

typedef struct MdCpuState {
    uint32_t d[8]; /* Data registers D0-D7 */
    uint32_t a[8]; /* Address registers A0-A7 (a[7] = active SP) */
    uint32_t usp;  /* Saved User Stack Pointer */
    uint32_t ssp;  /* Saved Supervisor Stack Pointer */
    uint32_t pc;   /* 24-bit Program Counter */
    uint16_t sr;   /* Status Register */

    uint8_t stopped;     /* 1 if STOP instruction active */
    uint8_t pending_irq; /* Highest pending IRQ level (0..7) */
    uint32_t reserved;

    uint64_t cycles;     /* Master M68000 cycle counter (7.67 MHz) */
    uint64_t next_event; /* Next cycle count to poll VDP/Z80/audio/IRQs */

    void* bus;          /* Opaque pointer to dragon::md::Bus */
    uint8_t* wram;      /* Direct pointer to 64 KiB Work RAM ($FF0000-$FFFFFF) */
    const uint8_t* rom; /* Direct pointer to loaded Cartridge ROM */
    uint32_t rom_size;  /* Loaded ROM size in bytes */
} MdCpuState;

typedef void (*MdRecompFunc)(MdCpuState* cpu);

typedef struct MdRecompFuncEntry {
    uint32_t addr;
    MdRecompFunc func;
} MdRecompFuncEntry;

/* Big-Endian 24-bit Bus Access */
uint8_t md_bus_read8(MdCpuState* cpu, uint32_t addr);
uint16_t md_bus_read16(MdCpuState* cpu, uint32_t addr);
uint32_t md_bus_read32(MdCpuState* cpu, uint32_t addr);
void md_bus_write8(MdCpuState* cpu, uint32_t addr, uint8_t value);
void md_bus_write16(MdCpuState* cpu, uint32_t addr, uint16_t value);
void md_bus_write32(MdCpuState* cpu, uint32_t addr, uint32_t value);

/* Runtime synchronization, dispatch & exceptions */
int md_runtime_poll(MdCpuState* cpu);
int md_runtime_stop(MdCpuState* cpu, uint16_t new_sr);
int md_runtime_dispatch(MdCpuState* cpu, uint32_t target, uint32_t from_pc);
int md_runtime_step(MdCpuState* cpu);
void md_runtime_exception(MdCpuState* cpu, uint8_t vector_num);

/* Update SR while swapping USP/SSP when Supervisor bit (0x2000) changes */
static inline void md_set_sr(MdCpuState* cpu, uint16_t new_sr) {
    uint16_t old_s = (uint16_t)(cpu->sr & MD_SR_S);
    uint16_t new_s = (uint16_t)(new_sr & MD_SR_S);
    if (old_s != new_s) {
        if (old_s) {
            cpu->ssp = cpu->a[7];
            cpu->a[7] = cpu->usp;
        } else {
            cpu->usp = cpu->a[7];
            cpu->a[7] = cpu->ssp;
        }
    }
    cpu->sr = (uint16_t)(new_sr & MD_SR_VALID);
}

static inline void md_set_ccr(MdCpuState* cpu, uint8_t ccr) {
    cpu->sr = (uint16_t)((cpu->sr & ~MD_SR_CCR_MASK) | (ccr & MD_SR_CCR_MASK));
}

static inline uint8_t md_cc_test(uint16_t sr, uint8_t cc) {
    uint8_t c = (sr & MD_FLAG_C) != 0U;
    uint8_t v = (sr & MD_FLAG_V) != 0U;
    uint8_t z = (sr & MD_FLAG_Z) != 0U;
    uint8_t n = (sr & MD_FLAG_N) != 0U;
    switch (cc & 0x0FU) {
        case 0x0U:
            return 1U; /* T  */
        case 0x1U:
            return 0U; /* F  */
        case 0x2U:
            return (uint8_t)(!c && !z); /* HI */
        case 0x3U:
            return (uint8_t)(c || z); /* LS */
        case 0x4U:
            return (uint8_t)(!c); /* CC */
        case 0x5U:
            return c; /* CS */
        case 0x6U:
            return (uint8_t)(!z); /* NE */
        case 0x7U:
            return z; /* EQ */
        case 0x8U:
            return (uint8_t)(!v); /* VC */
        case 0x9U:
            return v; /* VS */
        case 0xAU:
            return (uint8_t)(!n); /* PL */
        case 0xBU:
            return n; /* MI */
        case 0xCU:
            return (uint8_t)(n == v); /* GE */
        case 0xDU:
            return (uint8_t)(n != v); /* LT */
        case 0xEU:
            return (uint8_t)(!z && (n == v)); /* GT */
        case 0xFU:
            return (uint8_t)(z || (n != v)); /* LE */
        default:
            return 0U;
    }
}

static inline void md_set_nz8(MdCpuState* cpu, uint8_t val) {
    uint16_t f = (uint16_t)(cpu->sr & ~(MD_FLAG_N | MD_FLAG_Z | MD_FLAG_V | MD_FLAG_C));
    if (val == 0U)
        f |= MD_FLAG_Z;
    if (val & 0x80U)
        f |= MD_FLAG_N;
    cpu->sr = f;
}

static inline void md_set_nz16(MdCpuState* cpu, uint16_t val) {
    uint16_t f = (uint16_t)(cpu->sr & ~(MD_FLAG_N | MD_FLAG_Z | MD_FLAG_V | MD_FLAG_C));
    if (val == 0U)
        f |= MD_FLAG_Z;
    if (val & 0x8000U)
        f |= MD_FLAG_N;
    cpu->sr = f;
}

static inline void md_set_nz32(MdCpuState* cpu, uint32_t val) {
    uint16_t f = (uint16_t)(cpu->sr & ~(MD_FLAG_N | MD_FLAG_Z | MD_FLAG_V | MD_FLAG_C));
    if (val == 0U)
        f |= MD_FLAG_Z;
    if (val & 0x80000000U)
        f |= MD_FLAG_N;
    cpu->sr = f;
}

#ifdef __cplusplus
}
#endif
