#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "dragon/config.h"
#include "dragon/mips.h"
#include "psp/loader.h"

namespace dragon::psprecomp {

struct PspFunction {
    uint32_t entry_addr = 0;
    uint32_t end_addr = 0;
    std::map<uint32_t, dragon::mips::DecodedMips> instructions;
    std::set<uint32_t> local_labels;
    std::set<uint32_t> direct_calls;
};

struct PspCfg {
    std::map<uint32_t, PspFunction> functions;
    std::set<uint32_t> stub_addresses;
    size_t total_instructions = 0;
};

// Recursively disassemble a loaded PSP ELF/PRX image into a function-level CFG.
PspCfg disassemble_psp(const dragon::psp::LoadedPspImage& img, const dragon::GameConfig& config);

}  // namespace dragon::psprecomp
