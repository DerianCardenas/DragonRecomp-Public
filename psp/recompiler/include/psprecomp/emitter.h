#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "dragon/config.h"
#include "psprecomp/disassembler.h"

namespace dragon::psprecomp {

struct EmitResult {
    size_t functions_emitted = 0;
    size_t instructions_emitted = 0;
    size_t files_written = 0;
};

// Emit C99 source files (psp_recomp.h, psp_funcs_*.c, psp_dispatch.c) for a PSP CFG.
EmitResult emit_psp_c99(const PspCfg& cfg, const dragon::GameConfig& config,
                        const std::filesystem::path& out_dir, const std::string& rom_sha1,
                        const std::string& default_rom_path);

}  // namespace dragon::psprecomp
