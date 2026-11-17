#pragma once

// ROM resolution for ROM-gated tests (spec 003, ADR-007).
//
// Tests that need a commercial ROM never commit one to the repository. They
// resolve the ROM at run time, in order:
//   1. The path recorded in the game's TOML config (`roms/<platform>/...`),
//      if that file exists on disk (preserves the pre-existing workflow).
//   2. A recursive SHA-1 search inside `DRAGON_ROM_DIR` (env var; defaults to
//      `roms/` when unset), because a real ROM library uses release-group
//      filenames that never match the config's fixed `rom_path`.
//
// If neither resolves, callers skip (CTest `SKIP_RETURN_CODE`, see
// `roms_required()` below for when a skip must instead be a failure).
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace dragon {

// Returns the resolved path to a ROM matching `expected_sha1` (lowercase hex),
// or an empty path if none was found. `declared_path` is tried first verbatim;
// `search_root_env` names the environment variable holding the search root
// for the SHA-1 fallback (callers pass "DRAGON_ROM_DIR").
std::filesystem::path resolve_rom_path(const std::filesystem::path& declared_path,
                                       std::string_view expected_sha1,
                                       const char* search_root_env = "DRAGON_ROM_DIR");

// Recursively searches `root` for a regular file whose SHA-1 digest equals
// `expected_sha1_hex` (case-insensitive). Only files whose extension is in
// `extensions` (lowercase, with leading dot, e.g. ".nes") are hashed, to avoid
// hashing unrelated large files (video dumps, Switch images, ...) in a mixed
// ROM library. An empty `extensions` list hashes every regular file.
std::filesystem::path find_rom_by_sha1(const std::filesystem::path& root,
                                       std::string_view expected_sha1_hex,
                                       std::initializer_list<std::string_view> extensions = {});

// True when DRAGON_REQUIRE_ROMS=1 (or any non-empty, non-"0" value) is set in
// the environment: on the self-hosted/local CI tier, a ROM that fails to
// resolve must fail the test instead of being skipped (spec 003, ADR-007).
bool roms_required() noexcept;

}  // namespace dragon
