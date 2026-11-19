#pragma once

// Sega Master System & Game Gear cartridge loader, header parser, and mappers.
// Reference: https://www.smspower.org/Development/Mappers, docs/SMS_SPEC.md §1 & §3.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dragon::sms {

enum class SystemType : uint8_t {
    MasterSystem,
    GameGear,
};

enum class MapperType : uint8_t {
    Sega,        // Standard Sega pager ($FFFC-$FFFF) + fixed $0000-$03FF
    Codemasters, // Paging via writes to $0000, $4000, $8000 (no fixed 1 KiB)
    Korean,      // Paging slot 2 via writes to $A000
};

struct RomHeader {
    bool has_tmr_sega = false;
    uint16_t header_offset = 0;
    uint16_t checksum = 0;
    uint32_t product_code = 0;
    uint8_t version = 0;
    uint8_t region_code = 0;
    uint8_t rom_size_code = 0;
};

struct CartridgeState {
    uint8_t ram_control = 0;
    uint8_t slot_banks[3] = {0, 1, 2};
    uint8_t cm_ram_enable = 0;
};

class Cartridge {
public:
    static constexpr size_t kBankSize = 16384U;
    static constexpr size_t kSramSize = 32768U;  // 2 x 16 KiB banks

    bool load_from_file(const std::filesystem::path& path, std::string* err = nullptr);
    bool load_from_bytes(std::vector<uint8_t> bytes, SystemType hint = SystemType::MasterSystem,
                         MapperType mapper_override = MapperType::Sega,
                         bool use_mapper_override = false);

    void reset() noexcept;

    uint8_t read(uint16_t addr) const noexcept;
    void write(uint16_t addr, uint8_t value) noexcept;

    // Called by Bus when writing to $FFFC-$FFFF (Sega Mapper registers)
    void write_sega_mapper(uint16_t addr, uint8_t value) noexcept;

    // Resolve the active 16 KiB ROM bank for a given CPU address ($0000-$BFFF)
    uint8_t current_bank(uint16_t addr) const noexcept;

    // Battery SRAM persistence
    bool load_sram(const std::filesystem::path& path);
    bool save_sram(const std::filesystem::path& path) const;
    bool sram_dirty() const noexcept { return sram_dirty_; }

    const std::vector<uint8_t>& rom() const noexcept { return rom_; }
    size_t bank_count() const noexcept { return bank_count_; }
    SystemType system_type() const noexcept { return system_type_; }
    MapperType mapper_type() const noexcept { return mapper_type_; }
    const RomHeader& header() const noexcept { return header_; }
    const std::string& sha1_hex() const noexcept { return sha1_hex_; }

    const CartridgeState& state() const noexcept { return state_; }
    void set_state(const CartridgeState& st) noexcept { state_ = st; }
    const std::array<uint8_t, kSramSize>& sram() const noexcept { return sram_; }
    std::array<uint8_t, kSramSize>& sram_mut() noexcept { return sram_; }

private:
    void parse_header();
    void detect_mapper();

    std::vector<uint8_t> rom_;
    std::array<uint8_t, kSramSize> sram_{};
    size_t bank_count_ = 1;
    SystemType system_type_ = SystemType::MasterSystem;
    MapperType mapper_type_ = MapperType::Sega;
    RomHeader header_{};
    CartridgeState state_{};
    std::string sha1_hex_;
    bool sram_dirty_ = false;
};

}  // namespace dragon::sms
