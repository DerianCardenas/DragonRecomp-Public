#include "dragon/config.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <stdexcept>
#include <string>

#include <toml.hpp>

#include "dragon/file_io.h"

namespace dragon {

namespace {

void check_allowed_keys(const toml::table& tbl, std::string_view section,
                        std::initializer_list<std::string_view> allowed) {
    for (const auto& [k, v] : tbl) {
        (void)v;
        std::string_view key_sv = k.str();
        bool found = false;
        for (std::string_view a : allowed) {
            if (key_sv == a) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::runtime_error("Unknown key '" + std::string(key_sv) + "' in " +
                                     std::string(section));
        }
    }
}

uint32_t require_u32(const toml::table& tbl, std::string_view key, std::string_view section) {
    auto node = tbl[key];
    if (!node) {
        throw std::runtime_error("Missing required integer key '" + std::string(key) + "' in " +
                                 std::string(section));
    }
    auto val = node.value<int64_t>();
    if (!val || *val < 0 || *val > 0xFFFFFFFFLL) {
        throw std::runtime_error("Invalid integer value for key '" + std::string(key) + "' in " +
                                 std::string(section));
    }
    return static_cast<uint32_t>(*val);
}

std::string require_string(const toml::table& tbl, std::string_view key, std::string_view section) {
    auto node = tbl[key];
    if (!node) {
        throw std::runtime_error("Missing required string key '" + std::string(key) + "' in " +
                                 std::string(section));
    }
    auto val = node.value<std::string>();
    if (!val) {
        throw std::runtime_error("Expected string for key '" + std::string(key) + "' in " +
                                 std::string(section));
    }
    return *val;
}

void add_jump_engine_target(GameConfig& cfg, uint32_t target) {
    if (std::find(cfg.jump_engine.begin(), cfg.jump_engine.end(), target) ==
        cfg.jump_engine.end()) {
        cfg.jump_engine.push_back(target);
    }
}

void parse_jump_engine_array(const toml::node& node, std::string_view section, GameConfig& cfg) {
    const auto* arr = node.as_array();
    if (!arr) {
        throw std::runtime_error("'jump_engine' in " + std::string(section) + " must be an array");
    }
    for (const auto& elem : *arr) {
        auto val = elem.value<int64_t>();
        if (!val || *val < 0 || *val > 0xFFFFFFLL) {
            throw std::runtime_error("Invalid address in 'jump_engine'");
        }
        uint32_t target = static_cast<uint32_t>(*val);
        add_jump_engine_target(cfg, target);
        InlineDataCall idc;
        idc.target = target;
        idc.kind = "jump_engine";
        cfg.inline_data_calls.push_back(std::move(idc));
    }
}

std::string sanitize_toml_backslashes(std::string_view text) {
    std::string out(text);
    bool in_double_quotes = false;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '"') {
            if (i == 0 || out[i - 1] != '\\') {
                in_double_quotes = !in_double_quotes;
            }
        } else if (in_double_quotes && out[i] == '\\') {
            if (i + 1 < out.size()) {
                char next = out[i + 1];
                if (next != '"' && next != '\\' && next != 'b' && next != 'f' &&
                    next != 'n' && next != 'r' && next != 't') {
                    out[i] = '/';
                }
            }
        }
    }
    return out;
}

}  // namespace

GameConfig parse_game_config_str(std::string_view toml_text,
                                 const std::filesystem::path& base_dir) {
    toml::table root;
    try {
        root = toml::parse(toml_text);
    } catch (const toml::parse_error& err) {
        // Fallback for Windows paths containing backslashes in double-quoted strings
        // e.g. "C:\Users\..." which triggers "Error while parsing unicode scalar sequence"
        bool recovered = false;
        if (toml_text.find('\\') != std::string_view::npos) {
            std::string sanitized = sanitize_toml_backslashes(toml_text);
            try {
                root = toml::parse(sanitized);
                recovered = true;
            } catch (...) {
            }
        }
        if (!recovered) {
            throw std::runtime_error("TOML parse error: " + std::string(err.description()));
        }
    }

    check_allowed_keys(root, "root",
                       {"game", "output", "entry_points", "jump_tables", "data_ranges",
                        "inline_data_calls", "jump_engine", "stack_tricks", "mx_hints"});

    GameConfig cfg;

    // [game]
    const auto* game_tbl = root["game"].as_table();
    if (!game_tbl) {
        throw std::runtime_error("Missing required [game] table in config");
    }
    check_allowed_keys(
        *game_tbl, "[game]",
        {"name", "platform", "rom", "sha1", "mapper", "map", "allow_unofficial", "jump_engine",
         "palette", "interpreter_only", "cartridge_type", "mbc", "classic_palette", "save_type"});

    cfg.name = require_string(*game_tbl, "name", "[game]");
    cfg.platform = require_string(*game_tbl, "platform", "[game]");
    if (cfg.platform != "nes" && cfg.platform != "snes" && cfg.platform != "gb" &&
        cfg.platform != "gbc" && cfg.platform != "gba" && cfg.platform != "sms" &&
        cfg.platform != "gg") {
        throw std::runtime_error("Unsupported platform '" + cfg.platform +
                                 "' in [game] (expected 'nes', 'snes', 'gb', 'gbc', 'gba', 'sms' "
                                 "or 'gg')");
    }

    std::filesystem::path rel_rom = require_string(*game_tbl, "rom", "[game]");
    cfg.rom_path = (base_dir.empty() || rel_rom.is_absolute()) ? rel_rom : (base_dir / rel_rom);

    cfg.sha1 = require_string(*game_tbl, "sha1", "[game]");
    std::transform(cfg.sha1.begin(), cfg.sha1.end(), cfg.sha1.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (auto mapper_node = (*game_tbl)["mapper"]) {
        auto val = mapper_node.value<int64_t>();
        if (!val || *val < 0 || *val > 4095) {
            throw std::runtime_error("Invalid 'mapper' in [game]");
        }
        cfg.mapper = static_cast<int>(*val);
    }
    if (auto map_node = (*game_tbl)["map"]) {
        auto val = map_node.value<std::string>();
        if (!val) {
            throw std::runtime_error("Invalid 'map' in [game]");
        }
        cfg.map = *val;
    }
    if (auto unoff_node = (*game_tbl)["allow_unofficial"]) {
        auto val = unoff_node.value<bool>();
        if (!val) {
            throw std::runtime_error("Invalid 'allow_unofficial' in [game]");
        }
        cfg.allow_unofficial = *val;
    }
    if (auto je_node = (*game_tbl)["jump_engine"]) {
        parse_jump_engine_array(*je_node.node(), "[game]", cfg);
    }
    if (auto pal_node = (*game_tbl)["palette"]) {
        auto val = pal_node.value<std::string>();
        if (val) {
            cfg.palette = *val;
        }
    }
    if (auto interp_node = (*game_tbl)["interpreter_only"]) {
        auto val = interp_node.value<bool>();
        if (!val) {
            throw std::runtime_error("Invalid 'interpreter_only' in [game]");
        }
        cfg.interpreter_only = *val;
    }
    if (auto cart_node = (*game_tbl)["cartridge_type"]) {
        auto val = cart_node.value<std::string>();
        if (val) {
            cfg.cartridge_type = *val;
        }
    } else if (auto mbc_node = (*game_tbl)["mbc"]) {
        auto val = mbc_node.value<std::string>();
        if (val) {
            cfg.cartridge_type = *val;
        }
    } else if (auto save_node = (*game_tbl)["save_type"]) {
        auto val = save_node.value<std::string>();
        if (val) {
            cfg.cartridge_type = *val;
        }
    }
    if (auto pal_mode_node = (*game_tbl)["classic_palette"]) {
        auto val = pal_mode_node.value<bool>();
        if (val) {
            cfg.classic_palette = *val;
        }
    }

    // [output]
    std::filesystem::path rel_out = "generated";
    if (const auto* out_tbl = root["output"].as_table()) {
        check_allowed_keys(*out_tbl, "[output]", {"dir", "split_every"});
        if (auto dir_node = (*out_tbl)["dir"]) {
            auto val = dir_node.value<std::string>();
            if (!val) {
                throw std::runtime_error("Invalid 'dir' in [output]");
            }
            rel_out = *val;
        }
        if (auto split_node = (*out_tbl)["split_every"]) {
            auto val = split_node.value<int64_t>();
            if (!val || *val <= 0) {
                throw std::runtime_error("Invalid 'split_every' in [output]");
            }
            cfg.split_every = static_cast<size_t>(*val);
        }
    }
    cfg.output_dir = (base_dir.empty() || rel_out.is_absolute()) ? rel_out : (base_dir / rel_out);

    // [[entry_points]]
    if (auto ep_node = root["entry_points"]) {
        const auto* arr = ep_node.as_array();
        if (!arr) {
            throw std::runtime_error("'entry_points' must be an array of tables");
        }
        for (const auto& item : *arr) {
            const auto* tbl = item.as_table();
            if (!tbl) {
                throw std::runtime_error("Each element of 'entry_points' must be a table");
            }
            check_allowed_keys(*tbl, "[[entry_points]]", {"addr", "bank", "name", "thumb", "mode"});
            EntryPoint ep;
            ep.addr = require_u32(*tbl, "addr", "[[entry_points]]");
            if (auto b = (*tbl)["bank"]) {
                auto val = b.value<int64_t>();
                if (!val || *val < 0) {
                    throw std::runtime_error("Invalid 'bank' in [[entry_points]]");
                }
                ep.bank = static_cast<int>(*val);
            }
            if (auto t = (*tbl)["thumb"]) {
                auto val = t.value<bool>();
                if (!val) {
                    throw std::runtime_error("Invalid 'thumb' in [[entry_points]]");
                }
                ep.thumb = *val;
                ep.has_thumb = true;
            } else if (auto m = (*tbl)["mode"]) {
                if (auto s = m.value<std::string>()) {
                    ep.thumb = (*s == "thumb" || *s == "THUMB");
                    ep.has_thumb = true;
                } else if (auto iv = m.value<int64_t>()) {
                    ep.thumb = (*iv != 0);
                    ep.has_thumb = true;
                }
            }
            if (auto n = (*tbl)["name"]) {
                auto val = n.value<std::string>();
                if (!val) {
                    throw std::runtime_error("Invalid 'name' in [[entry_points]]");
                }
                ep.name = *val;
            }
            cfg.entry_points.push_back(std::move(ep));
        }
    }

    // [[jump_tables]]
    if (auto jt_node = root["jump_tables"]) {
        const auto* arr = jt_node.as_array();
        if (!arr) {
            throw std::runtime_error("'jump_tables' must be an array of tables");
        }
        for (const auto& item : *arr) {
            const auto* tbl = item.as_table();
            if (!tbl) {
                throw std::runtime_error("Each element of 'jump_tables' must be a table");
            }
            check_allowed_keys(*tbl, "[[jump_tables]]",
                               {"addr", "lo", "hi", "count", "format", "bank"});
            JumpTable jt;
            jt.count = static_cast<size_t>(require_u32(*tbl, "count", "[[jump_tables]]"));
            std::string fmt = "word";
            if (auto f = (*tbl)["format"]) {
                auto val = f.value<std::string>();
                if (!val) {
                    throw std::runtime_error("Invalid 'format' in [[jump_tables]]");
                }
                fmt = *val;
            }
            if (fmt == "word") {
                jt.format = JumpTableFormat::Word;
                jt.addr = require_u32(*tbl, "addr", "[[jump_tables]]");
            } else if (fmt == "word_minus_one") {
                jt.format = JumpTableFormat::WordMinusOne;
                jt.addr = require_u32(*tbl, "addr", "[[jump_tables]]");
            } else if (fmt == "split") {
                jt.format = JumpTableFormat::Split;
                jt.lo = require_u32(*tbl, "lo", "[[jump_tables]]");
                jt.hi = require_u32(*tbl, "hi", "[[jump_tables]]");
                if (auto a = (*tbl)["addr"]) {
                    jt.addr = require_u32(*tbl, "addr", "[[jump_tables]]");
                }
            } else if (fmt == "long") {
                jt.format = JumpTableFormat::Long;
                jt.addr = require_u32(*tbl, "addr", "[[jump_tables]]");
            } else {
                throw std::runtime_error("Unknown jump table format '" + fmt + "'");
            }
            if (auto b = (*tbl)["bank"]) {
                auto val = b.value<int64_t>();
                if (!val || *val < 0) {
                    throw std::runtime_error("Invalid 'bank' in [[jump_tables]]");
                }
                jt.bank = static_cast<int>(*val);
            }
            cfg.jump_tables.push_back(jt);
        }
    }

    // [[data_ranges]]
    if (auto dr_node = root["data_ranges"]) {
        const auto* arr = dr_node.as_array();
        if (!arr) {
            throw std::runtime_error("'data_ranges' must be an array of tables");
        }
        for (const auto& item : *arr) {
            const auto* tbl = item.as_table();
            if (!tbl) {
                throw std::runtime_error("Each element of 'data_ranges' must be a table");
            }
            check_allowed_keys(*tbl, "[[data_ranges]]", {"start", "end", "bank"});
            DataRange dr;
            dr.start = require_u32(*tbl, "start", "[[data_ranges]]");
            dr.end = require_u32(*tbl, "end", "[[data_ranges]]");
            if (dr.end < dr.start) {
                throw std::runtime_error("'end' < 'start' in [[data_ranges]]");
            }
            if (auto b = (*tbl)["bank"]) {
                auto val = b.value<int64_t>();
                if (!val || *val < 0) {
                    throw std::runtime_error("Invalid 'bank' in [[data_ranges]]");
                }
                dr.bank = static_cast<int>(*val);
            }
            cfg.data_ranges.push_back(dr);
        }
    }

    // [[inline_data_calls]]
    if (auto idc_node = root["inline_data_calls"]) {
        const auto* arr = idc_node.as_array();
        if (!arr) {
            throw std::runtime_error("'inline_data_calls' must be an array of tables");
        }
        for (const auto& item : *arr) {
            const auto* tbl = item.as_table();
            if (!tbl) {
                throw std::runtime_error("Each element of 'inline_data_calls' must be a table");
            }
            check_allowed_keys(*tbl, "[[inline_data_calls]]", {"target", "kind", "count"});
            InlineDataCall idc;
            idc.target = require_u32(*tbl, "target", "[[inline_data_calls]]");
            if (auto k = (*tbl)["kind"]) {
                auto val = k.value<std::string>();
                if (!val) {
                    throw std::runtime_error("Invalid 'kind' in [[inline_data_calls]]");
                }
                idc.kind = *val;
            }
            if (auto c = (*tbl)["count"]) {
                idc.count =
                    static_cast<size_t>(require_u32(*tbl, "count", "[[inline_data_calls]]"));
            }
            if (idc.kind == "jump_engine") {
                add_jump_engine_target(cfg, idc.target);
            }
            cfg.inline_data_calls.push_back(std::move(idc));
        }
    }

    // Top-level jump_engine = [...]
    if (auto je_node = root["jump_engine"]) {
        parse_jump_engine_array(*je_node.node(), "root", cfg);
    }

    // stack_tricks = [...]
    if (auto st_node = root["stack_tricks"]) {
        const auto* arr = st_node.as_array();
        if (!arr) {
            throw std::runtime_error("'stack_tricks' must be an array");
        }
        for (const auto& elem : *arr) {
            auto val = elem.value<int64_t>();
            if (!val || *val < 0 || *val > 0xFFFFFFLL) {
                throw std::runtime_error("Invalid address in 'stack_tricks'");
            }
            cfg.stack_tricks.push_back(static_cast<uint32_t>(*val));
        }
    }

    // [[mx_hints]]
    if (auto mx_node = root["mx_hints"]) {
        const auto* arr = mx_node.as_array();
        if (!arr) {
            throw std::runtime_error("'mx_hints' must be an array of tables");
        }
        for (const auto& item : *arr) {
            const auto* tbl = item.as_table();
            if (!tbl) {
                throw std::runtime_error("Each element of 'mx_hints' must be a table");
            }
            check_allowed_keys(*tbl, "[[mx_hints]]", {"addr", "m", "x"});
            MxHint hint;
            hint.addr = require_u32(*tbl, "addr", "[[mx_hints]]");
            hint.m = static_cast<uint8_t>(require_u32(*tbl, "m", "[[mx_hints]]"));
            hint.x = static_cast<uint8_t>(require_u32(*tbl, "x", "[[mx_hints]]"));
            cfg.mx_hints.push_back(hint);
        }
    }

    return cfg;
}

GameConfig load_game_config(const std::filesystem::path& path) {
    const std::string text = read_text_file(path);
    GameConfig cfg = parse_game_config_str(text, path.parent_path());
    cfg.config_path = path;
    return cfg;
}

}  // namespace dragon
