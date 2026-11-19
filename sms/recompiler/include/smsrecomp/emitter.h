#pragma once

// Pure C99 static recompiler code emitter for Sega Master System & Game Gear.
// Reference: docs/SMS_SPEC.md §6.

#include <filesystem>
#include <string>

#include "smsrecomp/disassembler.h"

namespace dragon::smsrecomp {

struct EmitOptions {
    std::filesystem::path output_dir;
    bool single_file = true;
    size_t split_every = 200;
};

struct EmitResult {
    bool success = false;
    size_t files_written = 0;
    size_t functions_emitted = 0;
    std::string error;
};

EmitResult emit_c99(const DisassemblyResult& disasm, const EmitOptions& opts);

}  // namespace dragon::smsrecomp
