#pragma once

/*
 * C99 / C++20 compatible Game Boy / Game Boy Color CPU state and runtime interface.
 * Used by both the GB runtime and gbrecomp-generated C99 code.
 *
 * Reference: docs/SPEC.md, docs/GB_SPEC.md §2.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__cplusplus) && !defined(inline)
#define inline __inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define GB_FLAG_Z 0x80U
#define GB_FLAG_N 0x40U
#define GB_FLAG_H 0x20U
#define GB_FLAG_C 0x10U

typedef struct CpuState {
    uint8_t a, f;
    uint8_t b, c;
    uint8_t d, e;
    uint8_t h, l;
    uint16_t sp;
    uint16_t pc;

    /* Interrupt and CPU execution state */
    uint8_t ime;          /* Interrupt Master Enable (0 or 1) */
    uint8_t ime_delay;    /* EI delay counter (activates IME after next instruction) */
    uint8_t halted;       /* 1 if CPU executed HALT */
    uint8_t halt_bug;     /* 1 if HALT bug triggered (repeat next byte) */
    uint8_t double_speed; /* 1 if CGB double-speed mode is active */

    uint64_t cycles;     /* Master CPU cycle counter (T-cycles) */
    uint64_t next_event; /* Cycles at which runtime must be polled */

    void* bus;          /* Opaque runtime Bus pointer */
    uint8_t* hram;      /* Direct pointer to HRAM ($FF80-$FFFE) */
    const uint8_t* rom; /* Direct pointer to loaded ROM */
    uint32_t rom_size;  /* Loaded ROM size in bytes */
} CpuState;

typedef void (*RecompFunc)(CpuState* cpu);

typedef struct RecompFuncEntry {
    uint16_t addr;
    uint8_t bank;
    RecompFunc func;
} RecompFuncEntry;

uint8_t gb_bus_read(CpuState* cpu, uint16_t addr);
void gb_bus_write(CpuState* cpu, uint16_t addr, uint8_t value);
void gb_runtime_poll(CpuState* cpu);
void gb_runtime_dispatch(CpuState* cpu, uint16_t target, uint16_t from_pc);
void gb_runtime_stop(CpuState* cpu);
uint32_t gb_runtime_step(CpuState* cpu);

static inline uint16_t gb_bus_read16(CpuState* cpu, uint16_t addr) {
    uint8_t lo = gb_bus_read(cpu, addr);
    uint8_t hi = gb_bus_read(cpu, (uint16_t)(addr + 1U));
    return (uint16_t)(((uint16_t)hi << 8U) | lo);
}

static inline void gb_bus_write16(CpuState* cpu, uint16_t addr, uint16_t value) {
    gb_bus_write(cpu, addr, (uint8_t)(value & 0xFFU));
    gb_bus_write(cpu, (uint16_t)(addr + 1U), (uint8_t)((value >> 8U) & 0xFFU));
}

/* Register pair helpers */
static inline uint16_t cpu_af(const CpuState* cpu) {
    return (uint16_t)(((uint16_t)cpu->a << 8U) | (cpu->f & 0xF0U));
}

static inline void cpu_set_af(CpuState* cpu, uint16_t val) {
    cpu->a = (uint8_t)((val >> 8U) & 0xFFU);
    cpu->f = (uint8_t)(val & 0xF0U); /* Lower 4 bits are always 0 */
}

static inline uint16_t cpu_bc(const CpuState* cpu) {
    return (uint16_t)(((uint16_t)cpu->b << 8U) | cpu->c);
}

static inline void cpu_set_bc(CpuState* cpu, uint16_t val) {
    cpu->b = (uint8_t)((val >> 8U) & 0xFFU);
    cpu->c = (uint8_t)(val & 0xFFU);
}

static inline uint16_t cpu_de(const CpuState* cpu) {
    return (uint16_t)(((uint16_t)cpu->d << 8U) | cpu->e);
}

static inline void cpu_set_de(CpuState* cpu, uint16_t val) {
    cpu->d = (uint8_t)((val >> 8U) & 0xFFU);
    cpu->e = (uint8_t)(val & 0xFFU);
}

static inline uint16_t cpu_hl(const CpuState* cpu) {
    return (uint16_t)(((uint16_t)cpu->h << 8U) | cpu->l);
}

static inline void cpu_set_hl(CpuState* cpu, uint16_t val) {
    cpu->h = (uint8_t)((val >> 8U) & 0xFFU);
    cpu->l = (uint8_t)(val & 0xFFU);
}

/* Stack helpers */
static inline void cpu_push(CpuState* cpu, uint16_t val) {
    cpu->sp = (uint16_t)(cpu->sp - 1U);
    gb_bus_write(cpu, cpu->sp, (uint8_t)((val >> 8U) & 0xFFU));
    cpu->sp = (uint16_t)(cpu->sp - 1U);
    gb_bus_write(cpu, cpu->sp, (uint8_t)(val & 0xFFU));
}

static inline uint16_t cpu_pop(CpuState* cpu) {
    uint8_t lo = gb_bus_read(cpu, cpu->sp);
    cpu->sp = (uint16_t)(cpu->sp + 1U);
    uint8_t hi = gb_bus_read(cpu, cpu->sp);
    cpu->sp = (uint16_t)(cpu->sp + 1U);
    return (uint16_t)(((uint16_t)hi << 8U) | lo);
}

/* Arithmetic & logic helpers used by interpreter and emitted C99 code */
static inline void gb_alu_add(CpuState* cpu, uint8_t val) {
    uint16_t res = (uint16_t)cpu->a + val;
    cpu->f = 0;
    if ((res & 0xFFU) == 0)
        cpu->f |= GB_FLAG_Z;
    if (((cpu->a & 0x0FU) + (val & 0x0FU)) > 0x0FU)
        cpu->f |= GB_FLAG_H;
    if (res > 0xFFU)
        cpu->f |= GB_FLAG_C;
    cpu->a = (uint8_t)res;
}

static inline void gb_alu_adc(CpuState* cpu, uint8_t val) {
    uint8_t c = (cpu->f & GB_FLAG_C) ? 1U : 0U;
    uint16_t res = (uint16_t)cpu->a + val + c;
    cpu->f = 0;
    if ((res & 0xFFU) == 0)
        cpu->f |= GB_FLAG_Z;
    if (((cpu->a & 0x0FU) + (val & 0x0FU) + c) > 0x0FU)
        cpu->f |= GB_FLAG_H;
    if (res > 0xFFU)
        cpu->f |= GB_FLAG_C;
    cpu->a = (uint8_t)res;
}

static inline void gb_alu_sub(CpuState* cpu, uint8_t val) {
    uint16_t res = (uint16_t)cpu->a - val;
    cpu->f = GB_FLAG_N;
    if ((res & 0xFFU) == 0)
        cpu->f |= GB_FLAG_Z;
    if ((cpu->a & 0x0FU) < (val & 0x0FU))
        cpu->f |= GB_FLAG_H;
    if (cpu->a < val)
        cpu->f |= GB_FLAG_C;
    cpu->a = (uint8_t)res;
}

static inline void gb_alu_sbc(CpuState* cpu, uint8_t val) {
    uint8_t c = (cpu->f & GB_FLAG_C) ? 1U : 0U;
    int32_t res = (int32_t)cpu->a - val - c;
    cpu->f = GB_FLAG_N;
    if ((res & 0xFF) == 0)
        cpu->f |= GB_FLAG_Z;
    if ((cpu->a & 0x0FU) < ((val & 0x0FU) + c))
        cpu->f |= GB_FLAG_H;
    if (cpu->a < ((uint16_t)val + c))
        cpu->f |= GB_FLAG_C;
    cpu->a = (uint8_t)res;
}

static inline void gb_alu_and(CpuState* cpu, uint8_t val) {
    cpu->a &= val;
    cpu->f = GB_FLAG_H | (cpu->a == 0 ? GB_FLAG_Z : 0);
}

static inline void gb_alu_xor(CpuState* cpu, uint8_t val) {
    cpu->a ^= val;
    cpu->f = (cpu->a == 0 ? GB_FLAG_Z : 0);
}

static inline void gb_alu_or(CpuState* cpu, uint8_t val) {
    cpu->a |= val;
    cpu->f = (cpu->a == 0 ? GB_FLAG_Z : 0);
}

static inline void gb_alu_cp(CpuState* cpu, uint8_t val) {
    uint8_t orig_a = cpu->a;
    gb_alu_sub(cpu, val);
    cpu->a = orig_a;
}

static inline uint8_t gb_alu_inc(CpuState* cpu, uint8_t val) {
    uint8_t res = (uint8_t)(val + 1U);
    cpu->f = (uint8_t)(cpu->f & GB_FLAG_C);
    if (res == 0)
        cpu->f |= GB_FLAG_Z;
    if ((val & 0x0FU) == 0x0FU)
        cpu->f |= GB_FLAG_H;
    return res;
}

static inline uint8_t gb_alu_dec(CpuState* cpu, uint8_t val) {
    uint8_t res = (uint8_t)(val - 1U);
    cpu->f = (uint8_t)((cpu->f & GB_FLAG_C) | GB_FLAG_N);
    if (res == 0)
        cpu->f |= GB_FLAG_Z;
    if ((val & 0x0FU) == 0)
        cpu->f |= GB_FLAG_H;
    return res;
}

static inline void gb_alu_add_hl(CpuState* cpu, uint16_t val) {
    uint16_t hl = cpu_hl(cpu);
    uint32_t res = (uint32_t)hl + val;
    cpu->f &= GB_FLAG_Z;
    if (((hl & 0x0FFFU) + (val & 0x0FFFU)) > 0x0FFFU)
        cpu->f |= GB_FLAG_H;
    if (res > 0xFFFFU)
        cpu->f |= GB_FLAG_C;
    cpu_set_hl(cpu, (uint16_t)res);
}

static inline void gb_alu_daa(CpuState* cpu) {
    if (!(cpu->f & GB_FLAG_N)) {
        if ((cpu->f & GB_FLAG_C) || cpu->a > 0x99U) {
            cpu->a += 0x60U;
            cpu->f |= GB_FLAG_C;
        }
        if ((cpu->f & GB_FLAG_H) || (cpu->a & 0x0FU) > 0x09U) {
            cpu->a += 0x06U;
        }
    } else {
        if (cpu->f & GB_FLAG_C) {
            cpu->a -= 0x60U;
        }
        if (cpu->f & GB_FLAG_H) {
            cpu->a -= 0x06U;
        }
    }
    cpu->f = (uint8_t)((cpu->f & (GB_FLAG_N | GB_FLAG_C)) | (cpu->a == 0 ? GB_FLAG_Z : 0));
}

static inline void gb_alu_cpl(CpuState* cpu) {
    cpu->a = ~cpu->a;
    cpu->f |= (GB_FLAG_N | GB_FLAG_H);
}

static inline uint8_t gb_cb_rlc(CpuState* cpu, uint8_t val) {
    uint8_t c = (val >> 7U) & 1U;
    uint8_t res = (uint8_t)((val << 1U) | c);
    cpu->f = (c ? GB_FLAG_C : 0) | (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

static inline uint8_t gb_cb_rrc(CpuState* cpu, uint8_t val) {
    uint8_t c = val & 1U;
    uint8_t res = (uint8_t)((val >> 1U) | (c << 7U));
    cpu->f = (c ? GB_FLAG_C : 0) | (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

static inline uint8_t gb_cb_rl(CpuState* cpu, uint8_t val) {
    uint8_t old_c = (cpu->f & GB_FLAG_C) ? 1U : 0U;
    uint8_t new_c = (val >> 7U) & 1U;
    uint8_t res = (uint8_t)((val << 1U) | old_c);
    cpu->f = (new_c ? GB_FLAG_C : 0) | (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

static inline uint8_t gb_cb_rr(CpuState* cpu, uint8_t val) {
    uint8_t old_c = (cpu->f & GB_FLAG_C) ? 1U : 0U;
    uint8_t new_c = val & 1U;
    uint8_t res = (uint8_t)((val >> 1U) | (old_c << 7U));
    cpu->f = (new_c ? GB_FLAG_C : 0) | (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

static inline uint8_t gb_cb_sla(CpuState* cpu, uint8_t val) {
    uint8_t c = (val >> 7U) & 1U;
    uint8_t res = (uint8_t)(val << 1U);
    cpu->f = (c ? GB_FLAG_C : 0) | (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

static inline uint8_t gb_cb_sra(CpuState* cpu, uint8_t val) {
    uint8_t c = val & 1U;
    uint8_t res = (uint8_t)((val >> 1U) | (val & 0x80U));
    cpu->f = (c ? GB_FLAG_C : 0) | (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

static inline uint8_t gb_cb_swap(CpuState* cpu, uint8_t val) {
    uint8_t res = (uint8_t)(((val & 0x0FU) << 4U) | ((val & 0xF0U) >> 4U));
    cpu->f = (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

static inline uint8_t gb_cb_srl(CpuState* cpu, uint8_t val) {
    uint8_t c = val & 1U;
    uint8_t res = (uint8_t)(val >> 1U);
    cpu->f = (c ? GB_FLAG_C : 0) | (res == 0 ? GB_FLAG_Z : 0);
    return res;
}

static inline void gb_cb_bit(CpuState* cpu, uint8_t bit, uint8_t val) {
    cpu->f = (uint8_t)((cpu->f & GB_FLAG_C) | GB_FLAG_H);
    if ((val & (1U << bit)) == 0)
        cpu->f |= GB_FLAG_Z;
}

#ifdef __cplusplus
}
#endif
