#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dragon::ps1 {

enum class Ps1ImageFormat : uint8_t {
    Unknown = 0,
    PsExe,
    CueBin,
    Bin,
    Iso,
};

struct Ps1IsoEntry {
    std::string path;  // Uppercase normalized path (e.g. "SYSTEM.CNF", "SLUS_012.34")
    uint32_t lba = 0;
    uint32_t size = 0;
    bool is_dir = false;
};

struct LoadedPs1Image {
    Ps1ImageFormat format = Ps1ImageFormat::Unknown;
    std::string title;
    std::string boot_id;  // e.g. SLUS_012.34
    std::string sha1_hex;

    uint32_t initial_pc = 0x80010000U;
    uint32_t initial_gp = 0U;
    uint32_t load_addr = 0x80010000U;
    uint32_t load_size = 0U;
    uint32_t sp_base = 0x801FFFF0U;
    uint32_t sp_offset = 0U;
    std::vector<uint8_t> exe_body;  // Raw program bytes loaded at load_addr

    // Backing disc image for CD-ROM / ISO9660
    std::filesystem::path source_path;
    std::filesystem::path disc_bin_path;
    uint32_t sector_size = 2048U;      // 2048 or 2352
    uint32_t sector_data_offset = 0U;  // 0 for 2048, 24 for Mode 2/2352, 16 for Mode 1/2352
    uint32_t total_sectors = 0U;
    std::vector<Ps1IsoEntry> iso_entries;

    bool has_disc() const noexcept { return !disc_bin_path.empty() && total_sectors > 0U; }
    bool read_sector_2048(uint32_t lba, uint8_t* dst2048) const;
    bool read_disc_file(const Ps1IsoEntry& entry, uint32_t offset, uint8_t* dst, size_t len) const;
    const Ps1IsoEntry* find_iso_entry(const std::string& path) const;
};

// Load a PS-X EXE, .cue, .bin, or .iso file.
bool load_ps1_image(const std::filesystem::path& path, LoadedPs1Image& out, std::string& error);

// Load a raw PS-X EXE buffer directly from memory (used by disc extraction & unit tests).
bool load_ps1_exe_buffer(const uint8_t* data, size_t size, LoadedPs1Image& out, std::string& error);

// Fast probe: returns true if a .cue/.bin/.iso/.exe/.psexe looks like a PlayStation 1 image.
bool probe_ps1_image(const std::filesystem::path& path, std::string& out_title,
                     std::string& out_boot_id, std::string& out_sha1);

}  // namespace dragon::ps1
