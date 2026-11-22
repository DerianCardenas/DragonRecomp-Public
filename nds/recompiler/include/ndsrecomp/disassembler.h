#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "dragon/config.h"
#include "nds/cartridge.h"

namespace dragon::ndsrecomp {

struct Instruction {
    uint32_t addr = 0;
    uint32_t raw = 0;
    uint8_t length = 4;  // 2 or 4 bytes
    bool thumb = false;
    bool is_thumb_bl32 = false;
    bool is_thumb_blx32 = false;
    uint32_t branch_target = 0;
    bool has_branch_target = false;
};

struct BasicBlock {
    uint32_t start_addr = 0;
    uint32_t end_addr = 0;
    bool thumb = false;
    std::vector<Instruction> instructions;
    std::vector<uint32_t> successors;
};

struct FunctionKey {
    uint32_t addr = 0;
    bool thumb = false;

    bool operator<(const FunctionKey& o) const noexcept {
        return std::tie(addr, thumb) < std::tie(o.addr, o.thumb);
    }
    bool operator==(const FunctionKey& o) const noexcept {
        return addr == o.addr && thumb == o.thumb;
    }
};

struct Function {
    uint32_t entry_addr = 0;
    bool thumb = false;
    std::string name;
    std::set<uint32_t> block_addrs;
    std::map<uint32_t, Instruction> instructions;

    std::string c_symbol() const;
};

struct ProgramCFG {
    uint32_t arm9_ram_base = 0;
    uint32_t arm9_ram_size = 0;
    std::map<FunctionKey, BasicBlock> blocks;
    std::map<FunctionKey, Function> functions;

    const Function* find_function(uint32_t addr, bool thumb) const;
};

ProgramCFG disassemble_nds(const dragon::nds::Cartridge& cart, const GameConfig& config);

}  // namespace dragon::ndsrecomp
