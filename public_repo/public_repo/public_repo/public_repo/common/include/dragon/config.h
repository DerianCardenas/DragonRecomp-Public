#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dragon {

struct EntryPoint {
    uint32_t addr = 0;
    int bank = -1;
    std::string name;
    bool thumb = false;
    bool has_thumb = false;
};

enum class JumpTableFormat {
    Word,
    WordMinusOne,
    Split,
    Long,
};

struct JumpTable {
    uint32_t addr = 0;
    uint32_t lo = 0;
    uint32_t hi = 0;
    size_t count = 0;
    JumpTableFormat format = JumpTableFormat::Word;
    int bank = -1;
};

struct DataRange {
    uint32_t start = 0;
    uint32_t end = 0;
    int bank = -1;
};

struct InlineDataCall {
    uint32_t target = 0;
    std::string kind = "jump_engine";
    size_t count = 0;
};

struct MxHint {
    uint32_t addr = 0;
    uint8_t m = 1;
    uint8_t x = 1;
};

struct GameConfig {
    std::filesystem::path config_path;
    std::string name;
    std::string platform;
    std::filesystem::path rom_path;
    std::string sha1;
    int mapper = 0;
    std::string map;
    bool allow_unofficial = false;
    std::string palette;
    std::string cartridge_type;
    bool classic_palette = false;
    // When true, emit no recompiled functions so the native build runs the
    // cycle-accurate interpreter. Needed for games whose interrupt handlers use
    // a stack trick / depend on cycle-exact interrupt timing a block recompiler
    // cannot reproduce (e.g. the SMB2(J) MMC3 hack).
    bool interpreter_only = false;

    std::filesystem::path output_dir;
    size_t split_every = 200;

    std::vector<EntryPoint> entry_points;
    std::vector<JumpTable> jump_tables;
    std::vector<DataRange> data_ranges;
    std::vector<InlineDataCall> inline_data_calls;
    std::vector<uint32_t> jump_engine;
    std::vector<uint32_t> stack_tricks;
    std::vector<MxHint> mx_hints;
};

GameConfig load_game_config(const std::filesystem::path& path);
GameConfig parse_game_config_str(std::string_view toml_text,
                                 const std::filesystem::path& base_dir = {});

}  // namespace dragon
