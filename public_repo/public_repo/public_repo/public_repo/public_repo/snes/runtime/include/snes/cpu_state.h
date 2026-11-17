#pragma once

/*
 * C99 / C++20 compatible SNES 65816 (Ricoh 5A22) CPU state and runtime interface
 * used by both the SNES runtime and snesrecomp-generated C99 code.
 *
 * Reference: docs/SPEC.md §4.2-4.6, docs/SNES_SPEC.md §1.
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
    uint16_t a, x, y, s, d;            /* C accumulator (B:A), index, stack, direct page */
    uint8_t db, pb;                    /* data bank, program bank */
    uint16_t pc;                       /* program counter within PB */
    uint8_t c, v, i, d_flag, m, xf, e; /* unpacked flags; e = emulation mode */
    uint16_t n, z;                     /* unpacked/lazy N/Z: N = (n != 0), Z = (z == 0) */
    uint64_t master_cycles;            /* master clock counter (~21.477 MHz) */
    uint64_t next_event;               /* master_cycles at which runtime must be polled */
    void* bus;                         /* opaque runtime Bus pointer */
    uint8_t halted;                    /* non-zero if STP or test halt */
    uint8_t waiting;                   /* non-zero if WAI waiting for interrupt */
} CpuState;

typedef void (*RecompFunc)(CpuState* cpu);

typedef struct RecompFuncEntry {
    uint16_t addr;
    uint8_t bank;
    uint8_t mode_flags; /* bit 2: e, bit 1: m, bit 0: xf */
    RecompFunc func;
} RecompFuncEntry;

uint8_t bus_read(CpuState* cpu, uint32_t addr);
void bus_write(CpuState* cpu, uint32_t addr, uint8_t value);
void runtime_poll(CpuState* cpu);
void runtime_dispatch(CpuState* cpu, uint8_t bank, uint16_t target, uint32_t from_addr);
void runtime_wai(CpuState* cpu);

static inline uint8_t cpu_mode_flags(const CpuState* cpu) {
    if (cpu->e) {
        return 0x07u; /* e=1 forces m=1, xf=1 */
    }
    return (uint8_t)(((cpu->m ? 1u : 0u) << 1) | (cpu->xf ? 1u : 0u));
}

static inline uint32_t snes_addr24(uint8_t bank, uint16_t offset) {
    return ((uint32_t)bank << 16) | (uint32_t)offset;
}

static inline uint16_t bus_read16(CpuState* cpu, uint32_t addr) {
    uint8_t lo = bus_read(cpu, addr & 0xFFFFFFu);
    uint8_t hi = bus_read(cpu, (addr + 1u) & 0xFFFFFFu);
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static inline void bus_write16(CpuState* cpu, uint32_t addr, uint16_t value) {
    bus_write(cpu, addr & 0xFFFFFFu, (uint8_t)(value & 0xFFu));
    bus_write(cpu, (addr + 1u) & 0xFFFFFFu, (uint8_t)(value >> 8));
}

static inline uint16_t bus_read16_bank0(CpuState* cpu, uint16_t addr) {
    uint8_t lo = bus_read(cpu, (uint32_t)addr);
    uint8_t hi = bus_read(cpu, (uint32_t)(uint16_t)(addr + 1u));
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static inline void bus_write16_bank0(CpuState* cpu, uint16_t addr, uint16_t value) {
    bus_write(cpu, (uint32_t)addr, (uint8_t)(value & 0xFFu));
    bus_write(cpu, (uint32_t)(uint16_t)(addr + 1u), (uint8_t)(value >> 8));
}

static inline uint16_t bus_read16_bank_wrap(CpuState* cpu, uint8_t bank, uint16_t addr) {
    uint32_t base = (uint32_t)bank << 16;
    uint8_t lo = bus_read(cpu, base | addr);
    uint8_t hi = bus_read(cpu, base | (uint16_t)(addr + 1u));
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static inline uint32_t bus_read24_bank0(CpuState* cpu, uint16_t addr) {
    uint8_t b0 = bus_read(cpu, (uint32_t)addr);
    uint8_t b1 = bus_read(cpu, (uint32_t)(uint16_t)(addr + 1u));
    uint8_t b2 = bus_read(cpu, (uint32_t)(uint16_t)(addr + 2u));
    return ((uint32_t)b2 << 16) | ((uint32_t)b1 << 8) | (uint32_t)b0;
}

static inline void cpu_set_nz8(CpuState* cpu, uint8_t val) {
    cpu->n = (val & 0x80u) ? 0x8080u : 0x0000u;
    cpu->z = val;
}

static inline void cpu_set_nz16(CpuState* cpu, uint16_t val) {
    cpu->n = (val & 0x8000u) ? 0x8080u : 0x0000u;
    cpu->z = val;
}

static inline uint8_t cpu_is_negative(const CpuState* cpu) {
    return (cpu->n != 0u) ? 1u : 0u;
}

static inline uint8_t cpu_is_zero(const CpuState* cpu) {
    return (cpu->z == 0u) ? 1u : 0u;
}

static inline uint8_t cpu_pack_flags(const CpuState* cpu, uint8_t b_flag) {
    uint8_t p = 0u;
    if (cpu->c) {
        p |= 0x01u;
    }
    if (cpu->z == 0u) {
        p |= 0x02u;
    }
    if (cpu->i) {
        p |= 0x04u;
    }
    if (cpu->d_flag) {
        p |= 0x08u;
    }
    if (cpu->e) {
        p |= 0x20u;
        if (b_flag) {
            p |= 0x10u;
        }
    } else {
        if (cpu->xf) {
            p |= 0x10u;
        }
        if (cpu->m) {
            p |= 0x20u;
        }
    }
    if (cpu->v) {
        p |= 0x40u;
    }
    if (cpu->n != 0u) {
        p |= 0x80u;
    }
    return p;
}

static inline void cpu_unpack_flags(CpuState* cpu, uint8_t p) {
    cpu->c = (p & 0x01u) ? 1u : 0u;
    cpu->z = (p & 0x02u) ? 0u : 1u;
    cpu->i = (p & 0x04u) ? 1u : 0u;
    cpu->d_flag = (p & 0x08u) ? 1u : 0u;
    cpu->v = (p & 0x40u) ? 1u : 0u;
    cpu->n = (p & 0x80u) ? 0x8080u : 0x0000u;
    if (cpu->e) {
        cpu->m = 1u;
        cpu->xf = 1u;
        cpu->x &= 0x00FFu;
        cpu->y &= 0x00FFu;
        cpu->s = (uint16_t)(0x0100u | (cpu->s & 0x00FFu));
    } else {
        cpu->xf = (p & 0x10u) ? 1u : 0u;
        cpu->m = (p & 0x20u) ? 1u : 0u;
        if (cpu->xf) {
            cpu->x &= 0x00FFu;
            cpu->y &= 0x00FFu;
        }
    }
}

static inline void cpu_sep(CpuState* cpu, uint8_t mask) {
    uint8_t p = cpu_pack_flags(cpu, 1u);
    p |= mask;
    cpu_unpack_flags(cpu, p);
}

static inline void cpu_rep(CpuState* cpu, uint8_t mask) {
    uint8_t p = cpu_pack_flags(cpu, 1u);
    p = (uint8_t)(p & (uint8_t)~mask);
    cpu_unpack_flags(cpu, p);
}

static inline void cpu_xce(CpuState* cpu) {
    uint8_t old_c = cpu->c ? 1u : 0u;
    cpu->c = cpu->e ? 1u : 0u;
    cpu->e = old_c;
    if (cpu->e) {
        cpu->m = 1u;
        cpu->xf = 1u;
        cpu->x &= 0x00FFu;
        cpu->y &= 0x00FFu;
        cpu->s = (uint16_t)(0x0100u | (cpu->s & 0x00FFu));
    }
}

static inline void cpu_push8(CpuState* cpu, uint8_t val) {
    if (cpu->e) {
        uint16_t addr = (uint16_t)(0x0100u | (cpu->s & 0x00FFu));
        bus_write(cpu, addr, val);
        cpu->s = (uint16_t)(0x0100u | ((cpu->s - 1u) & 0x00FFu));
    } else {
        bus_write(cpu, cpu->s, val);
        cpu->s = (uint16_t)(cpu->s - 1u);
    }
}

static inline uint8_t cpu_pop8(CpuState* cpu) {
    if (cpu->e) {
        cpu->s = (uint16_t)(0x0100u | ((cpu->s + 1u) & 0x00FFu));
        return bus_read(cpu, cpu->s);
    } else {
        cpu->s = (uint16_t)(cpu->s + 1u);
        return bus_read(cpu, cpu->s);
    }
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

/* 8-bit binary + BCD ADC */
static inline void cpu_adc8(CpuState* cpu, uint8_t val) {
    uint8_t a8 = (uint8_t)(cpu->a & 0xFFu);
    uint8_t cin = cpu->c ? 1u : 0u;
    if (!cpu->d_flag) {
        uint16_t sum = (uint16_t)a8 + (uint16_t)val + (uint16_t)cin;
        cpu->v = (uint8_t)((~(a8 ^ val) & (a8 ^ sum) & 0x80u) != 0u);
        cpu->c = (uint8_t)(sum > 0xFFu);
        uint8_t res = (uint8_t)(sum & 0xFFu);
        cpu->a = (uint16_t)((cpu->a & 0xFF00u) | res);
        cpu_set_nz8(cpu, res);
    } else {
        uint16_t res = (uint16_t)(a8 & 0x0Fu) + (uint16_t)(val & 0x0Fu) + (uint16_t)cin;
        if (res > 0x09u) {
            res = (uint16_t)(res + 0x06u);
        }
        uint16_t c_lo = (res > 0x0Fu) ? 0x10u : 0x00u;
        res = (uint16_t)(a8 & 0xF0u) + (uint16_t)(val & 0xF0u) + c_lo + (res & 0x0Fu);
        cpu->v = (uint8_t)((~(a8 ^ val) & (a8 ^ res) & 0x80u) != 0u);
        if (res > 0x9Fu) {
            res = (uint16_t)(res + 0x60u);
        }
        cpu->c = (uint8_t)(res > 0xFFu);
        uint8_t out8 = (uint8_t)(res & 0xFFu);
        cpu->a = (uint16_t)((cpu->a & 0xFF00u) | out8);
        cpu_set_nz8(cpu, out8);
    }
}

/* 16-bit binary + BCD ADC */
static inline void cpu_adc16(CpuState* cpu, uint16_t val) {
    uint16_t a16 = cpu->a;
    uint32_t cin = cpu->c ? 1u : 0u;
    if (!cpu->d_flag) {
        uint32_t sum = (uint32_t)a16 + (uint32_t)val + cin;
        cpu->v = (uint8_t)((~(a16 ^ val) & (a16 ^ sum) & 0x8000u) != 0u);
        cpu->c = (uint8_t)(sum > 0xFFFFu);
        cpu->a = (uint16_t)(sum & 0xFFFFu);
        cpu_set_nz16(cpu, cpu->a);
    } else {
        uint32_t n0 = (uint32_t)(a16 & 0x000Fu) + (uint32_t)(val & 0x000Fu) + cin;
        if (n0 > 0x0009u) {
            n0 += 0x0006u;
        }
        uint32_t n1 = (uint32_t)(a16 & 0x00F0u) + (uint32_t)(val & 0x00F0u) +
                      (n0 > 0x000Fu ? 0x0010u : 0u) + (n0 & 0x000Fu);
        if (n1 > 0x009Fu) {
            n1 += 0x0060u;
        }
        uint32_t n2 = (uint32_t)(a16 & 0x0F00u) + (uint32_t)(val & 0x0F00u) +
                      (n1 > 0x00FFu ? 0x0100u : 0u) + (n1 & 0x00FFu);
        if (n2 > 0x09FFu) {
            n2 += 0x0600u;
        }
        uint32_t n3 = (uint32_t)(a16 & 0xF000u) + (uint32_t)(val & 0xF000u) +
                      (n2 > 0x0FFFu ? 0x1000u : 0u) + (n2 & 0x0FFFu);
        cpu->v = (uint8_t)((~(a16 ^ val) & (a16 ^ n3) & 0x8000u) != 0u);
        if (n3 > 0x9FFFu) {
            n3 += 0x6000u;
        }
        cpu->c = (uint8_t)(n3 > 0xFFFFu);
        cpu->a = (uint16_t)(n3 & 0xFFFFu);
        cpu_set_nz16(cpu, cpu->a);
    }
}

/* 8-bit binary + BCD SBC */
static inline void cpu_sbc8(CpuState* cpu, uint8_t val) {
    uint8_t inv = (uint8_t)(val ^ 0xFFu);
    if (!cpu->d_flag) {
        cpu_adc8(cpu, inv);
    } else {
        uint8_t a8 = (uint8_t)(cpu->a & 0xFFu);
        int32_t cin = cpu->c ? 1 : 0;
        uint16_t bin_diff = (uint16_t)a8 + (uint16_t)inv + (uint16_t)cin;
        cpu->v = (uint8_t)((~(a8 ^ inv) & (a8 ^ bin_diff) & 0x80u) != 0u);
        int32_t n0 = (int32_t)(a8 & 0x0Fu) + (int32_t)(inv & 0x0Fu) + cin;
        if (n0 <= 0x0F) {
            n0 -= 0x06;
        }
        int32_t n1 =
            (int32_t)(a8 & 0xF0u) + (int32_t)(inv & 0xF0u) + (n0 > 0x0F ? 0x10 : 0) + (n0 & 0x0F);
        if (n1 <= 0xFF) {
            n1 -= 0x60;
        }
        cpu->c = (uint8_t)(bin_diff > 0xFFu);
        uint8_t out8 = (uint8_t)(n1 & 0xFF);
        cpu->a = (uint16_t)((cpu->a & 0xFF00u) | out8);
        cpu_set_nz8(cpu, out8);
    }
}

/* 16-bit binary + BCD SBC */
static inline void cpu_sbc16(CpuState* cpu, uint16_t val) {
    uint16_t inv = (uint16_t)(val ^ 0xFFFFu);
    if (!cpu->d_flag) {
        cpu_adc16(cpu, inv);
    } else {
        uint16_t a16 = cpu->a;
        int32_t cin = cpu->c ? 1 : 0;
        uint32_t bin_diff = (uint32_t)a16 + (uint32_t)inv + (uint32_t)cin;
        cpu->v = (uint8_t)((~(a16 ^ inv) & (a16 ^ bin_diff) & 0x8000u) != 0u);
        int32_t n0 = (int32_t)(a16 & 0x000Fu) + (int32_t)(inv & 0x000Fu) + cin;
        if (n0 <= 0x000F) {
            n0 -= 0x0006;
        }
        int32_t n1 = (int32_t)(a16 & 0x00F0u) + (int32_t)(inv & 0x00F0u) +
                     (n0 > 0x000F ? 0x0010 : 0) + (n0 & 0x000F);
        if (n1 <= 0x00FF) {
            n1 -= 0x0060;
        }
        int32_t n2 = (int32_t)(a16 & 0x0F00u) + (int32_t)(inv & 0x0F00u) +
                     (n1 > 0x00FF ? 0x0100 : 0) + (n1 & 0x00FF);
        if (n2 <= 0x0FFF) {
            n2 -= 0x0600;
        }
        int32_t n3 = (int32_t)(a16 & 0xF000u) + (int32_t)(inv & 0xF000u) +
                     (n2 > 0x0FFF ? 0x1000 : 0) + (n2 & 0x0FFF);
        if (n3 <= 0xFFFF) {
            n3 -= 0x6000;
        }
        cpu->c = (uint8_t)(bin_diff > 0xFFFFu);
        cpu->a = (uint16_t)(n3 & 0xFFFF);
        cpu_set_nz16(cpu, cpu->a);
    }
}

static inline void cpu_cmp8(CpuState* cpu, uint8_t reg, uint8_t val) {
    cpu->c = (uint8_t)(reg >= val);
    cpu_set_nz8(cpu, (uint8_t)(reg - val));
}

static inline void cpu_cmp16(CpuState* cpu, uint16_t reg, uint16_t val) {
    cpu->c = (uint8_t)(reg >= val);
    cpu_set_nz16(cpu, (uint16_t)(reg - val));
}

static inline void cpu_bit8(CpuState* cpu, uint8_t val) {
    cpu->n = (val & 0x80u) ? 0x8080u : 0x0000u;
    cpu->v = (uint8_t)((val >> 6) & 1u);
    cpu->z = (uint16_t)((cpu->a & 0xFFu) & val);
}

static inline void cpu_bit16(CpuState* cpu, uint16_t val) {
    cpu->n = (val & 0x8000u) ? 0x8080u : 0x0000u;
    cpu->v = (uint8_t)((val >> 14) & 1u);
    cpu->z = (uint16_t)(cpu->a & val);
}

static inline uint8_t cpu_asl8(CpuState* cpu, uint8_t val) {
    cpu->c = (uint8_t)((val >> 7) & 1u);
    uint8_t res = (uint8_t)(val << 1);
    cpu_set_nz8(cpu, res);
    return res;
}

static inline uint16_t cpu_asl16(CpuState* cpu, uint16_t val) {
    cpu->c = (uint8_t)((val >> 15) & 1u);
    uint16_t res = (uint16_t)(val << 1);
    cpu_set_nz16(cpu, res);
    return res;
}

static inline uint8_t cpu_lsr8(CpuState* cpu, uint8_t val) {
    cpu->c = (uint8_t)(val & 1u);
    uint8_t res = (uint8_t)(val >> 1);
    cpu_set_nz8(cpu, res);
    return res;
}

static inline uint16_t cpu_lsr16(CpuState* cpu, uint16_t val) {
    cpu->c = (uint8_t)(val & 1u);
    uint16_t res = (uint16_t)(val >> 1);
    cpu_set_nz16(cpu, res);
    return res;
}

static inline uint8_t cpu_rol8(CpuState* cpu, uint8_t val) {
    uint8_t old_c = (uint8_t)(cpu->c ? 1u : 0u);
    cpu->c = (uint8_t)((val >> 7) & 1u);
    uint8_t res = (uint8_t)((val << 1) | old_c);
    cpu_set_nz8(cpu, res);
    return res;
}

static inline uint16_t cpu_rol16(CpuState* cpu, uint16_t val) {
    uint16_t old_c = (uint16_t)(cpu->c ? 1u : 0u);
    cpu->c = (uint8_t)((val >> 15) & 1u);
    uint16_t res = (uint16_t)((val << 1) | old_c);
    cpu_set_nz16(cpu, res);
    return res;
}

static inline uint8_t cpu_ror8(CpuState* cpu, uint8_t val) {
    uint8_t old_c = (uint8_t)(cpu->c ? 0x80u : 0u);
    cpu->c = (uint8_t)(val & 1u);
    uint8_t res = (uint8_t)((val >> 1) | old_c);
    cpu_set_nz8(cpu, res);
    return res;
}

static inline uint16_t cpu_ror16(CpuState* cpu, uint16_t val) {
    uint16_t old_c = (uint16_t)(cpu->c ? 0x8000u : 0u);
    cpu->c = (uint8_t)(val & 1u);
    uint16_t res = (uint16_t)((val >> 1) | old_c);
    cpu_set_nz16(cpu, res);
    return res;
}

static inline uint8_t cpu_tsb8(CpuState* cpu, uint8_t val) {
    uint8_t a8 = (uint8_t)(cpu->a & 0xFFu);
    cpu->z = (uint16_t)(a8 & val);
    return (uint8_t)(val | a8);
}

static inline uint16_t cpu_tsb16(CpuState* cpu, uint16_t val) {
    cpu->z = (uint16_t)(cpu->a & val);
    return (uint16_t)(val | cpu->a);
}

static inline uint8_t cpu_trb8(CpuState* cpu, uint8_t val) {
    uint8_t a8 = (uint8_t)(cpu->a & 0xFFu);
    cpu->z = (uint16_t)(a8 & val);
    return (uint8_t)(val & (uint8_t)~a8);
}

static inline uint16_t cpu_trb16(CpuState* cpu, uint16_t val) {
    cpu->z = (uint16_t)(cpu->a & val);
    return (uint16_t)(val & (uint16_t)~cpu->a);
}

static inline void cpu_mvn(CpuState* cpu, uint8_t dst_bank, uint8_t src_bank) {
    cpu->db = dst_bank;
    uint32_t count = (uint32_t)cpu->a + 1u;
    for (uint32_t i = 0u; i < count; ++i) {
        uint8_t val = bus_read(cpu, snes_addr24(src_bank, cpu->x));
        bus_write(cpu, snes_addr24(dst_bank, cpu->y), val);
        if (cpu->e || cpu->xf) {
            cpu->x = (uint16_t)((cpu->x + 1u) & 0x00FFu);
            cpu->y = (uint16_t)((cpu->y + 1u) & 0x00FFu);
        } else {
            cpu->x = (uint16_t)(cpu->x + 1u);
            cpu->y = (uint16_t)(cpu->y + 1u);
        }
        cpu->a = (uint16_t)(cpu->a - 1u);
        cpu->master_cycles += 42u;
    }
}

static inline void cpu_mvp(CpuState* cpu, uint8_t dst_bank, uint8_t src_bank) {
    cpu->db = dst_bank;
    uint32_t count = (uint32_t)cpu->a + 1u;
    for (uint32_t i = 0u; i < count; ++i) {
        uint8_t val = bus_read(cpu, snes_addr24(src_bank, cpu->x));
        bus_write(cpu, snes_addr24(dst_bank, cpu->y), val);
        if (cpu->e || cpu->xf) {
            cpu->x = (uint16_t)((cpu->x - 1u) & 0x00FFu);
            cpu->y = (uint16_t)((cpu->y - 1u) & 0x00FFu);
        } else {
            cpu->x = (uint16_t)(cpu->x - 1u);
            cpu->y = (uint16_t)(cpu->y - 1u);
        }
        cpu->a = (uint16_t)(cpu->a - 1u);
        cpu->master_cycles += 42u;
    }
}

#ifdef __cplusplus
}  // extern "C"
#endif
