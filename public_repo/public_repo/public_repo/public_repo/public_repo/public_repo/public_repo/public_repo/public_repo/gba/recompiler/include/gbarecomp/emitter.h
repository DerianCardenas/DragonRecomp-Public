#pragma once

// Game Boy Advance (ARM7TDMI) C99 Code Emitter.
// Reference: docs/GBA_SPEC.md, docs/SPEC.md §4.

#include <filesystem>
#include <string>
#include <vector>

#include "dragon/config.h"
#include "gbarecomp/disassembler.h"

namespace dragon::gbarecomp {

struct EmitOptions {
    size_t split_every = 200;
};

struct GeneratedFiles {
    std::string header_content;
    std::vector<std::string> source_chunks;
    std::string dispatch_source;
};

GeneratedFiles emit_c99(const ProgramCFG& cfg, const GameConfig& config,
                        const EmitOptions& options = {});

std::vector<std::filesystem::path> write_generated_output(const ProgramCFG& cfg,
                                                          const GameConfig& config,
                                                          const std::filesystem::path& output_dir,
                                                          const EmitOptions& options = {});

}  // namespace dragon::gbarecomp
