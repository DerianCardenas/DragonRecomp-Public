#include "md/interpreter.h"

#include <utility>

namespace dragon::md {

namespace {

// Size in bytes: 1 (B), 2 (W), 4 (L)
inline uint32_t size_mask(int sz) noexcept {
    return (sz == 1) ? 0xFFU : (sz == 2) ? 0xFFFFU : 0xFFFFFFFFU;
}
inline uint32_t size_msb(int sz) noexcept {
    return (sz == 1) ? 0x80U : (sz == 2) ? 0x8000U : 0x80000000U;
}
inline int32_t sign_ext(uint32_t v, int sz) noexcept {
    if (sz == 1)
        return static_cast<int8_t>(v & 0xFFU);
    if (sz == 2)
        return static_cast<int16_t>(v & 0xFFFFU);
    return static_cast<int32_t>(v);
}

inline uint16_t fetch16(MdCpuState& cpu, Bus& bus) noexcept {
    const uint16_t w = bus.read16(cpu.pc);
    cpu.pc = (cpu.pc + 2U) & 0x00FFFFFFU;
    return w;
}

inline uint32_t fetch32(MdCpuState& cpu, Bus& bus) noexcept {
    const uint32_t hi = fetch16(cpu, bus);
    const uint32_t lo = fetch16(cpu, bus);
    return (hi << 16U) | lo;
}

inline uint32_t read_sized(Bus& bus, uint32_t addr, int sz) noexcept {
    if (sz == 1)
        return bus.read8(addr);
    if (sz == 2)
        return bus.read16(addr);
    return bus.read32(addr);
}

inline void write_sized(Bus& bus, uint32_t addr, uint32_t val, int sz) noexcept {
    if (sz == 1)
        bus.write8(addr, static_cast<uint8_t>(val));
    else if (sz == 2)
        bus.write16(addr, static_cast<uint16_t>(val));
    else
        bus.write32(addr, val);
}

inline void write_dn(MdCpuState& cpu, uint8_t reg, uint32_t val, int sz) noexcept {
    if (sz == 1)
        cpu.d[reg] = (cpu.d[reg] & 0xFFFFFF00U) | (val & 0xFFU);
    else if (sz == 2)
        cpu.d[reg] = (cpu.d[reg] & 0xFFFF0000U) | (val & 0xFFFFU);
    else
        cpu.d[reg] = val;
}

inline uint32_t eval_brief_ext(MdCpuState& cpu, uint32_t base, uint16_t ext) noexcept {
    const int8_t disp = static_cast<int8_t>(ext & 0xFFU);
    const uint8_t idx_reg = static_cast<uint8_t>((ext >> 12U) & 0x07U);
    const bool is_addr = (ext & 0x8000U) != 0U;
    const bool is_long = (ext & 0x0800U) != 0U;
    const uint32_t raw_idx = is_addr ? cpu.a[idx_reg] : cpu.d[idx_reg];
    const int32_t idx = is_long ? static_cast<int32_t>(raw_idx) : static_cast<int16_t>(raw_idx);
    return static_cast<uint32_t>(static_cast<int32_t>(base) + disp + idx);
}

struct EaRef {
    bool is_reg = false;
    bool is_addr_reg = false;
    bool is_imm = false;
    uint8_t reg = 0;
    uint32_t addr = 0;
    uint32_t imm = 0;
    uint32_t cycles = 0;
};

EaRef resolve_ea(MdCpuState& cpu, Bus& bus, uint8_t mode, uint8_t reg, int sz) noexcept {
    EaRef ea{};
    const uint32_t step = (sz == 1 && reg == 7U) ? 2U : static_cast<uint32_t>(sz);
    switch (mode & 0x07U) {
        case 0:  // Dn
            ea.is_reg = true;
            ea.reg = reg;
            break;
        case 1:  // An
            ea.is_reg = true;
            ea.is_addr_reg = true;
            ea.reg = reg;
            break;
        case 2:  // (An)
            ea.addr = cpu.a[reg];
            ea.cycles = (sz == 4) ? 8U : 4U;
            break;
        case 3:  // (An)+
            ea.addr = cpu.a[reg];
            cpu.a[reg] += step;
            ea.cycles = (sz == 4) ? 8U : 4U;
            break;
        case 4:  // -(An)
            cpu.a[reg] -= step;
            ea.addr = cpu.a[reg];
            ea.cycles = (sz == 4) ? 10U : 6U;
            break;
        case 5: {  // (d16, An)
            const int16_t d = static_cast<int16_t>(fetch16(cpu, bus));
            ea.addr = static_cast<uint32_t>(static_cast<int32_t>(cpu.a[reg]) + d);
            ea.cycles = (sz == 4) ? 12U : 8U;
            break;
        }
        case 6: {  // (d8, An, Xn)
            const uint16_t ext = fetch16(cpu, bus);
            ea.addr = eval_brief_ext(cpu, cpu.a[reg], ext);
            ea.cycles = (sz == 4) ? 14U : 10U;
            break;
        }
        case 7:
            switch (reg & 0x07U) {
                case 0:  // (xxx).W
                    ea.addr = static_cast<uint32_t>(static_cast<int16_t>(fetch16(cpu, bus)));
                    ea.cycles = (sz == 4) ? 12U : 8U;
                    break;
                case 1:  // (xxx).L
                    ea.addr = fetch32(cpu, bus);
                    ea.cycles = (sz == 4) ? 16U : 12U;
                    break;
                case 2: {  // (d16, PC)
                    const uint32_t base = cpu.pc;
                    const int16_t d = static_cast<int16_t>(fetch16(cpu, bus));
                    ea.addr = static_cast<uint32_t>(static_cast<int32_t>(base) + d);
                    ea.cycles = (sz == 4) ? 12U : 8U;
                    break;
                }
                case 3: {  // (d8, PC, Xn)
                    const uint32_t base = cpu.pc;
                    const uint16_t ext = fetch16(cpu, bus);
                    ea.addr = eval_brief_ext(cpu, base, ext);
                    ea.cycles = (sz == 4) ? 14U : 10U;
                    break;
                }
                case 4:  // #imm
                    ea.is_imm = true;
                    if (sz == 1)
                        ea.imm = fetch16(cpu, bus) & 0xFFU;
                    else if (sz == 2)
                        ea.imm = fetch16(cpu, bus);
                    else
                        ea.imm = fetch32(cpu, bus);
                    ea.cycles = (sz == 4) ? 8U : 4U;
                    break;
                default:
                    break;
            }
            break;
    }
    return ea;
}

inline uint32_t ea_read(MdCpuState& cpu, Bus& bus, const EaRef& ea, int sz) noexcept {
    if (ea.is_imm)
        return ea.imm & size_mask(sz);
    if (ea.is_reg) {
        const uint32_t v = ea.is_addr_reg ? cpu.a[ea.reg] : cpu.d[ea.reg];
        return v & size_mask(sz);
    }
    return read_sized(bus, ea.addr, sz);
}

inline void ea_write(MdCpuState& cpu, Bus& bus, const EaRef& ea, uint32_t val, int sz) noexcept {
    if (ea.is_reg) {
        if (ea.is_addr_reg)
            cpu.a[ea.reg] = (sz == 2) ? static_cast<uint32_t>(static_cast<int16_t>(val)) : val;
        else
            write_dn(cpu, ea.reg, val, sz);
        return;
    }
    if (!ea.is_imm) {
        write_sized(bus, ea.addr, val, sz);
    }
}

inline void set_logic_flags(MdCpuState& cpu, uint32_t val, int sz) noexcept {
    const uint32_t m = size_mask(sz);
    const uint32_t v = val & m;
    uint16_t sr = static_cast<uint16_t>(cpu.sr & ~(MD_FLAG_N | MD_FLAG_Z | MD_FLAG_V | MD_FLAG_C));
    if (v == 0U)
        sr |= MD_FLAG_Z;
    if (v & size_msb(sz))
        sr |= MD_FLAG_N;
    cpu.sr = sr;
}

inline uint32_t do_add(MdCpuState& cpu, uint32_t dst, uint32_t src, int sz, bool with_x,
                       bool set_x) noexcept {
    const uint32_t m = size_mask(sz);
    const uint32_t msb = size_msb(sz);
    const uint64_t x = (with_x && (cpu.sr & MD_FLAG_X)) ? 1ULL : 0ULL;
    const uint64_t d = dst & m;
    const uint64_t s = src & m;
    const uint64_t r64 = d + s + x;
    const uint32_t res = static_cast<uint32_t>(r64) & m;

    uint16_t sr =
        static_cast<uint16_t>(cpu.sr & ~(MD_FLAG_N | MD_FLAG_V | MD_FLAG_C |
                                         (with_x ? 0U : MD_FLAG_Z) | (set_x ? MD_FLAG_X : 0U)));
    if (res & msb)
        sr |= MD_FLAG_N;
    if (with_x) {
        if (res != 0U)
            sr &= ~MD_FLAG_Z;
    } else if (res == 0U) {
        sr |= MD_FLAG_Z;
    }
    if (((~(d ^ s)) & (d ^ res)) & msb)
        sr |= MD_FLAG_V;
    if (r64 > m) {
        sr |= MD_FLAG_C;
        if (set_x)
            sr |= MD_FLAG_X;
    }
    cpu.sr = sr;
    return res;
}

inline uint32_t do_sub(MdCpuState& cpu, uint32_t dst, uint32_t src, int sz, bool with_x,
                       bool set_x) noexcept {
    const uint32_t m = size_mask(sz);
    const uint32_t msb = size_msb(sz);
    const uint64_t x = (with_x && (cpu.sr & MD_FLAG_X)) ? 1ULL : 0ULL;
    const uint64_t d = dst & m;
    const uint64_t s = src & m;
    const uint64_t r64 = d - s - x;
    const uint32_t res = static_cast<uint32_t>(r64) & m;

    uint16_t sr =
        static_cast<uint16_t>(cpu.sr & ~(MD_FLAG_N | MD_FLAG_V | MD_FLAG_C |
                                         (with_x ? 0U : MD_FLAG_Z) | (set_x ? MD_FLAG_X : 0U)));
    if (res & msb)
        sr |= MD_FLAG_N;
    if (with_x) {
        if (res != 0U)
            sr &= ~MD_FLAG_Z;
    } else if (res == 0U) {
        sr |= MD_FLAG_Z;
    }
    if (((d ^ s) & (d ^ res)) & msb)
        sr |= MD_FLAG_V;
    if (r64 > m) {
        sr |= MD_FLAG_C;
        if (set_x)
            sr |= MD_FLAG_X;
    }
    cpu.sr = sr;
    return res;
}

inline uint8_t do_abcd(MdCpuState& cpu, uint8_t dst, uint8_t src) noexcept {
    const uint8_t x = (cpu.sr & MD_FLAG_X) ? 1U : 0U;
    uint32_t lo = (dst & 0x0FU) + (src & 0x0FU) + x;
    uint32_t hi = (dst & 0xF0U) + (src & 0xF0U);
    uint32_t res = hi + lo;
    uint32_t carry = 0;
    if (lo > 9U)
        res += 6U;
    if (res > 0x99U) {
        res += 0x60U;
        carry = 1U;
    }
    const uint8_t r8 = static_cast<uint8_t>(res & 0xFFU);
    uint16_t sr = static_cast<uint16_t>(cpu.sr & ~(MD_FLAG_X | MD_FLAG_C | MD_FLAG_N | MD_FLAG_V));
    if (carry)
        sr |= (MD_FLAG_X | MD_FLAG_C);
    if (r8 != 0U)
        sr &= ~MD_FLAG_Z;
    if (r8 & 0x80U)
        sr |= MD_FLAG_N;
    cpu.sr = sr;
    return r8;
}

inline uint8_t do_sbcd(MdCpuState& cpu, uint8_t dst, uint8_t src) noexcept {
    const uint8_t x = (cpu.sr & MD_FLAG_X) ? 1U : 0U;
    int32_t lo = static_cast<int32_t>(dst & 0x0FU) - static_cast<int32_t>(src & 0x0FU) - x;
    int32_t borrow_lo = 0;
    if (lo < 0) {
        lo -= 6;
        borrow_lo = 16;
    }
    int32_t res = static_cast<int32_t>(dst & 0xF0U) - static_cast<int32_t>(src & 0xF0U) +
                  (lo & 0x0F) - borrow_lo;
    uint32_t carry = 0;
    if (res < 0) {
        res -= 0x60;
        carry = 1U;
    }
    const uint8_t r8 = static_cast<uint8_t>(res & 0xFF);
    uint16_t sr = static_cast<uint16_t>(cpu.sr & ~(MD_FLAG_X | MD_FLAG_C | MD_FLAG_N | MD_FLAG_V));
    if (carry)
        sr |= (MD_FLAG_X | MD_FLAG_C);
    if (r8 != 0U)
        sr &= ~MD_FLAG_Z;
    if (r8 & 0x80U)
        sr |= MD_FLAG_N;
    cpu.sr = sr;
    return r8;
}

inline int decode_size2(uint8_t bits) noexcept {
    switch (bits & 0x03U) {
        case 0:
            return 1;
        case 1:
            return 2;
        case 2:
            return 4;
        default:
            return 0;
    }
}

uint32_t do_shift_rotate(MdCpuState& cpu, uint32_t val, int sz, uint8_t type, bool left,
                         uint32_t count) noexcept {
    const uint32_t m = size_mask(sz);
    const uint32_t msb = size_msb(sz);
    const uint32_t bits = static_cast<uint32_t>(sz * 8);
    uint32_t v = val & m;

    uint16_t sr = static_cast<uint16_t>(cpu.sr & ~(MD_FLAG_N | MD_FLAG_Z | MD_FLAG_V | MD_FLAG_C));
    if (count == 0U) {
        if (type == 2U && (cpu.sr & MD_FLAG_X))
            sr |= MD_FLAG_C;
        if (v == 0U)
            sr |= MD_FLAG_Z;
        if (v & msb)
            sr |= MD_FLAG_N;
        cpu.sr = sr;
        return v;
    }

    uint32_t last_out = 0;
    bool overflow = false;
    uint32_t x_bit = (cpu.sr & MD_FLAG_X) ? 1U : 0U;

    for (uint32_t i = 0; i < count; ++i) {
        if (type == 0U) {  // ASd
            if (left) {
                const uint32_t old_msb = (v & msb) ? 1U : 0U;
                last_out = old_msb;
                v = (v << 1U) & m;
                if (((v & msb) ? 1U : 0U) != old_msb)
                    overflow = true;
            } else {
                const uint32_t sign = v & msb;
                last_out = v & 1U;
                v = ((v >> 1U) | sign) & m;
            }
            x_bit = last_out;
        } else if (type == 1U) {  // LSd
            if (left) {
                last_out = (v & msb) ? 1U : 0U;
                v = (v << 1U) & m;
            } else {
                last_out = v & 1U;
                v = (v >> 1U) & m;
            }
            x_bit = last_out;
        } else if (type == 2U) {  // ROXd
            if (left) {
                last_out = (v & msb) ? 1U : 0U;
                v = ((v << 1U) | x_bit) & m;
                x_bit = last_out;
            } else {
                last_out = v & 1U;
                v = ((v >> 1U) | (x_bit << (bits - 1U))) & m;
                x_bit = last_out;
            }
        } else {  // ROd
            if (left) {
                last_out = (v & msb) ? 1U : 0U;
                v = ((v << 1U) | last_out) & m;
            } else {
                last_out = v & 1U;
                v = ((v >> 1U) | (last_out << (bits - 1U))) & m;
            }
        }
    }

    if (type != 3U) {
        sr = static_cast<uint16_t>((sr & ~MD_FLAG_X) | (x_bit ? MD_FLAG_X : 0U));
    }
    if (last_out)
        sr |= MD_FLAG_C;
    if (overflow)
        sr |= MD_FLAG_V;
    if (v == 0U)
        sr |= MD_FLAG_Z;
    if (v & msb)
        sr |= MD_FLAG_N;
    cpu.sr = sr;
    return v;
}

}  // namespace

void cpu_reset(MdCpuState& cpu, Bus& bus) noexcept {
    for (int i = 0; i < 8; ++i) {
        cpu.d[i] = 0;
        cpu.a[i] = 0;
    }
    cpu.usp = 0;
    cpu.sr = 0x2700U;  // Supervisor mode, IPL mask = 7
    cpu.stopped = 0;
    cpu.pending_irq = 0;
    cpu.cycles = 0;
    cpu.next_event = Vdp::kCyclesPerScanline;
    cpu.bus = &bus;
    cpu.wram = bus.wram_mut().data();
    cpu.rom = bus.cartridge().rom_data();
    cpu.rom_size = bus.cartridge().rom_size();

    cpu.ssp = bus.read32(0x000000U);
    cpu.a[7] = cpu.ssp;
    cpu.pc = bus.read32(0x000004U) & 0x00FFFFFFU;
}

void cpu_exception(MdCpuState& cpu, Bus& bus, uint8_t vector_num, uint8_t new_ipl) noexcept {
    const uint16_t old_sr = cpu.sr;
    uint16_t next_sr = static_cast<uint16_t>((old_sr | MD_SR_S) & ~MD_SR_T);
    if (new_ipl <= 7U) {
        next_sr = static_cast<uint16_t>((next_sr & ~MD_SR_I_MASK) |
                                        (static_cast<uint16_t>(new_ipl) << 8U));
    }
    md_set_sr(&cpu, next_sr);
    cpu.stopped = 0;

    cpu.a[7] -= 4U;
    bus.write32(cpu.a[7], cpu.pc);
    cpu.a[7] -= 2U;
    bus.write16(cpu.a[7], old_sr);

    cpu.pc = bus.read32(static_cast<uint32_t>(vector_num) * 4U) & 0x00FFFFFFU;
    cpu.cycles += 34U;
}

uint32_t cpu_check_interrupts(MdCpuState& cpu, Bus& bus) noexcept {
    const uint8_t mask = static_cast<uint8_t>((cpu.sr >> 8U) & 0x07U);
    if (bus.vdp().vint_pending() && bus.vdp().vint_enabled() && mask < 6U) {
        bus.vdp().ack_vint();
        cpu_exception(cpu, bus, MD_VEC_IRQ_L6, 6U);
        return 44U;
    }
    if (bus.vdp().hint_pending() && bus.vdp().hint_enabled() && mask < 4U) {
        bus.vdp().ack_hint();
        cpu_exception(cpu, bus, MD_VEC_IRQ_L4, 4U);
        return 44U;
    }
    return 0U;
}

uint32_t step_instruction(MdCpuState& cpu, Bus& bus) noexcept {
    if (cpu.stopped == 1U) {
        cpu.cycles += 4U;
        return 4U;
    }

    const uint16_t op = fetch16(cpu, bus);
    const uint8_t line = static_cast<uint8_t>(op >> 12U);
    const uint8_t rx = static_cast<uint8_t>((op >> 9U) & 0x07U);
    const uint8_t mode = static_cast<uint8_t>((op >> 3U) & 0x07U);
    const uint8_t ry = static_cast<uint8_t>(op & 0x07U);
    uint32_t cyc = 4U;

    switch (line) {
        case 0x0U: {  // Bit ops, MOVEP, Immediate ALU
            if ((op & 0x0138U) == 0x0108U) {
                // MOVEP
                const int16_t disp = static_cast<int16_t>(fetch16(cpu, bus));
                uint32_t addr = static_cast<uint32_t>(static_cast<int32_t>(cpu.a[ry]) + disp);
                const uint8_t dir_sz = static_cast<uint8_t>((op >> 6U) & 0x03U);
                if (dir_sz == 0U) {  // M->R word
                    const uint16_t v = static_cast<uint16_t>(
                        (static_cast<uint16_t>(bus.read8(addr)) << 8U) | bus.read8(addr + 2U));
                    write_dn(cpu, rx, v, 2);
                    cyc = 16U;
                } else if (dir_sz == 1U) {  // M->R long
                    const uint32_t v = (static_cast<uint32_t>(bus.read8(addr)) << 24U) |
                                       (static_cast<uint32_t>(bus.read8(addr + 2U)) << 16U) |
                                       (static_cast<uint32_t>(bus.read8(addr + 4U)) << 8U) |
                                       bus.read8(addr + 6U);
                    cpu.d[rx] = v;
                    cyc = 24U;
                } else if (dir_sz == 2U) {  // R->M word
                    bus.write8(addr, static_cast<uint8_t>((cpu.d[rx] >> 8U) & 0xFFU));
                    bus.write8(addr + 2U, static_cast<uint8_t>(cpu.d[rx] & 0xFFU));
                    cyc = 16U;
                } else {  // R->M long
                    bus.write8(addr, static_cast<uint8_t>((cpu.d[rx] >> 24U) & 0xFFU));
                    bus.write8(addr + 2U, static_cast<uint8_t>((cpu.d[rx] >> 16U) & 0xFFU));
                    bus.write8(addr + 4U, static_cast<uint8_t>((cpu.d[rx] >> 8U) & 0xFFU));
                    bus.write8(addr + 6U, static_cast<uint8_t>(cpu.d[rx] & 0xFFU));
                    cyc = 24U;
                }
            } else if ((op & 0x0100U) != 0U || (op & 0x0F00U) == 0x0800U) {
                // Dynamic or static BTST / BCHG / BCLR / BSET
                const uint32_t bit_raw = (op & 0x0100U) ? cpu.d[rx] : fetch16(cpu, bus);
                const int sz = (mode == 0U) ? 4 : 1;
                const uint32_t bit = bit_raw & ((sz == 4) ? 31U : 7U);
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                uint32_t v = ea_read(cpu, bus, ea, sz);
                const uint32_t mask = 1U << bit;
                if (v & mask)
                    cpu.sr &= ~MD_FLAG_Z;
                else
                    cpu.sr |= MD_FLAG_Z;
                const uint8_t b_op = static_cast<uint8_t>((op >> 6U) & 0x03U);
                if (b_op == 1U) {
                    v ^= mask;
                    ea_write(cpu, bus, ea, v, sz);
                } else if (b_op == 2U) {
                    v &= ~mask;
                    ea_write(cpu, bus, ea, v, sz);
                } else if (b_op == 3U) {
                    v |= mask;
                    ea_write(cpu, bus, ea, v, sz);
                }
                cyc = 8U + ea.cycles;
            } else {
                // Immediate ALU: ORI, ANDI, SUBI, ADDI, EORI, CMPI (incl. CCR / SR)
                const uint8_t sub = static_cast<uint8_t>((op >> 9U) & 0x07U);
                const int sz = decode_size2(static_cast<uint8_t>((op >> 6U) & 0x03U));
                if ((op & 0x003FU) == 0x003CU) {
                    // to CCR / to SR
                    const uint16_t imm = fetch16(cpu, bus);
                    if (sz == 1) {
                        uint8_t ccr = static_cast<uint8_t>(cpu.sr & MD_SR_CCR_MASK);
                        if (sub == 0U)
                            ccr |= static_cast<uint8_t>(imm);
                        else if (sub == 1U)
                            ccr &= static_cast<uint8_t>(imm);
                        else if (sub == 5U)
                            ccr ^= static_cast<uint8_t>(imm);
                        md_set_ccr(&cpu, ccr);
                    } else {
                        uint16_t nsr = cpu.sr;
                        if (sub == 0U)
                            nsr |= imm;
                        else if (sub == 1U)
                            nsr &= imm;
                        else if (sub == 5U)
                            nsr ^= imm;
                        md_set_sr(&cpu, nsr);
                    }
                    cyc = 20U;
                } else {
                    const uint32_t imm =
                        (sz == 4) ? fetch32(cpu, bus) : (fetch16(cpu, bus) & size_mask(sz));
                    const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                    const uint32_t dst = ea_read(cpu, bus, ea, sz);
                    uint32_t res = dst;
                    switch (sub) {
                        case 0:
                            res = dst | imm;
                            set_logic_flags(cpu, res, sz);
                            ea_write(cpu, bus, ea, res, sz);
                            break;
                        case 1:
                            res = dst & imm;
                            set_logic_flags(cpu, res, sz);
                            ea_write(cpu, bus, ea, res, sz);
                            break;
                        case 2:
                            res = do_sub(cpu, dst, imm, sz, false, true);
                            ea_write(cpu, bus, ea, res, sz);
                            break;
                        case 3:
                            res = do_add(cpu, dst, imm, sz, false, true);
                            ea_write(cpu, bus, ea, res, sz);
                            break;
                        case 5:
                            res = dst ^ imm;
                            set_logic_flags(cpu, res, sz);
                            ea_write(cpu, bus, ea, res, sz);
                            break;
                        case 6:
                            (void)do_sub(cpu, dst, imm, sz, false, false);
                            break;
                        default:
                            break;
                    }
                    cyc = (sz == 4 ? 16U : 8U) + ea.cycles;
                }
            }
            break;
        }

        case 0x1U:
        case 0x2U:
        case 0x3U: {  // MOVE.B / MOVE.L / MOVE.W & MOVEA
            const int sz = (line == 1U) ? 1 : (line == 2U) ? 4 : 2;
            const EaRef src = resolve_ea(cpu, bus, mode, ry, sz);
            const uint32_t val = ea_read(cpu, bus, src, sz);
            const uint8_t dst_mode = static_cast<uint8_t>((op >> 6U) & 0x07U);
            const uint8_t dst_reg = rx;
            if (dst_mode == 1U) {
                // MOVEA: sign-extends word to 32-bit, does not affect flags
                cpu.a[dst_reg] = (sz == 2) ? static_cast<uint32_t>(static_cast<int16_t>(val)) : val;
                cyc = 4U + src.cycles;
            } else {
                const EaRef dst = resolve_ea(cpu, bus, dst_mode, dst_reg, sz);
                ea_write(cpu, bus, dst, val, sz);
                set_logic_flags(cpu, val, sz);
                cyc = 4U + src.cycles + dst.cycles;
            }
            break;
        }

        case 0x4U: {  // Miscellaneous
            if ((op & 0x01C0U) == 0x01C0U) {
                // LEA
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, 4);
                cpu.a[rx] = ea.addr;
                cyc = 4U + ea.cycles;
            } else if ((op & 0x01C0U) == 0x0180U) {
                // CHK.W
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, 2);
                const int16_t bound = static_cast<int16_t>(ea_read(cpu, bus, ea, 2));
                const int16_t d = static_cast<int16_t>(cpu.d[rx] & 0xFFFFU);
                if (d < 0) {
                    cpu.sr |= MD_FLAG_N;
                    cpu_exception(cpu, bus, MD_VEC_CHK);
                } else if (d > bound) {
                    cpu.sr &= ~MD_FLAG_N;
                    cpu_exception(cpu, bus, MD_VEC_CHK);
                }
                cyc = 10U + ea.cycles;
            } else if ((op & 0xFF00U) == 0x4000U) {
                if ((op & 0x00C0U) == 0x00C0U) {  // MOVE from SR
                    const EaRef ea = resolve_ea(cpu, bus, mode, ry, 2);
                    ea_write(cpu, bus, ea, cpu.sr, 2);
                    cyc = 6U + ea.cycles;
                } else {  // NEGX
                    const int sz = decode_size2(static_cast<uint8_t>((op >> 6U) & 0x03U));
                    const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                    const uint32_t res = do_sub(cpu, 0U, ea_read(cpu, bus, ea, sz), sz, true, true);
                    ea_write(cpu, bus, ea, res, sz);
                    cyc = 6U + ea.cycles;
                }
            } else if ((op & 0xFF00U) == 0x4200U) {  // CLR
                const int sz = decode_size2(static_cast<uint8_t>((op >> 6U) & 0x03U));
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                ea_write(cpu, bus, ea, 0U, sz);
                set_logic_flags(cpu, 0U, sz);
                cyc = 6U + ea.cycles;
            } else if ((op & 0xFF00U) == 0x4400U) {
                if ((op & 0x00C0U) == 0x00C0U) {  // MOVE to CCR
                    const EaRef ea = resolve_ea(cpu, bus, mode, ry, 2);
                    md_set_ccr(&cpu, static_cast<uint8_t>(ea_read(cpu, bus, ea, 2)));
                    cyc = 12U + ea.cycles;
                } else {  // NEG
                    const int sz = decode_size2(static_cast<uint8_t>((op >> 6U) & 0x03U));
                    const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                    const uint32_t res =
                        do_sub(cpu, 0U, ea_read(cpu, bus, ea, sz), sz, false, true);
                    ea_write(cpu, bus, ea, res, sz);
                    cyc = 6U + ea.cycles;
                }
            } else if ((op & 0xFF00U) == 0x4600U) {
                if ((op & 0x00C0U) == 0x00C0U) {  // MOVE to SR
                    const EaRef ea = resolve_ea(cpu, bus, mode, ry, 2);
                    md_set_sr(&cpu, static_cast<uint16_t>(ea_read(cpu, bus, ea, 2)));
                    cyc = 12U + ea.cycles;
                } else {  // NOT
                    const int sz = decode_size2(static_cast<uint8_t>((op >> 6U) & 0x03U));
                    const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                    const uint32_t res = (~ea_read(cpu, bus, ea, sz)) & size_mask(sz);
                    ea_write(cpu, bus, ea, res, sz);
                    set_logic_flags(cpu, res, sz);
                    cyc = 6U + ea.cycles;
                }
            } else if ((op & 0xFFC0U) == 0x4800U) {  // NBCD
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, 1);
                const uint8_t res =
                    do_sbcd(cpu, 0U, static_cast<uint8_t>(ea_read(cpu, bus, ea, 1)));
                ea_write(cpu, bus, ea, res, 1);
                cyc = 8U + ea.cycles;
            } else if ((op & 0xFFF8U) == 0x4840U) {  // SWAP
                cpu.d[ry] = (cpu.d[ry] >> 16U) | (cpu.d[ry] << 16U);
                set_logic_flags(cpu, cpu.d[ry], 4);
                cyc = 4U;
            } else if ((op & 0xFFC0U) == 0x4840U) {  // PEA
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, 4);
                cpu.a[7] -= 4U;
                bus.write32(cpu.a[7], ea.addr);
                cyc = 12U + ea.cycles;
            } else if ((op & 0xFFF8U) == 0x4880U) {  // EXT.W
                const uint16_t v = static_cast<uint16_t>(static_cast<int8_t>(cpu.d[ry] & 0xFFU));
                write_dn(cpu, ry, v, 2);
                set_logic_flags(cpu, v, 2);
                cyc = 4U;
            } else if ((op & 0xFFF8U) == 0x48C0U) {  // EXT.L
                const uint32_t v = static_cast<uint32_t>(static_cast<int16_t>(cpu.d[ry] & 0xFFFFU));
                cpu.d[ry] = v;
                set_logic_flags(cpu, v, 4);
                cyc = 4U;
            } else if ((op & 0xFB80U) == 0x4880U) {  // MOVEM
                const bool to_reg = (op & 0x0400U) != 0U;
                const int sz = (op & 0x0040U) ? 4 : 2;
                const uint16_t mask = fetch16(cpu, bus);
                if (!to_reg) {
                    if (mode == 4U) {  // -(An): mask bits 0..15 correspond to A7..A0, D7..D0
                        uint32_t addr = cpu.a[ry];
                        for (int i = 0; i < 16; ++i) {
                            if (mask & (1U << i)) {
                                addr -= static_cast<uint32_t>(sz);
                                const int r = 15 - i;
                                const uint32_t v = (r < 8) ? cpu.d[r] : cpu.a[r - 8];
                                write_sized(bus, addr, v, sz);
                                cyc += (sz == 4) ? 8U : 4U;
                            }
                        }
                        cpu.a[ry] = addr;
                    } else {
                        const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                        uint32_t addr = ea.addr;
                        for (int i = 0; i < 16; ++i) {
                            if (mask & (1U << i)) {
                                const uint32_t v = (i < 8) ? cpu.d[i] : cpu.a[i - 8];
                                write_sized(bus, addr, v, sz);
                                addr += static_cast<uint32_t>(sz);
                                cyc += (sz == 4) ? 8U : 4U;
                            }
                        }
                        cyc += ea.cycles;
                    }
                } else {
                    uint32_t addr = 0;
                    if (mode == 3U) {
                        addr = cpu.a[ry];
                    } else {
                        const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                        addr = ea.addr;
                        cyc += ea.cycles;
                    }
                    for (int i = 0; i < 16; ++i) {
                        if (mask & (1U << i)) {
                            uint32_t v = read_sized(bus, addr, sz);
                            if (sz == 2)
                                v = static_cast<uint32_t>(static_cast<int16_t>(v));
                            if (i < 8)
                                cpu.d[i] = v;
                            else
                                cpu.a[i - 8] = v;
                            addr += static_cast<uint32_t>(sz);
                            cyc += (sz == 4) ? 8U : 4U;
                        }
                    }
                    if (mode == 3U) {
                        cpu.a[ry] = addr;
                    }
                }
            } else if (op == 0x4AFCU) {  // ILLEGAL
                cpu_exception(cpu, bus, MD_VEC_ILLEGAL);
                cyc = 34U;
            } else if ((op & 0xFFC0U) == 0x4AC0U) {  // TAS (Genesis memory write-inhibit quirk!)
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, 1);
                const uint8_t v = static_cast<uint8_t>(ea_read(cpu, bus, ea, 1));
                set_logic_flags(cpu, v, 1);
                if (ea.is_reg) {
                    write_dn(cpu, ea.reg, v | 0x80U, 1);
                }
                // On real Genesis hardware, TAS <mem> does NOT write bit 7 back to memory!
                cyc = 10U + ea.cycles;
            } else if ((op & 0xFF00U) == 0x4A00U) {  // TST
                const int sz = decode_size2(static_cast<uint8_t>((op >> 6U) & 0x03U));
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                set_logic_flags(cpu, ea_read(cpu, bus, ea, sz), sz);
                cyc = 4U + ea.cycles;
            } else if ((op & 0xFFF0U) == 0x4E40U) {  // TRAP #v
                cpu_exception(cpu, bus, static_cast<uint8_t>(MD_VEC_TRAP_BASE + (op & 0x0FU)));
                cyc = 34U;
            } else if ((op & 0xFFF8U) == 0x4E50U) {  // LINK
                const int16_t disp = static_cast<int16_t>(fetch16(cpu, bus));
                cpu.a[7] -= 4U;
                bus.write32(cpu.a[7], cpu.a[ry]);
                cpu.a[ry] = cpu.a[7];
                cpu.a[7] = static_cast<uint32_t>(static_cast<int32_t>(cpu.a[7]) + disp);
                cyc = 16U;
            } else if ((op & 0xFFF8U) == 0x4E58U) {  // UNLK
                cpu.a[7] = cpu.a[ry];
                cpu.a[ry] = bus.read32(cpu.a[7]);
                cpu.a[7] += 4U;
                cyc = 12U;
            } else if ((op & 0xFFF0U) == 0x4E60U) {  // MOVE USP
                if (op & 0x0008U)
                    cpu.a[ry] = cpu.usp;
                else
                    cpu.usp = cpu.a[ry];
                cyc = 4U;
            } else if (op == 0x4E70U) {  // RESET
                cyc = 132U;
            } else if (op == 0x4E71U) {  // NOP
                cyc = 4U;
            } else if (op == 0x4E72U) {  // STOP #imm
                const uint16_t imm = fetch16(cpu, bus);
                md_set_sr(&cpu, imm);
                cpu.stopped = 1U;
                cyc = 4U;
            } else if (op == 0x4E73U) {  // RTE
                const uint16_t nsr = bus.read16(cpu.a[7]);
                cpu.a[7] += 2U;
                const uint32_t npc = bus.read32(cpu.a[7]);
                cpu.a[7] += 4U;
                md_set_sr(&cpu, nsr);
                cpu.pc = npc & 0x00FFFFFFU;
                cyc = 20U;
            } else if (op == 0x4E75U) {  // RTS
                cpu.pc = bus.read32(cpu.a[7]) & 0x00FFFFFFU;
                cpu.a[7] += 4U;
                cyc = 16U;
            } else if (op == 0x4E76U) {  // TRAPV
                if (cpu.sr & MD_FLAG_V) {
                    cpu_exception(cpu, bus, MD_VEC_TRAPV);
                    cyc = 34U;
                } else {
                    cyc = 4U;
                }
            } else if (op == 0x4E77U) {  // RTR
                const uint16_t ccr = bus.read16(cpu.a[7]);
                cpu.a[7] += 2U;
                cpu.pc = bus.read32(cpu.a[7]) & 0x00FFFFFFU;
                cpu.a[7] += 4U;
                md_set_ccr(&cpu, static_cast<uint8_t>(ccr));
                cyc = 20U;
            } else if ((op & 0xFFC0U) == 0x4E80U) {  // JSR
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, 4);
                cpu.a[7] -= 4U;
                bus.write32(cpu.a[7], cpu.pc);
                cpu.pc = ea.addr & 0x00FFFFFFU;
                cyc = 16U + ea.cycles;
            } else if ((op & 0xFFC0U) == 0x4EC0U) {  // JMP
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, 4);
                cpu.pc = ea.addr & 0x00FFFFFFU;
                cyc = 8U + ea.cycles;
            }
            break;
        }

        case 0x5U: {  // ADDQ / SUBQ / Scc / DBcc
            if ((op & 0x00C0U) == 0x00C0U) {
                const uint8_t cc = static_cast<uint8_t>((op >> 8U) & 0x0FU);
                if (mode == 1U) {  // DBcc
                    const uint32_t base = cpu.pc;
                    const int16_t disp = static_cast<int16_t>(fetch16(cpu, bus));
                    if (!md_cc_test(cpu.sr, cc)) {
                        const uint16_t cnt = static_cast<uint16_t>((cpu.d[ry] & 0xFFFFU) - 1U);
                        write_dn(cpu, ry, cnt, 2);
                        if (cnt != 0xFFFFU) {
                            cpu.pc = static_cast<uint32_t>(static_cast<int32_t>(base) + disp) &
                                     0x00FFFFFFU;
                            cyc = 10U;
                        } else {
                            cyc = 14U;
                        }
                    } else {
                        cyc = 12U;
                    }
                } else {  // Scc
                    const EaRef ea = resolve_ea(cpu, bus, mode, ry, 1);
                    const bool ok = md_cc_test(cpu.sr, cc) != 0U;
                    ea_write(cpu, bus, ea, ok ? 0xFFU : 0x00U, 1);
                    cyc = (ok ? 6U : 4U) + ea.cycles;
                }
            } else {
                const uint32_t imm = (rx == 0U) ? 8U : rx;
                const int sz = decode_size2(static_cast<uint8_t>((op >> 6U) & 0x03U));
                const bool is_sub = (op & 0x0100U) != 0U;
                if (mode == 1U) {  // ADDQ/SUBQ to An: always 32-bit, no flags
                    cpu.a[ry] = is_sub ? (cpu.a[ry] - imm) : (cpu.a[ry] + imm);
                    cyc = 8U;
                } else {
                    const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                    const uint32_t dst = ea_read(cpu, bus, ea, sz);
                    const uint32_t res = is_sub ? do_sub(cpu, dst, imm, sz, false, true)
                                                : do_add(cpu, dst, imm, sz, false, true);
                    ea_write(cpu, bus, ea, res, sz);
                    cyc = (sz == 4 ? 8U : 4U) + ea.cycles;
                }
            }
            break;
        }

        case 0x6U: {  // BRA / BSR / Bcc
            const uint8_t cc = static_cast<uint8_t>((op >> 8U) & 0x0FU);
            const uint32_t base = cpu.pc;
            int32_t disp = static_cast<int8_t>(op & 0xFFU);
            if (disp == 0) {
                disp = static_cast<int16_t>(fetch16(cpu, bus));
            }
            if (cc == 0U) {  // BRA
                cpu.pc = static_cast<uint32_t>(static_cast<int32_t>(base) + disp) & 0x00FFFFFFU;
                cyc = 10U;
            } else if (cc == 1U) {  // BSR
                cpu.a[7] -= 4U;
                bus.write32(cpu.a[7], cpu.pc);
                cpu.pc = static_cast<uint32_t>(static_cast<int32_t>(base) + disp) & 0x00FFFFFFU;
                cyc = 18U;
            } else if (md_cc_test(cpu.sr, cc)) {
                cpu.pc = static_cast<uint32_t>(static_cast<int32_t>(base) + disp) & 0x00FFFFFFU;
                cyc = 10U;
            } else {
                cyc = 8U;
            }
            break;
        }

        case 0x7U: {  // MOVEQ
            const uint32_t v = static_cast<uint32_t>(static_cast<int8_t>(op & 0xFFU));
            cpu.d[rx] = v;
            set_logic_flags(cpu, v, 4);
            cyc = 4U;
            break;
        }

        case 0x8U: {  // OR / DIVU / DIVS / SBCD
            const uint8_t opmode = static_cast<uint8_t>((op >> 6U) & 0x07U);
            if (opmode == 3U) {  // DIVU.W
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, 2);
                const uint16_t divisor = static_cast<uint16_t>(ea_read(cpu, bus, ea, 2));
                if (divisor == 0U) {
                    cpu_exception(cpu, bus, MD_VEC_ZERO_DIV);
                } else {
                    const uint32_t q = cpu.d[rx] / divisor;
                    const uint32_t r = cpu.d[rx] % divisor;
                    if (q > 0xFFFFU) {
                        cpu.sr = static_cast<uint16_t>((cpu.sr & ~MD_FLAG_C) | MD_FLAG_V);
                    } else {
                        cpu.d[rx] = (r << 16U) | (q & 0xFFFFU);
                        set_logic_flags(cpu, q, 2);
                    }
                }
                cyc = 140U + ea.cycles;
            } else if (opmode == 7U) {  // DIVS.W
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, 2);
                const int16_t divisor = static_cast<int16_t>(ea_read(cpu, bus, ea, 2));
                if (divisor == 0) {
                    cpu_exception(cpu, bus, MD_VEC_ZERO_DIV);
                } else {
                    const int32_t dividend = static_cast<int32_t>(cpu.d[rx]);
                    if (dividend == INT32_MIN && divisor == -1) {
                        cpu.sr = static_cast<uint16_t>((cpu.sr & ~MD_FLAG_C) | MD_FLAG_V);
                    } else {
                        const int32_t q = dividend / divisor;
                        const int32_t r = dividend % divisor;
                        if (q < -32768 || q > 32767) {
                            cpu.sr = static_cast<uint16_t>((cpu.sr & ~MD_FLAG_C) | MD_FLAG_V);
                        } else {
                            cpu.d[rx] = (static_cast<uint32_t>(static_cast<uint16_t>(r)) << 16U) |
                                        static_cast<uint16_t>(q);
                            set_logic_flags(cpu, static_cast<uint16_t>(q), 2);
                        }
                    }
                }
                cyc = 158U + ea.cycles;
            } else if ((op & 0x01F0U) == 0x0100U) {  // SBCD
                if (op & 0x0008U) {
                    cpu.a[ry] -= (ry == 7U ? 2U : 1U);
                    cpu.a[rx] -= (rx == 7U ? 2U : 1U);
                    const uint8_t res = do_sbcd(cpu, bus.read8(cpu.a[rx]), bus.read8(cpu.a[ry]));
                    bus.write8(cpu.a[rx], res);
                    cyc = 18U;
                } else {
                    const uint8_t res = do_sbcd(cpu, static_cast<uint8_t>(cpu.d[rx]),
                                                static_cast<uint8_t>(cpu.d[ry]));
                    write_dn(cpu, rx, res, 1);
                    cyc = 6U;
                }
            } else {  // OR
                const int sz = decode_size2(opmode & 0x03U);
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                const uint32_t res = (ea_read(cpu, bus, ea, sz) | cpu.d[rx]) & size_mask(sz);
                if (opmode & 0x04U)
                    ea_write(cpu, bus, ea, res, sz);
                else
                    write_dn(cpu, rx, res, sz);
                set_logic_flags(cpu, res, sz);
                cyc = (sz == 4 ? 8U : 4U) + ea.cycles;
            }
            break;
        }

        case 0x9U:
        case 0xDU: {  // SUB/SUBA/SUBX (0x9) and ADD/ADDA/ADDX (0xD)
            const bool is_add = (line == 0xDU);
            const uint8_t opmode = static_cast<uint8_t>((op >> 6U) & 0x07U);
            if (opmode == 3U || opmode == 7U) {  // SUBA / ADDA
                const int sz = (opmode == 7U) ? 4 : 2;
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                const int32_t v = sign_ext(ea_read(cpu, bus, ea, sz), sz);
                cpu.a[rx] = is_add ? static_cast<uint32_t>(static_cast<int32_t>(cpu.a[rx]) + v)
                                   : static_cast<uint32_t>(static_cast<int32_t>(cpu.a[rx]) - v);
                cyc = 8U + ea.cycles;
            } else if ((op & 0x0130U) == 0x0100U && (opmode & 0x03U) != 3U) {  // SUBX / ADDX
                const int sz = decode_size2(opmode & 0x03U);
                if (op & 0x0008U) {
                    const uint32_t sy = (sz == 1 && ry == 7U) ? 2U : static_cast<uint32_t>(sz);
                    const uint32_t sx = (sz == 1 && rx == 7U) ? 2U : static_cast<uint32_t>(sz);
                    cpu.a[ry] -= sy;
                    cpu.a[rx] -= sx;
                    const uint32_t s = read_sized(bus, cpu.a[ry], sz);
                    const uint32_t d = read_sized(bus, cpu.a[rx], sz);
                    const uint32_t r = is_add ? do_add(cpu, d, s, sz, true, true)
                                              : do_sub(cpu, d, s, sz, true, true);
                    write_sized(bus, cpu.a[rx], r, sz);
                    cyc = (sz == 4) ? 30U : 18U;
                } else {
                    const uint32_t r = is_add ? do_add(cpu, cpu.d[rx], cpu.d[ry], sz, true, true)
                                              : do_sub(cpu, cpu.d[rx], cpu.d[ry], sz, true, true);
                    write_dn(cpu, rx, r, sz);
                    cyc = (sz == 4) ? 8U : 4U;
                }
            } else {  // SUB / ADD
                const int sz = decode_size2(opmode & 0x03U);
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                const uint32_t eav = ea_read(cpu, bus, ea, sz);
                if (opmode & 0x04U) {
                    const uint32_t r = is_add ? do_add(cpu, eav, cpu.d[rx], sz, false, true)
                                              : do_sub(cpu, eav, cpu.d[rx], sz, false, true);
                    ea_write(cpu, bus, ea, r, sz);
                } else {
                    const uint32_t r = is_add ? do_add(cpu, cpu.d[rx], eav, sz, false, true)
                                              : do_sub(cpu, cpu.d[rx], eav, sz, false, true);
                    write_dn(cpu, rx, r, sz);
                }
                cyc = (sz == 4 ? 8U : 4U) + ea.cycles;
            }
            break;
        }

        case 0xAU:  // Line-A exception
            cpu.pc = (cpu.pc - 2U) & 0x00FFFFFFU;
            cpu_exception(cpu, bus, MD_VEC_LINE_A);
            cyc = 34U;
            break;

        case 0xBU: {  // CMP / CMPA / CMPM / EOR
            const uint8_t opmode = static_cast<uint8_t>((op >> 6U) & 0x07U);
            if (opmode == 3U || opmode == 7U) {  // CMPA
                const int sz = (opmode == 7U) ? 4 : 2;
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                const uint32_t v = static_cast<uint32_t>(sign_ext(ea_read(cpu, bus, ea, sz), sz));
                (void)do_sub(cpu, cpu.a[rx], v, 4, false, false);
                cyc = 6U + ea.cycles;
            } else if ((op & 0x0138U) == 0x0108U) {  // CMPM
                const int sz = decode_size2(opmode & 0x03U);
                const uint32_t sy = (sz == 1 && ry == 7U) ? 2U : static_cast<uint32_t>(sz);
                const uint32_t sx = (sz == 1 && rx == 7U) ? 2U : static_cast<uint32_t>(sz);
                const uint32_t s = read_sized(bus, cpu.a[ry], sz);
                cpu.a[ry] += sy;
                const uint32_t d = read_sized(bus, cpu.a[rx], sz);
                cpu.a[rx] += sx;
                (void)do_sub(cpu, d, s, sz, false, false);
                cyc = (sz == 4) ? 20U : 12U;
            } else if (opmode < 3U) {  // CMP
                const int sz = decode_size2(opmode);
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                (void)do_sub(cpu, cpu.d[rx], ea_read(cpu, bus, ea, sz), sz, false, false);
                cyc = 4U + ea.cycles;
            } else {  // EOR
                const int sz = decode_size2(opmode & 0x03U);
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                const uint32_t r = (ea_read(cpu, bus, ea, sz) ^ cpu.d[rx]) & size_mask(sz);
                ea_write(cpu, bus, ea, r, sz);
                set_logic_flags(cpu, r, sz);
                cyc = (sz == 4 ? 8U : 4U) + ea.cycles;
            }
            break;
        }

        case 0xCU: {  // AND / MULU / MULS / ABCD / EXG
            const uint8_t opmode = static_cast<uint8_t>((op >> 6U) & 0x07U);
            if (opmode == 3U) {  // MULU.W
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, 2);
                const uint32_t r = (cpu.d[rx] & 0xFFFFU) * (ea_read(cpu, bus, ea, 2) & 0xFFFFU);
                cpu.d[rx] = r;
                set_logic_flags(cpu, r, 4);
                cyc = 70U + ea.cycles;
            } else if (opmode == 7U) {  // MULS.W
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, 2);
                const int32_t a = static_cast<int16_t>(cpu.d[rx] & 0xFFFFU);
                const int32_t b = static_cast<int16_t>(ea_read(cpu, bus, ea, 2) & 0xFFFFU);
                const uint32_t r = static_cast<uint32_t>(a * b);
                cpu.d[rx] = r;
                set_logic_flags(cpu, r, 4);
                cyc = 70U + ea.cycles;
            } else if ((op & 0x01F0U) == 0x0100U) {  // ABCD
                if (op & 0x0008U) {
                    cpu.a[ry] -= (ry == 7U ? 2U : 1U);
                    cpu.a[rx] -= (rx == 7U ? 2U : 1U);
                    const uint8_t res = do_abcd(cpu, bus.read8(cpu.a[rx]), bus.read8(cpu.a[ry]));
                    bus.write8(cpu.a[rx], res);
                    cyc = 18U;
                } else {
                    const uint8_t res = do_abcd(cpu, static_cast<uint8_t>(cpu.d[rx]),
                                                static_cast<uint8_t>(cpu.d[ry]));
                    write_dn(cpu, rx, res, 1);
                    cyc = 6U;
                }
            } else if ((op & 0x01F8U) == 0x0140U) {  // EXG Dx,Dy
                std::swap(cpu.d[rx], cpu.d[ry]);
                cyc = 6U;
            } else if ((op & 0x01F8U) == 0x0148U) {  // EXG Ax,Ay
                std::swap(cpu.a[rx], cpu.a[ry]);
                cyc = 6U;
            } else if ((op & 0x01F8U) == 0x0188U) {  // EXG Dx,Ay
                std::swap(cpu.d[rx], cpu.a[ry]);
                cyc = 6U;
            } else {  // AND
                const int sz = decode_size2(opmode & 0x03U);
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, sz);
                const uint32_t r = (ea_read(cpu, bus, ea, sz) & cpu.d[rx]) & size_mask(sz);
                if (opmode & 0x04U)
                    ea_write(cpu, bus, ea, r, sz);
                else
                    write_dn(cpu, rx, r, sz);
                set_logic_flags(cpu, r, sz);
                cyc = (sz == 4 ? 8U : 4U) + ea.cycles;
            }
            break;
        }

        case 0xEU: {  // Shift / Rotate
            const bool left = (op & 0x0100U) != 0U;
            if ((op & 0x00C0U) == 0x00C0U) {  // Memory shift (word, count=1)
                const uint8_t type = static_cast<uint8_t>((op >> 9U) & 0x03U);
                const EaRef ea = resolve_ea(cpu, bus, mode, ry, 2);
                const uint32_t r =
                    do_shift_rotate(cpu, ea_read(cpu, bus, ea, 2), 2, type, left, 1U);
                ea_write(cpu, bus, ea, r, 2);
                cyc = 8U + ea.cycles;
            } else {  // Register shift
                const int sz = decode_size2(static_cast<uint8_t>((op >> 6U) & 0x03U));
                const uint8_t type = static_cast<uint8_t>((op >> 3U) & 0x03U);
                const bool reg_cnt = (op & 0x0020U) != 0U;
                const uint32_t cnt = reg_cnt ? (cpu.d[rx] & 63U) : ((rx == 0U) ? 8U : rx);
                const uint32_t r = do_shift_rotate(cpu, cpu.d[ry], sz, type, left, cnt);
                write_dn(cpu, ry, r, sz);
                cyc = (sz == 4 ? 8U : 6U) + cnt * 2U;
            }
            break;
        }

        case 0xFU:  // Line-F exception
            cpu.pc = (cpu.pc - 2U) & 0x00FFFFFFU;
            cpu_exception(cpu, bus, MD_VEC_LINE_F);
            cyc = 34U;
            break;
    }

    cpu.cycles += cyc;
    return cyc;
}

uint32_t run_frame_interpreter(MdCpuState& cpu, Bus& bus, uint32_t max_cycles) noexcept {
    uint32_t elapsed = 0;
    while (elapsed < max_cycles && cpu.stopped != 2U) {
        const uint32_t c = step_instruction(cpu, bus);
        elapsed += c;
        if (cpu.cycles >= cpu.next_event) {
            bus.sync_hardware(cpu.cycles);
            cpu_check_interrupts(cpu, bus);
            cpu.next_event = cpu.cycles + Vdp::kCyclesPerScanline;
            if (bus.vdp().consume_frame_complete()) {
                bus.on_frame_complete(&cpu);
                break;
            }
        }
    }
    return elapsed;
}

}  // namespace dragon::md
