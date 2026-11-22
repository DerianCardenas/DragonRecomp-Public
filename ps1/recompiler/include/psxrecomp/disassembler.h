#pragma once

#include <cstdint>
#include <map>
#include <set>

#include "dragon/config.h"
#include "dragon/mips.h"
#include "ps1/loader.h"

namespace dragon::psxrecomp {

struct Ps1Function {
    uint32_t entry_addr = 0;
    uint32_t end_addr = 0;
    std::map<uint32_t, dragon::mips::DecodedMips> instructions;
    std::set<uint32_t> local_labels;
    std::set<uint32_t> direct_calls;
};

struct Ps1Cfg {
    std::map<uint32_t, Ps1Function> functions;
    size_t total_instructions = 0;
};

// Recursively disassemble a PS-X EXE into a function-level R3000A CFG.
Ps1Cfg disassemble_ps1(const dragon::ps1::LoadedPs1Image& img, const dragon::GameConfig& config);

}  // namespace dragon::psxrecomp
