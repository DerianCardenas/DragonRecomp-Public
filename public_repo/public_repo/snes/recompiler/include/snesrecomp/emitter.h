#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "dragon/config.h"
#include "snesrecomp/disassembler.h"

namespace dragon::snesrecomp {

struct EmitOptions {
    bool enable_flag_opt = true;
    size_t split_every = 200;
};

struct GeneratedFiles {
    std::string header_content;
    std::vector<std::string> source_chunks;
    std::string dispatch_source;
};

// Emit C99 header, function implementation chunks, and dispatch table source in memory.
GeneratedFiles emit_c99(const ProgramCFG& cfg, const GameConfig& config,
                        const EmitOptions& options = {});

// Write all generated C99 files and disassembly listing to `output_dir`.
std::vector<std::filesystem::path> write_generated_output(const ProgramCFG& cfg,
                                                          const GameConfig& config,
                                                          const std::filesystem::path& output_dir,
                                                          const EmitOptions& options = {});

}  // namespace dragon::snesrecomp
