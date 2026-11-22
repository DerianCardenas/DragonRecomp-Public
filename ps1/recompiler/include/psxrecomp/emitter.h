#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "dragon/config.h"
#include "psxrecomp/disassembler.h"

namespace dragon::psxrecomp {

struct EmitResult {
    size_t functions_emitted = 0;
    size_t instructions_emitted = 0;
    size_t files_written = 0;
};

// Emit C99 source files (ps1_recomp.h, ps1_funcs_*.c, ps1_dispatch.c) for a PS1 CFG.
EmitResult emit_ps1_c99(const Ps1Cfg& cfg, const dragon::GameConfig& config,
                        const std::filesystem::path& out_dir, const std::string& rom_sha1,
                        const std::string& default_rom_path);

}  // namespace dragon::psxrecomp
