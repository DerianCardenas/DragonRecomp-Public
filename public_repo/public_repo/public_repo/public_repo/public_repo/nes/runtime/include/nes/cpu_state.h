#pragma once

/*
 * C99 / C++20 compatible NES CPU state and runtime interface used by both
 * the NES runtime and nesrecomp-generated C99 code.
 *
 * Reference: docs/SPEC.md §4.2-4.6, docs/NES_SPEC.md §1.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__cplusplus) && !defined(inline)
#define inline __inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CpuState {
    uint8_t a, x, y, s; /* registers; stack is page $01 */
    uint16_t pc;        /* program counter */
    /* flags kept unpacked for speed: c, v, i, d are 0 or 1 */
    uint8_t c, v, i, d;
    /* lazy N/Z:
       N = (n & 0x80) != 0, Z = (z == 0).
       Normally n = z = result byte; BIT/PLP/RTI set n and z independently. */
    uint8_t n, z;
    uint64_t cycles;        /* master CPU cycle counter */
    uint64_t next_event;    /* cycles at which runtime must be polled */
    void* bus;              /* opaque runtime Bus pointer */
    uint8_t* ram;           /* direct pointer to 2 KiB internal RAM ($0000-$07FF) */
    const uint8_t* prg_rom; /* direct pointer to PRG ROM */
    uint32_t prg_rom_size;  /* PRG ROM size in bytes */
    uint8_t halted;         /* non-zero if CPU executed KIL/JAM or test halt */
} CpuState;

typedef void (*RecompFunc)(CpuState* cpu);

typedef struct RecompFuncEntry {
    uint16_t addr;
    uint8_t bank;
    RecompFunc func;
} RecompFuncEntry;

uint8_t bus_read(CpuState* cpu, uint16_t addr);
void bus_write(CpuState* cpu, uint16_t addr, uint8_t value);
void runtime_poll(CpuState* cpu);
void runtime_dispatch(CpuState* cpu, uint16_t target, uint16_t from_pc);

static inline uint16_t bus_read16(CpuState* cpu, uint16_t addr) {
    uint8_t lo = bus_read(cpu, addr);
    uint8_t hi = bus_read(cpu, (uint16_t)(addr + 1u));
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

/* 6502 page-wrapping 16-bit read used by JMP ($xxFF), ($nn,X) and ($nn),Y */
static inline uint16_t bus_read16_wrap(CpuState* cpu, uint16_t addr) {
    uint8_t lo = bus_read(cpu, addr);
    uint16_t hi_addr = (uint16_t)((addr & 0xFF00u) | ((addr + 1u) & 0x00FFu));
    uint8_t hi = bus_read(cpu, hi_addr);
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static inline uint16_t bus_read16_zp(CpuState* cpu, uint8_t zp_addr) {
    uint8_t lo = bus_read(cpu, zp_addr);
    uint8_t hi = bus_read(cpu, (uint8_t)(zp_addr + 1u));
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static inline void cpu_set_nz(CpuState* cpu, uint8_t val) {
    cpu->n = val;
    cpu->z = val;
}

static inline uint8_t cpu_pack_flags(const CpuState* cpu, uint8_t b_flag) {
    uint8_t p = 0x20u; /* bit 5 is always 1 */
    if (cpu->c) {
        p |= 0x01u;
    }
    if (cpu->z == 0u) {
        p |= 0x02u;
    }
    if (cpu->i) {
        p |= 0x04u;
    }
    if (cpu->d) {
        p |= 0x08u;
    }
    if (b_flag) {
        p |= 0x10u;
    }
    if (cpu->v) {
        p |= 0x40u;
    }
    if (cpu->n & 0x80u) {
        p |= 0x80u;
    }
    return p;
}

static inline void cpu_unpack_flags(CpuState* cpu, uint8_t p) {
    cpu->c = (p & 0x01u) ? 1u : 0u;
    cpu->z = (p & 0x02u) ? 0u : 1u;
    cpu->i = (p & 0x04u) ? 1u : 0u;
    cpu->d = (p & 0x08u) ? 1u : 0u;
    cpu->v = (p & 0x40u) ? 1u : 0u;
    cpu->n = (p & 0x80u) ? 0x80u : 0x00u;
}

static inline void cpu_push8(CpuState* cpu, uint8_t val) {
    bus_write(cpu, (uint16_t)(0x0100u | cpu->s), val);
    cpu->s = (uint8_t)(cpu->s - 1u);
}

static inline uint8_t cpu_pop8(CpuState* cpu) {
    cpu->s = (uint8_t)(cpu->s + 1u);
    return bus_read(cpu, (uint16_t)(0x0100u | cpu->s));
}

static inline void cpu_push16(CpuState* cpu, uint16_t val) {
    cpu_push8(cpu, (uint8_t)(val >> 8));
    cpu_push8(cpu, (uint8_t)(val & 0xFFu));
}

static inline uint16_t cpu_pop16(CpuState* cpu) {
    uint8_t lo = cpu_pop8(cpu);
    uint8_t hi = cpu_pop8(cpu);
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static inline uint16_t cpu_peek16_stack(CpuState* cpu, uint8_t s) {
    uint8_t lo = bus_read(cpu, (uint16_t)(0x0100u | (uint8_t)(s + 1u)));
    uint8_t hi = bus_read(cpu, (uint16_t)(0x0100u | (uint8_t)(s + 2u)));
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

/* Ricoh 2A03 binary ADC (decimal flag ignored for arithmetic on NES) */
static inline void cpu_adc(CpuState* cpu, uint8_t m) {
    uint16_t a = cpu->a;
    uint16_t t = (uint16_t)(a + m + (cpu->c ? 1u : 0u));
    cpu->v = (uint8_t)((~(a ^ m) & (a ^ t) & 0x80u) != 0u);
    cpu->c = (uint8_t)(t > 0xFFu);
    cpu->a = (uint8_t)t;
    cpu_set_nz(cpu, cpu->a);
}

static inline void cpu_sbc(CpuState* cpu, uint8_t m) {
    cpu_adc(cpu, (uint8_t)(m ^ 0xFFu));
}

static inline void cpu_cmp(CpuState* cpu, uint8_t reg, uint8_t m) {
    cpu->c = (uint8_t)(reg >= m);
    cpu_set_nz(cpu, (uint8_t)(reg - m));
}

static inline void cpu_bit(CpuState* cpu, uint8_t m) {
    cpu->n = m;
    cpu->v = (uint8_t)((m >> 6) & 1u);
    cpu->z = (uint8_t)(cpu->a & m);
}

static inline uint8_t cpu_asl(CpuState* cpu, uint8_t val) {
    cpu->c = (uint8_t)((val >> 7) & 1u);
    uint8_t res = (uint8_t)(val << 1);
    cpu_set_nz(cpu, res);
    return res;
}

static inline uint8_t cpu_lsr(CpuState* cpu, uint8_t val) {
    cpu->c = (uint8_t)(val & 1u);
    uint8_t res = (uint8_t)(val >> 1);
    cpu_set_nz(cpu, res);
    return res;
}

static inline uint8_t cpu_rol(CpuState* cpu, uint8_t val) {
    uint8_t old_c = (uint8_t)(cpu->c ? 1u : 0u);
    cpu->c = (uint8_t)((val >> 7) & 1u);
    uint8_t res = (uint8_t)((val << 1) | old_c);
    cpu_set_nz(cpu, res);
    return res;
}

static inline uint8_t cpu_ror(CpuState* cpu, uint8_t val) {
    uint8_t old_c = (uint8_t)(cpu->c ? 0x80u : 0u);
    cpu->c = (uint8_t)(val & 1u);
    uint8_t res = (uint8_t)((val >> 1) | old_c);
    cpu_set_nz(cpu, res);
    return res;
}

#ifdef __cplusplus
}  // extern "C"
#endif
