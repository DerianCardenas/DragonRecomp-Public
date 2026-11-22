#pragma once

#include <filesystem>
#include <string>

#include "dragon/config.h"
#include "ndsrecomp/disassembler.h"

namespace dragon::ndsrecomp {

struct EmitResult {
    size_t files_written = 0;
    size_t functions_emitted = 0;
    size_t instructions_emitted = 0;
};

EmitResult emit_nds_c99(const ProgramCFG& cfg, const GameConfig& config,
                        const std::filesystem::path& out_dir, const std::string& rom_sha1,
                        const std::string& default_rom_path);

}  // namespace dragon::ndsrecomp
