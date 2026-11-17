#pragma once

/*
 * C99 / C++20 compatible Game Boy Advance (ARM7TDMI) CPU state and runtime interface.
 * Used by both the GBA runtime and gbarecomp-generated C99 code.
 *
 * Reference: docs/SPEC.md, docs/GBA_SPEC.md, GBATEK.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__cplusplus) && !defined(inline)
#define inline __inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define GBA_CPSR_N (1U << 31)
#define GBA_CPSR_Z (1U << 30)
#define GBA_CPSR_C (1U << 29)
#define GBA_CPSR_V (1U << 28)
#define GBA_CPSR_I (1U << 7)
#define GBA_CPSR_F (1U << 6)
#define GBA_CPSR_T (1U << 5)
#define GBA_CPSR_MODE_MASK 0x1FU

#define GBA_MODE_USR 0x10U
#define GBA_MODE_FIQ 0x11U
#define GBA_MODE_IRQ 0x12U
#define GBA_MODE_SVC 0x13U
#define GBA_MODE_ABT 0x17U
#define GBA_MODE_UND 0x1BU
#define GBA_MODE_SYS 0x1FU

typedef struct CpuState {
    uint32_t r[16]; /* r[0..12] GPRs, r[13]=SP, r[14]=LR, r[15]=PC */
    uint32_t cpsr;
    uint32_t spsr;

    /* Unpacked flags (0 or 1) for fast C99 access */
    uint8_t n;
    uint8_t z;
    uint8_t c;
    uint8_t v;
    uint8_t i_disable;
    uint8_t f_disable;
    uint8_t thumb; /* 0 = ARM (32-bit), 1 = Thumb (16-bit) */
    uint8_t halted;

    /* Banked registers across processor modes */
    uint32_t r13_usr, r14_usr;
    uint32_t r13_irq, r14_irq, spsr_irq;
    uint32_t r13_svc, r14_svc, spsr_svc;
    uint32_t r13_abt, r14_abt, spsr_abt;
    uint32_t r13_und, r14_und, spsr_und;
    uint32_t r8_usr[5];
    uint32_t r8_fiq[5];
    uint32_t r13_fiq, r14_fiq, spsr_fiq;

    uint64_t cycles;     /* Master CPU cycle counter (16.78 MHz) */
    uint64_t next_event; /* Cycle threshold at which runtime_poll must be called */

    void* bus; /* Opaque runtime Bus pointer */
} CpuState;

typedef void (*RecompFunc)(CpuState* cpu);

typedef struct RecompFuncEntry {
    uint32_t addr;
    uint8_t thumb;
    RecompFunc func;
} RecompFuncEntry;

uint8_t gba_bus_read8(CpuState* cpu, uint32_t addr);
uint16_t gba_bus_read16(CpuState* cpu, uint32_t addr);
uint32_t gba_bus_read32(CpuState* cpu, uint32_t addr);
void gba_bus_write8(CpuState* cpu, uint32_t addr, uint8_t value);
void gba_bus_write16(CpuState* cpu, uint32_t addr, uint16_t value);
void gba_bus_write32(CpuState* cpu, uint32_t addr, uint32_t value);

void gba_runtime_poll(CpuState* cpu);
void gba_runtime_dispatch(CpuState* cpu, uint32_t target, uint32_t from_pc);
// Delivers a pending hardware IRQ: enters the BIOS IRQ vector and runs the
// user handler to completion (through its internal IRQ->System mode switch)
// before resuming the interrupted code. Replaces the ad-hoc "dispatch then
// restore pc" sequence, which aborted handlers at their first mode change.
void gba_dispatch_irq(CpuState* cpu);
void gba_swi(CpuState* cpu, uint8_t swi_num);
uint32_t gba_runtime_step(CpuState* cpu);

static inline uint32_t gba_ror32(uint32_t val, uint32_t amt) {
    amt &= 31U;
    if (amt == 0U) {
        return val;
    }
    return (val >> amt) | (val << (32U - amt));
}

/* ARM7TDMI unaligned LDR rotates the aligned word right by (addr & 3) * 8 */
static inline uint32_t gba_ldr32(CpuState* cpu, uint32_t addr) {
    uint32_t word = gba_bus_read32(cpu, addr & ~3U);
    uint32_t rot = (addr & 3U) * 8U;
    return rot ? gba_ror32(word, rot) : word;
}

/* ARM7TDMI unaligned LDRH rotates the aligned halfword right by 8 */
static inline uint32_t gba_ldrh16(CpuState* cpu, uint32_t addr) {
    uint32_t hw = gba_bus_read16(cpu, addr & ~1U);
    if (addr & 1U) {
        return gba_ror32(hw, 8U);
    }
    return hw;
}

/* ARM7TDMI unaligned LDRSH sign-extends only the single byte at the odd address */
static inline uint32_t gba_ldrsh16(CpuState* cpu, uint32_t addr) {
    if (addr & 1U) {
        int8_t b = (int8_t)gba_bus_read8(cpu, addr);
        return (uint32_t)(int32_t)b;
    }
    int16_t hw = (int16_t)gba_bus_read16(cpu, addr);
    return (uint32_t)(int32_t)hw;
}

static inline uint32_t gba_ldrsb8(CpuState* cpu, uint32_t addr) {
    int8_t b = (int8_t)gba_bus_read8(cpu, addr);
    return (uint32_t)(int32_t)b;
}

static inline uint32_t gba_pack_cpsr(CpuState* cpu) {
    uint32_t mode = cpu->cpsr & GBA_CPSR_MODE_MASK;
    if (mode == 0U) {
        mode = GBA_MODE_SYS;
    }
    uint32_t psr = mode;
    if (cpu->n)
        psr |= GBA_CPSR_N;
    if (cpu->z)
        psr |= GBA_CPSR_Z;
    if (cpu->c)
        psr |= GBA_CPSR_C;
    if (cpu->v)
        psr |= GBA_CPSR_V;
    if (cpu->i_disable)
        psr |= GBA_CPSR_I;
    if (cpu->f_disable)
        psr |= GBA_CPSR_F;
    if (cpu->thumb)
        psr |= GBA_CPSR_T;
    cpu->cpsr = psr;
    return psr;
}

static inline void gba_switch_mode(CpuState* cpu, uint8_t new_mode) {
    uint8_t old_mode = (uint8_t)(cpu->cpsr & GBA_CPSR_MODE_MASK);
    if (old_mode == 0U) {
        old_mode = GBA_MODE_SYS;
    }
    if (old_mode == new_mode) {
        cpu->cpsr = (cpu->cpsr & ~GBA_CPSR_MODE_MASK) | new_mode;
        return;
    }

    /* Save outgoing mode's banked registers */
    switch (old_mode) {
        case GBA_MODE_USR:
        case GBA_MODE_SYS:
            cpu->r13_usr = cpu->r[13];
            cpu->r14_usr = cpu->r[14];
            break;
        case GBA_MODE_IRQ:
            cpu->r13_irq = cpu->r[13];
            cpu->r14_irq = cpu->r[14];
            cpu->spsr_irq = cpu->spsr;
            break;
        case GBA_MODE_SVC:
            cpu->r13_svc = cpu->r[13];
            cpu->r14_svc = cpu->r[14];
            cpu->spsr_svc = cpu->spsr;
            break;
        case GBA_MODE_ABT:
            cpu->r13_abt = cpu->r[13];
            cpu->r14_abt = cpu->r[14];
            cpu->spsr_abt = cpu->spsr;
            break;
        case GBA_MODE_UND:
            cpu->r13_und = cpu->r[13];
            cpu->r14_und = cpu->r[14];
            cpu->spsr_und = cpu->spsr;
            break;
        case GBA_MODE_FIQ: {
            for (int i = 0; i < 5; ++i) {
                cpu->r8_fiq[i] = cpu->r[8 + i];
                cpu->r[8 + i] = cpu->r8_usr[i];
            }
            cpu->r13_fiq = cpu->r[13];
            cpu->r14_fiq = cpu->r[14];
            cpu->spsr_fiq = cpu->spsr;
            break;
        }
        default:
            break;
    }

    /* Load incoming mode's banked registers */
    switch (new_mode) {
        case GBA_MODE_USR:
        case GBA_MODE_SYS:
            cpu->r[13] = cpu->r13_usr;
            cpu->r[14] = cpu->r14_usr;
            break;
        case GBA_MODE_IRQ:
            cpu->r[13] = cpu->r13_irq;
            cpu->r[14] = cpu->r14_irq;
            cpu->spsr = cpu->spsr_irq;
            break;
        case GBA_MODE_SVC:
            cpu->r[13] = cpu->r13_svc;
            cpu->r[14] = cpu->r14_svc;
            cpu->spsr = cpu->spsr_svc;
            break;
        case GBA_MODE_ABT:
            cpu->r[13] = cpu->r13_abt;
            cpu->r[14] = cpu->r14_abt;
            cpu->spsr = cpu->spsr_abt;
            break;
        case GBA_MODE_UND:
            cpu->r[13] = cpu->r13_und;
            cpu->r[14] = cpu->r14_und;
            cpu->spsr = cpu->spsr_und;
            break;
        case GBA_MODE_FIQ: {
            for (int i = 0; i < 5; ++i) {
                cpu->r8_usr[i] = cpu->r[8 + i];
                cpu->r[8 + i] = cpu->r8_fiq[i];
            }
            cpu->r[13] = cpu->r13_fiq;
            cpu->r[14] = cpu->r14_fiq;
            cpu->spsr = cpu->spsr_fiq;
            break;
        }
        default:
            break;
    }

    cpu->cpsr = (cpu->cpsr & ~GBA_CPSR_MODE_MASK) | new_mode;
}

static inline void gba_unpack_cpsr(CpuState* cpu, uint32_t val) {
    uint8_t new_mode = (uint8_t)(val & GBA_CPSR_MODE_MASK);
    if (new_mode != 0U) {
        gba_switch_mode(cpu, new_mode);
    }
    cpu->n = (val & GBA_CPSR_N) ? 1U : 0U;
    cpu->z = (val & GBA_CPSR_Z) ? 1U : 0U;
    cpu->c = (val & GBA_CPSR_C) ? 1U : 0U;
    cpu->v = (val & GBA_CPSR_V) ? 1U : 0U;
    cpu->i_disable = (val & GBA_CPSR_I) ? 1U : 0U;
    cpu->f_disable = (val & GBA_CPSR_F) ? 1U : 0U;
    cpu->thumb = (val & GBA_CPSR_T) ? 1U : 0U;
    cpu->cpsr = val;
}

static inline int gba_check_cond(const CpuState* cpu, uint8_t cond) {
    switch (cond & 0x0FU) {
        case 0x0:
            return cpu->z != 0; /* EQ */
        case 0x1:
            return cpu->z == 0; /* NE */
        case 0x2:
            return cpu->c != 0; /* CS / HS */
        case 0x3:
            return cpu->c == 0; /* CC / LO */
        case 0x4:
            return cpu->n != 0; /* MI */
        case 0x5:
            return cpu->n == 0; /* PL */
        case 0x6:
            return cpu->v != 0; /* VS */
        case 0x7:
            return cpu->v == 0; /* VC */
        case 0x8:
            return (cpu->c != 0) && (cpu->z == 0); /* HI */
        case 0x9:
            return (cpu->c == 0) || (cpu->z != 0); /* LS */
        case 0xA:
            return cpu->n == cpu->v; /* GE */
        case 0xB:
            return cpu->n != cpu->v; /* LT */
        case 0xC:
            return (cpu->z == 0) && (cpu->n == cpu->v); /* GT */
        case 0xD:
            return (cpu->z != 0) || (cpu->n != cpu->v); /* LE */
        case 0xE:
            return 1; /* AL */
        default:
            return 0; /* NV */
    }
}

static inline void gba_set_nz(CpuState* cpu, uint32_t res) {
    cpu->n = (uint8_t)((res >> 31U) & 1U);
    cpu->z = (res == 0U) ? 1U : 0U;
}

static inline uint32_t gba_add(CpuState* cpu, uint32_t a, uint32_t b, int set_flags) {
    uint64_t sum = (uint64_t)a + (uint64_t)b;
    uint32_t res = (uint32_t)sum;
    if (set_flags) {
        cpu->n = (uint8_t)((res >> 31U) & 1U);
        cpu->z = (res == 0U) ? 1U : 0U;
        cpu->c = (sum > 0xFFFFFFFFULL) ? 1U : 0U;
        cpu->v = ((~(a ^ b) & (a ^ res)) >> 31U) & 1U;
    }
    return res;
}

static inline uint32_t gba_adc(CpuState* cpu, uint32_t a, uint32_t b, int set_flags) {
    uint64_t cin = cpu->c ? 1ULL : 0ULL;
    uint64_t sum = (uint64_t)a + (uint64_t)b + cin;
    uint32_t res = (uint32_t)sum;
    if (set_flags) {
        cpu->n = (uint8_t)((res >> 31U) & 1U);
        cpu->z = (res == 0U) ? 1U : 0U;
        cpu->c = (sum > 0xFFFFFFFFULL) ? 1U : 0U;
        cpu->v = ((~(a ^ b) & (a ^ res)) >> 31U) & 1U;
    }
    return res;
}

static inline uint32_t gba_sub(CpuState* cpu, uint32_t a, uint32_t b, int set_flags) {
    uint32_t res = a - b;
    if (set_flags) {
        cpu->n = (uint8_t)((res >> 31U) & 1U);
        cpu->z = (res == 0U) ? 1U : 0U;
        cpu->c = (a >= b) ? 1U : 0U;
        cpu->v = (((a ^ b) & (a ^ res)) >> 31U) & 1U;
    }
    return res;
}

static inline uint32_t gba_sbc(CpuState* cpu, uint32_t a, uint32_t b, int set_flags) {
    uint64_t borrow = cpu->c ? 0ULL : 1ULL;
    uint64_t diff = (uint64_t)a - (uint64_t)b - borrow;
    uint32_t res = (uint32_t)diff;
    if (set_flags) {
        cpu->n = (uint8_t)((res >> 31U) & 1U);
        cpu->z = (res == 0U) ? 1U : 0U;
        cpu->c = ((uint64_t)a >= ((uint64_t)b + borrow)) ? 1U : 0U;
        cpu->v = (((a ^ b) & (a ^ res)) >> 31U) & 1U;
    }
    return res;
}

/* Barrel shifter by 5-bit immediate (implements ARMv4T special cases for imm5 == 0) */
static inline uint32_t gba_shift_imm(uint32_t val, uint8_t type, uint8_t imm5, uint8_t* carry_io) {
    switch (type & 3U) {
        case 0: /* LSL */
            if (imm5 == 0U) {
                return val;
            }
            *carry_io = (uint8_t)((val >> (32U - imm5)) & 1U);
            return val << imm5;
        case 1:               /* LSR */
            if (imm5 == 0U) { /* LSR #32 */
                *carry_io = (uint8_t)((val >> 31U) & 1U);
                return 0U;
            }
            *carry_io = (uint8_t)((val >> (imm5 - 1U)) & 1U);
            return val >> imm5;
        case 2:               /* ASR */
            if (imm5 == 0U) { /* ASR #32 */
                uint8_t sign = (uint8_t)((val >> 31U) & 1U);
                *carry_io = sign;
                return sign ? 0xFFFFFFFFU : 0U;
            }
            *carry_io = (uint8_t)((val >> (imm5 - 1U)) & 1U);
            return (uint32_t)(((int32_t)val) >> imm5);
        default:              /* ROR / RRX */
            if (imm5 == 0U) { /* RRX #1 */
                uint32_t old_c = (*carry_io) ? 1U : 0U;
                *carry_io = (uint8_t)(val & 1U);
                return (val >> 1U) | (old_c << 31U);
            }
            *carry_io = (uint8_t)((val >> (imm5 - 1U)) & 1U);
            return gba_ror32(val, imm5);
    }
}

/* Barrel shifter by register bottom byte */
static inline uint32_t gba_shift_reg(uint32_t val, uint8_t type, uint8_t amt, uint8_t* carry_io) {
    if (amt == 0U) {
        return val;
    }
    switch (type & 3U) {
        case 0: /* LSL */
            if (amt < 32U) {
                *carry_io = (uint8_t)((val >> (32U - amt)) & 1U);
                return val << amt;
            } else if (amt == 32U) {
                *carry_io = (uint8_t)(val & 1U);
                return 0U;
            } else {
                *carry_io = 0U;
                return 0U;
            }
        case 1: /* LSR */
            if (amt < 32U) {
                *carry_io = (uint8_t)((val >> (amt - 1U)) & 1U);
                return val >> amt;
            } else if (amt == 32U) {
                *carry_io = (uint8_t)((val >> 31U) & 1U);
                return 0U;
            } else {
                *carry_io = 0U;
                return 0U;
            }
        case 2: /* ASR */
            if (amt < 32U) {
                *carry_io = (uint8_t)((val >> (amt - 1U)) & 1U);
                return (uint32_t)(((int32_t)val) >> amt);
            } else {
                uint8_t sign = (uint8_t)((val >> 31U) & 1U);
                *carry_io = sign;
                return sign ? 0xFFFFFFFFU : 0U;
            }
        default: { /* ROR */
            uint8_t rot = amt & 31U;
            if (rot == 0U) {
                *carry_io = (uint8_t)((val >> 31U) & 1U);
                return val;
            }
            *carry_io = (uint8_t)((val >> (rot - 1U)) & 1U);
            return gba_ror32(val, rot);
        }
    }
}

static inline void gba_bx(CpuState* cpu, uint32_t target) {
    cpu->thumb = (uint8_t)(target & 1U);
    if (cpu->thumb) {
        cpu->r[15] = target & ~1U;
        cpu->cpsr |= GBA_CPSR_T;
    } else {
        cpu->r[15] = target & ~3U;
        cpu->cpsr &= ~GBA_CPSR_T;
    }
}

#ifdef __cplusplus
}
#endif
