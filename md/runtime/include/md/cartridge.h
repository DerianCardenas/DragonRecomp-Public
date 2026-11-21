#pragma once

// Sega Genesis / Mega Drive cartridge loader (.md, .bin, .gen, .smd).
// Parses header ($0100-$01FF), auto-deinterleaves Super Magic Drive (.smd) blocks,
// detects SRAM range/type, and supports SSF2 8x512 KiB ROM banking (>4 MiB).

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dragon::md {

enum class SramMode {
    None,
    BothBytes,
    OddBytes,
    EvenBytes,
};

struct CartridgeHeader {
    std::string console_name;
    std::string domestic_title;
    std::string overseas_title;
    std::string serial;
    std::string region;
    uint16_t checksum = 0;
    uint32_t rom_start = 0;
    uint32_t rom_end = 0;
    bool has_sram = false;
    bool sram_battery = false;
    SramMode sram_mode = SramMode::None;
    uint32_t sram_start = 0;
    uint32_t sram_end = 0;
    bool pal_only = false;
};

class Cartridge {
public:
    bool load_from_file(const std::filesystem::path& path, std::string* err = nullptr);
    bool load_from_bytes(const std::vector<uint8_t>& raw, std::string* err = nullptr);
    void reset() noexcept;

    uint8_t read8(uint32_t addr) const noexcept;
    uint16_t read16(uint32_t addr) const noexcept;
    void write8(uint32_t addr, uint8_t value) noexcept;
    void write16(uint32_t addr, uint16_t value) noexcept;

    // $A130F0-$A130FF: SRAM control ($A130F1) & SSF2 banks ($A130F3..$A130FF)
    void write_time_reg(uint32_t addr, uint8_t value) noexcept;

    bool load_sram(const std::filesystem::path& path);
    bool save_sram(const std::filesystem::path& path) const;

    const std::vector<uint8_t>& rom() const noexcept { return rom_; }
    const uint8_t* rom_data() const noexcept { return rom_.data(); }
    uint32_t rom_size() const noexcept { return static_cast<uint32_t>(rom_.size()); }
    const CartridgeHeader& header() const noexcept { return header_; }
    const std::string& sha1() const noexcept { return sha1_; }
    bool is_loaded() const noexcept { return !rom_.empty(); }
    bool uses_ssf2() const noexcept { return uses_ssf2_; }

    // Save-state helpers
    const std::vector<uint8_t>& sram() const noexcept { return sram_; }
    std::vector<uint8_t>& sram_mut() noexcept { return sram_; }
    bool sram_enabled() const noexcept { return sram_enabled_; }
    void set_sram_enabled(bool en) noexcept { sram_enabled_ = en; }
    bool sram_write_protect() const noexcept { return sram_write_protect_; }
    void set_sram_write_protect(bool wp) noexcept { sram_write_protect_ = wp; }
    const std::array<uint8_t, 8>& ssf2_banks() const noexcept { return ssf2_banks_; }
    void set_ssf2_banks(const std::array<uint8_t, 8>& b) noexcept { ssf2_banks_ = b; }

private:
    void parse_header();
    bool is_sram_addr(uint32_t addr) const noexcept;
    uint32_t map_rom_addr(uint32_t addr) const noexcept;

    std::vector<uint8_t> rom_;
    std::vector<uint8_t> sram_;
    CartridgeHeader header_{};
    std::string sha1_;

    bool uses_ssf2_ = false;
    bool sram_enabled_ = true;
    bool sram_write_protect_ = false;
    std::array<uint8_t, 8> ssf2_banks_{0, 1, 2, 3, 4, 5, 6, 7};
};

}  // namespace dragon::md
