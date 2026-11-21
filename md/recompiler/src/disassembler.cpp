#include "mdrecomp/disassembler.h"

#include <queue>

namespace dragon::mdrecomp {

namespace {

uint8_t ea_extra_bytes(uint8_t mode, uint8_t reg, int sz) noexcept {
    switch (mode & 0x07U) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
            return 0;
        case 5:
        case 6:
            return 2;
        case 7:
            switch (reg & 0x07U) {
                case 0:
                case 2:
                case 3:
                    return 2;
                case 1:
                    return 4;
                case 4:
                    return (sz == 4) ? 4 : 2;
                default:
                    return 0;
            }
        default:
            return 0;
    }
}

int decode_sz2(uint8_t bits) noexcept {
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

bool resolve_static_ea_target(const dragon::md::Cartridge& cart, uint32_t ext_pc, uint8_t mode,
                              uint8_t reg, uint32_t& out_target) noexcept {
    if (mode != 7U)
        return false;
    if (reg == 0U) {
        out_target = static_cast<uint32_t>(static_cast<int16_t>(cart.read16(ext_pc))) & 0x00FFFFFFU;
        return true;
    }
    if (reg == 1U) {
        const uint32_t hi = cart.read16(ext_pc);
        const uint32_t lo = cart.read16(ext_pc + 2U);
        out_target = ((hi << 16U) | lo) & 0x00FFFFFFU;
        return true;
    }
    if (reg == 2U) {
        const int16_t d = static_cast<int16_t>(cart.read16(ext_pc));
        out_target = static_cast<uint32_t>(static_cast<int32_t>(ext_pc) + d) & 0x00FFFFFFU;
        return true;
    }
    return false;
}

bool is_valid_rom_pc(const dragon::md::Cartridge& cart, uint32_t addr) noexcept {
    const uint32_t a = addr & 0x00FFFFFFU;
    return (a & 1U) == 0U && a >= 0x000100U && a + 2U <= cart.rom_size();
}

}  // namespace

DecodedInsn decode_m68k(const dragon::md::Cartridge& cart, uint32_t addr) noexcept {
    DecodedInsn d{};
    d.addr = addr & 0x00FFFFFFU;
    d.op = cart.read16(d.addr);
    d.ext0 = cart.read16(d.addr + 2U);
    d.ext1 = cart.read16(d.addr + 4U);
    d.imm32 = (static_cast<uint32_t>(d.ext0) << 16U) | d.ext1;
    d.length = 2;

    const uint8_t line = static_cast<uint8_t>(d.op >> 12U);
    const uint8_t mode = static_cast<uint8_t>((d.op >> 3U) & 0x07U);
    const uint8_t ry = static_cast<uint8_t>(d.op & 0x07U);

    switch (line) {
        case 0x0U:
            if ((d.op & 0x0138U) == 0x0108U) {
                d.length = 4;  // MOVEP
            } else if (d.op & 0x0100U) {
                d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, (mode == 0U) ? 4 : 1));
            } else if ((d.op & 0x0F00U) == 0x0800U) {
                d.length = static_cast<uint8_t>(4 + ea_extra_bytes(mode, ry, (mode == 0U) ? 4 : 1));
            } else {
                const int sz = decode_sz2(static_cast<uint8_t>((d.op >> 6U) & 0x03U));
                if ((d.op & 0x003FU) == 0x003CU) {
                    d.length = 4;
                } else {
                    const uint8_t imm_len = (sz == 4) ? 4 : 2;
                    d.length = static_cast<uint8_t>(2 + imm_len + ea_extra_bytes(mode, ry, sz));
                }
            }
            break;

        case 0x1U:
        case 0x2U:
        case 0x3U: {
            const int sz = (line == 1U) ? 1 : (line == 2U) ? 4 : 2;
            const uint8_t dst_mode = static_cast<uint8_t>((d.op >> 6U) & 0x07U);
            const uint8_t dst_reg = static_cast<uint8_t>((d.op >> 9U) & 0x07U);
            d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, sz) +
                                            ea_extra_bytes(dst_mode, dst_reg, sz));
            break;
        }

        case 0x4U:
            if ((d.op & 0x01C0U) == 0x01C0U) {
                d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, 4));
                if (resolve_static_ea_target(cart, d.addr + 2U, mode, ry, d.target)) {
                    d.has_target = true;
                }
            } else if ((d.op & 0x01C0U) == 0x0180U) {
                d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, 2));
            } else if ((d.op & 0xFFF8U) == 0x4840U || (d.op & 0xFFF8U) == 0x4880U ||
                       (d.op & 0xFFF8U) == 0x48C0U || (d.op & 0xFFF8U) == 0x4E58U ||
                       (d.op & 0xFFF0U) == 0x4E60U || d.op == 0x4E70U || d.op == 0x4E71U ||
                       d.op == 0x4E76U) {
                d.length = 2;
            } else if ((d.op & 0xFFF8U) == 0x4E50U) {
                d.length = 4;  // LINK
            } else if (d.op == 0x4E72U) {
                d.length = 4;  // STOP #imm
                d.flow = FlowKind::Stop;
            } else if (d.op == 0x4E73U || d.op == 0x4E75U || d.op == 0x4E77U) {
                d.length = 2;  // RTE / RTS / RTR
                d.flow = FlowKind::Return;
            } else if (d.op == 0x4AFCU || (d.op & 0xFFF0U) == 0x4E40U) {
                d.length = 2;  // ILLEGAL / TRAP
                d.flow = FlowKind::Trap;
            } else if ((d.op & 0xFFC0U) == 0x4E80U) {  // JSR
                d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, 4));
                if (resolve_static_ea_target(cart, d.addr + 2U, mode, ry, d.target)) {
                    d.flow = FlowKind::Call;
                    d.has_target = true;
                } else {
                    d.flow = FlowKind::CallIndirect;
                }
            } else if ((d.op & 0xFFC0U) == 0x4EC0U) {  // JMP
                d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, 4));
                if (resolve_static_ea_target(cart, d.addr + 2U, mode, ry, d.target)) {
                    d.flow = FlowKind::BranchUncond;
                    d.has_target = true;
                } else {
                    d.flow = FlowKind::JumpIndirect;
                }
            } else if ((d.op & 0xFB80U) == 0x4880U) {  // MOVEM
                const int sz = (d.op & 0x0040U) ? 4 : 2;
                d.length = static_cast<uint8_t>(4 + ea_extra_bytes(mode, ry, sz));
            } else {
                const uint8_t sbits = static_cast<uint8_t>((d.op >> 6U) & 0x03U);
                const int sz = (sbits == 3U) ? 2 : decode_sz2(sbits);
                d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, sz));
            }
            break;

        case 0x5U:
            if ((d.op & 0x00C0U) == 0x00C0U) {
                if (mode == 1U) {  // DBcc
                    d.length = 4;
                    d.flow = FlowKind::BranchCond;
                    d.target = static_cast<uint32_t>(static_cast<int32_t>(d.addr + 2U) +
                                                     static_cast<int16_t>(d.ext0)) &
                               0x00FFFFFFU;
                    d.has_target = true;
                } else {
                    d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, 1));
                }
            } else {
                const int sz = decode_sz2(static_cast<uint8_t>((d.op >> 6U) & 0x03U));
                d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, sz));
            }
            break;

        case 0x6U: {  // BRA / BSR / Bcc
            const uint8_t cc = static_cast<uint8_t>((d.op >> 8U) & 0x0FU);
            int32_t disp = static_cast<int8_t>(d.op & 0xFFU);
            if (disp == 0) {
                d.length = 4;
                disp = static_cast<int16_t>(d.ext0);
            } else {
                d.length = 2;
            }
            d.target =
                static_cast<uint32_t>(static_cast<int32_t>(d.addr + 2U) + disp) & 0x00FFFFFFU;
            d.has_target = true;
            d.flow = (cc == 0U)   ? FlowKind::BranchUncond
                     : (cc == 1U) ? FlowKind::Call
                                  : FlowKind::BranchCond;
            break;
        }

        case 0x7U:
            d.length = 2;  // MOVEQ
            break;

        case 0x8U:
        case 0xCU: {
            const uint8_t opm = static_cast<uint8_t>((d.op >> 6U) & 0x07U);
            if (opm == 3U || opm == 7U) {
                d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, 2));
            } else if ((d.op & 0x01F0U) == 0x0100U || (d.op & 0x01F8U) == 0x0140U ||
                       (d.op & 0x01F8U) == 0x0148U || (d.op & 0x01F8U) == 0x0188U) {
                d.length = 2;
            } else {
                const int sz = decode_sz2(opm & 0x03U);
                d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, sz));
            }
            break;
        }

        case 0x9U:
        case 0xDU: {
            const uint8_t opm = static_cast<uint8_t>((d.op >> 6U) & 0x07U);
            if (opm == 3U || opm == 7U) {
                d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, (opm == 7U) ? 4 : 2));
            } else if ((d.op & 0x0130U) == 0x0100U) {
                d.length = 2;  // SUBX/ADDX
            } else {
                const int sz = decode_sz2(opm & 0x03U);
                d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, sz));
            }
            break;
        }

        case 0xAU:
        case 0xFU:
            d.length = 2;
            d.flow = FlowKind::Trap;
            break;

        case 0xBU: {
            const uint8_t opm = static_cast<uint8_t>((d.op >> 6U) & 0x07U);
            if (opm == 3U || opm == 7U) {
                d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, (opm == 7U) ? 4 : 2));
            } else if ((d.op & 0x0138U) == 0x0108U) {
                d.length = 2;  // CMPM
            } else {
                const int sz = decode_sz2(opm & 0x03U);
                d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, sz));
            }
            break;
        }

        case 0xEU:
            if ((d.op & 0x00C0U) == 0x00C0U) {
                d.length = static_cast<uint8_t>(2 + ea_extra_bytes(mode, ry, 2));
            } else {
                d.length = 2;
            }
            break;
    }

    return d;
}

ProgramCfg build_cfg(const dragon::md::Cartridge& cart, const std::vector<uint32_t>& extra_entries,
                     size_t max_functions) {
    ProgramCfg cfg;
    if (!cart.is_loaded() || cart.rom_size() < 0x100U) {
        return cfg;
    }

    std::queue<uint32_t> func_queue;
    std::set<uint32_t> queued_funcs;

    auto enqueue_func = [&](uint32_t addr) {
        const uint32_t a = addr & 0x00FFFFFFU;
        if (is_valid_rom_pc(cart, a) && queued_funcs.size() < max_functions &&
            queued_funcs.insert(a).second) {
            func_queue.push(a);
        }
    };

    // Seed from M68000 vector table: RESET ($04), Line-A ($28), Line-F ($2C), Ext ($68),
    // H-Int ($70), V-Int ($78), TRAP #0..#15 ($80..$BC)
    auto read_vec = [&](uint32_t off) -> uint32_t {
        return (static_cast<uint32_t>(cart.read16(off)) << 16U) | cart.read16(off + 2U);
    };
    enqueue_func(read_vec(0x04U));
    enqueue_func(read_vec(0x28U));
    enqueue_func(read_vec(0x2CU));
    enqueue_func(read_vec(0x68U));
    enqueue_func(read_vec(0x70U));
    enqueue_func(read_vec(0x78U));
    for (uint32_t v = 0; v < 16U; ++v) {
        enqueue_func(read_vec(0x80U + v * 4U));
    }
    for (uint32_t e : extra_entries) {
        enqueue_func(e);
    }

    while (!func_queue.empty()) {
        const uint32_t entry = func_queue.front();
        func_queue.pop();

        FunctionCfg fn;
        fn.entry = entry;
        fn.internal_labels.insert(entry);

        std::queue<uint32_t> blk_q;
        std::set<uint32_t> visited;
        blk_q.push(entry);

        while (!blk_q.empty() && fn.insns.size() < 2048U) {
            uint32_t pc = blk_q.front();
            blk_q.pop();
            if (!is_valid_rom_pc(cart, pc) || visited.count(pc)) {
                continue;
            }
            ++cfg.total_blocks;

            while (is_valid_rom_pc(cart, pc) && !visited.count(pc) && fn.insns.size() < 2048U) {
                visited.insert(pc);
                const DecodedInsn insn = decode_m68k(cart, pc);
                fn.insns[pc] = insn;
                ++cfg.total_instructions;

                const uint32_t next_pc = (pc + insn.length) & 0x00FFFFFFU;

                if (insn.flow == FlowKind::Return) {
                    break;
                }
                if (insn.flow == FlowKind::Trap) {
                    // TRAP/Line-A/Line-F may return via RTE; continue fallthrough
                    pc = next_pc;
                    continue;
                }
                if (insn.flow == FlowKind::JumpIndirect) {
                    ++cfg.indirect_jumps;
                    break;
                }
                if (insn.flow == FlowKind::Call) {
                    if (insn.has_target && is_valid_rom_pc(cart, insn.target)) {
                        fn.callees.insert(insn.target);
                        enqueue_func(insn.target);
                    }
                    pc = next_pc;
                    continue;
                }
                if (insn.flow == FlowKind::BranchUncond) {
                    if (insn.has_target && is_valid_rom_pc(cart, insn.target)) {
                        // Treat nearby jumps as internal labels, distant jumps as tail calls
                        const int32_t dist =
                            static_cast<int32_t>(insn.target) - static_cast<int32_t>(entry);
                        if (dist >= -4096 && dist <= 8192) {
                            fn.internal_labels.insert(insn.target);
                            blk_q.push(insn.target);
                        } else {
                            fn.callees.insert(insn.target);
                            enqueue_func(insn.target);
                        }
                    }
                    break;
                }
                if (insn.flow == FlowKind::BranchCond) {
                    if (insn.has_target && is_valid_rom_pc(cart, insn.target)) {
                        fn.internal_labels.insert(insn.target);
                        blk_q.push(insn.target);
                    }
                    fn.internal_labels.insert(next_pc);
                    pc = next_pc;
                    continue;
                }
                pc = next_pc;
            }
        }

        cfg.functions[entry] = std::move(fn);
    }

    return cfg;
}

}  // namespace dragon::mdrecomp
