#include "gbarecomp/disassembler.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <queue>
#include <set>

#include "gba/bus.h"

namespace dragon::gbarecomp {

std::string Function::c_symbol() const {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "func_%08X_%s", entry_addr, thumb ? "thumb" : "arm");
    return buf;
}

const Function* ProgramCFG::find_function(uint32_t addr, bool thumb) const {
    const uint32_t norm = dragon::gba::Bus::normalize_rom_addr(addr & (thumb ? ~1U : ~3U));
    auto it = functions.find(FunctionKey{norm, thumb});
    if (it != functions.end()) {
        return &it->second;
    }
    return nullptr;
}

namespace {

struct RomView {
    std::span<const uint8_t> rom;

    bool is_valid_rom_addr(uint32_t addr) const noexcept {
        const uint32_t norm = dragon::gba::Bus::normalize_rom_addr(addr);
        if (norm < 0x08000000U || norm >= 0x0A000000U) {
            return false;
        }
        return (norm - 0x08000000U) < rom.size();
    }

    uint8_t read8(uint32_t addr) const noexcept {
        const uint32_t norm = dragon::gba::Bus::normalize_rom_addr(addr);
        if (norm < 0x08000000U) {
            return 0U;
        }
        const size_t off = norm - 0x08000000U;
        return (off < rom.size()) ? rom[off] : 0U;
    }

    uint16_t read16(uint32_t addr) const noexcept {
        const uint32_t norm = dragon::gba::Bus::normalize_rom_addr(addr & ~1U);
        if (norm < 0x08000000U) {
            return 0U;
        }
        const size_t off = norm - 0x08000000U;
        if (off + 1U >= rom.size()) {
            return 0U;
        }
        return static_cast<uint16_t>(rom[off] | (static_cast<uint16_t>(rom[off + 1U]) << 8U));
    }

    uint32_t read32(uint32_t addr) const noexcept {
        const uint32_t norm = dragon::gba::Bus::normalize_rom_addr(addr & ~3U);
        if (norm < 0x08000000U) {
            return 0U;
        }
        const size_t off = norm - 0x08000000U;
        if (off + 3U >= rom.size()) {
            return 0U;
        }
        return static_cast<uint32_t>(rom[off]) | (static_cast<uint32_t>(rom[off + 1U]) << 8U) |
               (static_cast<uint32_t>(rom[off + 2U]) << 16U) |
               (static_cast<uint32_t>(rom[off + 3U]) << 24U);
    }
};

bool is_in_data_range(uint32_t addr, const GameConfig& config) noexcept {
    for (const auto& dr : config.data_ranges) {
        if (addr >= dr.start && addr <= dr.end) {
            return true;
        }
    }
    return false;
}

bool is_valid_arm_word(uint32_t raw) noexcept {
    if (raw == 0x00000000U || raw == 0xFFFFFFFFU) {
        return false;
    }
    const uint8_t cond = static_cast<uint8_t>((raw >> 28U) & 0x0FU);
    if (cond == 0x0FU) {
        return false;
    }
    // ARMv4T undefined instruction space
    if ((raw & 0x0E000010U) == 0x06000010U) {
        return false;
    }
    // Coprocessor instructions (LDC/STC/CDP/MCR/MRC) are not used on GBA ARM7TDMI
    if ((raw & 0x0E000000U) == 0x0C000000U || (raw & 0x0F000000U) == 0x0E000000U) {
        return false;
    }
    // SWI on GBA has comment field (bits 23..16) <= 0x2A and low 16 bits == 0
    if ((raw & 0x0F000000U) == 0x0F000000U) {
        const uint32_t swi_num = (raw >> 16U) & 0xFFU;
        if ((raw & 0x0000FFFFU) != 0U || swi_num > 0x2AU) {
            return false;
        }
    }
    return true;
}

bool is_valid_thumb_inst(const RomView& view, uint32_t addr, const Instruction& inst) noexcept {
    if (inst.is_thumb_bl32) {
        return true;
    }
    const uint16_t hw = static_cast<uint16_t>(inst.raw & 0xFFFFU);
    // Unpaired 0xF000..0xFFFF halfwords (including 0xFFFF empty ROM padding) are not valid Thumb
    if ((hw & 0xF000U) == 0xF000U) {
        return false;
    }
    // 0xDE00..0xDEFF is permanently undefined in Thumb
    if ((hw & 0xFF00U) == 0xDE00U) {
        return false;
    }
    // Thumb SWI on GBA only uses BIOS functions 0x00..0x2A
    if ((hw & 0xFF00U) == 0xDF00U && (hw & 0xFFU) > 0x2AU) {
        return false;
    }
    // Two consecutive 0x0000 halfwords indicate a 32-bit NULL literal or zero padding
    if (hw == 0x0000U && view.is_valid_rom_addr(addr + 2U) && view.read16(addr + 2U) == 0x0000U) {
        return false;
    }
    return true;
}

bool is_plausible_thumb_prologue(const RomView& view, uint32_t addr) noexcept {
    if (!view.is_valid_rom_addr(addr) || (addr & 1U) != 0U) {
        return false;
    }
    const uint16_t first_hw = view.read16(addr);
    if (first_hw == 0x4770U) {
        return true;
    }
    // Require PUSH {rlist, LR} (0xB5xx with non-empty rlist or LR)
    if ((first_hw & 0xFF00U) != 0xB500U) {
        return false;
    }
    uint32_t pc = addr + 2U;
    for (int step = 0; step < 64; ++step) {
        if (!view.is_valid_rom_addr(pc)) {
            return false;
        }
        const uint16_t hw = view.read16(pc);
        if ((hw & 0xF800U) == 0xF000U) {
            if (!view.is_valid_rom_addr(pc + 2U) || (view.read16(pc + 2U) & 0xF800U) != 0xF800U) {
                return false;
            }
            pc += 4U;
            continue;
        }
        if ((hw & 0xF800U) == 0xF800U || (hw & 0xFF00U) == 0xDE00U) {
            return false;
        }
        if (hw == 0x0000U && view.is_valid_rom_addr(pc + 2U) && view.read16(pc + 2U) == 0x0000U) {
            return false;
        }
        // Found a valid return or unconditional branch
        if ((hw & 0xFF00U) == 0xBD00U || (hw & 0xFF80U) == 0x4700U || (hw & 0xF800U) == 0xE000U) {
            return true;
        }
        pc += 2U;
    }
    return true;
}

bool is_plausible_arm_entry(const RomView& view, uint32_t addr) noexcept {
    if (!view.is_valid_rom_addr(addr) || (addr & 3U) != 0U) {
        return false;
    }
    const uint32_t w0 = view.read32(addr);
    // ARM function entry points start with an unconditional instruction (cond == 0x0E)
    if (((w0 >> 28U) & 0x0FU) != 0x0EU || !is_valid_arm_word(w0)) {
        return false;
    }
    return true;
}

Instruction decode_instruction(const RomView& view, uint32_t addr, bool thumb) noexcept {
    Instruction inst;
    inst.addr = addr;
    inst.thumb = thumb;

    if (!thumb) {
        inst.length = 4U;
        inst.raw = view.read32(addr);
        if (((inst.raw >> 25U) & 7U) == 5U) {
            int32_t offset = static_cast<int32_t>(inst.raw & 0x00FFFFFFU);
            if (offset & 0x00800000) {
                offset |= static_cast<int32_t>(0xFF000000U);
            }
            offset <<= 2;
            inst.branch_target = dragon::gba::Bus::normalize_rom_addr(
                static_cast<uint32_t>(static_cast<int32_t>(addr + 8U) + offset) & ~3U);
            inst.has_branch_target = true;
        }
    } else {
        const uint16_t hw1 = view.read16(addr);
        if ((hw1 & 0xF800U) == 0xF000U && view.is_valid_rom_addr(addr + 2U)) {
            const uint16_t hw2 = view.read16(addr + 2U);
            if ((hw2 & 0xF800U) == 0xF800U) {
                inst.length = 4U;
                inst.is_thumb_bl32 = true;
                inst.raw = static_cast<uint32_t>(hw1) | (static_cast<uint32_t>(hw2) << 16U);
                int32_t hi = static_cast<int32_t>(hw1 & 0x07FFU);
                if (hi & 0x0400) {
                    hi |= static_cast<int32_t>(0xFFFFF800U);
                }
                const uint32_t lo = static_cast<uint32_t>(hw2 & 0x07FFU) << 1U;
                const uint32_t target =
                    static_cast<uint32_t>(static_cast<int32_t>(addr + 4U) + (hi << 12)) + lo;
                inst.branch_target = dragon::gba::Bus::normalize_rom_addr(target & ~1U);
                inst.has_branch_target = true;
                return inst;
            }
        }
        inst.length = 2U;
        inst.raw = hw1;
        if ((hw1 & 0xF000U) == 0xD000U && ((hw1 >> 8U) & 0x0FU) < 0x0EU) {
            const int8_t rel = static_cast<int8_t>(hw1 & 0xFFU);
            inst.branch_target = dragon::gba::Bus::normalize_rom_addr(
                static_cast<uint32_t>(static_cast<int32_t>(addr + 4U) +
                                      static_cast<int32_t>(rel) * 2) &
                ~1U);
            inst.has_branch_target = true;
        } else if ((hw1 & 0xF800U) == 0xE000U) {
            int32_t offset = static_cast<int32_t>(hw1 & 0x07FFU);
            if (offset & 0x0400) {
                offset |= static_cast<int32_t>(0xFFFFF800U);
            }
            inst.branch_target = dragon::gba::Bus::normalize_rom_addr(
                static_cast<uint32_t>(static_cast<int32_t>(addr + 4U) + offset * 2) & ~1U);
            inst.has_branch_target = true;
        }
    }
    return inst;
}

// Check if a Thumb function at `target` is a GCC switch helper
// (__gnu_thumb1_case_uqi/sqi/uhi/shi/si) Returns element size in bytes (1, 2, 4) and whether
// signed, or 0 if not a switch helper.
int detect_thumb_case_helper(const RomView& view, uint32_t target, bool& is_signed) noexcept {
    is_signed = false;
    if (!view.is_valid_rom_addr(target)) {
        return 0;
    }
    // GCC __gnu_thumb1_case_* helpers are 6-12 instructions long, reference LR (R14),
    // load an offset from the table at LR, add it to LR, and return via BX R14 / MOV PC, R14.
    bool touches_lr = false;
    int load_size = 0;
    bool load_signed = false;
    for (uint32_t off = 0; off < 24U; off += 2U) {
        if (!view.is_valid_rom_addr(target + off)) {
            break;
        }
        const uint16_t hw = view.read16(target + off);
        // MOV Rxx, LR or ADD LR, ...
        if ((hw & 0xFF00U) == 0x4600U || (hw & 0xFF00U) == 0x4400U) {
            const uint8_t rs = static_cast<uint8_t>(((hw >> 3U) & 7U) | (((hw >> 6U) & 1U) << 3U));
            const uint8_t rd = static_cast<uint8_t>((hw & 7U) | (((hw >> 7U) & 1U) << 3U));
            if (rs == 14U || rd == 14U) {
                touches_lr = true;
            }
        }
        // LDRB (0x7800..0x7FFF or 0x5C00..0x5DFF)
        if ((hw & 0xF800U) == 0x7800U || (hw & 0xFE00U) == 0x5C00U) {
            load_size = 1;
            load_signed = false;
        }
        // LDSB (0x5600..0x57FF)
        if ((hw & 0xFE00U) == 0x5600U) {
            load_size = 1;
            load_signed = true;
        }
        // LDRH (0x8800..0x8FFF or 0x5A00..0x5BFF)
        if ((hw & 0xF800U) == 0x8800U || (hw & 0xFE00U) == 0x5A00U) {
            load_size = 2;
            load_signed = false;
        }
        // LDSH (0x5E00..0x5FFF)
        if ((hw & 0xFE00U) == 0x5E00U) {
            load_size = 2;
            load_signed = true;
        }
        // LDR (0x6800..0x6FFF or 0x5800..0x59FF)
        if ((hw & 0xF800U) == 0x6800U || (hw & 0xFE00U) == 0x5800U) {
            load_size = 4;
            load_signed = true;
        }
        // BX LR (0x4770) or MOV PC, LR (0x46F7)
        if (hw == 0x4770U || hw == 0x46F7U) {
            if (touches_lr && load_size > 0) {
                is_signed = load_signed;
                return load_size;
            }
            break;
        }
    }
    return 0;
}

}  // namespace

ProgramCFG disassemble_gba(std::span<const uint8_t> rom, const GameConfig& config) {
    ProgramCFG cfg;
    if (config.interpreter_only || rom.size() < 4U) {
        return cfg;
    }

    RomView view{rom};

    std::set<FunctionKey> block_leaders;
    std::set<FunctionKey> function_seeds;
    std::set<FunctionKey> queued_keys;
    std::queue<FunctionKey> worklist;
    std::map<FunctionKey, Instruction> decoded_insts;
    std::map<FunctionKey, std::vector<uint32_t>> indirect_successors;
    std::set<uint32_t> literal_pool_addrs;

    auto add_leader = [&](uint32_t raw_addr, bool thumb, bool is_func) {
        const uint32_t addr = dragon::gba::Bus::normalize_rom_addr(raw_addr & (thumb ? ~1U : ~3U));
        if (!view.is_valid_rom_addr(addr) || is_in_data_range(addr, config)) {
            return;
        }
        if (literal_pool_addrs.count(addr & ~3U) > 0) {
            return;
        }
        const FunctionKey key{addr, thumb};
        block_leaders.insert(key);
        if (is_func) {
            function_seeds.insert(key);
        }
        if (decoded_insts.find(key) == decoded_insts.end() && queued_keys.insert(key).second) {
            worklist.push(key);
        }
    };

    // 1. Entry point at $08000000 (always ARM mode on GBA)
    add_leader(0x08000000U, false, true);

    // 2. Configured entry points from TOML
    std::set<FunctionKey> config_seeds;
    for (const auto& ep : config.entry_points) {
        bool thumb = ep.thumb;
        uint32_t addr = ep.addr;
        if (!ep.has_thumb) {
            if (addr & 1U) {
                thumb = true;
                addr &= ~1U;
            } else if (addr != 0x08000000U && (addr & 3U) == 2U) {
                thumb = true;
            } else if (addr != 0x08000000U) {
                // Heuristic: check if the 16-bit halfword is a standard Thumb PUSH {.., LR}
                // (0xB5xx)
                const uint16_t hw = view.read16(addr);
                if ((hw & 0xFF00U) == 0xB500U) {
                    thumb = true;
                }
            }
        }
        // Never seed on the second halfword of a 32-bit Thumb BL pair
        if (thumb && view.is_valid_rom_addr(addr) && (view.read16(addr) & 0xF800U) == 0xF800U) {
            continue;
        }
        const bool was_seed =
            function_seeds.count(FunctionKey{
                dragon::gba::Bus::normalize_rom_addr(addr & (thumb ? ~1U : ~3U)), thumb}) > 0;
        add_leader(addr, thumb, true);
        if (!was_seed && addr != 0x08000000U) {
            config_seeds.insert(FunctionKey{
                dragon::gba::Bus::normalize_rom_addr(addr & (thumb ? ~1U : ~3U)), thumb});
        }
    }
    // Leaders/seeds discovered by analysis (not just because the TOML listed them)
    std::set<FunctionKey> analysis_leaders;
    std::set<FunctionKey> analysis_seeds;
    auto add_leader_tracked = [&](uint32_t raw_addr, bool thumb, bool is_func) {
        const uint32_t addr = dragon::gba::Bus::normalize_rom_addr(raw_addr & (thumb ? ~1U : ~3U));
        analysis_leaders.insert(FunctionKey{addr, thumb});
        if (is_func) {
            analysis_seeds.insert(FunctionKey{addr, thumb});
        }
        add_leader(raw_addr, thumb, is_func);
    };

    // 3. Configured jump tables from TOML
    for (const auto& jt : config.jump_tables) {
        for (size_t i = 0; i < jt.count; ++i) {
            const uint32_t entry_addr = jt.addr + static_cast<uint32_t>(i * 4U);
            if (!view.is_valid_rom_addr(entry_addr)) {
                break;
            }
            const uint32_t target = view.read32(entry_addr);
            const bool thumb = (target & 1U) != 0U;
            add_leader_tracked(target & ~1U, thumb, false);
        }
    }

    constexpr size_t kMaxBlockInstructions = 1024U;

    // Pass 1: Recursive-descent instruction decoding with constant/literal-pool tracking
    while (!worklist.empty()) {
        const FunctionKey start_key = worklist.front();
        worklist.pop();

        uint32_t pc = start_key.addr;
        const bool thumb = start_key.thumb;

        std::array<bool, 16> reg_known{};
        std::array<uint32_t, 16> reg_val{};
        uint32_t last_cmp_imm = 0U;
        bool has_last_cmp = false;
        size_t block_inst_count = 0U;

        while (view.is_valid_rom_addr(pc) && !is_in_data_range(pc, config)) {
            if (literal_pool_addrs.count(pc & ~3U) > 0 ||
                block_inst_count >= kMaxBlockInstructions) {
                break;
            }
            const FunctionKey cur_key{pc, thumb};
            if (decoded_insts.find(cur_key) != decoded_insts.end()) {
                break;
            }

            Instruction inst = decode_instruction(view, pc, thumb);
            if (!view.is_valid_rom_addr(pc + inst.length - 1U)) {
                break;
            }
            if (!thumb) {
                if (!is_valid_arm_word(inst.raw)) {
                    break;
                }
            } else {
                if (!is_valid_thumb_inst(view, pc, inst)) {
                    break;
                }
            }

            decoded_insts[cur_key] = inst;
            ++block_inst_count;
            const uint32_t next_pc = pc + inst.length;

            if (!thumb) {
                const uint32_t raw = inst.raw;
                const uint8_t cond = static_cast<uint8_t>((raw >> 28U) & 0x0FU);
                const bool uncond = (cond == 0x0EU);

                // Track PC-relative LDR Rd, [PC, #+-imm12]
                if ((raw & 0x0F7F0000U) == 0x051F0000U) {
                    const bool up = (raw & (1U << 23)) != 0U;
                    const uint8_t rd = static_cast<uint8_t>((raw >> 12U) & 0x0FU);
                    const uint32_t imm12 = raw & 0x0FFFU;
                    const uint32_t lit_addr = up ? ((pc + 8U) + imm12) : ((pc + 8U) - imm12);
                    if (view.is_valid_rom_addr(lit_addr)) {
                        literal_pool_addrs.insert(lit_addr & ~3U);
                        const uint32_t val = view.read32(lit_addr);
                        if (rd < 15U) {
                            reg_known[rd] = true;
                            reg_val[rd] = val;
                        } else if (rd == 15U && is_plausible_arm_entry(view, val & ~3U)) {
                            add_leader_tracked(val & ~3U, false, true);
                        }
                    } else if (rd < 15U) {
                        reg_known[rd] = false;
                    }
                } else if ((raw & 0x0FEF0000U) == 0x028F0000U) {
                    // ADD Rd, PC, #imm
                    const uint8_t rd = static_cast<uint8_t>((raw >> 12U) & 0x0FU);
                    const uint32_t imm8 = raw & 0xFFU;
                    const uint32_t rot = ((raw >> 8U) & 0x0FU) * 2U;
                    const uint32_t imm = gba_ror32(imm8, rot);
                    if (rd < 15U) {
                        reg_known[rd] = true;
                        reg_val[rd] = (pc + 8U) + imm;
                    }
                } else if ((raw & 0x0FFF0000U) == 0x03A00000U) {
                    // MOV Rd, #imm
                    const uint8_t rd = static_cast<uint8_t>((raw >> 12U) & 0x0FU);
                    const uint32_t imm8 = raw & 0xFFU;
                    const uint32_t rot = ((raw >> 8U) & 0x0FU) * 2U;
                    if (rd < 15U) {
                        reg_known[rd] = true;
                        reg_val[rd] = gba_ror32(imm8, rot);
                    }
                } else if ((raw & 0x0FFFFFFFU) == 0x01A0E00FU) {
                    // MOV LR, PC
                    reg_known[14] = true;
                    reg_val[14] = pc + 8U;
                } else if ((raw & 0x0FF0F000U) == 0x03500000U) {
                    // CMP Rn, #imm
                    const uint32_t imm8 = raw & 0xFFU;
                    const uint32_t rot = ((raw >> 8U) & 0x0FU) * 2U;
                    last_cmp_imm = gba_ror32(imm8, rot);
                    has_last_cmp = true;
                } else {
                    // Invalidate destination registers on non-constant writes
                    if (((raw >> 26U) & 3U) == 0U || ((raw >> 26U) & 3U) == 1U) {
                        const bool is_store =
                            (((raw >> 26U) & 3U) == 1U) && ((raw & (1U << 20)) == 0U);
                        const uint8_t op = static_cast<uint8_t>((raw >> 21U) & 0x0FU);
                        const bool is_cmp_tst =
                            (((raw >> 26U) & 3U) == 0U) && (op >= 0x08U && op <= 0x0BU);
                        if (!is_store && !is_cmp_tst) {
                            const uint8_t rd = static_cast<uint8_t>((raw >> 12U) & 0x0FU);
                            if (rd < 15U) {
                                reg_known[rd] = false;
                            }
                        }
                    } else if (((raw >> 25U) & 7U) == 4U && (raw & (1U << 20)) != 0U) {
                        // LDM
                        for (uint8_t r = 0; r < 15U; ++r) {
                            if (raw & (1U << r)) {
                                reg_known[r] = false;
                            }
                        }
                    }
                }

                // Detect store of user IRQ vector to [0x03007FFC] or [0x03FFFFFC]
                // Single Data Transfer, immediate offset, word store: (raw & 0x0E500000) ==
                // 0x04000000
                if ((raw & 0x0E500000U) == 0x04000000U) {
                    const bool pre = (raw & (1U << 24)) != 0U;
                    const bool up = (raw & (1U << 23)) != 0U;
                    const uint8_t rn = static_cast<uint8_t>((raw >> 16U) & 0x0FU);
                    const uint8_t rd = static_cast<uint8_t>((raw >> 12U) & 0x0FU);
                    const uint32_t imm12 = raw & 0x0FFFU;
                    if (rn < 15U && reg_known[rn] && rd < 15U && reg_known[rd]) {
                        const uint32_t mem_addr =
                            pre ? (up ? (reg_val[rn] + imm12) : (reg_val[rn] - imm12))
                                : reg_val[rn];
                        if (mem_addr == 0x03007FFCU || mem_addr == 0x03FFFFFCU) {
                            const uint32_t v = reg_val[rd];
                            if (view.is_valid_rom_addr(v & ~1U)) {
                                add_leader_tracked(v & ~1U, (v & 1U) != 0U, true);
                            }
                        }
                    }
                }

                // ARM Jump Table: LDR<cc> PC, [PC, Rm, LSL #2] (0x079FF100) or ADD<cc> PC, PC, Rm,
                // LSL #2 (0x008FF100)
                if ((raw & 0x0FFFFFF0U) == 0x079FF100U || (raw & 0x0FEFFFF0U) == 0x008FF100U) {
                    const bool is_ldr = (raw & 0x04000000U) != 0U;
                    const uint32_t count =
                        has_last_cmp ? std::min<uint32_t>(last_cmp_imm + 1U, 64U) : 8U;
                    const uint32_t tbl_base = pc + 8U;
                    for (uint32_t i = 0; i < count; ++i) {
                        const uint32_t entry_addr = tbl_base + i * 4U;
                        if (!view.is_valid_rom_addr(entry_addr)) {
                            break;
                        }
                        if (is_ldr) {
                            literal_pool_addrs.insert(entry_addr & ~3U);
                            const uint32_t target = view.read32(entry_addr);
                            if (!view.is_valid_rom_addr(target & ~1U)) {
                                break;
                            }
                            const uint32_t norm_t =
                                dragon::gba::Bus::normalize_rom_addr(target & ~1U);
                            const bool t_thumb = (target & 1U) != 0U;
                            if (!t_thumb && !is_plausible_arm_entry(view, norm_t)) {
                                break;
                            }
                            add_leader_tracked(norm_t, t_thumb, false);
                            if (!t_thumb) {
                                indirect_successors[cur_key].push_back(norm_t);
                            }
                        } else {
                            const uint32_t norm_t =
                                dragon::gba::Bus::normalize_rom_addr(entry_addr);
                            if (!is_plausible_arm_entry(view, norm_t)) {
                                break;
                            }
                            add_leader_tracked(norm_t, false, false);
                            indirect_successors[cur_key].push_back(norm_t);
                        }
                    }
                    has_last_cmp = false;
                    if (uncond) {
                        break;
                    }
                    add_leader_tracked(next_pc, false, false);
                    pc = next_pc;
                    continue;
                }

                // BX Rm
                if ((raw & 0x0FFFFFF0U) == 0x012FFF10U) {
                    const uint8_t rm = static_cast<uint8_t>(raw & 0x0FU);
                    if (rm < 15U && reg_known[rm]) {
                        const uint32_t target = reg_val[rm];
                        const bool t_thumb = (target & 1U) != 0U;
                        const uint32_t norm_t =
                            dragon::gba::Bus::normalize_rom_addr(target & (t_thumb ? ~1U : ~3U));
                        if (t_thumb || is_plausible_arm_entry(view, norm_t)) {
                            add_leader_tracked(norm_t, t_thumb, true);
                        }
                    }
                    // If LR was just set to next_pc (MOV LR, PC; BX Rm), continue after BX!
                    const bool is_indirect_call = reg_known[14] && (reg_val[14] == next_pc);
                    if (!uncond || is_indirect_call) {
                        for (uint8_t r : {0U, 1U, 2U, 3U, 12U, 14U}) {
                            reg_known[r] = false;
                        }
                        has_last_cmp = false;
                        add_leader_tracked(next_pc, false, false);
                        pc = next_pc;
                        continue;
                    }
                    break;
                }

                // B / BL
                if (((raw >> 25U) & 7U) == 5U) {
                    const bool link = (raw & (1U << 24)) != 0U;
                    if (link) {
                        if (is_plausible_arm_entry(view, inst.branch_target)) {
                            add_leader_tracked(inst.branch_target, false, true);
                        }
                        for (uint8_t r : {0U, 1U, 2U, 3U, 12U, 14U}) {
                            reg_known[r] = false;
                        }
                        has_last_cmp = false;
                        add_leader_tracked(next_pc, false, false);
                        pc = next_pc;
                        continue;
                    }
                    if (is_valid_arm_word(view.read32(inst.branch_target))) {
                        add_leader_tracked(inst.branch_target, false, false);
                    }
                    if (!uncond) {
                        add_leader_tracked(next_pc, false, false);
                        pc = next_pc;
                        continue;
                    }
                    break;
                }

                // SWI 0x00 (SoftReset) does not return
                if ((raw & 0x0F000000U) == 0x0F000000U && ((raw >> 16U) & 0xFFU) == 0x00U &&
                    uncond) {
                    break;
                }

                // Return / PC write via Data Processing (Rd == 15) or LDR PC or LDM {.., PC}
                const bool dp_writes_pc =
                    (((raw >> 26U) & 3U) == 0U) && (((raw >> 12U) & 0x0FU) == 15U) &&
                    (((raw >> 21U) & 0x0FU) < 0x8U || ((raw >> 21U) & 0x0FU) >= 0xCU);
                const bool ldr_writes_pc = (((raw >> 26U) & 3U) == 1U) &&
                                           ((raw & (1U << 20)) != 0U) &&
                                           (((raw >> 12U) & 0x0FU) == 15U);
                const bool ldm_writes_pc = (((raw >> 25U) & 7U) == 4U) &&
                                           ((raw & (1U << 20)) != 0U) && ((raw & 0x8000U) != 0U);

                if (dp_writes_pc || ldr_writes_pc || ldm_writes_pc) {
                    if (!uncond) {
                        add_leader_tracked(next_pc, false, false);
                        pc = next_pc;
                        continue;
                    }
                    break;
                }
            } else {
                // Thumb mode
                if (inst.is_thumb_bl32) {
                    add_leader_tracked(inst.branch_target, true, true);

                    // Check if target is a `_call_via_rX` veneer (single `BX Rx`, 0x4700..0x4778)
                    // and Rx is known in the caller right before the BL!
                    if (view.is_valid_rom_addr(inst.branch_target)) {
                        const uint16_t thunk_hw = view.read16(inst.branch_target);
                        if ((thunk_hw & 0xFF87U) == 0x4700U) {
                            const uint8_t rx = static_cast<uint8_t>((thunk_hw >> 3U) & 0x0FU);
                            if (rx < 15U && reg_known[rx]) {
                                const uint32_t via_target = reg_val[rx];
                                const bool t_thumb = (via_target & 1U) != 0U;
                                const uint32_t norm_t = dragon::gba::Bus::normalize_rom_addr(
                                    via_target & (t_thumb ? ~1U : ~3U));
                                if (t_thumb || is_plausible_arm_entry(view, norm_t)) {
                                    add_leader_tracked(norm_t, t_thumb, true);
                                }
                            }
                        }
                    }

                    // Check if target is a GCC __gnu_thumb1_case_* helper consuming an inline
                    // offset table!
                    bool is_signed = false;
                    const int elem_sz =
                        detect_thumb_case_helper(view, inst.branch_target, is_signed);
                    if (elem_sz > 0) {
                        const uint32_t count =
                            has_last_cmp ? std::min<uint32_t>(last_cmp_imm + 1U, 128U) : 4U;
                        const uint32_t table_base =
                            (elem_sz == 2) ? ((next_pc + 1U) & ~1U)
                                           : ((elem_sz == 4) ? ((next_pc + 3U) & ~3U) : next_pc);
                        for (uint32_t i = 0; i < count; ++i) {
                            const uint32_t entry_addr =
                                table_base + i * static_cast<uint32_t>(elem_sz);
                            if (!view.is_valid_rom_addr(entry_addr)) {
                                break;
                            }
                            int32_t offset = 0;
                            if (elem_sz == 1) {
                                const uint8_t b = view.read8(entry_addr);
                                offset = is_signed
                                             ? static_cast<int32_t>(static_cast<int8_t>(b)) * 2
                                             : static_cast<int32_t>(b) * 2;
                            } else if (elem_sz == 2) {
                                const uint16_t h = view.read16(entry_addr);
                                offset = is_signed
                                             ? static_cast<int32_t>(static_cast<int16_t>(h)) * 2
                                             : static_cast<int32_t>(h) * 2;
                            } else if (elem_sz == 4) {
                                offset = static_cast<int32_t>(view.read32(entry_addr));
                            }
                            const uint32_t base_for_offset =
                                (elem_sz == 4) ? (table_base + 4U) : table_base;
                            const uint32_t case_target = dragon::gba::Bus::normalize_rom_addr(
                                static_cast<uint32_t>(static_cast<int32_t>(base_for_offset) +
                                                      offset) &
                                ~1U);
                            add_leader_tracked(case_target, true, false);
                            indirect_successors[cur_key].push_back(case_target);
                        }
                        break;
                    }

                    // Caller-saved registers are clobbered across BL
                    for (uint8_t r : {0U, 1U, 2U, 3U, 12U, 14U}) {
                        reg_known[r] = false;
                    }
                    has_last_cmp = false;
                    add_leader_tracked(next_pc, true, false);
                    pc = next_pc;
                    continue;
                }

                const uint16_t hw = static_cast<uint16_t>(inst.raw & 0xFFFFU);

                // Track Thumb Format 3 MOV/CMP Rd, #imm8
                if ((hw & 0xF800U) == 0x2000U) {
                    const uint8_t rd = static_cast<uint8_t>((hw >> 8U) & 7U);
                    reg_known[rd] = true;
                    reg_val[rd] = hw & 0xFFU;
                } else if ((hw & 0xF800U) == 0x2800U) {
                    last_cmp_imm = hw & 0xFFU;
                    has_last_cmp = true;
                } else if ((hw & 0xF800U) == 0x3000U) {
                    const uint8_t rd = static_cast<uint8_t>((hw >> 8U) & 7U);
                    if (reg_known[rd]) {
                        reg_val[rd] += (hw & 0xFFU);
                    }
                } else if ((hw & 0xF800U) == 0x3800U) {
                    const uint8_t rd = static_cast<uint8_t>((hw >> 8U) & 7U);
                    if (reg_known[rd]) {
                        reg_val[rd] -= (hw & 0xFFU);
                    }
                } else if ((hw & 0xF800U) == 0x4800U) {
                    // Format 6: LDR Rd, [PC, #imm8*4]
                    const uint8_t rd = static_cast<uint8_t>((hw >> 8U) & 7U);
                    const uint32_t lit_addr = ((pc + 4U) & ~3U) + (hw & 0xFFU) * 4U;
                    if (view.is_valid_rom_addr(lit_addr)) {
                        literal_pool_addrs.insert(lit_addr & ~3U);
                        const uint32_t val = view.read32(lit_addr);
                        reg_known[rd] = true;
                        reg_val[rd] = val;
                        const uint32_t norm_v = dragon::gba::Bus::normalize_rom_addr(val & ~1U);
                        if ((val & 1U) != 0U && (norm_v & 3U) == 0U &&
                            is_plausible_thumb_prologue(view, norm_v)) {
                            add_leader_tracked(norm_v, true, true);
                        }
                    } else {
                        reg_known[rd] = false;
                    }
                } else if ((hw & 0xF800U) == 0xA000U) {
                    // Format 12: ADD Rd, PC, #imm8*4
                    const uint8_t rd = static_cast<uint8_t>((hw >> 8U) & 7U);
                    reg_known[rd] = true;
                    reg_val[rd] = ((pc + 4U) & ~3U) + (hw & 0xFFU) * 4U;
                } else if ((hw & 0xFF00U) == 0x4600U) {
                    // Format 5: MOV Rd, Rs
                    const uint8_t rs =
                        static_cast<uint8_t>(((hw >> 3U) & 7U) | (((hw >> 6U) & 1U) << 3U));
                    const uint8_t rd = static_cast<uint8_t>((hw & 7U) | (((hw >> 7U) & 1U) << 3U));
                    if (rd < 15U) {
                        reg_known[rd] = (rs < 15U) ? reg_known[rs] : false;
                        reg_val[rd] = (rs < 15U) ? reg_val[rs] : 0U;
                    }
                } else if ((hw & 0xE000U) == 0x0000U || (hw & 0xFC00U) == 0x4000U ||
                           (hw & 0xF000U) == 0x5000U || (hw & 0xE000U) == 0x6000U ||
                           (hw & 0xF000U) == 0x8000U) {
                    // Formats 1, 2, 4, 7, 8, 9, 10: destination/load register is bits 0..2
                    const uint8_t rd = static_cast<uint8_t>(hw & 7U);
                    reg_known[rd] = false;
                } else if ((hw & 0xF800U) == 0x9800U || (hw & 0xF800U) == 0xA800U) {
                    // Format 11 LDR Rd, [SP, #imm] / Format 12 ADD Rd, SP, #imm: bits 8..10
                    const uint8_t rd = static_cast<uint8_t>((hw >> 8U) & 7U);
                    reg_known[rd] = false;
                } else if ((hw & 0xFE00U) == 0xBC00U || (hw & 0xF800U) == 0xC800U) {
                    // POP {Rlist} or LDMIA Rb!, {Rlist}
                    for (uint8_t r = 0; r < 8U; ++r) {
                        if (hw & (1U << r)) {
                            reg_known[r] = false;
                        }
                    }
                }

                // Format 5: BX Rs (0x4700..0x477F) or MOV/ADD PC, Rs
                if ((hw & 0xFF80U) == 0x4700U) {
                    const uint8_t rs = static_cast<uint8_t>((hw >> 3U) & 0x0FU);
                    if (rs == 15U) {
                        // Thumb `BX PC` veneer switches to ARM at (pc + 4) & ~2!
                        const uint32_t arm_target = (pc + 4U) & ~3U;
                        if (is_plausible_arm_entry(view, arm_target)) {
                            add_leader_tracked(arm_target, false, true);
                        }
                    } else if (rs < 15U && reg_known[rs]) {
                        const uint32_t target = reg_val[rs];
                        const bool t_thumb = (target & 1U) != 0U;
                        const uint32_t norm_t =
                            dragon::gba::Bus::normalize_rom_addr(target & (t_thumb ? ~1U : ~3U));
                        if (t_thumb || is_plausible_arm_entry(view, norm_t)) {
                            add_leader_tracked(norm_t, t_thumb, true);
                        }
                    }
                    break;
                }
                if (((hw & 0xFF00U) == 0x4600U || (hw & 0xFF00U) == 0x4400U) &&
                    ((hw & 0x0087U) == 0x0087U)) {
                    // MOV PC, Rs or ADD PC, Rs (Thumb switch / jump table)
                    for (uint8_t r = 0; r < 15U; ++r) {
                        if (!reg_known[r]) {
                            continue;
                        }
                        const uint32_t tbl_base = reg_val[r];
                        if (!view.is_valid_rom_addr(tbl_base) || (tbl_base & 3U) != 0U) {
                            continue;
                        }
                        const uint32_t first_target = view.read32(tbl_base);
                        if (first_target > tbl_base && first_target <= tbl_base + 0x400U &&
                            (first_target & 1U) == 0U && view.is_valid_rom_addr(first_target)) {
                            const uint32_t auto_count = (first_target - tbl_base) / 4U;
                            if (auto_count >= 2U && auto_count <= 64U) {
                                bool all_valid = true;
                                for (uint32_t i = 0; i < auto_count; ++i) {
                                    const uint32_t t = view.read32(tbl_base + i * 4U);
                                    if (t < first_target || t >= tbl_base + 0x2000U ||
                                        (t & 1U) != 0U || !view.is_valid_rom_addr(t)) {
                                        all_valid = false;
                                        break;
                                    }
                                }
                                if (all_valid) {
                                    for (uint32_t i = 0; i < auto_count; ++i) {
                                        const uint32_t entry_addr = tbl_base + i * 4U;
                                        literal_pool_addrs.insert(entry_addr & ~3U);
                                        const uint32_t t = view.read32(entry_addr);
                                        const uint32_t norm_t =
                                            dragon::gba::Bus::normalize_rom_addr(t & ~1U);
                                        add_leader_tracked(norm_t, true, false);
                                        indirect_successors[cur_key].push_back(norm_t);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    break;
                }

                // Format 14: POP {.., PC} (0xBD00..0xBDFF)
                if ((hw & 0xFF00U) == 0xBD00U) {
                    break;
                }

                // Format 17: SWI 0x00 (SoftReset) does not return
                if (hw == 0xDF00U) {
                    break;
                }

                // Format 16: Conditional branch B<cc> label
                if ((hw & 0xF000U) == 0xD000U && ((hw >> 8U) & 0x0FU) < 0x0EU) {
                    add_leader_tracked(inst.branch_target, true, false);
                    add_leader_tracked(next_pc, true, false);
                    pc = next_pc;
                    continue;
                }

                // Format 18: Unconditional branch B label
                if ((hw & 0xF800U) == 0xE000U) {
                    add_leader_tracked(inst.branch_target, true, false);
                    break;
                }
            }

            pc = next_pc;
        }
    }

    // Remove any decoded instructions that turned out to lie inside literal pools
    for (uint32_t lit_addr : literal_pool_addrs) {
        decoded_insts.erase(FunctionKey{lit_addr, false});
        decoded_insts.erase(FunctionKey{lit_addr, true});
        decoded_insts.erase(FunctionKey{lit_addr + 2U, true});
    }

    // Demote configured entry points that are merely fall-through addresses inside straight-line
    // code (e.g. per-instruction addresses polluted into the TOML by an older runtime fallback
    // logger, or BL return sites). Treating those as function boundaries shatters functions into
    // 1-instruction fragments that bounce through the dispatcher on every step.
    auto is_thumb_terminator = [](uint16_t hw) {
        return (hw & 0xFF80U) == 0x4700U || (hw & 0xFF00U) == 0xBD00U ||
               (hw & 0xF800U) == 0xE000U || hw == 0xDF00U ||
               (((hw & 0xFF00U) == 0x4600U || (hw & 0xFF00U) == 0x4400U) &&
                ((hw & 0x0087U) == 0x0087U));
    };
    auto is_arm_terminator = [](uint32_t raw) {
        if (((raw >> 28U) & 0x0FU) != 0x0EU) {
            return false;
        }
        if ((raw & 0x0FFFFFF0U) == 0x012FFF10U) {
            return true;  // BX
        }
        if (((raw >> 25U) & 7U) == 5U && (raw & (1U << 24)) == 0U) {
            return true;  // B
        }
        const bool dp_pc = (((raw >> 26U) & 3U) == 0U) && (((raw >> 12U) & 0x0FU) == 15U);
        const bool ldr_pc = (((raw >> 26U) & 3U) == 1U) && ((raw & (1U << 20)) != 0U) &&
                            (((raw >> 12U) & 0x0FU) == 15U);
        const bool ldm_pc =
            (((raw >> 25U) & 7U) == 4U) && ((raw & (1U << 20)) != 0U) && ((raw & 0x8000U) != 0U);
        return dp_pc || ldr_pc || ldm_pc;
    };
    for (const FunctionKey& k : config_seeds) {
        if (analysis_seeds.count(k) > 0) {
            continue;
        }
        bool mid_block = false;
        if (k.thumb) {
            auto p2 = decoded_insts.find(FunctionKey{k.addr - 2U, true});
            if (p2 != decoded_insts.end() && p2->second.length == 2U &&
                !is_thumb_terminator(static_cast<uint16_t>(p2->second.raw & 0xFFFFU))) {
                mid_block = true;
            }
            auto p4 = decoded_insts.find(FunctionKey{k.addr - 4U, true});
            if (p4 != decoded_insts.end() && p4->second.is_thumb_bl32) {
                mid_block = true;
            }
        } else {
            auto p4 = decoded_insts.find(FunctionKey{k.addr - 4U, false});
            if (p4 != decoded_insts.end() && !is_arm_terminator(p4->second.raw)) {
                mid_block = true;
            }
        }
        if (mid_block) {
            function_seeds.erase(k);
            if (analysis_leaders.count(k) == 0) {
                block_leaders.erase(k);
            }
        }
    }

    // Pass 2: Build basic blocks from leaders
    for (const FunctionKey& leader : block_leaders) {
        if (decoded_insts.find(leader) == decoded_insts.end()) {
            continue;
        }

        BasicBlock block;
        block.start_addr = leader.addr;
        block.thumb = leader.thumb;
        uint32_t pc = leader.addr;

        while (true) {
            auto it = decoded_insts.find(FunctionKey{pc, leader.thumb});
            if (it == decoded_insts.end()) {
                break;
            }
            const Instruction& inst = it->second;
            block.instructions.push_back(inst);
            const uint32_t next_pc = pc + inst.length;
            block.end_addr = next_pc;

            const auto ind_it = indirect_successors.find(FunctionKey{pc, leader.thumb});
            if (ind_it != indirect_successors.end()) {
                for (uint32_t succ : ind_it->second) {
                    block.successors.push_back(succ);
                }
            }

            if (!leader.thumb) {
                const uint32_t raw = inst.raw;
                const uint8_t cond = static_cast<uint8_t>((raw >> 28U) & 0x0FU);
                const bool uncond = (cond == 0x0EU);

                if ((raw & 0x0FFFFFF0U) == 0x079FF100U || (raw & 0x0FEFFFF0U) == 0x008FF100U) {
                    if (!uncond) {
                        block.successors.push_back(next_pc);
                    }
                    break;
                }
                if ((raw & 0x0FFFFFF0U) == 0x012FFF10U) {  // BX
                    if (!uncond) {
                        block.successors.push_back(next_pc);
                    }
                    break;
                }
                if (((raw >> 25U) & 7U) == 5U) {  // B / BL
                    const bool link = (raw & (1U << 24)) != 0U;
                    if (link) {
                        block.successors.push_back(next_pc);
                        break;
                    }
                    block.successors.push_back(inst.branch_target);
                    if (!uncond) {
                        block.successors.push_back(next_pc);
                    }
                    break;
                }
                if ((raw & 0x0F000000U) == 0x0F000000U && ((raw >> 16U) & 0xFFU) == 0x00U &&
                    uncond) {
                    break;
                }
                const bool dp_writes_pc =
                    (((raw >> 26U) & 3U) == 0U) && (((raw >> 12U) & 0x0FU) == 15U) &&
                    (((raw >> 21U) & 0x0FU) < 0x8U || ((raw >> 21U) & 0x0FU) >= 0xCU);
                const bool ldr_writes_pc = (((raw >> 26U) & 3U) == 1U) &&
                                           ((raw & (1U << 20)) != 0U) &&
                                           (((raw >> 12U) & 0x0FU) == 15U);
                const bool ldm_writes_pc = (((raw >> 25U) & 7U) == 4U) &&
                                           ((raw & (1U << 20)) != 0U) && ((raw & 0x8000U) != 0U);
                if (dp_writes_pc || ldr_writes_pc || ldm_writes_pc) {
                    if (!uncond) {
                        block.successors.push_back(next_pc);
                    }
                    break;
                }
            } else {
                if (inst.is_thumb_bl32) {
                    if (ind_it == indirect_successors.end()) {
                        block.successors.push_back(next_pc);
                    }
                    break;
                }
                const uint16_t hw = static_cast<uint16_t>(inst.raw & 0xFFFFU);
                if ((hw & 0xFF80U) == 0x4700U || (hw & 0xFF00U) == 0xBD00U || hw == 0xDF00U ||
                    (((hw & 0xFF00U) == 0x4600U || (hw & 0xFF00U) == 0x4400U) &&
                     ((hw & 0x0087U) == 0x0087U))) {
                    break;
                }
                if ((hw & 0xF000U) == 0xD000U && ((hw >> 8U) & 0x0FU) < 0x0EU) {
                    block.successors.push_back(inst.branch_target);
                    block.successors.push_back(next_pc);
                    break;
                }
                if ((hw & 0xF800U) == 0xE000U) {
                    block.successors.push_back(inst.branch_target);
                    break;
                }
            }

            const FunctionKey next_key{next_pc, leader.thumb};
            if (block_leaders.count(next_key) > 0 ||
                decoded_insts.find(next_key) == decoded_insts.end()) {
                if (decoded_insts.find(next_key) != decoded_insts.end()) {
                    block.successors.push_back(next_pc);
                }
                break;
            }
            pc = next_pc;
        }

        cfg.blocks[leader] = std::move(block);
    }

    // Pass 3: Partition reachable blocks into strictly disjoint functions (no block duplication)
    constexpr size_t kMaxFunctionInstructions = 2048U;
    std::set<FunctionKey> claimed_blocks;
    std::queue<FunctionKey> seed_queue;
    // Snapshot of true seeds: blocks promoted to standalone functions below must NOT shrink the
    // natural range of other functions (that caused a domino effect of micro-functions).
    const std::set<FunctionKey> initial_seeds = function_seeds;
    for (const FunctionKey& f_key : function_seeds) {
        if (cfg.blocks.count(f_key)) {
            seed_queue.push(f_key);
        }
    }

    while (!seed_queue.empty()) {
        const FunctionKey f_key = seed_queue.front();
        seed_queue.pop();
        if (!cfg.blocks.count(f_key) || cfg.functions.count(f_key)) {
            continue;
        }

        // Find the next known function seed address to bound this function's natural range
        uint32_t next_seed_addr = 0x0E000000U;
        for (auto it = initial_seeds.upper_bound(FunctionKey{f_key.addr, true});
             it != initial_seeds.end(); ++it) {
            if (it->addr > f_key.addr) {
                next_seed_addr = it->addr;
                break;
            }
        }

        Function func;
        func.entry_addr = f_key.addr;
        func.thumb = f_key.thumb;

        std::queue<uint32_t> q;
        std::set<uint32_t> visited;
        q.push(f_key.addr);
        visited.insert(f_key.addr);
        claimed_blocks.insert(f_key);

        while (!q.empty()) {
            const uint32_t b_addr = q.front();
            q.pop();
            func.block_addrs.insert(b_addr);

            auto b_it = cfg.blocks.find(FunctionKey{b_addr, f_key.thumb});
            if (b_it != cfg.blocks.end()) {
                for (const auto& inst : b_it->second.instructions) {
                    func.instructions[inst.addr] = inst;
                }
                for (uint32_t succ : b_it->second.successors) {
                    const FunctionKey succ_key{succ, f_key.thumb};
                    if (!cfg.blocks.count(succ_key) || visited.count(succ)) {
                        continue;
                    }
                    // If successor crosses into another function or exceeds size cap, promote it to
                    // a standalone function instead of duplicating blocks across functions.
                    if (function_seeds.count(succ_key) || claimed_blocks.count(succ_key) ||
                        succ < f_key.addr || succ >= next_seed_addr ||
                        func.instructions.size() >= kMaxFunctionInstructions) {
                        if (function_seeds.insert(succ_key).second) {
                            seed_queue.push(succ_key);
                        }
                        continue;
                    }
                    visited.insert(succ);
                    claimed_blocks.insert(succ_key);
                    q.push(succ);
                }
            }
        }

        cfg.functions[f_key] = std::move(func);
    }

    return cfg;
}

}  // namespace dragon::gbarecomp
