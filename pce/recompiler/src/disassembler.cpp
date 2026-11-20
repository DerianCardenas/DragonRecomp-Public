#include "pcerecomp/disassembler.h"

#include <algorithm>
#include <queue>
#include <tuple>

#include "pce/cpu_state.h"

namespace dragon::pcerecomp {

namespace {

uint8_t read_rom_byte(const pce::Cartridge& cart, uint8_t bank, uint16_t addr) noexcept {
    if (bank >= 0x80U || cart.rom().empty()) {
        return 0x00U;
    }
    return cart.read_bank(bank, static_cast<uint16_t>(addr & 0x1FFFU));
}

uint16_t read_rom_word(const pce::Cartridge& cart, uint8_t bank, uint16_t addr) noexcept {
    const uint8_t lo = read_rom_byte(cart, bank, addr);
    const uint8_t hi = read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 1U));
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8U));
}

bool in_data_range(const dragon::GameConfig& cfg, uint8_t bank, uint16_t addr) noexcept {
    for (const auto& dr : cfg.data_ranges) {
        if ((dr.bank < 0 || static_cast<uint8_t>(dr.bank) == bank) &&
            addr >= dr.start && addr < dr.end) {
            return true;
        }
    }
    return false;
}

}  // namespace

DecodedInstr decode_instruction(const pce::Cartridge& cart, uint8_t bank, uint16_t addr) noexcept {
    DecodedInstr ins{};
    ins.addr = addr;
    ins.bank = bank;

    const uint8_t op = read_rom_byte(cart, bank, addr);
    const auto& info = pce::opcode_info(op);
    ins.opcode = op;
    ins.mode = info.mode;
    ins.length = info.length;
    ins.cycles = info.cycles;

    using M = pce::AddrMode;
    switch (info.mode) {
        case M::Imm:
            ins.imm8 = read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 1U));
            break;
        case M::Zp: case M::ZpX: case M::ZpY:
        case M::ZpInd: case M::ZpIndX: case M::ZpIndY:
            ins.zp = read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 1U));
            break;
        case M::Abs: case M::AbsX: case M::AbsY:
        case M::Ind: case M::IndX:
            ins.addr16 = read_rom_word(cart, bank, static_cast<uint16_t>(addr + 1U));
            break;
        case M::Rel:
            ins.rel = static_cast<int8_t>(read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 1U)));
            ins.target_addr = static_cast<uint16_t>(addr + ins.length + ins.rel);
            break;
        case M::ZpRel:
            ins.zp = read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 1U));
            ins.rel = static_cast<int8_t>(read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 2U)));
            ins.target_addr = static_cast<uint16_t>(addr + ins.length + ins.rel);
            break;
        case M::ImmZp: case M::ImmZpX:
            ins.imm8 = read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 1U));
            ins.zp = read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 2U));
            break;
        case M::ImmAbs: case M::ImmAbsX:
            ins.imm8 = read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 1U));
            ins.addr16 = read_rom_word(cart, bank, static_cast<uint16_t>(addr + 2U));
            break;
        case M::Block:
            ins.blk_src = read_rom_word(cart, bank, static_cast<uint16_t>(addr + 1U));
            ins.blk_dst = read_rom_word(cart, bank, static_cast<uint16_t>(addr + 3U));
            ins.blk_len = read_rom_word(cart, bank, static_cast<uint16_t>(addr + 5U));
            break;
        default:
            break;
    }

    switch (op) {
        case 0x10U: case 0x30U: case 0x50U: case 0x70U:
        case 0x90U: case 0xB0U: case 0xD0U: case 0xF0U:  // Conditional branches
            ins.has_branch = true;
            ins.is_conditional = true;
            break;
        case 0x80U:  // BRA
            ins.has_branch = true;
            ins.is_terminator = true;
            break;
        case 0x0FU: case 0x1FU: case 0x2FU: case 0x3FU:
        case 0x4FU: case 0x5FU: case 0x6FU: case 0x7FU:
        case 0x8FU: case 0x9FU: case 0xAFU: case 0xBFU:
        case 0xCFU: case 0xDFU: case 0xEFU: case 0xFFU:  // BBR / BBS
            ins.has_branch = true;
            ins.is_conditional = true;
            break;
        case 0x4CU:  // JMP abs
            ins.has_branch = true;
            ins.is_terminator = true;
            ins.target_addr = ins.addr16;
            break;
        case 0x6CU: case 0x7CU:  // JMP (abs), JMP (abs,X)
            ins.is_terminator = true;
            ins.is_indirect_jump = true;
            break;
        case 0x20U:  // JSR abs
            ins.has_branch = true;
            ins.is_call = true;
            ins.target_addr = ins.addr16;
            break;
        case 0x44U:  // BSR rel
            ins.has_branch = true;
            ins.is_call = true;
            break;
        case 0x40U: case 0x60U: case 0x00U:  // RTI, RTS, BRK
            ins.is_terminator = true;
            break;
        default:
            break;
    }

    return ins;
}

DisassemblyResult disassemble_rom(const pce::Cartridge& cart, const dragon::GameConfig& cfg) {
    DisassemblyResult result;
    if (cfg.interpreter_only || cart.rom().empty()) {
        return result;
    }

    using FuncKey = std::pair<uint8_t, uint16_t>;
    std::set<FuncKey> queued_funcs;
    std::queue<FuncKey> func_queue;

    // Default known MPR mapping propagated from boot bank 0
    std::array<int, 8> default_mpr = {0xFF, 0xF8, -1, -1, -1, -1, -1, 0x00};

    auto add_func = [&](uint8_t bank, uint16_t addr) {
        if (bank >= 0x80U || addr < 0x4000U || in_data_range(cfg, bank, addr)) {
            return;
        }
        const FuncKey key{bank, addr};
        if (queued_funcs.insert(key).second) {
            func_queue.push(key);
        }
    };

    // Seed from PC Engine hardware vectors in boot bank 0 ($E000-$FFFF)
    const uint16_t vecs[] = {PCE_VEC_RESET, PCE_VEC_IRQ1, PCE_VEC_TIQ, PCE_VEC_IRQ2, PCE_VEC_NMI};
    for (uint16_t v : vecs) {
        const uint16_t tgt = read_rom_word(cart, 0U, v);
        if (tgt >= 0xE000U && tgt < 0xFFF6U) {
            add_func(0U, tgt);
        }
    }

    // User/auto-learned TOML entry points
    for (const auto& ep : cfg.entry_points) {
        const uint16_t a = static_cast<uint16_t>(ep.addr & 0xFFFFU);
        const uint8_t b = (ep.bank >= 0) ? static_cast<uint8_t>(ep.bank) : 0U;
        add_func(b, a);
    }

    // User/auto-learned jump tables
    for (const auto& jt : cfg.jump_tables) {
        const uint8_t jb = (jt.bank >= 0) ? static_cast<uint8_t>(jt.bank) : 0U;
        for (size_t i = 0; i < jt.count; ++i) {
            const uint16_t entry_addr = static_cast<uint16_t>(jt.addr + i * 2U);
            const uint16_t tgt = read_rom_word(cart, jb, entry_addr);
            const uint8_t tb = (tgt >= 0xE000U) ? 0U : jb;
            add_func(tb, tgt);
        }
    }

    constexpr size_t kMaxInstrPerFunc = 4096U;
    constexpr size_t kMaxFunctions = 4096U;

    while (!func_queue.empty() && result.functions.size() < kMaxFunctions) {
        const auto [bank, entry_addr] = func_queue.front();
        func_queue.pop();

        FunctionCfg fn;
        fn.bank = bank;
        fn.entry_addr = entry_addr;
        fn.local_labels.insert(entry_addr);

        const uint32_t page_start = static_cast<uint32_t>(entry_addr & 0xE000U);
        const uint32_t page_end = page_start + 0x2000U;

        // Track constant LDA #imm; TAM #mask mappings within each straight-line block
        std::array<int, 8> known_mpr = default_mpr;
        known_mpr[entry_addr >> 13U] = static_cast<int>(bank);

        std::queue<uint16_t> block_q;
        block_q.push(entry_addr);

        while (!block_q.empty() && fn.instructions.size() < kMaxInstrPerFunc) {
            uint32_t pc = block_q.front();
            block_q.pop();
            int known_a = -1;

            while (pc >= page_start && pc < page_end && fn.instructions.size() < kMaxInstrPerFunc) {
                const uint16_t pc16 = static_cast<uint16_t>(pc);
                if (fn.instructions.count(pc16) > 0U || in_data_range(cfg, bank, pc16)) {
                    break;
                }

                const DecodedInstr ins = decode_instruction(cart, bank, pc16);
                fn.instructions.emplace(pc16, ins);

                // Simple constant propagation for LDA #imm / CLA -> TAM #mask
                if (ins.opcode == 0xA9U) {
                    known_a = static_cast<int>(ins.imm8);
                } else if (ins.opcode == 0x62U) {
                    known_a = 0;
                } else if (ins.opcode == 0x53U && known_a >= 0) {
                    for (int m = 0; m < 8; ++m) {
                        if (ins.imm8 & (1U << m)) {
                            known_mpr[static_cast<size_t>(m)] = known_a;
                            if (entry_addr >= 0xE000U && default_mpr[static_cast<size_t>(m)] < 0) {
                                default_mpr[static_cast<size_t>(m)] = known_a;
                            }
                        }
                    }
                } else if (ins.opcode != 0xEA && ins.opcode != 0xD4 && ins.opcode != 0x54 &&
                           ins.opcode != 0x78 && ins.opcode != 0xD8) {
                    known_a = -1;
                }

                if (ins.has_branch) {
                    const uint16_t tgt = ins.target_addr;
                    const uint8_t tgt_slot = static_cast<uint8_t>(tgt >> 13U);
                    if (ins.is_call) {
                        if (tgt >= page_start && tgt < page_end) {
                            add_func(bank, tgt);
                        } else if (known_mpr[tgt_slot] >= 0 && known_mpr[tgt_slot] < 0x80) {
                            add_func(static_cast<uint8_t>(known_mpr[tgt_slot]), tgt);
                        }
                    } else {
                        if (tgt >= page_start && tgt < page_end) {
                            fn.local_labels.insert(tgt);
                            if (fn.instructions.count(tgt) == 0U) {
                                block_q.push(tgt);
                            }
                        } else if (known_mpr[tgt_slot] >= 0 && known_mpr[tgt_slot] < 0x80) {
                            add_func(static_cast<uint8_t>(known_mpr[tgt_slot]), tgt);
                        }
                    }
                }

                if (ins.is_terminator) {
                    break;
                }
                const uint32_t next_pc = pc + ins.length;
                if (next_pc >= page_end || next_pc >= 0x10000U) {
                    break;
                }
                pc = next_pc;
            }
        }

        if (!fn.instructions.empty()) {
            result.total_instructions += fn.instructions.size();
            result.functions.push_back(std::move(fn));
        }
    }

    std::sort(result.functions.begin(), result.functions.end(),
              [](const FunctionCfg& a, const FunctionCfg& b) {
                  return std::tie(a.bank, a.entry_addr) < std::tie(b.bank, b.entry_addr);
              });

    return result;
}

}  // namespace dragon::pcerecomp
