#pragma once

// HuC6280 -> C99 static code emitter for pcerecomp.
// Reference: docs/PCE_SPEC.md §5.

#include <cstddef>
#include <filesystem>
#include <string>

#include "pcerecomp/disassembler.h"

namespace dragon::pcerecomp {

struct EmitOptions {
    std::filesystem::path output_dir;
    bool single_file = false;
    size_t split_every = 200;
};

struct EmitResult {
    bool success = false;
    std::string error;
    size_t functions_emitted = 0;
    size_t files_written = 0;
};

EmitResult emit_c99(const DisassemblyResult& disasm, const EmitOptions& opts);

}  // namespace dragon::pcerecomp
