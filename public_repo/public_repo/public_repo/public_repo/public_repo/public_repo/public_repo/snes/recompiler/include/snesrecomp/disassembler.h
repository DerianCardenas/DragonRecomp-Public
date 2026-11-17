#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "dragon/config.h"
#include "snes/cartridge.h"
#include "snes/opcodes.h"

namespace dragon::snesrecomp {

struct CpuModeState {
    uint8_t m = 1;
    uint8_t xf = 1;
    uint8_t e = 0;

    constexpr uint8_t mode_flags() const noexcept {
        if (e != 0U) {
            return 0x07U;
        }
        return static_cast<uint8_t>(((m ? 1U : 0U) << 1U) | (xf ? 1U : 0U));
    }

    constexpr bool eff_m() const noexcept { return (e != 0U) || (m != 0U); }
    constexpr bool eff_x() const noexcept { return (e != 0U) || (xf != 0U); }

    bool operator==(const CpuModeState&) const noexcept = default;
    auto operator<=>(const CpuModeState&) const noexcept = default;
};

struct FunctionKey {
    uint8_t bank = 0;
    uint16_t addr = 0;
    CpuModeState mode{};

    bool operator==(const FunctionKey&) const noexcept = default;
    auto operator<=>(const FunctionKey&) const noexcept = default;
};

struct BlockKey {
    uint16_t addr = 0;
    CpuModeState mode{};

    bool operator==(const BlockKey&) const noexcept = default;
    auto operator<=>(const BlockKey&) const noexcept = default;
};

struct Instruction {
    uint8_t bank = 0;
    uint16_t addr = 0;
    CpuModeState state{};       // M/X/E state at the start of this instruction
    CpuModeState next_state{};  // M/X/E state after this instruction executes
    uint8_t opcode = 0;
    uint8_t op1 = 0;
    uint8_t op2 = 0;
    uint8_t op3 = 0;
    uint8_t length = 1;
    snes::Mnemonic mnemonic = snes::Mnemonic::NOP;
    snes::AddrMode mode = snes::AddrMode::Implied;
    uint8_t base_cycles = 2;
    bool page_cross_penalty = false;

    uint16_t operand_u16() const noexcept {
        if (mode == snes::AddrMode::PCRel8) {
            const auto rel = static_cast<int8_t>(op1);
            return static_cast<uint16_t>(static_cast<int32_t>(addr + 2U) + rel);
        }
        if (mode == snes::AddrMode::PCRel16) {
            const auto rel16 = static_cast<int16_t>((static_cast<uint16_t>(op2) << 8U) |
                                                    static_cast<uint16_t>(op1));
            return static_cast<uint16_t>(static_cast<int32_t>(addr + 3U) + rel16);
        }
        return static_cast<uint16_t>((static_cast<uint16_t>(op2) << 8U) |
                                     static_cast<uint16_t>(op1));
    }

    uint32_t operand_u24() const noexcept {
        return (static_cast<uint32_t>(op3) << 16U) | (static_cast<uint32_t>(op2) << 8U) |
               static_cast<uint32_t>(op1);
    }
};

struct BasicBlock {
    BlockKey key{};
    uint16_t end_addr = 0;
    std::vector<Instruction> instructions;
    std::vector<BlockKey> successors;

    std::string label_name() const;
};

struct Function {
    FunctionKey key{};
    std::string name;
    std::map<BlockKey, BasicBlock> blocks;

    std::string c_symbol() const;
};

struct JumpEngineTable {
    uint8_t bank = 0;
    uint16_t jsr_addr = 0;
    uint16_t table_addr = 0;
    std::vector<uint16_t> entries;
};

struct ProgramCFG {
    snes::MapMode map_mode = snes::MapMode::LoROM;
    uint16_t reset_addr = 0;
    uint16_t native_nmi_addr = 0;
    uint16_t native_irq_addr = 0;
    uint16_t emu_nmi_addr = 0;
    uint16_t emu_irq_addr = 0;

    std::set<uint32_t> jump_engine_targets;
    std::vector<JumpEngineTable> jump_engine_tables;

    bool is_jump_engine(uint8_t bank, uint16_t addr) const noexcept {
        return jump_engine_targets.count(addr) > 0 ||
               jump_engine_targets.count((static_cast<uint32_t>(bank) << 16U) | addr) > 0;
    }

    std::map<FunctionKey, Function> functions;
    size_t total_blocks = 0;
    size_t total_instructions = 0;
};

// Disassemble a parsed SNES Cartridge using recursive descent with static M/X/E tracking.
ProgramCFG analyze_snes_rom(const snes::Cartridge& cart, const GameConfig& config);

// Format a human-readable disassembly & CFG summary listing.
std::string format_listing(const ProgramCFG& cfg);

}  // namespace dragon::snesrecomp
