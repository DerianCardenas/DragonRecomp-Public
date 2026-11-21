#pragma once

// Motorola 68000 -> C99 static code emitter for mdrecomp.

#include <filesystem>
#include <string>

#include "md/cartridge.h"
#include "mdrecomp/disassembler.h"

namespace dragon::mdrecomp {

struct EmitOptions {
    std::filesystem::path output_dir;
    std::filesystem::path rom_path;
    std::string game_name;
    bool interpreter_only = false;
    size_t funcs_per_file = 256U;
};

struct EmitResult {
    bool success = false;
    size_t files_written = 0;
    std::string error_message;
};

EmitResult emit_c99(const dragon::md::Cartridge& cart, const ProgramCfg& cfg,
                    const EmitOptions& opts);

}  // namespace dragon::mdrecomp
