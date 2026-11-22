#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dragon::psp {

enum class ImageFormat : uint8_t {
    Unknown = 0,
    Elf,
    Prx,
    Pbp,
    Iso,
    Cso,
};

struct NidStubImport {
    std::string module_name;
    uint32_t nid = 0;
    uint32_t stub_addr = 0;
    uint32_t syscall_code = 0;
};

struct LoadedSegment {
    uint32_t vaddr = 0;
    uint32_t memsz = 0;
    std::vector<uint8_t> data;
};

struct IsoEntry {
    std::string path;  // Uppercase normalized path, e.g. "PSP_GAME/SYSDIR/BOOT.BIN"
    uint32_t lba = 0;
    uint32_t size = 0;
    bool is_dir = false;
};

struct LoadedPspImage {
    ImageFormat format = ImageFormat::Unknown;
    std::string title;
    std::string disc_id;
    std::string sha1_hex;
    uint32_t entry_point = 0;
    uint32_t gp_value = 0;
    uint32_t base_addr = 0x08804000U;
    uint32_t image_end = 0x08804000U;
    bool is_prx = false;
    bool encrypted_eboot = false;

    std::vector<LoadedSegment> segments;
    std::vector<NidStubImport> imports;

    // Backing disc image for ISO/CSO virtual filesystem (disc0:/)
    std::filesystem::path source_path;
    std::vector<IsoEntry> iso_entries;
    bool is_cso = false;
    uint32_t cso_block_size = 2048U;
    uint8_t cso_index_shift = 0;
    uint64_t disc_total_bytes = 0;
    std::vector<uint32_t> cso_index;

    // Read bytes from an ISO/CSO disc at byte offset (for disc0:/ HLE file I/O)
    bool read_disc_bytes(uint64_t offset, uint8_t* dst, size_t len) const;
    const IsoEntry* find_iso_entry(const std::string& path) const;
};

// Load a PSP ELF, PRX, PBP, ISO, or CSO image from disk.
bool load_psp_image(const std::filesystem::path& path, LoadedPspImage& out, std::string& error);

// Load a raw ELF/PRX buffer directly from memory (used by PBP/ISO extraction and unit tests).
bool load_psp_elf_buffer(const uint8_t* data, size_t size, LoadedPspImage& out, std::string& error);

// Parse PARAM.SFO metadata (TITLE, DISC_ID) if present.
void parse_param_sfo(const uint8_t* data, size_t size, std::string& title, std::string& disc_id);

}  // namespace dragon::psp
