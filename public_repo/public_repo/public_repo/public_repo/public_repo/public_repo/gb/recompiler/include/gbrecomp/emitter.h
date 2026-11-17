#pragma once

// Game Boy / Game Boy Color C99 Code Emitter.
// Reference: docs/GB_SPEC.md §8, docs/SPEC.md §4.

#include <filesystem>
#include <string>
#include <vector>

#include "dragon/config.h"
#include "gbrecomp/disassembler.h"

namespace dragon::gbrecomp {

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

}  // namespace dragon::gbrecomp
