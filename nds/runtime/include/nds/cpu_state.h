#pragma once

/*
 * C99 / C++20 compatible Nintendo DS (ARM946E-S + ARM7TDMI) CPU state and runtime interface.
 * Used by both nds_runtime and ndsrecomp-generated C99 code.
 *
 * Reference: docs/NDS_SPEC.md, GBATEK (https://problemkaputt.de/gbatek.htm).
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__cplusplus) && !defined(inline)
#define inline __inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define NDS_CPSR_N (1U << 31)
#define NDS_CPSR_Z (1U << 30)
#define NDS_CPSR_C (1U << 29)
#define NDS_CPSR_V (1U << 28)
#define NDS_CPSR_Q (1U << 27)
#define NDS_CPSR_I (1U << 7)
#define NDS_CPSR_F (1U << 6)
#define NDS_CPSR_T (1U << 5)
#define NDS_CPSR_MODE_MASK 0x1FU

#define NDS_MODE_USR 0x10U
#define NDS_MODE_FIQ 0x11U
#define NDS_MODE_IRQ 0x12U
#define NDS_MODE_SVC 0x13U
#define NDS_MODE_ABT 0x17U
#define NDS_MODE_UND 0x1BU
#define NDS_MODE_SYS 0x1FU

typedef struct NdsCpuState {
    uint32_t r[16]; /* r[0..12] GPRs, r[13]=SP, r[14]=LR, r[15]=PC */
    uint32_t cpsr;
    uint32_t spsr;

    /* Unpacked flags (0 or 1) for fast C99 access */
    uint8_t n;
    uint8_t z;
    uint8_t c;
    uint8_t v;
    uint8_t q; /* ARMv5TE sticky saturation flag */
    uint8_t i_disable;
    uint8_t f_disable;
    uint8_t thumb;
    uint8_t halted;  /* 1 = WFI/Halt, 2 = Runner exit */
    uint8_t is_arm9; /* 1 = ARM946E-S (ARMv5TE), 0 = ARM7TDMI (ARMv4T) */

    /* Banked registers across processor modes */
    uint32_t r13_usr, r14_usr;
    uint32_t r13_irq, r14_irq, spsr_irq;
    uint32_t r13_svc, r14_svc, spsr_svc;
    uint32_t r13_abt, r14_abt, spsr_abt;
    uint32_t r13_und, r14_und, spsr_und;
    uint32_t r8_usr[5];
    uint32_t r8_fiq[5];
    uint32_t r13_fiq, r14_fiq, spsr_fiq;

    /* ARM9 CP15 state */
    uint32_t cp15_ctrl;
    uint32_t cp15_dtcm_reg;
    uint32_t cp15_itcm_reg;
    uint32_t dtcm_base;
    uint32_t dtcm_size;
    uint32_t itcm_size;

    uint64_t cycles;
    uint64_t next_event;

    void* bus; /* Opaque dragon::nds::Bus* */
} NdsCpuState;

typedef void (*NdsRecompFunc)(NdsCpuState* cpu);

typedef struct NdsRecompFuncEntry {
    uint32_t addr;
    uint8_t thumb;
    NdsRecompFunc func;
} NdsRecompFuncEntry;

uint8_t nds_bus_read8(NdsCpuState* cpu, uint32_t addr);
uint16_t nds_bus_read16(NdsCpuState* cpu, uint32_t addr);
uint32_t nds_bus_read32(NdsCpuState* cpu, uint32_t addr);
void nds_bus_write8(NdsCpuState* cpu, uint32_t addr, uint8_t value);
void nds_bus_write16(NdsCpuState* cpu, uint32_t addr, uint16_t value);
void nds_bus_write32(NdsCpuState* cpu, uint32_t addr, uint32_t value);

uint32_t nds_cp15_read(NdsCpuState* cpu, uint32_t cn, uint32_t cm, uint32_t cp);
void nds_cp15_write(NdsCpuState* cpu, uint32_t cn, uint32_t cm, uint32_t cp, uint32_t value);

void nds_runtime_poll(NdsCpuState* cpu);
void nds_runtime_dispatch(NdsCpuState* cpu, uint32_t target, uint32_t from_pc);
void nds_dispatch_irq(NdsCpuState* cpu);
void nds_swi(NdsCpuState* cpu, uint8_t swi_num);
uint32_t nds_runtime_step(NdsCpuState* cpu);

static inline uint32_t nds_ror32(uint32_t val, uint32_t amt) {
    amt &= 31U;
    return amt ? ((val >> amt) | (val << (32U - amt))) : val;
}

static inline uint32_t nds_ldr32(NdsCpuState* cpu, uint32_t addr) {
    uint32_t word = nds_bus_read32(cpu, addr & ~3U);
    uint32_t rot = (addr & 3U) * 8U;
    return rot ? nds_ror32(word, rot) : word;
}

static inline uint32_t nds_ldrh16(NdsCpuState* cpu, uint32_t addr) {
    uint32_t hw = nds_bus_read16(cpu, addr & ~1U);
    if (!cpu->is_arm9 && (addr & 1U)) {
        return nds_ror32(hw, 8U);
    }
    return hw;
}

static inline uint32_t nds_ldrsh16(NdsCpuState* cpu, uint32_t addr) {
    if (!cpu->is_arm9 && (addr & 1U)) {
        return (uint32_t)(int32_t)(int8_t)nds_bus_read8(cpu, addr);
    }
    return (uint32_t)(int32_t)(int16_t)nds_bus_read16(cpu, addr & ~1U);
}

static inline uint32_t nds_ldrsb8(NdsCpuState* cpu, uint32_t addr) {
    return (uint32_t)(int32_t)(int8_t)nds_bus_read8(cpu, addr);
}

static inline uint32_t nds_clz32(uint32_t val) {
    if (val == 0U) {
        return 32U;
    }
    uint32_t n = 0U;
    if ((val & 0xFFFF0000U) == 0U) {
        n += 16U;
        val <<= 16U;
    }
    if ((val & 0xFF000000U) == 0U) {
        n += 8U;
        val <<= 8U;
    }
    if ((val & 0xF0000000U) == 0U) {
        n += 4U;
        val <<= 4U;
    }
    if ((val & 0xC0000000U) == 0U) {
        n += 2U;
        val <<= 2U;
    }
    if ((val & 0x80000000U) == 0U) {
        n += 1U;
    }
    return n;
}

static inline uint32_t nds_pack_cpsr(const NdsCpuState* cpu) {
    uint32_t v = cpu->cpsr & ~(NDS_CPSR_N | NDS_CPSR_Z | NDS_CPSR_C | NDS_CPSR_V | NDS_CPSR_Q |
                               NDS_CPSR_I | NDS_CPSR_F | NDS_CPSR_T);
    if (cpu->n)
        v |= NDS_CPSR_N;
    if (cpu->z)
        v |= NDS_CPSR_Z;
    if (cpu->c)
        v |= NDS_CPSR_C;
    if (cpu->v)
        v |= NDS_CPSR_V;
    if (cpu->q)
        v |= NDS_CPSR_Q;
    if (cpu->i_disable)
        v |= NDS_CPSR_I;
    if (cpu->f_disable)
        v |= NDS_CPSR_F;
    if (cpu->thumb)
        v |= NDS_CPSR_T;
    return v;
}

static inline void nds_unpack_cpsr(NdsCpuState* cpu, uint32_t v) {
    cpu->cpsr = v;
    cpu->n = (uint8_t)((v >> 31) & 1U);
    cpu->z = (uint8_t)((v >> 30) & 1U);
    cpu->c = (uint8_t)((v >> 29) & 1U);
    cpu->v = (uint8_t)((v >> 28) & 1U);
    cpu->q = (uint8_t)((v >> 27) & 1U);
    cpu->i_disable = (uint8_t)((v >> 7) & 1U);
    cpu->f_disable = (uint8_t)((v >> 6) & 1U);
    cpu->thumb = (uint8_t)((v >> 5) & 1U);
}

static inline void nds_switch_mode(NdsCpuState* cpu, uint32_t new_mode) {
    uint32_t old_mode = cpu->cpsr & NDS_CPSR_MODE_MASK;
    new_mode &= NDS_CPSR_MODE_MASK;
    if (old_mode == new_mode) {
        return;
    }

    switch (old_mode) {
        case NDS_MODE_USR:
        case NDS_MODE_SYS:
            cpu->r13_usr = cpu->r[13];
            cpu->r14_usr = cpu->r[14];
            break;
        case NDS_MODE_IRQ:
            cpu->r13_irq = cpu->r[13];
            cpu->r14_irq = cpu->r[14];
            cpu->spsr_irq = cpu->spsr;
            break;
        case NDS_MODE_SVC:
            cpu->r13_svc = cpu->r[13];
            cpu->r14_svc = cpu->r[14];
            cpu->spsr_svc = cpu->spsr;
            break;
        case NDS_MODE_ABT:
            cpu->r13_abt = cpu->r[13];
            cpu->r14_abt = cpu->r[14];
            cpu->spsr_abt = cpu->spsr;
            break;
        case NDS_MODE_UND:
            cpu->r13_und = cpu->r[13];
            cpu->r14_und = cpu->r[14];
            cpu->spsr_und = cpu->spsr;
            break;
        case NDS_MODE_FIQ:
            for (int i = 0; i < 5; ++i)
                cpu->r8_fiq[i] = cpu->r[8 + i];
            cpu->r13_fiq = cpu->r[13];
            cpu->r14_fiq = cpu->r[14];
            cpu->spsr_fiq = cpu->spsr;
            break;
        default:
            break;
    }

    if (old_mode != NDS_MODE_FIQ && new_mode == NDS_MODE_FIQ) {
        for (int i = 0; i < 5; ++i) {
            cpu->r8_usr[i] = cpu->r[8 + i];
            cpu->r[8 + i] = cpu->r8_fiq[i];
        }
    } else if (old_mode == NDS_MODE_FIQ && new_mode != NDS_MODE_FIQ) {
        for (int i = 0; i < 5; ++i) {
            cpu->r[8 + i] = cpu->r8_usr[i];
        }
    }

    switch (new_mode) {
        case NDS_MODE_USR:
        case NDS_MODE_SYS:
            cpu->r[13] = cpu->r13_usr;
            cpu->r[14] = cpu->r14_usr;
            break;
        case NDS_MODE_IRQ:
            cpu->r[13] = cpu->r13_irq;
            cpu->r[14] = cpu->r14_irq;
            cpu->spsr = cpu->spsr_irq;
            break;
        case NDS_MODE_SVC:
            cpu->r[13] = cpu->r13_svc;
            cpu->r[14] = cpu->r14_svc;
            cpu->spsr = cpu->spsr_svc;
            break;
        case NDS_MODE_ABT:
            cpu->r[13] = cpu->r13_abt;
            cpu->r[14] = cpu->r14_abt;
            cpu->spsr = cpu->spsr_abt;
            break;
        case NDS_MODE_UND:
            cpu->r[13] = cpu->r13_und;
            cpu->r[14] = cpu->r14_und;
            cpu->spsr = cpu->spsr_und;
            break;
        case NDS_MODE_FIQ:
            cpu->r[13] = cpu->r13_fiq;
            cpu->r[14] = cpu->r14_fiq;
            cpu->spsr = cpu->spsr_fiq;
            break;
        default:
            break;
    }

    cpu->cpsr = (cpu->cpsr & ~NDS_CPSR_MODE_MASK) | new_mode;
}

static inline int nds_cond_passed(const NdsCpuState* cpu, uint32_t cond) {
    switch (cond & 0xFU) {
        case 0x0U:
            return cpu->z != 0U;
        case 0x1U:
            return cpu->z == 0U;
        case 0x2U:
            return cpu->c != 0U;
        case 0x3U:
            return cpu->c == 0U;
        case 0x4U:
            return cpu->n != 0U;
        case 0x5U:
            return cpu->n == 0U;
        case 0x6U:
            return cpu->v != 0U;
        case 0x7U:
            return cpu->v == 0U;
        case 0x8U:
            return (cpu->c != 0U) && (cpu->z == 0U);
        case 0x9U:
            return (cpu->c == 0U) || (cpu->z != 0U);
        case 0xAU:
            return cpu->n == cpu->v;
        case 0xBU:
            return cpu->n != cpu->v;
        case 0xCU:
            return (cpu->z == 0U) && (cpu->n == cpu->v);
        case 0xDU:
            return (cpu->z != 0U) || (cpu->n != cpu->v);
        case 0xEU:
            return 1;
        default:
            return 1; /* 0xF = unconditional ARMv5TE space */
    }
}

static inline uint32_t nds_add_flags(NdsCpuState* cpu, uint32_t a, uint32_t b, int set_flags) {
    uint32_t res = a + b;
    if (set_flags) {
        cpu->n = (uint8_t)(res >> 31);
        cpu->z = (uint8_t)(res == 0U);
        cpu->c = (uint8_t)(res < a);
        cpu->v = (uint8_t)(((~(a ^ b)) & (a ^ res)) >> 31);
    }
    return res;
}

static inline uint32_t nds_adc_flags(NdsCpuState* cpu, uint32_t a, uint32_t b, int set_flags) {
    uint64_t sum = (uint64_t)a + (uint64_t)b + (uint64_t)cpu->c;
    uint32_t res = (uint32_t)sum;
    if (set_flags) {
        cpu->n = (uint8_t)(res >> 31);
        cpu->z = (uint8_t)(res == 0U);
        cpu->c = (uint8_t)(sum >> 32);
        cpu->v = (uint8_t)(((~(a ^ b)) & (a ^ res)) >> 31);
    }
    return res;
}

static inline uint32_t nds_sub_flags(NdsCpuState* cpu, uint32_t a, uint32_t b, int set_flags) {
    uint32_t res = a - b;
    if (set_flags) {
        cpu->n = (uint8_t)(res >> 31);
        cpu->z = (uint8_t)(res == 0U);
        cpu->c = (uint8_t)(a >= b);
        cpu->v = (uint8_t)(((a ^ b) & (a ^ res)) >> 31);
    }
    return res;
}

static inline uint32_t nds_sbc_flags(NdsCpuState* cpu, uint32_t a, uint32_t b, int set_flags) {
    uint64_t diff = (uint64_t)a - (uint64_t)b - (uint64_t)(1U - cpu->c);
    uint32_t res = (uint32_t)diff;
    if (set_flags) {
        cpu->n = (uint8_t)(res >> 31);
        cpu->z = (uint8_t)(res == 0U);
        cpu->c = (uint8_t)((diff >> 32) == 0U);
        cpu->v = (uint8_t)(((a ^ b) & (a ^ res)) >> 31);
    }
    return res;
}

static inline void nds_set_nz(NdsCpuState* cpu, uint32_t res) {
    cpu->n = (uint8_t)(res >> 31);
    cpu->z = (uint8_t)(res == 0U);
}

static inline int32_t nds_sat32(NdsCpuState* cpu, int64_t val) {
    if (val > 0x7FFFFFFFLL) {
        cpu->q = 1U;
        return 0x7FFFFFFF;
    }
    if (val < -0x80000000LL) {
        cpu->q = 1U;
        return (int32_t)0x80000000U;
    }
    return (int32_t)val;
}

#ifdef __cplusplus
}
#endif
