#include "smsrecomp/disassembler.h"

#include <algorithm>
#include <queue>

#include "sms/opcodes.h"

namespace dragon::smsrecomp {

namespace {

uint8_t read_rom_byte(const sms::Cartridge& cart, uint8_t bank, uint16_t addr) noexcept {
    const auto& rom = cart.rom();
    if (rom.empty() || addr >= 0xC000U) {
        return 0x00U;
    }
    if (addr < 0x0400U && cart.mapper_type() == sms::MapperType::Sega) {
        return rom[addr];
    }
    const size_t b = bank % cart.bank_count();
    const uint16_t off = static_cast<uint16_t>(addr & 0x3FFFU);
    return rom[b * sms::Cartridge::kBankSize + off];
}

uint16_t read_rom_word(const sms::Cartridge& cart, uint8_t bank, uint16_t addr) noexcept {
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

uint8_t target_bank_for(const sms::Cartridge& cart, uint8_t cur_bank, uint16_t target) noexcept {
    if (target < 0x0400U && cart.mapper_type() == sms::MapperType::Sega) {
        return 0U;
    }
    // Same 16 KiB slot stays in cur_bank; default slot 0 -> bank 0, slot 1 -> bank 1, slot 2 -> bank 2
    const uint16_t cur_slot = 0xFFFFU;
    (void)cur_slot;
    if (target < 0x4000U) return 0U;
    if (target < 0x8000U) return (cur_bank > 0U) ? cur_bank : static_cast<uint8_t>(cart.bank_count() > 1U ? 1U : 0U);
    return (cur_bank >= 2U) ? cur_bank : static_cast<uint8_t>(cart.bank_count() > 2U ? 2U : 0U);
}

}  // namespace

DecodedInstr decode_instruction(const sms::Cartridge& cart, uint8_t bank, uint16_t addr) noexcept {
    DecodedInstr ins{};
    ins.addr = addr;
    ins.bank = bank;

    const uint8_t b0 = read_rom_byte(cart, bank, addr);
    if (b0 == 0xCBU) {
        const uint8_t op = read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 1U));
        const auto& info = sms::opcode_info_cb(op);
        ins.prefix = Prefix::CB;
        ins.opcode = op;
        ins.length = info.length;
        ins.cycles = info.cycles;
        ins.cycles_taken = info.cycles_taken;
        return ins;
    }

    if (b0 == 0xEDU) {
        const uint8_t op = read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 1U));
        const auto& info = sms::opcode_info_ed(op);
        ins.prefix = Prefix::ED;
        ins.opcode = op;
        ins.length = info.length;
        ins.cycles = info.cycles;
        ins.cycles_taken = info.cycles_taken;
        if (info.length == 4U) {
            ins.imm = read_rom_word(cart, bank, static_cast<uint16_t>(addr + 2U));
        }
        // RETN / RETI terminate basic block
        if (op == 0x45U || op == 0x4DU || op == 0x55U || op == 0x5DU || op == 0x65U ||
            op == 0x6DU || op == 0x75U || op == 0x7DU) {
            ins.is_terminator = true;
        }
        return ins;
    }

    if (b0 == 0xDDU || b0 == 0xFDU) {
        const bool is_ix = (b0 == 0xDDU);
        const uint8_t op = read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 1U));
        if (op == 0xCBU) {
            ins.prefix = is_ix ? Prefix::DDCB : Prefix::FDCB;
            ins.disp = static_cast<int8_t>(read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 2U)));
            ins.opcode = read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 3U));
            ins.length = 4U;
            ins.cycles = ((ins.opcode & 0xC0U) == 0x40U) ? 20U : 23U;
            ins.cycles_taken = ins.cycles;
            return ins;
        }
        const auto info = sms::opcode_info_index(op);
        ins.prefix = is_ix ? Prefix::DD : Prefix::FD;
        ins.opcode = op;
        ins.length = info.length;
        ins.cycles = info.cycles;
        ins.cycles_taken = info.cycles_taken;

        if (op == 0x21U || op == 0x22U || op == 0x2AU) {
            ins.imm = read_rom_word(cart, bank, static_cast<uint16_t>(addr + 2U));
        } else if (op == 0x26U || op == 0x2EU) {
            ins.imm = read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 2U));
        } else if (op == 0x36U) {
            ins.disp = static_cast<int8_t>(read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 2U)));
            ins.imm = read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 3U));
        } else if (info.length == 3U) {
            ins.disp = static_cast<int8_t>(read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 2U)));
        }
        if (op == 0xE9U) {
            ins.is_terminator = true;
            ins.is_indirect_jump = true;
        }
        return ins;
    }

    // Unprefixed base opcode
    const auto& info = sms::opcode_info_base(b0);
    ins.prefix = Prefix::None;
    ins.opcode = b0;
    ins.length = info.length > 0U ? info.length : 1U;
    ins.cycles = info.cycles;
    ins.cycles_taken = info.cycles_taken;

    if (ins.length == 2U) {
        ins.imm = read_rom_byte(cart, bank, static_cast<uint16_t>(addr + 1U));
        ins.disp = static_cast<int8_t>(ins.imm & 0xFFU);
    } else if (ins.length == 3U) {
        ins.imm = read_rom_word(cart, bank, static_cast<uint16_t>(addr + 1U));
    }

    switch (b0) {
        case 0x10U:  // DJNZ d
        case 0x20U: case 0x28U: case 0x30U: case 0x38U:  // JR cc, d
            ins.has_branch = true;
            ins.is_conditional = true;
            ins.target_addr = static_cast<uint16_t>(addr + ins.length + ins.disp);
            break;
        case 0x18U:  // JR d
            ins.has_branch = true;
            ins.is_terminator = true;
            ins.target_addr = static_cast<uint16_t>(addr + ins.length + ins.disp);
            break;
        case 0xC2U: case 0xCAU: case 0xD2U: case 0xDAU:
        case 0xE2U: case 0xEAU: case 0xF2U: case 0xFAU:  // JP cc, nn
            ins.has_branch = true;
            ins.is_conditional = true;
            ins.target_addr = ins.imm;
            break;
        case 0xC3U:  // JP nn
            ins.has_branch = true;
            ins.is_terminator = true;
            ins.target_addr = ins.imm;
            break;
        case 0xE9U:  // JP (HL)
            ins.is_terminator = true;
            ins.is_indirect_jump = true;
            break;
        case 0xC9U:  // RET
            ins.is_terminator = true;
            break;
        case 0xC4U: case 0xCCU: case 0xD4U: case 0xDCU:
        case 0xE4U: case 0xECU: case 0xF4U: case 0xFCU:  // CALL cc, nn
            ins.has_branch = true;
            ins.is_call = true;
            ins.is_conditional = true;
            ins.target_addr = ins.imm;
            break;
        case 0xCDU:  // CALL nn
            ins.has_branch = true;
            ins.is_call = true;
            ins.target_addr = ins.imm;
            break;
        case 0xC7U: case 0xCFU: case 0xD7U: case 0xDFU:
        case 0xE7U: case 0xEFU: case 0xF7U: case 0xFFU:  // RST p
            ins.has_branch = true;
            ins.is_call = true;
            ins.target_addr = static_cast<uint16_t>(b0 & 0x38U);
            break;
        default:
            break;
    }

    return ins;
}

DisassemblyResult disassemble_rom(const sms::Cartridge& cart, const dragon::GameConfig& cfg) {
    DisassemblyResult result;
    if (cfg.interpreter_only || cart.rom().empty()) {
        return result;
    }

    using FuncKey = std::pair<uint8_t, uint16_t>;
    std::set<FuncKey> queued_funcs;
    std::queue<FuncKey> func_queue;

    auto add_func = [&](uint8_t bank, uint16_t addr) {
        if (addr >= 0xC000U || in_data_range(cfg, bank, addr)) {
            return;
        }
        if (addr < 0x0400U && cart.mapper_type() == sms::MapperType::Sega) {
            bank = 0U;
        }
        const FuncKey key{bank, addr};
        if (queued_funcs.insert(key).second) {
            func_queue.push(key);
        }
    };

    // Standard SMS/GG hardware entry points: Reset ($0000), IRQ ($0038), NMI ($0066)
    add_func(0U, 0x0000U);
    add_func(0U, 0x0038U);
    add_func(0U, 0x0066U);

    // User/auto-learned TOML entry points
    for (const auto& ep : cfg.entry_points) {
        const uint16_t a = static_cast<uint16_t>(ep.addr & 0xFFFFU);
        const uint8_t b = (ep.bank >= 0) ? static_cast<uint8_t>(ep.bank) : target_bank_for(cart, 0U, a);
        add_func(b, a);
    }

    // User/auto-learned jump tables
    for (const auto& jt : cfg.jump_tables) {
        const uint8_t jt_bank =
            (jt.bank >= 0) ? static_cast<uint8_t>(jt.bank) : target_bank_for(cart, 0U, static_cast<uint16_t>(jt.addr));
        for (size_t i = 0; i < jt.count; ++i) {
            const uint16_t entry_addr = static_cast<uint16_t>(jt.addr + i * 2U);
            const uint16_t tgt = read_rom_word(cart, jt_bank, entry_addr);
            add_func(target_bank_for(cart, jt_bank, tgt), tgt);
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

        const uint16_t slot_start = static_cast<uint16_t>(entry_addr & 0xC000U);
        const uint16_t slot_end = static_cast<uint16_t>(slot_start + 0x4000U);

        std::queue<uint16_t> block_q;
        block_q.push(entry_addr);

        while (!block_q.empty() && fn.instructions.size() < kMaxInstrPerFunc) {
            uint16_t pc = block_q.front();
            block_q.pop();

            while (pc < 0xC000U && pc >= slot_start && pc < slot_end &&
                   fn.instructions.size() < kMaxInstrPerFunc) {
                if (fn.instructions.count(pc) > 0U || in_data_range(cfg, bank, pc)) {
                    break;
                }

                const DecodedInstr ins = decode_instruction(cart, bank, pc);
                fn.instructions.emplace(pc, ins);

                if (ins.has_branch) {
                    if (ins.is_call) {
                        const uint8_t cb = (ins.target_addr >= slot_start && ins.target_addr < slot_end)
                                               ? bank
                                               : target_bank_for(cart, bank, ins.target_addr);
                        add_func(cb, ins.target_addr);
                    } else {
                        // Local branch inside same 16 KiB slot vs tail-call to another slot
                        if (ins.target_addr >= slot_start && ins.target_addr < slot_end) {
                            fn.local_labels.insert(ins.target_addr);
                            if (fn.instructions.count(ins.target_addr) == 0U) {
                                block_q.push(ins.target_addr);
                            }
                        } else {
                            add_func(target_bank_for(cart, bank, ins.target_addr), ins.target_addr);
                        }
                    }
                }

                if (ins.is_terminator) {
                    break;
                }
                const uint32_t next_pc = static_cast<uint32_t>(pc) + ins.length;
                if (next_pc >= 0xC000U) {
                    break;
                }
                pc = static_cast<uint16_t>(next_pc);
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

}  // namespace dragon::smsrecomp
