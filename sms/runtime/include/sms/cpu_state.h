#pragma once

/*
 * C99 / C++20 compatible Sega Master System & Game Gear Z80 CPU state and runtime interface.
 * Used by both the SMS/GG runtime and smsrecomp-generated C99 code.
 *
 * Reference: docs/SPEC.md, docs/SMS_SPEC.md §2.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__cplusplus) && !defined(inline)
#define inline __inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Z80 Flag register (F) bit masks: S Z Y H X P/V N C */
#define SMS_FLAG_S  0x80U
#define SMS_FLAG_Z  0x40U
#define SMS_FLAG_Y  0x20U
#define SMS_FLAG_H  0x10U
#define SMS_FLAG_X  0x08U
#define SMS_FLAG_PV 0x04U
#define SMS_FLAG_N  0x02U
#define SMS_FLAG_C  0x01U

typedef struct SmsCpuState {
    uint8_t a, f;
    uint8_t b, c;
    uint8_t d, e;
    uint8_t h, l;

    /* Shadow (alternate) register bank */
    uint8_t a2, f2;
    uint8_t b2, c2;
    uint8_t d2, e2;
    uint8_t h2, l2;

    uint16_t ix, iy;
    uint16_t sp, pc;
    uint8_t i, r;

    /* Interrupt & execution state */
    uint8_t iff1;        /* Maskable interrupt enable */
    uint8_t iff2;        /* Saved IFF1 across NMI */
    uint8_t im;          /* Interrupt mode (0, 1, or 2) */
    uint8_t ei_delay;    /* EI takes effect after the next instruction */
    uint8_t halted;      /* 1 if CPU executed HALT */
    uint8_t nmi_pending; /* Edge-triggered NMI (SMS Pause button) */

    uint64_t cycles;     /* Master Z80 T-cycle counter */
    uint64_t next_event; /* Cycles at which runtime must be polled */

    void* bus;           /* Opaque pointer to dragon::sms::Bus */
    uint8_t* wram;       /* Direct pointer to 8 KiB Work RAM ($C000-$DFFF) */
    const uint8_t* rom;  /* Direct pointer to loaded ROM */
    uint32_t rom_size;   /* Loaded ROM size in bytes */
} SmsCpuState;

typedef void (*SmsRecompFunc)(SmsCpuState* cpu);

typedef struct SmsRecompFuncEntry {
    uint16_t addr;
    uint8_t bank;
    SmsRecompFunc func;
} SmsRecompFuncEntry;

uint8_t sms_bus_read(SmsCpuState* cpu, uint16_t addr);
void sms_bus_write(SmsCpuState* cpu, uint16_t addr, uint8_t value);
uint8_t sms_io_read(SmsCpuState* cpu, uint8_t port);
void sms_io_write(SmsCpuState* cpu, uint8_t port, uint8_t value);
int sms_runtime_poll(SmsCpuState* cpu);
int sms_runtime_halt(SmsCpuState* cpu);
int sms_runtime_dispatch(SmsCpuState* cpu, uint16_t target, uint16_t from_pc);
int sms_runtime_step(SmsCpuState* cpu);

static inline uint16_t sms_bus_read16(SmsCpuState* cpu, uint16_t addr) {
    uint8_t lo = sms_bus_read(cpu, addr);
    uint8_t hi = sms_bus_read(cpu, (uint16_t)(addr + 1U));
    return (uint16_t)(((uint16_t)hi << 8U) | lo);
}

static inline void sms_bus_write16(SmsCpuState* cpu, uint16_t addr, uint16_t value) {
    sms_bus_write(cpu, addr, (uint8_t)(value & 0xFFU));
    sms_bus_write(cpu, (uint16_t)(addr + 1U), (uint8_t)((value >> 8U) & 0xFFU));
}

/* Even parity helper (returns SMS_FLAG_PV if popcount(v) is even) */
static inline uint8_t sms_parity(uint8_t v) {
    v ^= (uint8_t)(v >> 4U);
    v ^= (uint8_t)(v >> 2U);
    v ^= (uint8_t)(v >> 1U);
    return (v & 1U) == 0U ? SMS_FLAG_PV : 0U;
}

static inline uint8_t sms_szxy(uint8_t v) {
    return (uint8_t)((v & (SMS_FLAG_S | SMS_FLAG_Y | SMS_FLAG_X)) |
                     (v == 0U ? SMS_FLAG_Z : 0U));
}

static inline uint8_t sms_szxyp(uint8_t v) {
    return (uint8_t)(sms_szxy(v) | sms_parity(v));
}

/* Register pair helpers */
static inline uint16_t sms_af(const SmsCpuState* cpu) {
    return (uint16_t)(((uint16_t)cpu->a << 8U) | cpu->f);
}
static inline void sms_set_af(SmsCpuState* cpu, uint16_t v) {
    cpu->a = (uint8_t)((v >> 8U) & 0xFFU);
    cpu->f = (uint8_t)(v & 0xFFU);
}
static inline uint16_t sms_bc(const SmsCpuState* cpu) {
    return (uint16_t)(((uint16_t)cpu->b << 8U) | cpu->c);
}
static inline void sms_set_bc(SmsCpuState* cpu, uint16_t v) {
    cpu->b = (uint8_t)((v >> 8U) & 0xFFU);
    cpu->c = (uint8_t)(v & 0xFFU);
}
static inline uint16_t sms_de(const SmsCpuState* cpu) {
    return (uint16_t)(((uint16_t)cpu->d << 8U) | cpu->e);
}
static inline void sms_set_de(SmsCpuState* cpu, uint16_t v) {
    cpu->d = (uint8_t)((v >> 8U) & 0xFFU);
    cpu->e = (uint8_t)(v & 0xFFU);
}
static inline uint16_t sms_hl(const SmsCpuState* cpu) {
    return (uint16_t)(((uint16_t)cpu->h << 8U) | cpu->l);
}
static inline void sms_set_hl(SmsCpuState* cpu, uint16_t v) {
    cpu->h = (uint8_t)((v >> 8U) & 0xFFU);
    cpu->l = (uint8_t)(v & 0xFFU);
}

/* Stack helpers */
static inline void sms_push(SmsCpuState* cpu, uint16_t val) {
    cpu->sp = (uint16_t)(cpu->sp - 1U);
    sms_bus_write(cpu, cpu->sp, (uint8_t)((val >> 8U) & 0xFFU));
    cpu->sp = (uint16_t)(cpu->sp - 1U);
    sms_bus_write(cpu, cpu->sp, (uint8_t)(val & 0xFFU));
}

static inline uint16_t sms_pop(SmsCpuState* cpu) {
    uint8_t lo = sms_bus_read(cpu, cpu->sp);
    cpu->sp = (uint16_t)(cpu->sp + 1U);
    uint8_t hi = sms_bus_read(cpu, cpu->sp);
    cpu->sp = (uint16_t)(cpu->sp + 1U);
    return (uint16_t)(((uint16_t)hi << 8U) | lo);
}

/* 8-bit ALU operations (exact Z80 flag semantics) */
static inline void sms_add8(SmsCpuState* cpu, uint8_t val, uint8_t carry) {
    uint16_t a = cpu->a;
    uint16_t res = (uint16_t)(a + val + carry);
    uint8_t r8 = (uint8_t)(res & 0xFFU);
    uint8_t f = sms_szxy(r8);
    if (((a & 0x0FU) + (val & 0x0FU) + carry) > 0x0FU) f |= SMS_FLAG_H;
    if ((~(a ^ val) & (a ^ res) & 0x80U) != 0U) f |= SMS_FLAG_PV;
    if (res > 0xFFU) f |= SMS_FLAG_C;
    cpu->a = r8;
    cpu->f = f;
}

static inline void sms_sub8(SmsCpuState* cpu, uint8_t val, uint8_t carry) {
    uint16_t a = cpu->a;
    uint16_t res = (uint16_t)(a - val - carry);
    uint8_t r8 = (uint8_t)(res & 0xFFU);
    uint8_t f = (uint8_t)(sms_szxy(r8) | SMS_FLAG_N);
    if ((int)(a & 0x0FU) - (int)(val & 0x0FU) - (int)carry < 0) f |= SMS_FLAG_H;
    if (((a ^ val) & (a ^ res) & 0x80U) != 0U) f |= SMS_FLAG_PV;
    if (res > 0xFFU) f |= SMS_FLAG_C;
    cpu->a = r8;
    cpu->f = f;
}

static inline void sms_cp8(SmsCpuState* cpu, uint8_t val) {
    uint16_t a = cpu->a;
    uint16_t res = (uint16_t)(a - val);
    uint8_t r8 = (uint8_t)(res & 0xFFU);
    /* On Z80 CP, X and Y flags come from the operand `val`, not the result */
    uint8_t f = (uint8_t)((r8 & SMS_FLAG_S) | (r8 == 0U ? SMS_FLAG_Z : 0U) |
                          (val & (SMS_FLAG_Y | SMS_FLAG_X)) | SMS_FLAG_N);
    if ((int)(a & 0x0FU) - (int)(val & 0x0FU) < 0) f |= SMS_FLAG_H;
    if (((a ^ val) & (a ^ res) & 0x80U) != 0U) f |= SMS_FLAG_PV;
    if (res > 0xFFU) f |= SMS_FLAG_C;
    cpu->f = f;
}

static inline void sms_and8(SmsCpuState* cpu, uint8_t val) {
    cpu->a = (uint8_t)(cpu->a & val);
    cpu->f = (uint8_t)(sms_szxyp(cpu->a) | SMS_FLAG_H);
}

static inline void sms_or8(SmsCpuState* cpu, uint8_t val) {
    cpu->a = (uint8_t)(cpu->a | val);
    cpu->f = sms_szxyp(cpu->a);
}

static inline void sms_xor8(SmsCpuState* cpu, uint8_t val) {
    cpu->a = (uint8_t)(cpu->a ^ val);
    cpu->f = sms_szxyp(cpu->a);
}

static inline uint8_t sms_inc8(SmsCpuState* cpu, uint8_t val) {
    uint8_t res = (uint8_t)(val + 1U);
    uint8_t f = (uint8_t)((cpu->f & SMS_FLAG_C) | sms_szxy(res));
    if ((val & 0x0FU) == 0x0FU) f |= SMS_FLAG_H;
    if (val == 0x7FU) f |= SMS_FLAG_PV;
    cpu->f = f;
    return res;
}

static inline uint8_t sms_dec8(SmsCpuState* cpu, uint8_t val) {
    uint8_t res = (uint8_t)(val - 1U);
    uint8_t f = (uint8_t)((cpu->f & SMS_FLAG_C) | sms_szxy(res) | SMS_FLAG_N);
    if ((val & 0x0FU) == 0x00U) f |= SMS_FLAG_H;
    if (val == 0x80U) f |= SMS_FLAG_PV;
    cpu->f = f;
    return res;
}

/* 16-bit ALU helpers */
static inline uint16_t sms_add16(SmsCpuState* cpu, uint16_t lhs, uint16_t rhs) {
    uint32_t res = (uint32_t)lhs + (uint32_t)rhs;
    uint8_t hi = (uint8_t)((res >> 8U) & 0xFFU);
    uint8_t f = (uint8_t)((cpu->f & (SMS_FLAG_S | SMS_FLAG_Z | SMS_FLAG_PV)) |
                          (hi & (SMS_FLAG_Y | SMS_FLAG_X)));
    if (((lhs & 0x0FFFU) + (rhs & 0x0FFFU)) > 0x0FFFU) f |= SMS_FLAG_H;
    if (res > 0xFFFFU) f |= SMS_FLAG_C;
    cpu->f = f;
    return (uint16_t)(res & 0xFFFFU);
}

static inline void sms_adc_hl(SmsCpuState* cpu, uint16_t rhs) {
    uint32_t lhs = sms_hl(cpu);
    uint32_t cy = (cpu->f & SMS_FLAG_C) ? 1U : 0U;
    uint32_t res = lhs + rhs + cy;
    uint16_t r16 = (uint16_t)(res & 0xFFFFU);
    uint8_t hi = (uint8_t)((r16 >> 8U) & 0xFFU);
    uint8_t f = (uint8_t)((hi & (SMS_FLAG_S | SMS_FLAG_Y | SMS_FLAG_X)) |
                          (r16 == 0U ? SMS_FLAG_Z : 0U));
    if (((lhs & 0x0FFFU) + (rhs & 0x0FFFU) + cy) > 0x0FFFU) f |= SMS_FLAG_H;
    if ((~(lhs ^ rhs) & (lhs ^ res) & 0x8000U) != 0U) f |= SMS_FLAG_PV;
    if (res > 0xFFFFU) f |= SMS_FLAG_C;
    sms_set_hl(cpu, r16);
    cpu->f = f;
}

static inline void sms_sbc_hl(SmsCpuState* cpu, uint16_t rhs) {
    uint32_t lhs = sms_hl(cpu);
    uint32_t cy = (cpu->f & SMS_FLAG_C) ? 1U : 0U;
    uint32_t res = lhs - rhs - cy;
    uint16_t r16 = (uint16_t)(res & 0xFFFFU);
    uint8_t hi = (uint8_t)((r16 >> 8U) & 0xFFU);
    uint8_t f = (uint8_t)((hi & (SMS_FLAG_S | SMS_FLAG_Y | SMS_FLAG_X)) |
                          (r16 == 0U ? SMS_FLAG_Z : 0U) | SMS_FLAG_N);
    if ((int)(lhs & 0x0FFFU) - (int)(rhs & 0x0FFFU) - (int)cy < 0) f |= SMS_FLAG_H;
    if (((lhs ^ rhs) & (lhs ^ res) & 0x8000U) != 0U) f |= SMS_FLAG_PV;
    if (res > 0xFFFFU) f |= SMS_FLAG_C;
    sms_set_hl(cpu, r16);
    cpu->f = f;
}

/* Z80 DAA (exact documented & Sean Young algorithm) */
static inline void sms_daa(SmsCpuState* cpu) {
    uint8_t a = cpu->a;
    uint8_t corr = 0U;
    uint8_t carry = (cpu->f & SMS_FLAG_C) ? 1U : 0U;
    uint8_t half = (cpu->f & SMS_FLAG_H) ? 1U : 0U;
    uint8_t sub = (cpu->f & SMS_FLAG_N) ? 1U : 0U;

    if (half || (a & 0x0FU) > 9U) corr |= 0x06U;
    if (carry || a > 0x99U) {
        corr |= 0x60U;
        carry = 1U;
    }

    uint8_t res = sub ? (uint8_t)(a - corr) : (uint8_t)(a + corr);
    uint8_t f = (uint8_t)(sms_szxyp(res) | (sub ? SMS_FLAG_N : 0U) | (carry ? SMS_FLAG_C : 0U));
    if (((a ^ res) & 0x10U) != 0U) f |= SMS_FLAG_H;
    cpu->a = res;
    cpu->f = f;
}

/* Rotate / shift helpers (CB-prefixed variants update S/Z/P/V/C) */
static inline uint8_t sms_rlc(SmsCpuState* cpu, uint8_t v) {
    uint8_t cy = (uint8_t)(v >> 7U);
    uint8_t r = (uint8_t)((v << 1U) | cy);
    cpu->f = (uint8_t)(sms_szxyp(r) | cy);
    return r;
}

static inline uint8_t sms_rrc(SmsCpuState* cpu, uint8_t v) {
    uint8_t cy = (uint8_t)(v & 1U);
    uint8_t r = (uint8_t)((v >> 1U) | (cy << 7U));
    cpu->f = (uint8_t)(sms_szxyp(r) | cy);
    return r;
}

static inline uint8_t sms_rl(SmsCpuState* cpu, uint8_t v) {
    uint8_t old_c = (cpu->f & SMS_FLAG_C) ? 1U : 0U;
    uint8_t cy = (uint8_t)(v >> 7U);
    uint8_t r = (uint8_t)((v << 1U) | old_c);
    cpu->f = (uint8_t)(sms_szxyp(r) | cy);
    return r;
}

static inline uint8_t sms_rr(SmsCpuState* cpu, uint8_t v) {
    uint8_t old_c = (cpu->f & SMS_FLAG_C) ? 1U : 0U;
    uint8_t cy = (uint8_t)(v & 1U);
    uint8_t r = (uint8_t)((v >> 1U) | (old_c << 7U));
    cpu->f = (uint8_t)(sms_szxyp(r) | cy);
    return r;
}

static inline uint8_t sms_sla(SmsCpuState* cpu, uint8_t v) {
    uint8_t cy = (uint8_t)(v >> 7U);
    uint8_t r = (uint8_t)(v << 1U);
    cpu->f = (uint8_t)(sms_szxyp(r) | cy);
    return r;
}

static inline uint8_t sms_sra(SmsCpuState* cpu, uint8_t v) {
    uint8_t cy = (uint8_t)(v & 1U);
    uint8_t r = (uint8_t)((v >> 1U) | (v & 0x80U));
    cpu->f = (uint8_t)(sms_szxyp(r) | cy);
    return r;
}

/* Undocumented Z80 SLL (Shift Left Logical, sets bit 0 to 1) */
static inline uint8_t sms_sll(SmsCpuState* cpu, uint8_t v) {
    uint8_t cy = (uint8_t)(v >> 7U);
    uint8_t r = (uint8_t)((v << 1U) | 1U);
    cpu->f = (uint8_t)(sms_szxyp(r) | cy);
    return r;
}

static inline uint8_t sms_srl(SmsCpuState* cpu, uint8_t v) {
    uint8_t cy = (uint8_t)(v & 1U);
    uint8_t r = (uint8_t)(v >> 1U);
    cpu->f = (uint8_t)(sms_szxyp(r) | cy);
    return r;
}

static inline void sms_bit(SmsCpuState* cpu, uint8_t bit, uint8_t v) {
    uint8_t masked = (uint8_t)(v & (uint8_t)(1U << bit));
    uint8_t f = (uint8_t)((cpu->f & SMS_FLAG_C) | SMS_FLAG_H | (v & (SMS_FLAG_Y | SMS_FLAG_X)));
    if (masked == 0U) {
        f |= (uint8_t)(SMS_FLAG_Z | SMS_FLAG_PV);
    } else if (bit == 7U) {
        f |= SMS_FLAG_S;
    }
    cpu->f = f;
}

static inline void sms_ex_af(SmsCpuState* cpu) {
    uint8_t ta = cpu->a, tf = cpu->f;
    cpu->a = cpu->a2; cpu->f = cpu->f2;
    cpu->a2 = ta;     cpu->f2 = tf;
}

static inline void sms_exx(SmsCpuState* cpu) {
    uint8_t t;
    t = cpu->b; cpu->b = cpu->b2; cpu->b2 = t;
    t = cpu->c; cpu->c = cpu->c2; cpu->c2 = t;
    t = cpu->d; cpu->d = cpu->d2; cpu->d2 = t;
    t = cpu->e; cpu->e = cpu->e2; cpu->e2 = t;
    t = cpu->h; cpu->h = cpu->h2; cpu->h2 = t;
    t = cpu->l; cpu->l = cpu->l2; cpu->l2 = t;
}

static inline void sms_ex_de_hl(SmsCpuState* cpu) {
    uint8_t td = cpu->d, te = cpu->e;
    cpu->d = cpu->h; cpu->e = cpu->l;
    cpu->h = td;     cpu->l = te;
}

#ifdef __cplusplus
}  // extern "C"
#endif
