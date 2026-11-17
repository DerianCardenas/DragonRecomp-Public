#include "gba/interpreter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dragon::gba {

void cpu_init(CpuState& cpu, Bus* bus) noexcept {
    std::memset(&cpu, 0, sizeof(CpuState));
    cpu.bus = bus;

    // Post-BIOS standard register state (GBATEK)
    cpu.r[13] = 0x03007F00U;
    cpu.r13_usr = 0x03007F00U;
    cpu.r13_irq = 0x03007FA0U;
    cpu.r13_svc = 0x03007FE0U;
    cpu.r[14] = 0x08000000U;
    cpu.r[15] = 0x08000000U;
    cpu.cpsr = GBA_MODE_SYS;
    cpu.spsr = GBA_MODE_SYS;
    cpu.next_event = 272ULL;
}

uint32_t cpu_check_interrupts(CpuState& cpu) noexcept {
    auto* bus = static_cast<Bus*>(cpu.bus);
    if (!bus) {
        return 0U;
    }

    if (cpu.halted && bus->has_wake_irq()) {
        cpu.halted = 0U;
    }

    if (!bus->has_pending_irq() || cpu.i_disable != 0U) {
        return 0U;
    }

    const uint32_t old_cpsr = gba_pack_cpsr(&cpu);
    const uint32_t return_pc = cpu.r[15] + 4U;

    gba_switch_mode(&cpu, GBA_MODE_IRQ);
    cpu.spsr = old_cpsr;
    cpu.spsr_irq = old_cpsr;
    cpu.r[14] = return_pc;
    cpu.i_disable = 1U;
    cpu.thumb = 0U;
    cpu.cpsr = (cpu.cpsr & ~GBA_CPSR_T) | GBA_CPSR_I;
    // Jump directly to BIOS IRQ entry at $00000128
    cpu.r[15] = 0x00000128U;
    cpu.cycles += 7ULL;
    return 7U;
}

void cpu_handle_swi(CpuState& cpu, uint8_t swi_num) noexcept {
    auto* bus = static_cast<Bus*>(cpu.bus);
    if (!bus) {
        return;
    }

    switch (swi_num) {
        case 0x00: {  // SoftReset
            const uint8_t flag = bus->read8(0x03007FFAU);
            for (uint32_t a = 0x03007E00U; a < 0x03008000U; ++a) {
                bus->write8(a, 0U);
            }
            cpu.r[13] = 0x03007F00U;
            cpu.r13_usr = 0x03007F00U;
            cpu.r13_irq = 0x03007FA0U;
            cpu.r13_svc = 0x03007FE0U;
            cpu.thumb = 0U;
            cpu.cpsr = GBA_MODE_SYS;
            cpu.r[15] = (flag != 0U) ? 0x02000000U : 0x08000000U;
            break;
        }
        case 0x01: {  // RegisterRamReset
            const uint32_t flags = cpu.r[0];
            if (flags & 0x01U) {
                for (uint32_t a = 0x02000000U; a < 0x02040000U; a += 4U)
                    bus->write32(a, 0U);
            }
            if (flags & 0x02U) {
                // Clear IWRAM except $03007E00-$03007FFF
                for (uint32_t a = 0x03000000U; a < 0x03007E00U; a += 4U)
                    bus->write32(a, 0U);
            }
            if (flags & 0x04U) {
                bus->ppu().palette_ram().fill(0U);
            }
            if (flags & 0x08U) {
                bus->ppu().vram().fill(0U);
            }
            if (flags & 0x10U) {
                bus->ppu().oam().fill(0U);
            }
            break;
        }
        case 0x02:  // Halt
        case 0x03:  // Stop
            cpu.halted = 1U;
            break;
        case 0x04:    // IntrWait(r0 = discard_old, r1 = mask)
        case 0x05: {  // VBlankIntrWait() -> IntrWait(1, 1)
            const uint32_t discard = (swi_num == 0x05U) ? 1U : cpu.r[0];
            const uint16_t mask =
                (swi_num == 0x05U) ? 1U : static_cast<uint16_t>(cpu.r[1] & 0xFFFFU);
            if (discard != 0U) {
                uint16_t cur = bus->read16(0x03007FF8U);
                cur &= static_cast<uint16_t>(~mask);
                bus->write16(0x03007FF8U, cur);
                bus->consume_acknowledged_irqs();
            }
            // Ensure IME is enabled while waiting for interrupt
            bus->write16(0x04000208U, 1U);
            cpu.i_disable = 0U;
            cpu.cpsr &= ~GBA_CPSR_I;

            // Advance hardware until the requested interrupt is serviced (or 1 frame elapses)
            const uint64_t start_cycles = cpu.cycles;
            while ((cpu.cycles - start_cycles) < (Ppu::kCyclesPerFrame * 2ULL)) {
                uint16_t check_flags = bus->read16(0x03007FF8U) | bus->consume_acknowledged_irqs();
                if ((check_flags & mask) != 0U) {
                    bus->write16(0x03007FF8U, static_cast<uint16_t>(check_flags & ~mask));
                    break;
                }
                cpu.halted = 1U;
                cpu.cycles += 272ULL;
                bus->sync_hardware(cpu.cycles);
                if (bus->ppu().consume_frame_complete()) {
                    bus->on_frame_complete(&cpu);
                    if (cpu.halted && (cpu.cycles - start_cycles) > Ppu::kCyclesPerFrame &&
                        (bus->ie() & mask) == 0U) {
                        break;
                    }
                }
                if (bus->has_pending_irq() && cpu.i_disable == 0U) {
                    cpu.halted = 0U;
                    gba_dispatch_irq(&cpu);
                }
            }
            cpu.halted = 0U;
            break;
        }
        case 0x06: {  // Div(r0 = num, r1 = den)
            const int32_t num = static_cast<int32_t>(cpu.r[0]);
            const int32_t den = static_cast<int32_t>(cpu.r[1]);
            if (den == 0) {
                cpu.r[0] = (num < 0) ? 1U : 0xFFFFFFFFU;
                cpu.r[1] = static_cast<uint32_t>(num);
                cpu.r[3] = 1U;
            } else if (num == (-2147483647 - 1) && den == -1) {
                cpu.r[0] = 0x80000000U;
                cpu.r[1] = 0U;
                cpu.r[3] = 0x80000000U;
            } else {
                const int32_t q = num / den;
                const int32_t r = num % den;
                cpu.r[0] = static_cast<uint32_t>(q);
                cpu.r[1] = static_cast<uint32_t>(r);
                cpu.r[3] = static_cast<uint32_t>(q < 0 ? -q : q);
            }
            cpu.cycles += 40ULL;
            break;
        }
        case 0x07: {  // DivArm(r0 = den, r1 = num)
            const int32_t den = static_cast<int32_t>(cpu.r[0]);
            const int32_t num = static_cast<int32_t>(cpu.r[1]);
            if (den == 0) {
                cpu.r[0] = (num < 0) ? 1U : 0xFFFFFFFFU;
                cpu.r[1] = static_cast<uint32_t>(num);
                cpu.r[3] = 1U;
            } else if (num == (-2147483647 - 1) && den == -1) {
                cpu.r[0] = 0x80000000U;
                cpu.r[1] = 0U;
                cpu.r[3] = 0x80000000U;
            } else {
                const int32_t q = num / den;
                const int32_t r = num % den;
                cpu.r[0] = static_cast<uint32_t>(q);
                cpu.r[1] = static_cast<uint32_t>(r);
                cpu.r[3] = static_cast<uint32_t>(q < 0 ? -q : q);
            }
            cpu.cycles += 40ULL;
            break;
        }
        case 0x08: {  // Sqrt(r0)
            const uint32_t val = cpu.r[0];
            uint32_t res = 0U;
            uint32_t bit = 1U << 30U;
            uint32_t n = val;
            while (bit > n) {
                bit >>= 2U;
            }
            while (bit != 0U) {
                if (n >= res + bit) {
                    n -= res + bit;
                    res = (res >> 1U) + bit;
                } else {
                    res >>= 1U;
                }
                bit >>= 2U;
            }
            cpu.r[0] = res;
            cpu.cycles += 30ULL;
            break;
        }
        case 0x09: {  // ArcTan(r0)
            const int32_t t = static_cast<int16_t>(cpu.r[0] & 0xFFFFU);
            const int32_t a = -((t * t) >> 14);
            int32_t b = ((0xA9 * a) >> 14) + 0x390;
            b = ((b * a) >> 14) + 0x91C;
            b = ((b * a) >> 14) + 0xFB6;
            b = ((b * a) >> 14) + 0x16AA;
            b = ((b * a) >> 14) + 0x2081;
            b = ((b * a) >> 14) + 0x3651;
            b = ((b * a) >> 14) + 0xA2F9;
            cpu.r[0] = static_cast<uint32_t>((t * b) >> 16);
            break;
        }
        case 0x0A: {  // ArcTan2(r0 = x, r1 = y)
            const int16_t x = static_cast<int16_t>(cpu.r[0] & 0xFFFFU);
            const int16_t y = static_cast<int16_t>(cpu.r[1] & 0xFFFFU);
            if (y == 0) {
                cpu.r[0] = (x >= 0) ? 0U : 0x8000U;
            } else if (x == 0) {
                cpu.r[0] = (y >= 0) ? 0x4000U : 0xC000U;
            } else {
                const double angle = std::atan2(static_cast<double>(y), static_cast<double>(x));
                int32_t res =
                    static_cast<int32_t>((angle / (2.0 * 3.14159265358979323846)) * 65536.0);
                if (res < 0) {
                    res += 65536;
                }
                cpu.r[0] = static_cast<uint32_t>(res & 0xFFFF);
            }
            break;
        }
        case 0x0B: {  // CpuSet(r0 = src, r1 = dst, r2 = ctrl)
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            const uint32_t ctrl = cpu.r[2];
            const uint32_t count = ctrl & 0x001FFFFFU;
            const bool fixed = (ctrl & (1U << 24)) != 0U;
            const bool is_32 = (ctrl & (1U << 26)) != 0U;

            if (is_32) {
                src &= ~3U;
                dst &= ~3U;
                const uint32_t fill_val = bus->read32(src);
                for (uint32_t i = 0; i < count; ++i) {
                    bus->write32(dst, fixed ? fill_val : bus->read32(src));
                    dst += 4U;
                    if (!fixed) {
                        src += 4U;
                    }
                }
            } else {
                src &= ~1U;
                dst &= ~1U;
                const uint16_t fill_val = bus->read16(src);
                for (uint32_t i = 0; i < count; ++i) {
                    bus->write16(dst, fixed ? fill_val : bus->read16(src));
                    dst += 2U;
                    if (!fixed) {
                        src += 2U;
                    }
                }
            }
            cpu.cycles += count * 2ULL;
            break;
        }
        case 0x0C: {  // CpuFastSet(r0 = src, r1 = dst, r2 = ctrl)
            uint32_t src = cpu.r[0] & ~3U;
            uint32_t dst = cpu.r[1] & ~3U;
            const uint32_t ctrl = cpu.r[2];
            const uint32_t count = ((ctrl & 0x001FFFFFU) + 7U) & ~7U;
            const bool fixed = (ctrl & (1U << 24)) != 0U;
            const uint32_t fill_val = bus->read32(src);
            for (uint32_t i = 0; i < count; ++i) {
                bus->write32(dst, fixed ? fill_val : bus->read32(src));
                dst += 4U;
                if (!fixed) {
                    src += 4U;
                }
            }
            cpu.cycles += count * 2ULL;
            break;
        }
        case 0x0D:  // GetBiosChecksum
            cpu.r[0] = 0xBAAE187FU;
            break;
        case 0x0E: {  // BgAffineSet(r0 = src, r1 = dst, r2 = count)
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            const uint32_t count = cpu.r[2];
            for (uint32_t i = 0; i < count; ++i) {
                const int32_t ox = static_cast<int32_t>(bus->read32(src + 0U));
                const int32_t oy = static_cast<int32_t>(bus->read32(src + 4U));
                const int16_t cx = static_cast<int16_t>(bus->read16(src + 8U));
                const int16_t cy = static_cast<int16_t>(bus->read16(src + 10U));
                const int16_t sx = static_cast<int16_t>(bus->read16(src + 12U));
                const int16_t sy = static_cast<int16_t>(bus->read16(src + 14U));
                const uint16_t angle = bus->read16(src + 16U);
                src += 20U;

                const double rad =
                    (static_cast<double>(angle) / 65536.0) * 2.0 * 3.14159265358979323846;
                const double c = std::cos(rad);
                const double s = std::sin(rad);

                const int16_t pa = static_cast<int16_t>(c * static_cast<double>(sx));
                const int16_t pb = static_cast<int16_t>(-s * static_cast<double>(sx));
                const int16_t pc = static_cast<int16_t>(s * static_cast<double>(sy));
                const int16_t pd = static_cast<int16_t>(c * static_cast<double>(sy));

                const int32_t rx = ox - (pa * cx + pb * cy);
                const int32_t ry = oy - (pc * cx + pd * cy);

                bus->write16(dst + 0U, static_cast<uint16_t>(pa));
                bus->write16(dst + 2U, static_cast<uint16_t>(pb));
                bus->write16(dst + 4U, static_cast<uint16_t>(pc));
                bus->write16(dst + 6U, static_cast<uint16_t>(pd));
                bus->write32(dst + 8U, static_cast<uint32_t>(rx));
                bus->write32(dst + 12U, static_cast<uint32_t>(ry));
                dst += 16U;
            }
            break;
        }
        case 0x0F: {  // ObjAffineSet(r0 = src, r1 = dst, r2 = count, r3 = stride)
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            const uint32_t count = cpu.r[2];
            const uint32_t stride = cpu.r[3];
            for (uint32_t i = 0; i < count; ++i) {
                const int16_t sx = static_cast<int16_t>(bus->read16(src + 0U));
                const int16_t sy = static_cast<int16_t>(bus->read16(src + 2U));
                const uint16_t angle = bus->read16(src + 4U);
                src += 8U;

                const double rad =
                    (static_cast<double>(angle) / 65536.0) * 2.0 * 3.14159265358979323846;
                const double c = std::cos(rad);
                const double s = std::sin(rad);

                const int16_t pa = static_cast<int16_t>(c * static_cast<double>(sx));
                const int16_t pb = static_cast<int16_t>(-s * static_cast<double>(sx));
                const int16_t pc = static_cast<int16_t>(s * static_cast<double>(sy));
                const int16_t pd = static_cast<int16_t>(c * static_cast<double>(sy));

                bus->write16(dst + stride * 0U, static_cast<uint16_t>(pa));
                bus->write16(dst + stride * 1U, static_cast<uint16_t>(pb));
                bus->write16(dst + stride * 2U, static_cast<uint16_t>(pc));
                bus->write16(dst + stride * 3U, static_cast<uint16_t>(pd));
                dst += stride * 4U;
            }
            break;
        }
        case 0x10: {  // BitUnPack(r0 = src, r1 = dst, r2 = info)
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1] & ~3U;
            const uint32_t info = cpu.r[2];
            const uint16_t src_len = bus->read16(info + 0U);
            const uint8_t src_width = bus->read8(info + 2U);
            const uint8_t dst_width = bus->read8(info + 3U);
            const uint32_t data_offset = bus->read32(info + 4U);
            const bool zero_flag = (data_offset & 0x80000000U) != 0U;
            const uint32_t add_val = data_offset & 0x7FFFFFFFU;

            if (src_width == 0U || dst_width == 0U) {
                break;
            }
            const uint32_t mask = (1U << src_width) - 1U;
            uint32_t out_word = 0U;
            uint32_t out_bits = 0U;
            for (uint16_t i = 0; i < src_len; ++i) {
                const uint8_t in_byte = bus->read8(src + i);
                for (uint8_t b = 0; b < 8U; b += src_width) {
                    uint32_t unit = (in_byte >> b) & mask;
                    if (unit != 0U || zero_flag) {
                        unit += add_val;
                    }
                    out_word |= (unit << out_bits);
                    out_bits += dst_width;
                    if (out_bits >= 32U) {
                        bus->write32(dst, out_word);
                        dst += 4U;
                        out_word = 0U;
                        out_bits = 0U;
                    }
                }
            }
            break;
        }
        case 0x11:    // LZ77UnCompWram
        case 0x12: {  // LZ77UnCompVram
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            const uint32_t header = bus->read32(src);
            src += 4U;
            const uint32_t decomp_size = header >> 8U;
            std::vector<uint8_t> out_buf;
            out_buf.reserve(decomp_size);

            while (out_buf.size() < decomp_size) {
                const uint8_t flags = bus->read8(src++);
                for (int bit = 7; bit >= 0 && out_buf.size() < decomp_size; --bit) {
                    if ((flags & (1 << bit)) == 0) {
                        out_buf.push_back(bus->read8(src++));
                    } else {
                        const uint8_t b1 = bus->read8(src++);
                        const uint8_t b2 = bus->read8(src++);
                        const uint32_t len = (b1 >> 4U) + 3U;
                        const uint32_t disp =
                            (((static_cast<uint32_t>(b1) & 0x0FU) << 8U) | b2) + 1U;
                        for (uint32_t k = 0; k < len && out_buf.size() < decomp_size; ++k) {
                            const size_t pos =
                                (out_buf.size() >= disp) ? (out_buf.size() - disp) : 0U;
                            out_buf.push_back(out_buf[pos]);
                        }
                    }
                }
            }

            if (swi_num == 0x11U) {
                for (size_t i = 0; i < out_buf.size(); ++i) {
                    bus->write8(dst + static_cast<uint32_t>(i), out_buf[i]);
                }
            } else {
                for (size_t i = 0; i + 1U < out_buf.size(); i += 2U) {
                    const uint16_t hw = static_cast<uint16_t>(
                        out_buf[i] | (static_cast<uint16_t>(out_buf[i + 1U]) << 8U));
                    bus->write16(dst + static_cast<uint32_t>(i), hw);
                }
            }
            cpu.cycles += decomp_size * 4ULL;
            break;
        }
        case 0x13: {  // HuffUnComp(r0 = src, r1 = dst)
            uint32_t src = cpu.r[0] & ~3U;
            uint32_t dst = cpu.r[1] & ~3U;
            const uint32_t header = bus->read32(src);
            src += 4U;
            const uint32_t bit_depth = header & 0x0FU;
            const uint32_t decomp_size = header >> 8U;
            if (bit_depth == 0U || decomp_size == 0U) {
                break;
            }
            const uint8_t tree_size_half = bus->read8(src);
            const uint32_t tree_table_base = src + 1U;
            src += (static_cast<uint32_t>(tree_size_half) + 1U) * 2U;

            uint32_t out_word = 0U;
            uint32_t out_bits = 0U;
            uint32_t bytes_written = 0U;
            uint32_t node_addr = tree_table_base;

            while (bytes_written < decomp_size) {
                const uint32_t bit_stream = bus->read32(src);
                src += 4U;
                for (int b = 31; b >= 0 && bytes_written < decomp_size; --b) {
                    const uint32_t bit = (bit_stream >> b) & 1U;
                    const uint8_t node = bus->read8(node_addr);
                    const bool is_leaf = (node & (bit ? 0x40U : 0x80U)) != 0U;
                    const uint32_t next_base = (node_addr & ~1U) + ((node & 0x3FU) + 1U) * 2U;
                    node_addr = next_base + bit;
                    if (is_leaf) {
                        const uint8_t symbol = bus->read8(node_addr);
                        out_word |= (static_cast<uint32_t>(symbol) & ((1U << bit_depth) - 1U))
                                    << out_bits;
                        out_bits += bit_depth;
                        if (out_bits >= 32U) {
                            bus->write32(dst, out_word);
                            dst += 4U;
                            bytes_written += 4U;
                            out_word = 0U;
                            out_bits = 0U;
                        }
                        node_addr = tree_table_base;
                    }
                }
            }
            cpu.cycles += decomp_size * 4ULL;
            break;
        }
        case 0x14:    // RLUnCompWram
        case 0x15: {  // RLUnCompVram
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            const uint32_t header = bus->read32(src);
            src += 4U;
            const uint32_t decomp_size = header >> 8U;
            std::vector<uint8_t> out_buf;
            out_buf.reserve(decomp_size);

            while (out_buf.size() < decomp_size) {
                const uint8_t flag = bus->read8(src++);
                if (flag & 0x80U) {
                    const uint32_t len = (flag & 0x7FU) + 3U;
                    const uint8_t val = bus->read8(src++);
                    for (uint32_t i = 0; i < len && out_buf.size() < decomp_size; ++i) {
                        out_buf.push_back(val);
                    }
                } else {
                    const uint32_t len = (flag & 0x7FU) + 1U;
                    for (uint32_t i = 0; i < len && out_buf.size() < decomp_size; ++i) {
                        out_buf.push_back(bus->read8(src++));
                    }
                }
            }

            if (swi_num == 0x14U) {
                for (size_t i = 0; i < out_buf.size(); ++i) {
                    bus->write8(dst + static_cast<uint32_t>(i), out_buf[i]);
                }
            } else {
                for (size_t i = 0; i + 1U < out_buf.size(); i += 2U) {
                    const uint16_t hw = static_cast<uint16_t>(
                        out_buf[i] | (static_cast<uint16_t>(out_buf[i + 1U]) << 8U));
                    bus->write16(dst + static_cast<uint32_t>(i), hw);
                }
            }
            break;
        }
        case 0x19:  // SoundBias
            bus->write16(0x04000088U, cpu.r[0] ? 0x0200U : 0x0000U);
            break;
        case 0x1F: {  // MidiKey2Freq(r0 = wave_data, r1 = mk, r2 = fp)
            const uint32_t freq = bus->read32(cpu.r[0] + 4U);
            const double mk = static_cast<double>(cpu.r[1] & 0xFFU);
            const double fp = static_cast<double>(cpu.r[2] & 0xFFU) / 256.0;
            const double factor = std::pow(2.0, (180.0 - mk - fp) / 12.0);
            cpu.r[0] = static_cast<uint32_t>(static_cast<double>(freq) / factor);
            break;
        }
        default:
            break;
    }
}

namespace {

uint32_t arm_reg_read(const CpuState& cpu, uint8_t reg, uint32_t inst_pc) noexcept {
    if (reg == 15U) {
        return inst_pc + 8U;
    }
    return cpu.r[reg];
}

void step_arm(CpuState& cpu, Bus& bus, uint32_t pc, uint32_t inst) noexcept {
    const uint8_t cond = static_cast<uint8_t>((inst >> 28U) & 0x0FU);
    if (!gba_check_cond(&cpu, cond)) {
        cpu.cycles += 1ULL;
        return;
    }

    // 1. Branch and Exchange (BX Rm): 0001 0010 1111 1111 1111 0001 Rm
    if ((inst & 0x0FFFFFF0U) == 0x012FFF10U) {
        const uint8_t rm = static_cast<uint8_t>(inst & 0x0FU);
        const uint32_t target = arm_reg_read(cpu, rm, pc);
        gba_bx(&cpu, target);
        cpu.cycles += 3ULL;
        return;
    }

    // 2. Branch / Branch with Link (B / BL): 101L offset24
    if (((inst >> 25U) & 7U) == 5U) {
        const bool link = (inst & (1U << 24)) != 0U;
        int32_t offset = static_cast<int32_t>(inst & 0x00FFFFFFU);
        if (offset & 0x00800000) {
            offset |= static_cast<int32_t>(0xFF000000U);
        }
        offset <<= 2;
        if (link) {
            cpu.r[14] = pc + 4U;
        }
        cpu.r[15] = static_cast<uint32_t>(static_cast<int32_t>(pc + 8U) + offset) & ~3U;
        cpu.cycles += 3ULL;
        return;
    }

    // 3. Software Interrupt (SWI): 1111 xxxx
    if (((inst >> 24U) & 0x0FU) == 0x0FU) {
        const uint8_t swi_num = static_cast<uint8_t>((inst >> 16U) & 0xFFU);
        cpu_handle_swi(cpu, swi_num);
        cpu.cycles += 3ULL;
        return;
    }

    // 4. Single Data Swap (SWP / SWPB): 0001 0B00 Rn Rd 0000 1001 Rm
    if ((inst & 0x0FB00FF0U) == 0x01000090U) {
        const bool byte_swap = (inst & (1U << 22)) != 0U;
        const uint8_t rn = static_cast<uint8_t>((inst >> 16U) & 0x0FU);
        const uint8_t rd = static_cast<uint8_t>((inst >> 12U) & 0x0FU);
        const uint8_t rm = static_cast<uint8_t>(inst & 0x0FU);
        const uint32_t addr = cpu.r[rn];
        const uint32_t src_val = cpu.r[rm];
        if (byte_swap) {
            const uint8_t old_val = bus.read8(addr);
            bus.write8(addr, static_cast<uint8_t>(src_val & 0xFFU));
            cpu.r[rd] = old_val;
        } else {
            const uint32_t old_val = gba_ldr32(&cpu, addr);
            bus.write32(addr, src_val);
            cpu.r[rd] = old_val;
        }
        cpu.cycles += 4ULL;
        return;
    }

    // 5. Multiply / Multiply-Accumulate (MUL / MLA): 0000 00AS Rd Rn Rs 1001 Rm
    if ((inst & 0x0FC000F0U) == 0x00000090U) {
        const bool accum = (inst & (1U << 21)) != 0U;
        const bool set_flags = (inst & (1U << 20)) != 0U;
        const uint8_t rd = static_cast<uint8_t>((inst >> 16U) & 0x0FU);
        const uint8_t rn = static_cast<uint8_t>((inst >> 12U) & 0x0FU);
        const uint8_t rs = static_cast<uint8_t>((inst >> 8U) & 0x0FU);
        const uint8_t rm = static_cast<uint8_t>(inst & 0x0FU);
        uint32_t res = cpu.r[rm] * cpu.r[rs];
        if (accum) {
            res += cpu.r[rn];
        }
        cpu.r[rd] = res;
        if (set_flags) {
            gba_set_nz(&cpu, res);
        }
        cpu.cycles += 3ULL;
        return;
    }

    // 6. Multiply Long (UMULL / UMLAL / SMULL / SMLAL): 0000 1UAS RdHi RdLo Rs 1001 Rm
    if ((inst & 0x0F8000F0U) == 0x00800090U) {
        const bool is_signed = (inst & (1U << 22)) != 0U;
        const bool accum = (inst & (1U << 21)) != 0U;
        const bool set_flags = (inst & (1U << 20)) != 0U;
        const uint8_t rd_hi = static_cast<uint8_t>((inst >> 16U) & 0x0FU);
        const uint8_t rd_lo = static_cast<uint8_t>((inst >> 12U) & 0x0FU);
        const uint8_t rs = static_cast<uint8_t>((inst >> 8U) & 0x0FU);
        const uint8_t rm = static_cast<uint8_t>(inst & 0x0FU);

        uint64_t prod = 0ULL;
        if (is_signed) {
            prod = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(cpu.r[rm])) *
                                         static_cast<int64_t>(static_cast<int32_t>(cpu.r[rs])));
        } else {
            prod = static_cast<uint64_t>(cpu.r[rm]) * static_cast<uint64_t>(cpu.r[rs]);
        }
        if (accum) {
            const uint64_t prev =
                (static_cast<uint64_t>(cpu.r[rd_hi]) << 32U) | static_cast<uint64_t>(cpu.r[rd_lo]);
            prod += prev;
        }
        cpu.r[rd_lo] = static_cast<uint32_t>(prod & 0xFFFFFFFFULL);
        cpu.r[rd_hi] = static_cast<uint32_t>(prod >> 32U);
        if (set_flags) {
            cpu.n = static_cast<uint8_t>((cpu.r[rd_hi] >> 31U) & 1U);
            cpu.z = (prod == 0ULL) ? 1U : 0U;
        }
        cpu.cycles += 4ULL;
        return;
    }

    // 7. Halfword & Signed Data Transfer (LDRH / STRH / LDRSB / LDRSH):
    //    000P UIWL Rn Rd offset_hi 1SH1 offset_lo
    if (((inst >> 25U) & 7U) == 0U && (inst & 0x90U) == 0x90U && (inst & 0x60U) != 0U) {
        const bool pre = (inst & (1U << 24)) != 0U;
        const bool up = (inst & (1U << 23)) != 0U;
        const bool imm = (inst & (1U << 22)) != 0U;
        const bool writeback = (inst & (1U << 21)) != 0U;
        const bool load = (inst & (1U << 20)) != 0U;
        const uint8_t rn = static_cast<uint8_t>((inst >> 16U) & 0x0FU);
        const uint8_t rd = static_cast<uint8_t>((inst >> 12U) & 0x0FU);
        const uint8_t sh = static_cast<uint8_t>((inst >> 5U) & 3U);

        uint32_t offset = 0U;
        if (imm) {
            offset = ((inst >> 4U) & 0xF0U) | (inst & 0x0FU);
        } else {
            offset = arm_reg_read(cpu, static_cast<uint8_t>(inst & 0x0FU), pc);
        }

        const uint32_t base = arm_reg_read(cpu, rn, pc);
        const uint32_t mod_addr = up ? (base + offset) : (base - offset);
        const uint32_t addr = pre ? mod_addr : base;

        if (!pre || writeback) {
            cpu.r[rn] = mod_addr;
        }

        if (load) {
            if (sh == 1U) {
                cpu.r[rd] = gba_ldrh16(&cpu, addr);
            } else if (sh == 2U) {
                cpu.r[rd] = gba_ldrsb8(&cpu, addr);
            } else if (sh == 3U) {
                cpu.r[rd] = gba_ldrsh16(&cpu, addr);
            }
        } else {
            if (sh == 1U) {
                bus.write16(addr, static_cast<uint16_t>(arm_reg_read(cpu, rd, pc) & 0xFFFFU));
            }
        }
        cpu.cycles += 3ULL;
        return;
    }

    // 8. PSR Transfer (MRS / MSR) when opcode in 0x8..0xB and S == 0
    if (((inst >> 26U) & 3U) == 0U && ((inst >> 23U) & 3U) == 2U && (inst & (1U << 20)) == 0U) {
        const bool use_spsr = (inst & (1U << 22)) != 0U;
        if ((inst & 0x003F0FFFU) == 0x000F0000U) {  // MRS Rd, CPSR/SPSR
            const uint8_t rd = static_cast<uint8_t>((inst >> 12U) & 0x0FU);
            cpu.r[rd] = use_spsr ? cpu.spsr : gba_pack_cpsr(&cpu);
        } else {  // MSR CPSR/SPSR, Rm or #imm
            uint32_t op2 = 0U;
            if (inst & (1U << 25)) {
                const uint32_t imm8 = inst & 0xFFU;
                const uint32_t rot = ((inst >> 8U) & 0x0FU) * 2U;
                op2 = gba_ror32(imm8, rot);
            } else {
                op2 = cpu.r[inst & 0x0FU];
            }
            uint32_t mask = 0U;
            if (inst & (1U << 19))
                mask |= 0xFF000000U;
            if (inst & (1U << 18))
                mask |= 0x00FF0000U;
            if (inst & (1U << 17))
                mask |= 0x0000FF00U;
            if (inst & (1U << 16))
                mask |= 0x000000FFU;

            if (use_spsr) {
                cpu.spsr = (cpu.spsr & ~mask) | (op2 & mask);
            } else {
                const uint32_t cur = gba_pack_cpsr(&cpu);
                gba_unpack_cpsr(&cpu, (cur & ~mask) | (op2 & mask));
            }
        }
        cpu.cycles += 1ULL;
        return;
    }

    // 9. Data Processing (00 I opcode S Rn Rd operand2)
    if (((inst >> 26U) & 3U) == 0U) {
        const bool imm = (inst & (1U << 25)) != 0U;
        const uint8_t opcode = static_cast<uint8_t>((inst >> 21U) & 0x0FU);
        const bool set_flags = (inst & (1U << 20)) != 0U;
        const uint8_t rn = static_cast<uint8_t>((inst >> 16U) & 0x0FU);
        const uint8_t rd = static_cast<uint8_t>((inst >> 12U) & 0x0FU);

        uint8_t shifter_carry = cpu.c;
        uint32_t op1 = arm_reg_read(cpu, rn, pc);
        uint32_t op2 = 0U;

        if (imm) {
            const uint32_t imm8 = inst & 0xFFU;
            const uint32_t rot = ((inst >> 8U) & 0x0FU) * 2U;
            op2 = gba_ror32(imm8, rot);
            if (rot != 0U) {
                shifter_carry = static_cast<uint8_t>((op2 >> 31U) & 1U);
            }
        } else {
            const uint8_t rm = static_cast<uint8_t>(inst & 0x0FU);
            const uint8_t shift_type = static_cast<uint8_t>((inst >> 5U) & 3U);
            if (inst & (1U << 4)) {
                // Shift by register: R15 reads as PC + 12
                if (rn == 15U)
                    op1 = pc + 12U;
                const uint32_t rm_val = (rm == 15U) ? (pc + 12U) : cpu.r[rm];
                const uint8_t rs = static_cast<uint8_t>((inst >> 8U) & 0x0FU);
                const uint8_t amt = static_cast<uint8_t>(cpu.r[rs] & 0xFFU);
                op2 = gba_shift_reg(rm_val, shift_type, amt, &shifter_carry);
            } else {
                const uint32_t rm_val = arm_reg_read(cpu, rm, pc);
                const uint8_t imm5 = static_cast<uint8_t>((inst >> 7U) & 0x1FU);
                op2 = gba_shift_imm(rm_val, shift_type, imm5, &shifter_carry);
            }
        }

        uint32_t res = 0U;
        bool write_rd = true;
        const bool logical_flags = set_flags && (rd != 15U);
        const bool arith_flags = set_flags && (rd != 15U);

        switch (opcode) {
            case 0x0:  // AND
                res = op1 & op2;
                if (logical_flags) {
                    gba_set_nz(&cpu, res);
                    cpu.c = shifter_carry;
                }
                break;
            case 0x1:  // EOR
                res = op1 ^ op2;
                if (logical_flags) {
                    gba_set_nz(&cpu, res);
                    cpu.c = shifter_carry;
                }
                break;
            case 0x2:  // SUB
                res = gba_sub(&cpu, op1, op2, arith_flags);
                break;
            case 0x3:  // RSB
                res = gba_sub(&cpu, op2, op1, arith_flags);
                break;
            case 0x4:  // ADD
                res = gba_add(&cpu, op1, op2, arith_flags);
                break;
            case 0x5:  // ADC
                res = gba_adc(&cpu, op1, op2, arith_flags);
                break;
            case 0x6:  // SBC
                res = gba_sbc(&cpu, op1, op2, arith_flags);
                break;
            case 0x7: {  // RSC
                const uint8_t old_c = cpu.c;
                res = gba_sbc(&cpu, op2, op1, arith_flags);
                (void)old_c;
                break;
            }
            case 0x8:  // TST
                res = op1 & op2;
                write_rd = false;
                gba_set_nz(&cpu, res);
                cpu.c = shifter_carry;
                break;
            case 0x9:  // TEQ
                res = op1 ^ op2;
                write_rd = false;
                gba_set_nz(&cpu, res);
                cpu.c = shifter_carry;
                break;
            case 0xA:  // CMP
                (void)gba_sub(&cpu, op1, op2, 1);
                write_rd = false;
                break;
            case 0xB:  // CMN
                (void)gba_add(&cpu, op1, op2, 1);
                write_rd = false;
                break;
            case 0xC:  // ORR
                res = op1 | op2;
                if (logical_flags) {
                    gba_set_nz(&cpu, res);
                    cpu.c = shifter_carry;
                }
                break;
            case 0xD:  // MOV
                res = op2;
                if (logical_flags) {
                    gba_set_nz(&cpu, res);
                    cpu.c = shifter_carry;
                }
                break;
            case 0xE:  // BIC
                res = op1 & ~op2;
                if (logical_flags) {
                    gba_set_nz(&cpu, res);
                    cpu.c = shifter_carry;
                }
                break;
            case 0xF:  // MVN
                res = ~op2;
                if (logical_flags) {
                    gba_set_nz(&cpu, res);
                    cpu.c = shifter_carry;
                }
                break;
        }

        if (write_rd) {
            if (rd == 15U) {
                if (set_flags) {
                    const uint32_t restored = cpu.spsr;
                    gba_unpack_cpsr(&cpu, restored);
                }
                cpu.r[15] = res & (cpu.thumb ? ~1U : ~3U);
            } else {
                cpu.r[rd] = res;
            }
        }
        cpu.cycles += 1ULL;
        return;
    }

    // 10. Single Data Transfer (LDR / STR / LDRB / STRB): 01 I P U B W L Rn Rd offset12
    if (((inst >> 26U) & 3U) == 1U) {
        const bool reg_offset = (inst & (1U << 25)) != 0U;
        const bool pre = (inst & (1U << 24)) != 0U;
        const bool up = (inst & (1U << 23)) != 0U;
        const bool byte_xfer = (inst & (1U << 22)) != 0U;
        const bool writeback = (inst & (1U << 21)) != 0U;
        const bool load = (inst & (1U << 20)) != 0U;
        const uint8_t rn = static_cast<uint8_t>((inst >> 16U) & 0x0FU);
        const uint8_t rd = static_cast<uint8_t>((inst >> 12U) & 0x0FU);

        uint32_t offset = 0U;
        if (!reg_offset) {
            offset = inst & 0x0FFFU;
        } else {
            const uint8_t rm = static_cast<uint8_t>(inst & 0x0FU);
            const uint8_t shift_type = static_cast<uint8_t>((inst >> 5U) & 3U);
            const uint8_t imm5 = static_cast<uint8_t>((inst >> 7U) & 0x1FU);
            uint8_t dummy_c = cpu.c;
            offset = gba_shift_imm(arm_reg_read(cpu, rm, pc), shift_type, imm5, &dummy_c);
        }

        const uint32_t base = arm_reg_read(cpu, rn, pc);
        const uint32_t mod_addr = up ? (base + offset) : (base - offset);
        const uint32_t addr = pre ? mod_addr : base;

        if (!pre || writeback) {
            cpu.r[rn] = mod_addr;
        }

        if (load) {
            const uint32_t val = byte_xfer ? bus.read8(addr) : gba_ldr32(&cpu, addr);
            if (rd == 15U) {
                cpu.r[15] = val & ~3U;
            } else {
                cpu.r[rd] = val;
            }
        } else {
            const uint32_t val = (rd == 15U) ? (pc + 12U) : cpu.r[rd];
            if (byte_xfer) {
                bus.write8(addr, static_cast<uint8_t>(val & 0xFFU));
            } else {
                bus.write32(addr, val);
            }
        }
        cpu.cycles += 3ULL;
        return;
    }

    // 11. Block Data Transfer (LDM / STM): 100 P U S W L Rn reg_list16
    if (((inst >> 25U) & 7U) == 4U) {
        const bool pre = (inst & (1U << 24)) != 0U;
        const bool up = (inst & (1U << 23)) != 0U;
        const bool s_bit = (inst & (1U << 22)) != 0U;
        const bool writeback = (inst & (1U << 21)) != 0U;
        const bool load = (inst & (1U << 20)) != 0U;
        const uint8_t rn = static_cast<uint8_t>((inst >> 16U) & 0x0FU);
        const uint16_t rlist = static_cast<uint16_t>(inst & 0xFFFFU);

        uint32_t base = cpu.r[rn];
        if (rlist == 0U) {
            // ARM7TDMI empty register list quirk: transfers R15 and adjusts base by 0x40
            if (load) {
                cpu.r[15] = bus.read32(base) & ~3U;
            } else {
                bus.write32(base, pc + 12U);
            }
            if (writeback) {
                cpu.r[rn] = up ? (base + 0x40U) : (base - 0x40U);
            }
            cpu.cycles += 4ULL;
            return;
        }

        uint32_t count = 0U;
        for (int i = 0; i < 16; ++i) {
            if (rlist & (1U << i)) {
                ++count;
            }
        }

        uint32_t addr = base;
        const uint32_t wb_val = up ? (base + count * 4U) : (base - count * 4U);
        if (up) {
            if (pre) {
                addr += 4U;
            }
        } else {
            addr -= count * 4U;
            if (!pre) {
                addr += 4U;
            }
        }

        const bool user_bank_transfer = s_bit && (!load || (rlist & 0x8000U) == 0U);
        const uint8_t saved_mode = static_cast<uint8_t>(cpu.cpsr & GBA_CPSR_MODE_MASK);
        if (user_bank_transfer) {
            gba_switch_mode(&cpu, GBA_MODE_SYS);
        }

        if (load) {
            if (writeback && (rlist & (1U << rn)) == 0U) {
                cpu.r[rn] = wb_val;
            }
            for (int i = 0; i < 16; ++i) {
                if (rlist & (1U << i)) {
                    const uint32_t val = bus.read32(addr);
                    addr += 4U;
                    if (i == 15) {
                        if (s_bit) {
                            gba_unpack_cpsr(&cpu, cpu.spsr);
                        }
                        cpu.r[15] = val & (cpu.thumb ? ~1U : ~3U);
                    } else {
                        cpu.r[i] = val;
                    }
                }
            }
        } else {
            bool first = true;
            for (int i = 0; i < 16; ++i) {
                if (rlist & (1U << i)) {
                    uint32_t val = (i == 15) ? (pc + 12U) : cpu.r[i];
                    if (i == rn && writeback && !first) {
                        val = wb_val;
                    }
                    bus.write32(addr, val);
                    addr += 4U;
                    first = false;
                }
            }
            if (writeback) {
                cpu.r[rn] = wb_val;
            }
        }

        if (user_bank_transfer) {
            gba_switch_mode(&cpu, saved_mode);
        }

        cpu.cycles += 2ULL + count;
        return;
    }

    cpu.cycles += 1ULL;
}

void step_thumb(CpuState& cpu, Bus& bus, uint32_t pc, uint16_t inst) noexcept {
    const uint8_t top3 = static_cast<uint8_t>((inst >> 13U) & 7U);

    switch (top3) {
        case 0: {
            const uint8_t op = static_cast<uint8_t>((inst >> 11U) & 3U);
            const uint8_t rs = static_cast<uint8_t>((inst >> 3U) & 7U);
            const uint8_t rd = static_cast<uint8_t>(inst & 7U);
            if (op != 3U) {
                // Format 1: Move shifted register (LSL / LSR / ASR)
                const uint8_t imm5 = static_cast<uint8_t>((inst >> 6U) & 0x1FU);
                uint8_t carry = cpu.c;
                const uint32_t res = gba_shift_imm(cpu.r[rs], op, imm5, &carry);
                cpu.r[rd] = res;
                gba_set_nz(&cpu, res);
                cpu.c = carry;
            } else {
                // Format 2: Add / Subtract register or 3-bit immediate
                const uint8_t sub_op = static_cast<uint8_t>((inst >> 9U) & 3U);
                const uint8_t rn_or_imm = static_cast<uint8_t>((inst >> 6U) & 7U);
                const uint32_t rhs = (sub_op & 2U) ? rn_or_imm : cpu.r[rn_or_imm];
                if (sub_op & 1U) {
                    cpu.r[rd] = gba_sub(&cpu, cpu.r[rs], rhs, 1);
                } else {
                    cpu.r[rd] = gba_add(&cpu, cpu.r[rs], rhs, 1);
                }
            }
            cpu.cycles += 1ULL;
            return;
        }

        case 1: {
            // Format 3: Move / Compare / Add / Subtract 8-bit immediate
            const uint8_t op = static_cast<uint8_t>((inst >> 11U) & 3U);
            const uint8_t rd = static_cast<uint8_t>((inst >> 8U) & 7U);
            const uint32_t imm8 = inst & 0xFFU;
            switch (op) {
                case 0:  // MOV Rd, #imm8
                    cpu.r[rd] = imm8;
                    gba_set_nz(&cpu, imm8);
                    break;
                case 1:  // CMP Rd, #imm8
                    (void)gba_sub(&cpu, cpu.r[rd], imm8, 1);
                    break;
                case 2:  // ADD Rd, #imm8
                    cpu.r[rd] = gba_add(&cpu, cpu.r[rd], imm8, 1);
                    break;
                case 3:  // SUB Rd, #imm8
                    cpu.r[rd] = gba_sub(&cpu, cpu.r[rd], imm8, 1);
                    break;
            }
            cpu.cycles += 1ULL;
            return;
        }

        case 2: {
            if (((inst >> 10U) & 7U) == 0U) {
                // Format 4: ALU operations
                const uint8_t alu_op = static_cast<uint8_t>((inst >> 6U) & 0x0FU);
                const uint8_t rs = static_cast<uint8_t>((inst >> 3U) & 7U);
                const uint8_t rd = static_cast<uint8_t>(inst & 7U);
                const uint32_t a = cpu.r[rd];
                const uint32_t b = cpu.r[rs];
                uint8_t carry = cpu.c;
                switch (alu_op) {
                    case 0x0:
                        cpu.r[rd] = a & b;
                        gba_set_nz(&cpu, cpu.r[rd]);
                        break;
                    case 0x1:
                        cpu.r[rd] = a ^ b;
                        gba_set_nz(&cpu, cpu.r[rd]);
                        break;
                    case 0x2:
                        cpu.r[rd] = gba_shift_reg(a, 0U, static_cast<uint8_t>(b & 0xFFU), &carry);
                        gba_set_nz(&cpu, cpu.r[rd]);
                        cpu.c = carry;
                        break;
                    case 0x3:
                        cpu.r[rd] = gba_shift_reg(a, 1U, static_cast<uint8_t>(b & 0xFFU), &carry);
                        gba_set_nz(&cpu, cpu.r[rd]);
                        cpu.c = carry;
                        break;
                    case 0x4:
                        cpu.r[rd] = gba_shift_reg(a, 2U, static_cast<uint8_t>(b & 0xFFU), &carry);
                        gba_set_nz(&cpu, cpu.r[rd]);
                        cpu.c = carry;
                        break;
                    case 0x5:
                        cpu.r[rd] = gba_adc(&cpu, a, b, 1);
                        break;
                    case 0x6:
                        cpu.r[rd] = gba_sbc(&cpu, a, b, 1);
                        break;
                    case 0x7:
                        cpu.r[rd] = gba_shift_reg(a, 3U, static_cast<uint8_t>(b & 0xFFU), &carry);
                        gba_set_nz(&cpu, cpu.r[rd]);
                        cpu.c = carry;
                        break;
                    case 0x8:
                        gba_set_nz(&cpu, a & b);
                        break;
                    case 0x9:
                        cpu.r[rd] = gba_sub(&cpu, 0U, b, 1);
                        break;
                    case 0xA:
                        (void)gba_sub(&cpu, a, b, 1);
                        break;
                    case 0xB:
                        (void)gba_add(&cpu, a, b, 1);
                        break;
                    case 0xC:
                        cpu.r[rd] = a | b;
                        gba_set_nz(&cpu, cpu.r[rd]);
                        break;
                    case 0xD:
                        cpu.r[rd] = a * b;
                        gba_set_nz(&cpu, cpu.r[rd]);
                        break;
                    case 0xE:
                        cpu.r[rd] = a & ~b;
                        gba_set_nz(&cpu, cpu.r[rd]);
                        break;
                    case 0xF:
                        cpu.r[rd] = ~b;
                        gba_set_nz(&cpu, cpu.r[rd]);
                        break;
                }
                cpu.cycles += 1ULL;
                return;
            }
            if (((inst >> 10U) & 7U) == 1U) {
                // Format 5: Hi-register operations / Branch Exchange
                const uint8_t op = static_cast<uint8_t>((inst >> 8U) & 3U);
                const uint8_t h1 = static_cast<uint8_t>((inst >> 7U) & 1U);
                const uint8_t h2 = static_cast<uint8_t>((inst >> 6U) & 1U);
                const uint8_t rs = static_cast<uint8_t>(((inst >> 3U) & 7U) | (h2 << 3U));
                const uint8_t rd = static_cast<uint8_t>((inst & 7U) | (h1 << 3U));
                const uint32_t src_val = (rs == 15U) ? (pc + 4U) : cpu.r[rs];
                const uint32_t dst_val = (rd == 15U) ? (pc + 4U) : cpu.r[rd];

                switch (op) {
                    case 0: {  // ADD Rd, Rs
                        const uint32_t res = dst_val + src_val;
                        if (rd == 15U) {
                            cpu.r[15] = res & ~1U;
                        } else {
                            cpu.r[rd] = res;
                        }
                        break;
                    }
                    case 1:  // CMP Rd, Rs
                        (void)gba_sub(&cpu, dst_val, src_val, 1);
                        break;
                    case 2:  // MOV Rd, Rs
                        if (rd == 15U) {
                            cpu.r[15] = src_val & ~1U;
                        } else {
                            cpu.r[rd] = src_val;
                        }
                        break;
                    case 3:  // BX Rs
                        gba_bx(&cpu, src_val);
                        break;
                }
                cpu.cycles += 1ULL;
                return;
            }
            if (((inst >> 11U) & 3U) == 1U) {
                // Format 6: PC-relative load LDR Rd, [PC, #imm8*4]
                const uint8_t rd = static_cast<uint8_t>((inst >> 8U) & 7U);
                const uint32_t imm = (inst & 0xFFU) * 4U;
                const uint32_t addr = ((pc + 4U) & ~3U) + imm;
                cpu.r[rd] = gba_ldr32(&cpu, addr);
                cpu.cycles += 3ULL;
                return;
            }
            // Format 7 & 8: Load / store with register offset
            const uint8_t ro = static_cast<uint8_t>((inst >> 6U) & 7U);
            const uint8_t rb = static_cast<uint8_t>((inst >> 3U) & 7U);
            const uint8_t rd = static_cast<uint8_t>(inst & 7U);
            const uint32_t addr = cpu.r[rb] + cpu.r[ro];

            if ((inst & 0x0200U) == 0U) {
                const uint8_t op = static_cast<uint8_t>((inst >> 10U) & 3U);
                switch (op) {
                    case 0:  // STR Rd, [Rb, Ro]
                        bus.write32(addr, cpu.r[rd]);
                        break;
                    case 1:  // STRB Rd, [Rb, Ro]
                        bus.write8(addr, static_cast<uint8_t>(cpu.r[rd] & 0xFFU));
                        break;
                    case 2:  // LDR Rd, [Rb, Ro]
                        cpu.r[rd] = gba_ldr32(&cpu, addr);
                        break;
                    case 3:  // LDRB Rd, [Rb, Ro]
                        cpu.r[rd] = bus.read8(addr);
                        break;
                }
            } else {
                const uint8_t op = static_cast<uint8_t>((inst >> 10U) & 3U);
                switch (op) {
                    case 0:  // STRH Rd, [Rb, Ro]
                        bus.write16(addr, static_cast<uint16_t>(cpu.r[rd] & 0xFFFFU));
                        break;
                    case 1:  // LDSB Rd, [Rb, Ro]
                        cpu.r[rd] = gba_ldrsb8(&cpu, addr);
                        break;
                    case 2:  // LDRH Rd, [Rb, Ro]
                        cpu.r[rd] = gba_ldrh16(&cpu, addr);
                        break;
                    case 3:  // LDSH Rd, [Rb, Ro]
                        cpu.r[rd] = gba_ldrsh16(&cpu, addr);
                        break;
                }
            }
            cpu.cycles += 3ULL;
            return;
        }

        case 3: {
            // Format 9: Load / store with immediate offset (word or byte)
            const uint8_t op = static_cast<uint8_t>((inst >> 11U) & 3U);
            const uint32_t imm5 = (inst >> 6U) & 0x1FU;
            const uint8_t rb = static_cast<uint8_t>((inst >> 3U) & 7U);
            const uint8_t rd = static_cast<uint8_t>(inst & 7U);

            switch (op) {
                case 0:  // STR Rd, [Rb, #imm5*4]
                    bus.write32(cpu.r[rb] + imm5 * 4U, cpu.r[rd]);
                    break;
                case 1:  // LDR Rd, [Rb, #imm5*4]
                    cpu.r[rd] = gba_ldr32(&cpu, cpu.r[rb] + imm5 * 4U);
                    break;
                case 2:  // STRB Rd, [Rb, #imm5]
                    bus.write8(cpu.r[rb] + imm5, static_cast<uint8_t>(cpu.r[rd] & 0xFFU));
                    break;
                case 3:  // LDRB Rd, [Rb, #imm5]
                    cpu.r[rd] = bus.read8(cpu.r[rb] + imm5);
                    break;
            }
            cpu.cycles += 3ULL;
            return;
        }

        case 4: {
            if ((inst & 0x1000U) == 0U) {
                // Format 10: Load / store halfword
                const bool load = (inst & 0x0800U) != 0U;
                const uint32_t imm5 = ((inst >> 6U) & 0x1FU) * 2U;
                const uint8_t rb = static_cast<uint8_t>((inst >> 3U) & 7U);
                const uint8_t rd = static_cast<uint8_t>(inst & 7U);
                const uint32_t addr = cpu.r[rb] + imm5;
                if (load) {
                    cpu.r[rd] = gba_ldrh16(&cpu, addr);
                } else {
                    bus.write16(addr, static_cast<uint16_t>(cpu.r[rd] & 0xFFFFU));
                }
            } else {
                // Format 11: SP-relative load / store
                const bool load = (inst & 0x0800U) != 0U;
                const uint8_t rd = static_cast<uint8_t>((inst >> 8U) & 7U);
                const uint32_t imm = (inst & 0xFFU) * 4U;
                const uint32_t addr = cpu.r[13] + imm;
                if (load) {
                    cpu.r[rd] = gba_ldr32(&cpu, addr);
                } else {
                    bus.write32(addr, cpu.r[rd]);
                }
            }
            cpu.cycles += 3ULL;
            return;
        }

        case 5: {
            if ((inst & 0x1000U) == 0U) {
                // Format 12: Load address ADD Rd, PC/SP, #imm8*4
                const bool use_sp = (inst & 0x0800U) != 0U;
                const uint8_t rd = static_cast<uint8_t>((inst >> 8U) & 7U);
                const uint32_t imm = (inst & 0xFFU) * 4U;
                cpu.r[rd] = (use_sp ? cpu.r[13] : ((pc + 4U) & ~3U)) + imm;
                cpu.cycles += 1ULL;
                return;
            }
            if ((inst & 0x0F00U) == 0x0000U) {
                // Format 13: Add offset to SP
                const uint32_t imm = (inst & 0x7FU) * 4U;
                if (inst & 0x0080U) {
                    cpu.r[13] -= imm;
                } else {
                    cpu.r[13] += imm;
                }
                cpu.cycles += 1ULL;
                return;
            }
            // Format 14: Push / Pop registers
            const bool pop = (inst & 0x0800U) != 0U;
            const bool pclr = (inst & 0x0100U) != 0U;
            const uint8_t rlist = static_cast<uint8_t>(inst & 0xFFU);
            if (!pop) {
                if (pclr) {
                    cpu.r[13] -= 4U;
                    bus.write32(cpu.r[13], cpu.r[14]);
                }
                for (int i = 7; i >= 0; --i) {
                    if (rlist & (1U << i)) {
                        cpu.r[13] -= 4U;
                        bus.write32(cpu.r[13], cpu.r[i]);
                    }
                }
            } else {
                for (int i = 0; i < 8; ++i) {
                    if (rlist & (1U << i)) {
                        cpu.r[i] = bus.read32(cpu.r[13]);
                        cpu.r[13] += 4U;
                    }
                }
                if (pclr) {
                    const uint32_t val = bus.read32(cpu.r[13]);
                    cpu.r[13] += 4U;
                    // On ARMv4T (ARM7TDMI), POP {PC} stays in Thumb state
                    cpu.r[15] = val & ~1U;
                }
            }
            cpu.cycles += 3ULL;
            return;
        }

        case 6: {
            if ((inst & 0x1000U) == 0U) {
                // Format 15: Multiple load / store (LDMIA / STMIA)
                const bool load = (inst & 0x0800U) != 0U;
                const uint8_t rb = static_cast<uint8_t>((inst >> 8U) & 7U);
                const uint8_t rlist = static_cast<uint8_t>(inst & 0xFFU);
                uint32_t addr = cpu.r[rb];
                for (int i = 0; i < 8; ++i) {
                    if (rlist & (1U << i)) {
                        if (load) {
                            cpu.r[i] = bus.read32(addr);
                        } else {
                            bus.write32(addr, cpu.r[i]);
                        }
                        addr += 4U;
                    }
                }
                if (!load || (rlist & (1U << rb)) == 0U) {
                    cpu.r[rb] = addr;
                }
                cpu.cycles += 3ULL;
                return;
            }
            const uint8_t cond = static_cast<uint8_t>((inst >> 8U) & 0x0FU);
            if (cond == 0x0FU) {
                // Format 17: Software Interrupt (SWI)
                cpu_handle_swi(cpu, static_cast<uint8_t>(inst & 0xFFU));
                cpu.cycles += 3ULL;
                return;
            }
            // Format 16: Conditional branch
            if (gba_check_cond(&cpu, cond)) {
                const int8_t rel = static_cast<int8_t>(inst & 0xFFU);
                cpu.r[15] = static_cast<uint32_t>(static_cast<int32_t>(pc + 4U) +
                                                  static_cast<int32_t>(rel) * 2) &
                            ~1U;
                cpu.cycles += 3ULL;
            } else {
                cpu.cycles += 1ULL;
            }
            return;
        }

        case 7: {
            const uint8_t sub = static_cast<uint8_t>((inst >> 11U) & 3U);
            if (sub == 0U) {
                // Format 18: Unconditional branch B label
                int32_t offset = static_cast<int32_t>(inst & 0x07FFU);
                if (offset & 0x0400) {
                    offset |= static_cast<int32_t>(0xFFFFF800U);
                }
                cpu.r[15] = static_cast<uint32_t>(static_cast<int32_t>(pc + 4U) + offset * 2) & ~1U;
                cpu.cycles += 3ULL;
            } else if (sub == 2U) {
                // Format 19 (first half): Long branch with link high offset
                int32_t offset = static_cast<int32_t>(inst & 0x07FFU);
                if (offset & 0x0400) {
                    offset |= static_cast<int32_t>(0xFFFFF800U);
                }
                cpu.r[14] = static_cast<uint32_t>(static_cast<int32_t>(pc + 4U) + (offset << 12));
                cpu.cycles += 1ULL;
            } else if (sub == 3U) {
                // Format 19 (second half): Long branch with link low offset
                const uint32_t offset_lo = (inst & 0x07FFU) << 1U;
                const uint32_t target = (cpu.r[14] + offset_lo) & ~1U;
                cpu.r[14] = (pc + 2U) | 1U;
                cpu.r[15] = target;
                cpu.cycles += 3ULL;
            } else {
                cpu.cycles += 1ULL;
            }
            return;
        }
    }
}

}  // namespace

uint32_t cpu_step(CpuState& cpu) noexcept {
    auto* bus = static_cast<Bus*>(cpu.bus);
    if (!bus) {
        return 0U;
    }

    const uint64_t before = cpu.cycles;

    if (cpu.halted) {
        cpu.cycles += 64ULL;
        bus->sync_hardware(cpu.cycles);
        if (bus->has_pending_irq() && cpu.i_disable == 0U) {
            // Run the full handler in one shot so the caller never observes IRQ mode.
            gba_dispatch_irq(&cpu);
        } else {
            // Wake from Halt/Stop even when IME masks dispatch (IntrWait semantics).
            cpu_check_interrupts(cpu);
        }
        return static_cast<uint32_t>(cpu.cycles - before);
    }

    if (bus->has_pending_irq() && cpu.i_disable == 0U) {
        // Deliver the interrupt and run the handler to completion before stepping
        // the interrupted instruction. Running it to completion (rather than just
        // entering IRQ mode) keeps the processor mode stable across a cpu_step, so
        // the recompiled dispatch loop never mistakes an in-flight handler for a
        // function returning in a different mode.
        gba_dispatch_irq(&cpu);
    }

    const uint32_t pc = cpu.r[15];
    if (cpu.thumb) {
        const uint16_t inst = bus->read16(pc);
        cpu.r[15] = (pc + 2U) & ~1U;
        step_thumb(cpu, *bus, pc, inst);
    } else {
        const uint32_t inst = bus->read32(pc);
        cpu.r[15] = (pc + 4U) & ~3U;
        step_arm(cpu, *bus, pc, inst);
    }

    if (cpu.cycles >= cpu.next_event) {
        bus->sync_hardware(cpu.cycles);
        cpu.next_event = cpu.cycles + 272ULL;
    }

    return static_cast<uint32_t>(cpu.cycles - before);
}

}  // namespace dragon::gba
