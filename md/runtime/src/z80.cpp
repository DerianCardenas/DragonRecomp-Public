#include "md/z80.h"

#include <utility>

#include "md/bus.h"

namespace dragon::md {

namespace {

constexpr uint8_t kS = 0x80U;
constexpr uint8_t kZ = 0x40U;
constexpr uint8_t kH = 0x10U;
constexpr uint8_t kP = 0x04U;
constexpr uint8_t kN = 0x02U;
constexpr uint8_t kC = 0x01U;

inline uint8_t parity(uint8_t v) noexcept {
    v ^= static_cast<uint8_t>(v >> 4U);
    v ^= static_cast<uint8_t>(v >> 2U);
    v ^= static_cast<uint8_t>(v >> 1U);
    return (v & 1U) == 0U ? kP : 0U;
}

inline uint8_t szp(uint8_t v) noexcept {
    return static_cast<uint8_t>((v & kS) | (v == 0U ? kZ : 0U) | parity(v));
}

inline uint8_t sz(uint8_t v) noexcept {
    return static_cast<uint8_t>((v & kS) | (v == 0U ? kZ : 0U));
}

}  // namespace

MdZ80::MdZ80() noexcept {
    reset();
}

void MdZ80::reset() noexcept {
    a_ = f_ = b_ = c_ = d_ = e_ = h_ = l_ = 0;
    a2_ = f2_ = b2_ = c2_ = d2_ = e2_ = h2_ = l2_ = 0;
    ix_ = iy_ = 0;
    sp_ = 0x2000U;
    pc_ = 0;
    i_ = r_ = 0;
    iff1_ = iff2_ = im_ = ei_delay_ = halted_ = 0;
    bank_reg_ = 0;
    irq_line_ = false;
    cycle_debt_ = 0;
}

void MdZ80::set_bus_request(bool requested) noexcept {
    bus_requested_ = requested;
}

void MdZ80::set_reset_line(bool in_reset) noexcept {
    if (in_reset && !in_reset_) {
        // Assert reset: reset CPU registers (Sound RAM is preserved!)
        a_ = f_ = b_ = c_ = d_ = e_ = h_ = l_ = 0;
        a2_ = f2_ = b2_ = c2_ = d2_ = e2_ = h2_ = l2_ = 0;
        ix_ = iy_ = 0;
        sp_ = 0x2000U;
        pc_ = 0;
        i_ = r_ = 0;
        iff1_ = iff2_ = im_ = ei_delay_ = halted_ = 0;
        bank_reg_ = 0;
        cycle_debt_ = 0;
    }
    in_reset_ = in_reset;
}

uint8_t MdZ80::mem_read(uint16_t addr, Bus* bus) noexcept {
    if (addr < 0x4000U) {
        return ram_[addr & 0x1FFFU];
    }
    if (addr < 0x6000U) {
        return bus ? bus->ym2612().read_status() : 0x00U;
    }
    if (addr >= 0x8000U && bus) {
        const uint32_t m68k_addr = (static_cast<uint32_t>(bank_reg_) << 15U) | (addr & 0x7FFFU);
        return bus->read8(m68k_addr);
    }
    return 0xFFU;
}

void MdZ80::mem_write(uint16_t addr, uint8_t val, Bus* bus) noexcept {
    if (addr < 0x4000U) {
        ram_[addr & 0x1FFFU] = val;
        return;
    }
    if (addr < 0x6000U) {
        if (bus) {
            bus->ym2612().write(static_cast<uint8_t>(addr & 0x03U), val);
        }
        return;
    }
    if (addr < 0x6100U) {
        // 9-bit serial shift register (LSB shifted into bit 8)
        bank_reg_ = static_cast<uint16_t>((bank_reg_ >> 1U) | ((val & 0x01U) << 8U));
        return;
    }
    if (addr == 0x7F11U || addr == 0x7F13U || addr == 0x7F15U || addr == 0x7F17U) {
        if (bus) {
            bus->psg().write(val);
        }
        return;
    }
    if (addr >= 0x8000U && bus) {
        const uint32_t m68k_addr = (static_cast<uint32_t>(bank_reg_) << 15U) | (addr & 0x7FFFU);
        bus->write8(m68k_addr, val);
    }
}

uint16_t MdZ80::mem_read16(uint16_t addr, Bus* bus) noexcept {
    const uint8_t lo = mem_read(addr, bus);
    const uint8_t hi = mem_read(static_cast<uint16_t>(addr + 1U), bus);
    return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
}

void MdZ80::mem_write16(uint16_t addr, uint16_t val, Bus* bus) noexcept {
    mem_write(addr, static_cast<uint8_t>(val & 0xFFU), bus);
    mem_write(static_cast<uint16_t>(addr + 1U), static_cast<uint8_t>(val >> 8U), bus);
}

void MdZ80::step(uint32_t z80_cycles, Bus* bus) noexcept {
    if (!is_running()) {
        return;
    }
    cycle_debt_ += static_cast<int32_t>(z80_cycles);
    while (cycle_debt_ > 0) {
        if (irq_line_ && iff1_ && ei_delay_ == 0U) {
            halted_ = 0;
            iff1_ = 0;
            iff2_ = 0;
            sp_ = static_cast<uint16_t>(sp_ - 2U);
            mem_write16(sp_, pc_, bus);
            pc_ =
                (im_ == 2U) ? mem_read16(static_cast<uint16_t>((i_ << 8U) | 0xFFU), bus) : 0x0038U;
            cycle_debt_ -= 13;
            continue;
        }
        if (halted_) {
            cycle_debt_ -= 4;
            continue;
        }
        const bool had_ei = (ei_delay_ != 0U);
        const uint32_t spent = exec_one(bus);
        if (had_ei) {
            ei_delay_ = 0;
            iff1_ = 1;
            iff2_ = 1;
        }
        cycle_debt_ -= static_cast<int32_t>(spent);
    }
}

uint32_t MdZ80::exec_cb(Bus* bus) noexcept {
    const uint8_t op = mem_read(pc_++, bus);
    const uint8_t reg_idx = op & 0x07U;
    const uint16_t hl = static_cast<uint16_t>((h_ << 8U) | l_);

    auto get_r = [&]() -> uint8_t {
        switch (reg_idx) {
            case 0:
                return b_;
            case 1:
                return c_;
            case 2:
                return d_;
            case 3:
                return e_;
            case 4:
                return h_;
            case 5:
                return l_;
            case 6:
                return mem_read(hl, bus);
            default:
                return a_;
        }
    };
    auto set_r = [&](uint8_t v) {
        switch (reg_idx) {
            case 0:
                b_ = v;
                break;
            case 1:
                c_ = v;
                break;
            case 2:
                d_ = v;
                break;
            case 3:
                e_ = v;
                break;
            case 4:
                h_ = v;
                break;
            case 5:
                l_ = v;
                break;
            case 6:
                mem_write(hl, v, bus);
                break;
            default:
                a_ = v;
                break;
        }
    };

    uint8_t v = get_r();
    const uint8_t grp = op >> 6U;
    const uint8_t bit = (op >> 3U) & 0x07U;

    if (grp == 0U) {
        uint8_t carry = 0;
        switch (bit) {
            case 0:
                carry = v >> 7U;
                v = static_cast<uint8_t>((v << 1U) | carry);
                break;
            case 1:
                carry = v & 1U;
                v = static_cast<uint8_t>((v >> 1U) | (carry << 7U));
                break;
            case 2:
                carry = v >> 7U;
                v = static_cast<uint8_t>((v << 1U) | (f_ & kC));
                break;
            case 3:
                carry = v & 1U;
                v = static_cast<uint8_t>((v >> 1U) | ((f_ & kC) << 7U));
                break;
            case 4:
                carry = v >> 7U;
                v = static_cast<uint8_t>(v << 1U);
                break;
            case 5:
                carry = v & 1U;
                v = static_cast<uint8_t>((v >> 1U) | (v & 0x80U));
                break;
            case 6:
                carry = v >> 7U;
                v = static_cast<uint8_t>((v << 1U) | 1U);
                break;
            default:
                carry = v & 1U;
                v = static_cast<uint8_t>(v >> 1U);
                break;
        }
        f_ = static_cast<uint8_t>(szp(v) | carry);
        set_r(v);
    } else if (grp == 1U) {
        const uint8_t res = static_cast<uint8_t>(v & (1U << bit));
        f_ = static_cast<uint8_t>((f_ & kC) | kH | (res == 0U ? (kZ | kP) : (res & kS)));
    } else if (grp == 2U) {
        set_r(static_cast<uint8_t>(v & ~(1U << bit)));
    } else {
        set_r(static_cast<uint8_t>(v | (1U << bit)));
    }
    return (reg_idx == 6U) ? (grp == 1U ? 12U : 15U) : 8U;
}

uint32_t MdZ80::exec_ed(Bus* bus) noexcept {
    const uint8_t op = mem_read(pc_++, bus);
    auto get_bc = [&]() { return static_cast<uint16_t>((b_ << 8U) | c_); };
    auto get_de = [&]() { return static_cast<uint16_t>((d_ << 8U) | e_); };
    auto get_hl = [&]() { return static_cast<uint16_t>((h_ << 8U) | l_); };
    auto set_bc = [&](uint16_t v) {
        b_ = static_cast<uint8_t>(v >> 8U);
        c_ = static_cast<uint8_t>(v);
    };
    auto set_de = [&](uint16_t v) {
        d_ = static_cast<uint8_t>(v >> 8U);
        e_ = static_cast<uint8_t>(v);
    };
    auto set_hl = [&](uint16_t v) {
        h_ = static_cast<uint8_t>(v >> 8U);
        l_ = static_cast<uint8_t>(v);
    };

    switch (op) {
        case 0x43U:
            mem_write16(mem_read16(pc_, bus), get_bc(), bus);
            pc_ += 2;
            return 20;
        case 0x53U:
            mem_write16(mem_read16(pc_, bus), get_de(), bus);
            pc_ += 2;
            return 20;
        case 0x63U:
            mem_write16(mem_read16(pc_, bus), get_hl(), bus);
            pc_ += 2;
            return 20;
        case 0x73U:
            mem_write16(mem_read16(pc_, bus), sp_, bus);
            pc_ += 2;
            return 20;
        case 0x4BU:
            set_bc(mem_read16(mem_read16(pc_, bus), bus));
            pc_ += 2;
            return 20;
        case 0x5BU:
            set_de(mem_read16(mem_read16(pc_, bus), bus));
            pc_ += 2;
            return 20;
        case 0x6BU:
            set_hl(mem_read16(mem_read16(pc_, bus), bus));
            pc_ += 2;
            return 20;
        case 0x7BU:
            sp_ = mem_read16(mem_read16(pc_, bus), bus);
            pc_ += 2;
            return 20;
        case 0x44U:
        case 0x4CU:
        case 0x54U:
        case 0x5CU:
        case 0x64U:
        case 0x6CU:
        case 0x74U:
        case 0x7CU: {
            const uint8_t old = a_;
            a_ = static_cast<uint8_t>(0U - old);
            f_ = static_cast<uint8_t>(sz(a_) | kN | ((old == 0x80U) ? kP : 0U) |
                                      ((old != 0U) ? kC : 0U) | ((0U - (old & 0x0FU)) & kH));
            return 8;
        }
        case 0x45U:
        case 0x4DU:
        case 0x55U:
        case 0x5DU:
        case 0x65U:
        case 0x6DU:
        case 0x75U:
        case 0x7DU:
            iff1_ = iff2_;
            pc_ = mem_read16(sp_, bus);
            sp_ = static_cast<uint16_t>(sp_ + 2U);
            return 14;
        case 0x46U:
        case 0x4EU:
        case 0x66U:
        case 0x6EU:
            im_ = 0;
            return 8;
        case 0x56U:
        case 0x76U:
            im_ = 1;
            return 8;
        case 0x5EU:
        case 0x7EU:
            im_ = 2;
            return 8;
        case 0x47U:
            i_ = a_;
            return 9;
        case 0x4FU:
            r_ = a_;
            return 9;
        case 0x57U:
            a_ = i_;
            f_ = static_cast<uint8_t>((f_ & kC) | sz(a_) | (iff2_ ? kP : 0U));
            return 9;
        case 0x5FU:
            a_ = r_;
            f_ = static_cast<uint8_t>((f_ & kC) | sz(a_) | (iff2_ ? kP : 0U));
            return 9;
        case 0x42U:
        case 0x52U:
        case 0x62U:
        case 0x72U:
        case 0x4AU:
        case 0x5AU:
        case 0x6AU:
        case 0x7AU: {
            const uint16_t lhs = get_hl();
            const uint16_t rhs = (op < 0x50U)   ? get_bc()
                                 : (op < 0x60U) ? get_de()
                                 : (op < 0x70U) ? lhs
                                                : sp_;
            const uint16_t cy = f_ & kC;
            const bool is_add = (op & 0x08U) != 0U;
            const uint32_t res32 = is_add ? (static_cast<uint32_t>(lhs) + rhs + cy)
                                          : (static_cast<uint32_t>(lhs) - rhs - cy);
            const uint16_t res = static_cast<uint16_t>(res32);
            set_hl(res);
            uint8_t nf = (res & 0x8000U) ? kS : 0U;
            if (res == 0U)
                nf |= kZ;
            if (res32 > 0xFFFFU)
                nf |= kC;
            if (!is_add)
                nf |= kN;
            if (is_add ? (((lhs ^ ~rhs) & (lhs ^ res) & 0x8000U) != 0U)
                       : (((lhs ^ rhs) & (lhs ^ res) & 0x8000U) != 0U)) {
                nf |= kP;
            }
            f_ = nf;
            return 15;
        }
        case 0xA0U:
        case 0xA8U:
        case 0xB0U:
        case 0xB8U: {
            const int16_t delta = (op & 0x08U) ? -1 : 1;
            const uint16_t hl = get_hl();
            const uint16_t de = get_de();
            const uint16_t bc = static_cast<uint16_t>(get_bc() - 1U);
            mem_write(de, mem_read(hl, bus), bus);
            set_hl(static_cast<uint16_t>(hl + delta));
            set_de(static_cast<uint16_t>(de + delta));
            set_bc(bc);
            f_ = static_cast<uint8_t>((f_ & (kS | kZ | kC)) | (bc != 0U ? kP : 0U));
            if ((op & 0x10U) && bc != 0U) {
                pc_ = static_cast<uint16_t>(pc_ - 2U);
                return 21;
            }
            return 16;
        }
        case 0xA1U:
        case 0xA9U:
        case 0xB1U:
        case 0xB9U: {
            const int16_t delta = (op & 0x08U) ? -1 : 1;
            const uint16_t hl = get_hl();
            const uint8_t v = mem_read(hl, bus);
            const uint8_t res = static_cast<uint8_t>(a_ - v);
            const uint16_t bc = static_cast<uint16_t>(get_bc() - 1U);
            set_hl(static_cast<uint16_t>(hl + delta));
            set_bc(bc);
            f_ = static_cast<uint8_t>((f_ & kC) | sz(res) | kN | (bc != 0U ? kP : 0U) |
                                      (((a_ & 0x0FU) < (v & 0x0FU)) ? kH : 0U));
            if ((op & 0x10U) && bc != 0U && res != 0U) {
                pc_ = static_cast<uint16_t>(pc_ - 2U);
                return 21;
            }
            return 16;
        }
        default:
            return 8;
    }
}

uint32_t MdZ80::exec_ixiy(uint16_t& reg, Bus* bus) noexcept {
    const uint8_t op = mem_read(pc_++, bus);
    auto disp_addr = [&]() -> uint16_t {
        const int8_t d = static_cast<int8_t>(mem_read(pc_++, bus));
        return static_cast<uint16_t>(reg + d);
    };
    switch (op) {
        case 0x21U:
            reg = mem_read16(pc_, bus);
            pc_ += 2;
            return 14;
        case 0x22U:
            mem_write16(mem_read16(pc_, bus), reg, bus);
            pc_ += 2;
            return 20;
        case 0x23U:
            ++reg;
            return 10;
        case 0x2AU:
            reg = mem_read16(mem_read16(pc_, bus), bus);
            pc_ += 2;
            return 20;
        case 0x2BU:
            --reg;
            return 10;
        case 0x09U:
        case 0x19U:
        case 0x29U:
        case 0x39U: {
            const uint16_t rhs = (op == 0x09U)   ? static_cast<uint16_t>((b_ << 8U) | c_)
                                 : (op == 0x19U) ? static_cast<uint16_t>((d_ << 8U) | e_)
                                 : (op == 0x29U) ? reg
                                                 : sp_;
            const uint32_t r = static_cast<uint32_t>(reg) + rhs;
            f_ = static_cast<uint8_t>((f_ & (kS | kZ | kP)) | (r > 0xFFFFU ? kC : 0U));
            reg = static_cast<uint16_t>(r);
            return 15;
        }
        case 0x34U: {
            const uint16_t ea = disp_addr();
            const uint8_t old = mem_read(ea, bus);
            const uint8_t res = static_cast<uint8_t>(old + 1U);
            mem_write(ea, res, bus);
            f_ = static_cast<uint8_t>((f_ & kC) | sz(res) | ((old == 0x7FU) ? kP : 0U) |
                                      (((old & 0x0FU) == 0x0FU) ? kH : 0U));
            return 23;
        }
        case 0x35U: {
            const uint16_t ea = disp_addr();
            const uint8_t old = mem_read(ea, bus);
            const uint8_t res = static_cast<uint8_t>(old - 1U);
            mem_write(ea, res, bus);
            f_ = static_cast<uint8_t>((f_ & kC) | sz(res) | kN | ((old == 0x80U) ? kP : 0U) |
                                      (((old & 0x0FU) == 0x00U) ? kH : 0U));
            return 23;
        }
        case 0x36U: {
            const uint16_t ea = disp_addr();
            const uint8_t imm = mem_read(pc_++, bus);
            mem_write(ea, imm, bus);
            return 19;
        }
        case 0x46U:
            b_ = mem_read(disp_addr(), bus);
            return 19;
        case 0x4EU:
            c_ = mem_read(disp_addr(), bus);
            return 19;
        case 0x56U:
            d_ = mem_read(disp_addr(), bus);
            return 19;
        case 0x5EU:
            e_ = mem_read(disp_addr(), bus);
            return 19;
        case 0x66U:
            h_ = mem_read(disp_addr(), bus);
            return 19;
        case 0x6EU:
            l_ = mem_read(disp_addr(), bus);
            return 19;
        case 0x7EU:
            a_ = mem_read(disp_addr(), bus);
            return 19;
        case 0x70U:
            mem_write(disp_addr(), b_, bus);
            return 19;
        case 0x71U:
            mem_write(disp_addr(), c_, bus);
            return 19;
        case 0x72U:
            mem_write(disp_addr(), d_, bus);
            return 19;
        case 0x73U:
            mem_write(disp_addr(), e_, bus);
            return 19;
        case 0x74U:
            mem_write(disp_addr(), h_, bus);
            return 19;
        case 0x75U:
            mem_write(disp_addr(), l_, bus);
            return 19;
        case 0x77U:
            mem_write(disp_addr(), a_, bus);
            return 19;
        case 0x86U:
        case 0x8EU:
        case 0x96U:
        case 0x9EU:
        case 0xA6U:
        case 0xAEU:
        case 0xB6U:
        case 0xBEU: {
            const uint8_t v = mem_read(disp_addr(), bus);
            const uint8_t alu = (op >> 3U) & 0x07U;
            if (alu == 0U || alu == 1U) {
                const uint8_t cy = (alu == 1U) ? (f_ & kC) : 0U;
                const uint16_t r = static_cast<uint16_t>(a_) + v + cy;
                const uint8_t res = static_cast<uint8_t>(r);
                f_ = static_cast<uint8_t>(sz(res) | (r > 0xFFU ? kC : 0U) |
                                          ((((a_ & 0x0FU) + (v & 0x0FU) + cy) > 0x0FU) ? kH : 0U) |
                                          (((a_ ^ ~v) & (a_ ^ res) & 0x80U) ? kP : 0U));
                a_ = res;
            } else if (alu == 2U || alu == 3U || alu == 7U) {
                const uint8_t cy = (alu == 3U) ? (f_ & kC) : 0U;
                const uint16_t r = static_cast<uint16_t>(a_) - v - cy;
                const uint8_t res = static_cast<uint8_t>(r);
                f_ = static_cast<uint8_t>(sz(res) | kN | (r > 0xFFU ? kC : 0U) |
                                          ((((a_ & 0x0FU) - (v & 0x0FU) - cy) & 0x10U) ? kH : 0U) |
                                          (((a_ ^ v) & (a_ ^ res) & 0x80U) ? kP : 0U));
                if (alu != 7U)
                    a_ = res;
            } else if (alu == 4U) {
                a_ &= v;
                f_ = static_cast<uint8_t>(szp(a_) | kH);
            } else if (alu == 5U) {
                a_ ^= v;
                f_ = szp(a_);
            } else {
                a_ |= v;
                f_ = szp(a_);
            }
            return 19;
        }
        case 0xCBU: {
            const uint16_t ea = disp_addr();
            const uint8_t cb_op = mem_read(pc_++, bus);
            uint8_t v = mem_read(ea, bus);
            const uint8_t grp = cb_op >> 6U;
            const uint8_t bit = (cb_op >> 3U) & 0x07U;
            if (grp == 1U) {
                const uint8_t res = static_cast<uint8_t>(v & (1U << bit));
                f_ = static_cast<uint8_t>((f_ & kC) | kH | (res == 0U ? (kZ | kP) : (res & kS)));
                return 20;
            }
            if (grp == 2U) {
                v = static_cast<uint8_t>(v & ~(1U << bit));
            } else if (grp == 3U) {
                v = static_cast<uint8_t>(v | (1U << bit));
            } else {
                uint8_t carry = 0;
                switch (bit) {
                    case 0:
                        carry = v >> 7U;
                        v = static_cast<uint8_t>((v << 1U) | carry);
                        break;
                    case 1:
                        carry = v & 1U;
                        v = static_cast<uint8_t>((v >> 1U) | (carry << 7U));
                        break;
                    case 2:
                        carry = v >> 7U;
                        v = static_cast<uint8_t>((v << 1U) | (f_ & kC));
                        break;
                    case 3:
                        carry = v & 1U;
                        v = static_cast<uint8_t>((v >> 1U) | ((f_ & kC) << 7U));
                        break;
                    case 4:
                        carry = v >> 7U;
                        v = static_cast<uint8_t>(v << 1U);
                        break;
                    case 5:
                        carry = v & 1U;
                        v = static_cast<uint8_t>((v >> 1U) | (v & 0x80U));
                        break;
                    case 6:
                        carry = v >> 7U;
                        v = static_cast<uint8_t>((v << 1U) | 1U);
                        break;
                    default:
                        carry = v & 1U;
                        v = static_cast<uint8_t>(v >> 1U);
                        break;
                }
                f_ = static_cast<uint8_t>(szp(v) | carry);
            }
            mem_write(ea, v, bus);
            return 23;
        }
        case 0xE1U:
            reg = mem_read16(sp_, bus);
            sp_ += 2;
            return 14;
        case 0xE3U: {
            const uint16_t top = mem_read16(sp_, bus);
            mem_write16(sp_, reg, bus);
            reg = top;
            return 23;
        }
        case 0xE5U:
            sp_ -= 2;
            mem_write16(sp_, reg, bus);
            return 15;
        case 0xE9U:
            pc_ = reg;
            return 8;
        case 0xF9U:
            sp_ = reg;
            return 10;
        default:
            return 8;
    }
}

uint32_t MdZ80::exec_one(Bus* bus) noexcept {
    const uint8_t op = mem_read(pc_++, bus);
    r_ = static_cast<uint8_t>((r_ & 0x80U) | ((r_ + 1U) & 0x7FU));

    auto get_bc = [&]() { return static_cast<uint16_t>((b_ << 8U) | c_); };
    auto get_de = [&]() { return static_cast<uint16_t>((d_ << 8U) | e_); };
    auto get_hl = [&]() { return static_cast<uint16_t>((h_ << 8U) | l_); };
    auto set_bc = [&](uint16_t v) {
        b_ = static_cast<uint8_t>(v >> 8U);
        c_ = static_cast<uint8_t>(v);
    };
    auto set_de = [&](uint16_t v) {
        d_ = static_cast<uint8_t>(v >> 8U);
        e_ = static_cast<uint8_t>(v);
    };
    auto set_hl = [&](uint16_t v) {
        h_ = static_cast<uint8_t>(v >> 8U);
        l_ = static_cast<uint8_t>(v);
    };

    auto get_r8 = [&](uint8_t idx) -> uint8_t {
        switch (idx & 7U) {
            case 0:
                return b_;
            case 1:
                return c_;
            case 2:
                return d_;
            case 3:
                return e_;
            case 4:
                return h_;
            case 5:
                return l_;
            case 6:
                return mem_read(get_hl(), bus);
            default:
                return a_;
        }
    };
    auto set_r8 = [&](uint8_t idx, uint8_t v) {
        switch (idx & 7U) {
            case 0:
                b_ = v;
                break;
            case 1:
                c_ = v;
                break;
            case 2:
                d_ = v;
                break;
            case 3:
                e_ = v;
                break;
            case 4:
                h_ = v;
                break;
            case 5:
                l_ = v;
                break;
            case 6:
                mem_write(get_hl(), v, bus);
                break;
            default:
                a_ = v;
                break;
        }
    };
    auto cc_ok = [&](uint8_t cc) -> bool {
        switch (cc & 7U) {
            case 0:
                return (f_ & kZ) == 0U;
            case 1:
                return (f_ & kZ) != 0U;
            case 2:
                return (f_ & kC) == 0U;
            case 3:
                return (f_ & kC) != 0U;
            case 4:
                return (f_ & kP) == 0U;
            case 5:
                return (f_ & kP) != 0U;
            case 6:
                return (f_ & kS) == 0U;
            default:
                return (f_ & kS) != 0U;
        }
    };
    auto do_alu = [&](uint8_t alu, uint8_t v) {
        if (alu == 0U || alu == 1U) {
            const uint8_t cy = (alu == 1U) ? (f_ & kC) : 0U;
            const uint16_t r = static_cast<uint16_t>(a_) + v + cy;
            const uint8_t res = static_cast<uint8_t>(r);
            f_ = static_cast<uint8_t>(sz(res) | (r > 0xFFU ? kC : 0U) |
                                      ((((a_ & 0x0FU) + (v & 0x0FU) + cy) > 0x0FU) ? kH : 0U) |
                                      (((a_ ^ ~v) & (a_ ^ res) & 0x80U) ? kP : 0U));
            a_ = res;
        } else if (alu == 2U || alu == 3U || alu == 7U) {
            const uint8_t cy = (alu == 3U) ? (f_ & kC) : 0U;
            const uint16_t r = static_cast<uint16_t>(a_) - v - cy;
            const uint8_t res = static_cast<uint8_t>(r);
            f_ = static_cast<uint8_t>(sz(res) | kN | (r > 0xFFU ? kC : 0U) |
                                      ((((a_ & 0x0FU) - (v & 0x0FU) - cy) & 0x10U) ? kH : 0U) |
                                      (((a_ ^ v) & (a_ ^ res) & 0x80U) ? kP : 0U));
            if (alu != 7U)
                a_ = res;
        } else if (alu == 4U) {
            a_ &= v;
            f_ = static_cast<uint8_t>(szp(a_) | kH);
        } else if (alu == 5U) {
            a_ ^= v;
            f_ = szp(a_);
        } else {
            a_ |= v;
            f_ = szp(a_);
        }
    };

    if (op >= 0x40U && op <= 0x7FU) {
        if (op == 0x76U) {
            halted_ = 1;
            return 4;
        }
        const uint8_t dst = (op >> 3U) & 7U;
        const uint8_t src = op & 7U;
        set_r8(dst, get_r8(src));
        return (dst == 6U || src == 6U) ? 7U : 4U;
    }
    if (op >= 0x80U && op <= 0xBFU) {
        const uint8_t src = op & 7U;
        do_alu((op >> 3U) & 7U, get_r8(src));
        return (src == 6U) ? 7U : 4U;
    }

    switch (op) {
        case 0x00U:
            return 4;
        case 0x01U:
            set_bc(mem_read16(pc_, bus));
            pc_ += 2;
            return 10;
        case 0x11U:
            set_de(mem_read16(pc_, bus));
            pc_ += 2;
            return 10;
        case 0x21U:
            set_hl(mem_read16(pc_, bus));
            pc_ += 2;
            return 10;
        case 0x31U:
            sp_ = mem_read16(pc_, bus);
            pc_ += 2;
            return 10;
        case 0x02U:
            mem_write(get_bc(), a_, bus);
            return 7;
        case 0x12U:
            mem_write(get_de(), a_, bus);
            return 7;
        case 0x22U:
            mem_write16(mem_read16(pc_, bus), get_hl(), bus);
            pc_ += 2;
            return 16;
        case 0x32U:
            mem_write(mem_read16(pc_, bus), a_, bus);
            pc_ += 2;
            return 13;
        case 0x0AU:
            a_ = mem_read(get_bc(), bus);
            return 7;
        case 0x1AU:
            a_ = mem_read(get_de(), bus);
            return 7;
        case 0x2AU:
            set_hl(mem_read16(mem_read16(pc_, bus), bus));
            pc_ += 2;
            return 16;
        case 0x3AU:
            a_ = mem_read(mem_read16(pc_, bus), bus);
            pc_ += 2;
            return 13;
        case 0x03U:
            set_bc(static_cast<uint16_t>(get_bc() + 1U));
            return 6;
        case 0x13U:
            set_de(static_cast<uint16_t>(get_de() + 1U));
            return 6;
        case 0x23U:
            set_hl(static_cast<uint16_t>(get_hl() + 1U));
            return 6;
        case 0x33U:
            ++sp_;
            return 6;
        case 0x0BU:
            set_bc(static_cast<uint16_t>(get_bc() - 1U));
            return 6;
        case 0x1BU:
            set_de(static_cast<uint16_t>(get_de() - 1U));
            return 6;
        case 0x2BU:
            set_hl(static_cast<uint16_t>(get_hl() - 1U));
            return 6;
        case 0x3BU:
            --sp_;
            return 6;
        case 0x04U:
        case 0x0CU:
        case 0x14U:
        case 0x1CU:
        case 0x24U:
        case 0x2CU:
        case 0x34U:
        case 0x3CU: {
            const uint8_t idx = (op >> 3U) & 7U;
            const uint8_t old = get_r8(idx);
            const uint8_t res = static_cast<uint8_t>(old + 1U);
            set_r8(idx, res);
            f_ = static_cast<uint8_t>((f_ & kC) | sz(res) | ((old == 0x7FU) ? kP : 0U) |
                                      (((old & 0x0FU) == 0x0FU) ? kH : 0U));
            return (idx == 6U) ? 11U : 4U;
        }
        case 0x05U:
        case 0x0DU:
        case 0x15U:
        case 0x1DU:
        case 0x25U:
        case 0x2DU:
        case 0x35U:
        case 0x3DU: {
            const uint8_t idx = (op >> 3U) & 7U;
            const uint8_t old = get_r8(idx);
            const uint8_t res = static_cast<uint8_t>(old - 1U);
            set_r8(idx, res);
            f_ = static_cast<uint8_t>((f_ & kC) | sz(res) | kN | ((old == 0x80U) ? kP : 0U) |
                                      (((old & 0x0FU) == 0x00U) ? kH : 0U));
            return (idx == 6U) ? 11U : 4U;
        }
        case 0x06U:
        case 0x0EU:
        case 0x16U:
        case 0x1EU:
        case 0x26U:
        case 0x2EU:
        case 0x36U:
        case 0x3EU: {
            const uint8_t idx = (op >> 3U) & 7U;
            set_r8(idx, mem_read(pc_++, bus));
            return (idx == 6U) ? 10U : 7U;
        }
        case 0x07U: {
            const uint8_t c = a_ >> 7U;
            a_ = static_cast<uint8_t>((a_ << 1U) | c);
            f_ = static_cast<uint8_t>((f_ & (kS | kZ | kP)) | c);
            return 4;
        }
        case 0x0FU: {
            const uint8_t c = a_ & 1U;
            a_ = static_cast<uint8_t>((a_ >> 1U) | (c << 7U));
            f_ = static_cast<uint8_t>((f_ & (kS | kZ | kP)) | c);
            return 4;
        }
        case 0x17U: {
            const uint8_t c = a_ >> 7U;
            a_ = static_cast<uint8_t>((a_ << 1U) | (f_ & kC));
            f_ = static_cast<uint8_t>((f_ & (kS | kZ | kP)) | c);
            return 4;
        }
        case 0x1FU: {
            const uint8_t c = a_ & 1U;
            a_ = static_cast<uint8_t>((a_ >> 1U) | ((f_ & kC) << 7U));
            f_ = static_cast<uint8_t>((f_ & (kS | kZ | kP)) | c);
            return 4;
        }
        case 0x08U:
            std::swap(a_, a2_);
            std::swap(f_, f2_);
            return 4;
        case 0xD9U:
            std::swap(b_, b2_);
            std::swap(c_, c2_);
            std::swap(d_, d2_);
            std::swap(e_, e2_);
            std::swap(h_, h2_);
            std::swap(l_, l2_);
            return 4;
        case 0x09U:
        case 0x19U:
        case 0x29U:
        case 0x39U: {
            const uint16_t hl = get_hl();
            const uint16_t rhs = (op == 0x09U)   ? get_bc()
                                 : (op == 0x19U) ? get_de()
                                 : (op == 0x29U) ? hl
                                                 : sp_;
            const uint32_t r = static_cast<uint32_t>(hl) + rhs;
            f_ = static_cast<uint8_t>((f_ & (kS | kZ | kP)) | (r > 0xFFFFU ? kC : 0U));
            set_hl(static_cast<uint16_t>(r));
            return 11;
        }
        case 0x10U: {
            const int8_t d = static_cast<int8_t>(mem_read(pc_++, bus));
            if (--b_ != 0U) {
                pc_ = static_cast<uint16_t>(pc_ + d);
                return 13;
            }
            return 8;
        }
        case 0x18U: {
            const int8_t d = static_cast<int8_t>(mem_read(pc_++, bus));
            pc_ = static_cast<uint16_t>(pc_ + d);
            return 12;
        }
        case 0x20U:
        case 0x28U:
        case 0x30U:
        case 0x38U: {
            const int8_t d = static_cast<int8_t>(mem_read(pc_++, bus));
            if (cc_ok((op >> 3U) & 3U)) {
                pc_ = static_cast<uint16_t>(pc_ + d);
                return 12;
            }
            return 7;
        }
        case 0x27U: {
            uint8_t corr = 0;
            uint8_t carry = f_ & kC;
            if ((f_ & kH) || (a_ & 0x0FU) > 9U)
                corr |= 0x06U;
            if (carry || a_ > 0x99U) {
                corr |= 0x60U;
                carry = kC;
            }
            a_ = (f_ & kN) ? static_cast<uint8_t>(a_ - corr) : static_cast<uint8_t>(a_ + corr);
            f_ = static_cast<uint8_t>(szp(a_) | (f_ & kN) | carry);
            return 4;
        }
        case 0x2FU:
            a_ = ~a_;
            f_ |= (kH | kN);
            return 4;
        case 0x37U:
            f_ = static_cast<uint8_t>((f_ & (kS | kZ | kP)) | kC);
            return 4;
        case 0x3FU:
            f_ = static_cast<uint8_t>((f_ & (kS | kZ | kP)) | ((f_ & kC) ? kH : kC));
            return 4;
        case 0xC0U:
        case 0xC8U:
        case 0xD0U:
        case 0xD8U:
        case 0xE0U:
        case 0xE8U:
        case 0xF0U:
        case 0xF8U:
            if (cc_ok((op >> 3U) & 7U)) {
                pc_ = mem_read16(sp_, bus);
                sp_ += 2;
                return 11;
            }
            return 5;
        case 0xC9U:
            pc_ = mem_read16(sp_, bus);
            sp_ += 2;
            return 10;
        case 0xC1U:
            set_bc(mem_read16(sp_, bus));
            sp_ += 2;
            return 10;
        case 0xD1U:
            set_de(mem_read16(sp_, bus));
            sp_ += 2;
            return 10;
        case 0xE1U:
            set_hl(mem_read16(sp_, bus));
            sp_ += 2;
            return 10;
        case 0xF1U: {
            const uint16_t af = mem_read16(sp_, bus);
            sp_ += 2;
            a_ = static_cast<uint8_t>(af >> 8U);
            f_ = static_cast<uint8_t>(af & 0xFFU);
            return 10;
        }
        case 0xC5U:
            sp_ -= 2;
            mem_write16(sp_, get_bc(), bus);
            return 11;
        case 0xD5U:
            sp_ -= 2;
            mem_write16(sp_, get_de(), bus);
            return 11;
        case 0xE5U:
            sp_ -= 2;
            mem_write16(sp_, get_hl(), bus);
            return 11;
        case 0xF5U:
            sp_ -= 2;
            mem_write16(sp_, static_cast<uint16_t>((a_ << 8U) | f_), bus);
            return 11;
        case 0xC2U:
        case 0xCAU:
        case 0xD2U:
        case 0xDAU:
        case 0xE2U:
        case 0xEAU:
        case 0xF2U:
        case 0xFAU: {
            const uint16_t tgt = mem_read16(pc_, bus);
            pc_ += 2;
            if (cc_ok((op >> 3U) & 7U))
                pc_ = tgt;
            return 10;
        }
        case 0xC3U:
            pc_ = mem_read16(pc_, bus);
            return 10;
        case 0xC4U:
        case 0xCCU:
        case 0xD4U:
        case 0xDCU:
        case 0xE4U:
        case 0xECU:
        case 0xF4U:
        case 0xFCU: {
            const uint16_t tgt = mem_read16(pc_, bus);
            pc_ += 2;
            if (cc_ok((op >> 3U) & 7U)) {
                sp_ -= 2;
                mem_write16(sp_, pc_, bus);
                pc_ = tgt;
                return 17;
            }
            return 10;
        }
        case 0xCDU: {
            const uint16_t tgt = mem_read16(pc_, bus);
            pc_ += 2;
            sp_ -= 2;
            mem_write16(sp_, pc_, bus);
            pc_ = tgt;
            return 17;
        }
        case 0xC6U:
        case 0xCEU:
        case 0xD6U:
        case 0xDEU:
        case 0xE6U:
        case 0xEEU:
        case 0xF6U:
        case 0xFEU:
            do_alu((op >> 3U) & 7U, mem_read(pc_++, bus));
            return 7;
        case 0xC7U:
        case 0xCFU:
        case 0xD7U:
        case 0xDFU:
        case 0xE7U:
        case 0xEFU:
        case 0xF7U:
        case 0xFFU:
            sp_ -= 2;
            mem_write16(sp_, pc_, bus);
            pc_ = static_cast<uint16_t>(op & 0x38U);
            return 11;
        case 0xCBU:
            return exec_cb(bus);
        case 0xDDU:
            return exec_ixiy(ix_, bus);
        case 0xEDU:
            return exec_ed(bus);
        case 0xFDU:
            return exec_ixiy(iy_, bus);
        case 0xD3U:
            ++pc_;
            return 11;  // OUT (n),A (no-op on Genesis)
        case 0xDBU:
            ++pc_;
            a_ = 0xFFU;
            return 11;  // IN A,(n)
        case 0xE3U: {
            const uint16_t top = mem_read16(sp_, bus);
            mem_write16(sp_, get_hl(), bus);
            set_hl(top);
            return 19;
        }
        case 0xE9U:
            pc_ = get_hl();
            return 4;
        case 0xEBU:
            std::swap(d_, h_);
            std::swap(e_, l_);
            return 4;
        case 0xF3U:
            iff1_ = 0;
            iff2_ = 0;
            ei_delay_ = 0;
            return 4;
        case 0xF9U:
            sp_ = get_hl();
            return 6;
        case 0xFBU:
            ei_delay_ = 1;
            return 4;
        default:
            return 4;
    }
}

void MdZ80::save_snapshot(Snapshot& s) const noexcept {
    s.ram = ram_;
    s.a = a_;
    s.f = f_;
    s.b = b_;
    s.c = c_;
    s.d = d_;
    s.e = e_;
    s.h = h_;
    s.l = l_;
    s.a2 = a2_;
    s.f2 = f2_;
    s.b2 = b2_;
    s.c2 = c2_;
    s.d2 = d2_;
    s.e2 = e2_;
    s.h2 = h2_;
    s.l2 = l2_;
    s.ix = ix_;
    s.iy = iy_;
    s.sp = sp_;
    s.pc = pc_;
    s.i = i_;
    s.r = r_;
    s.iff1 = iff1_;
    s.iff2 = iff2_;
    s.im = im_;
    s.ei_delay = ei_delay_;
    s.halted = halted_;
    s.bank_reg = bank_reg_;
    s.bus_requested = bus_requested_ ? 1U : 0U;
    s.in_reset = in_reset_ ? 1U : 0U;
    s.irq_line = irq_line_ ? 1U : 0U;
}

void MdZ80::load_snapshot(const Snapshot& s) noexcept {
    ram_ = s.ram;
    a_ = s.a;
    f_ = s.f;
    b_ = s.b;
    c_ = s.c;
    d_ = s.d;
    e_ = s.e;
    h_ = s.h;
    l_ = s.l;
    a2_ = s.a2;
    f2_ = s.f2;
    b2_ = s.b2;
    c2_ = s.c2;
    d2_ = s.d2;
    e2_ = s.e2;
    h2_ = s.h2;
    l2_ = s.l2;
    ix_ = s.ix;
    iy_ = s.iy;
    sp_ = s.sp;
    pc_ = s.pc;
    i_ = s.i;
    r_ = s.r;
    iff1_ = s.iff1;
    iff2_ = s.iff2;
    im_ = s.im;
    ei_delay_ = s.ei_delay;
    halted_ = s.halted;
    bank_reg_ = s.bank_reg;
    bus_requested_ = s.bus_requested != 0U;
    in_reset_ = s.in_reset != 0U;
    irq_line_ = s.irq_line != 0U;
    cycle_debt_ = 0;
}

}  // namespace dragon::md
