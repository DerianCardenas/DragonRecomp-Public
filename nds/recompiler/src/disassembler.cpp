#include "ndsrecomp/disassembler.h"

#include <cstdio>
#include <queue>

namespace dragon::ndsrecomp {

std::string Function::c_symbol() const {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "func_%08X_%s", entry_addr, thumb ? "thumb" : "arm");
    return buf;
}

const Function* ProgramCFG::find_function(uint32_t addr, bool thumb) const {
    const auto it = functions.find(FunctionKey{addr, thumb});
    return (it != functions.end()) ? &it->second : nullptr;
}

namespace {

inline uint16_t rd16(const dragon::nds::Cartridge& cart, uint32_t ram_addr, uint32_t base,
                     uint32_t rom_off) noexcept {
    const uint32_t off = rom_off + (ram_addr - base);
    return static_cast<uint16_t>(cart.read_rom8(off) |
                                 (static_cast<uint16_t>(cart.read_rom8(off + 1U)) << 8U));
}

inline uint32_t rd32(const dragon::nds::Cartridge& cart, uint32_t ram_addr, uint32_t base,
                     uint32_t rom_off) noexcept {
    const uint32_t off = rom_off + (ram_addr - base);
    return cart.read_rom32(off);
}

}  // namespace

ProgramCFG disassemble_nds(const dragon::nds::Cartridge& cart, const GameConfig& config) {
    ProgramCFG cfg{};
    const auto& hdr = cart.header();
    cfg.arm9_ram_base = hdr.arm9_ram_addr;
    cfg.arm9_ram_size = hdr.arm9_size;

    const uint32_t ram_start = hdr.arm9_ram_addr;
    const uint32_t ram_end = hdr.arm9_ram_addr + hdr.arm9_size;
    const uint32_t rom_off = hdr.arm9_rom_offset;

    auto in_arm9 = [&](uint32_t addr) -> bool { return addr >= ram_start && addr < ram_end; };

    std::queue<FunctionKey> func_queue;
    std::set<FunctionKey> queued_funcs;

    auto enqueue_func = [&](uint32_t addr, bool thumb) {
        addr = thumb ? (addr & ~1U) : (addr & ~3U);
        if (!in_arm9(addr))
            return;
        const FunctionKey key{addr, thumb};
        if (queued_funcs.insert(key).second) {
            func_queue.push(key);
        }
    };

    // Seed ARM9 entry point
    enqueue_func(hdr.arm9_entry_addr, (hdr.arm9_entry_addr & 1U) != 0U);

    // Seed TOML entry points
    for (const auto& ep : config.entry_points) {
        enqueue_func(ep.addr, (ep.addr & 1U) != 0U);
    }

    // Light prologue scan for ARM9 functions (STMFD SP!, {..., LR} = 0xE92D4xxx)
    constexpr size_t kMaxAutoPrologues = 1500U;
    size_t found_prologues = 0U;
    for (uint32_t a = ram_start; a + 4U <= ram_end && found_prologues < kMaxAutoPrologues;
         a += 4U) {
        const uint32_t w = rd32(cart, a, ram_start, rom_off);
        if ((w & 0xFFFF4000U) == 0xE92D4000U) {
            enqueue_func(a, false);
            ++found_prologues;
        }
    }

    constexpr size_t kMaxFunctions = 4096U;
    constexpr size_t kMaxInsnsPerFunc = 512U;

    while (!func_queue.empty() && cfg.functions.size() < kMaxFunctions) {
        const FunctionKey fkey = func_queue.front();
        func_queue.pop();
        if (cfg.functions.count(fkey))
            continue;

        Function fn{};
        fn.entry_addr = fkey.addr;
        fn.thumb = fkey.thumb;
        fn.name = fn.c_symbol();

        std::queue<uint32_t> bb_queue;
        std::set<uint32_t> visited_bb;
        bb_queue.push(fkey.addr);

        while (!bb_queue.empty() && fn.instructions.size() < kMaxInsnsPerFunc) {
            const uint32_t start = bb_queue.front();
            bb_queue.pop();
            if (!in_arm9(start) || !visited_bb.insert(start).second)
                continue;

            BasicBlock bb{};
            bb.start_addr = start;
            bb.thumb = fkey.thumb;
            uint32_t pc = start;

            while (in_arm9(pc) && fn.instructions.size() < kMaxInsnsPerFunc) {
                if (fn.instructions.count(pc)) {
                    bb.successors.push_back(pc);
                    break;
                }

                Instruction ins{};
                ins.addr = pc;
                ins.thumb = fkey.thumb;
                bool ends_block = false;

                if (!fkey.thumb) {
                    if (pc + 4U > ram_end)
                        break;
                    const uint32_t w = rd32(cart, pc, ram_start, rom_off);
                    ins.raw = w;
                    ins.length = 4U;

                    const uint32_t cond = w >> 28U;
                    if (cond == 0xFU && ((w & 0xFE000000U) == 0xFA000000U)) {
                        // BLX <label> -> Thumb function call
                        int32_t off = static_cast<int32_t>(w & 0x00FFFFFFU);
                        if (off & 0x00800000)
                            off |= static_cast<int32_t>(0xFF000000U);
                        const uint32_t h = (w >> 24U) & 1U;
                        const uint32_t target =
                            static_cast<uint32_t>(static_cast<int32_t>(pc + 8U) + (off << 2) +
                                                  static_cast<int32_t>(h << 1));
                        ins.branch_target = target;
                        ins.has_branch_target = true;
                        enqueue_func(target, true);
                    } else if (cond != 0xFU && ((w & 0x0E000000U) == 0x0A000000U)) {
                        // B / BL
                        int32_t off = static_cast<int32_t>(w & 0x00FFFFFFU);
                        if (off & 0x00800000)
                            off |= static_cast<int32_t>(0xFF000000U);
                        const uint32_t target =
                            static_cast<uint32_t>(static_cast<int32_t>(pc + 8U) + (off << 2));
                        ins.branch_target = target;
                        ins.has_branch_target = true;
                        if (w & 0x01000000U) {
                            enqueue_func(target, false);
                        } else {
                            if (in_arm9(target)) {
                                bb.successors.push_back(target);
                                bb_queue.push(target);
                            }
                            if (cond != 0xEU) {
                                bb.successors.push_back(pc + 4U);
                                bb_queue.push(pc + 4U);
                            }
                            ends_block = true;
                        }
                    } else if (cond == 0xEU) {
                        // Unconditional return / indirect jump
                        if ((w & 0x0FFFFFF0U) == 0x012FFF10U) {
                            ends_block = true;  // BX Rm
                        } else if ((w & 0x0E108000U) == 0x08108000U) {
                            ends_block = true;  // LDM ..., {..., PC}
                        } else if ((w & 0x0C50F000U) == 0x0410F000U) {
                            ends_block = true;  // LDR PC, [...]
                        } else if ((w & 0x0C00F000U) == 0x0000F000U &&
                                   (((w >> 21U) & 0xFU) < 8U || ((w >> 21U) & 0xFU) >= 12U)) {
                            ends_block = true;  // ALU write to PC
                        }
                    }

                    fn.instructions[pc] = ins;
                    bb.instructions.push_back(ins);
                    pc += 4U;
                } else {
                    if (pc + 2U > ram_end)
                        break;
                    const uint16_t h1 = rd16(cart, pc, ram_start, rom_off);
                    ins.raw = h1;
                    ins.length = 2U;

                    const uint32_t top5 = h1 >> 11U;
                    if (top5 == 0x1EU && pc + 4U <= ram_end) {
                        const uint16_t h2 = rd16(cart, pc + 2U, ram_start, rom_off);
                        const uint32_t top5_2 = h2 >> 11U;
                        if (top5_2 == 0x1FU || top5_2 == 0x1DU) {
                            ins.raw =
                                static_cast<uint32_t>(h1) | (static_cast<uint32_t>(h2) << 16U);
                            ins.length = 4U;
                            ins.is_thumb_bl32 = (top5_2 == 0x1FU);
                            ins.is_thumb_blx32 = (top5_2 == 0x1DU);
                            int32_t off = static_cast<int32_t>(h1 & 0x7FFU);
                            if (off & 0x400)
                                off |= static_cast<int32_t>(0xFFFFF800U);
                            uint32_t target =
                                static_cast<uint32_t>(static_cast<int32_t>(pc + 4U) + (off << 12) +
                                                      static_cast<int32_t>((h2 & 0x7FFU) << 1));
                            if (ins.is_thumb_blx32)
                                target &= ~3U;
                            ins.branch_target = target;
                            ins.has_branch_target = true;
                            enqueue_func(target, ins.is_thumb_bl32);
                        }
                    } else if ((h1 >> 12U) == 0x0DU && (((h1 >> 8U) & 0xFU) < 0xEU)) {
                        const int32_t off = static_cast<int32_t>(static_cast<int8_t>(h1 & 0xFFU))
                                            << 1;
                        const uint32_t target =
                            static_cast<uint32_t>(static_cast<int32_t>(pc + 4U) + off);
                        ins.branch_target = target;
                        ins.has_branch_target = true;
                        if (in_arm9(target)) {
                            bb.successors.push_back(target);
                            bb_queue.push(target);
                        }
                        bb.successors.push_back(pc + 2U);
                        bb_queue.push(pc + 2U);
                        ends_block = true;
                    } else if (top5 == 0x1CU) {
                        int32_t off = static_cast<int32_t>(h1 & 0x7FFU);
                        if (off & 0x400)
                            off |= static_cast<int32_t>(0xFFFFF800U);
                        const uint32_t target =
                            static_cast<uint32_t>(static_cast<int32_t>(pc + 4U) + (off << 1));
                        ins.branch_target = target;
                        ins.has_branch_target = true;
                        if (in_arm9(target)) {
                            bb.successors.push_back(target);
                            bb_queue.push(target);
                        }
                        ends_block = true;
                    } else if ((h1 & 0xFF87U) == 0x4700U || (h1 & 0xFF00U) == 0xBD00U) {
                        ends_block = true;  // BX Rm or POP {..., PC}
                    }

                    fn.instructions[pc] = ins;
                    bb.instructions.push_back(ins);
                    pc += ins.length;
                }

                if (ends_block)
                    break;
            }

            bb.end_addr = pc;
            if (!bb.instructions.empty()) {
                fn.block_addrs.insert(start);
                cfg.blocks[FunctionKey{start, fkey.thumb}] = std::move(bb);
            }
        }

        if (!fn.instructions.empty()) {
            cfg.functions[fkey] = std::move(fn);
        }
    }

    return cfg;
}

}  // namespace dragon::ndsrecomp
