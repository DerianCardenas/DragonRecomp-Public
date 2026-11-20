#pragma once

// PC Engine / TurboGrafx-16 HuCard ROM loader, 384 KiB mirror, SF2 2.5 MiB mapper, and 2 KiB BRAM.
// Reference: docs/PCE_SPEC.md §1.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dragon::pce {

enum class MapperType : uint8_t {
    Standard = 0,
    Mirror384K = 1,
    StreetFighter2 = 2,
};

struct CartridgeState {
    uint8_t sf2_bank = 0;
    uint8_t bram_unlocked = 0;
};

class Cartridge {
public:
    static constexpr size_t kBankSize = 8192U;
    static constexpr size_t kBramSize = 2048U;

    bool load_from_file(const std::filesystem::path& path, std::string* err = nullptr);
    bool load_from_bytes(std::vector<uint8_t> data, std::string* err = nullptr);

    void reset() noexcept;

    uint8_t read_bank(uint8_t phys_bank, uint16_t offset) const noexcept;
    void write_bank(uint8_t phys_bank, uint16_t offset, uint8_t value) noexcept;

    uint8_t read_bram(uint16_t offset) const noexcept;
    void write_bram(uint16_t offset, uint8_t value) noexcept;

    bool load_bram(const std::filesystem::path& path);
    bool save_bram(const std::filesystem::path& path) const;

    const std::vector<uint8_t>& rom() const noexcept { return rom_; }
    size_t bank_count() const noexcept { return bank_count_; }
    MapperType mapper_type() const noexcept { return mapper_; }
    bool was_bit_reversed() const noexcept { return bit_reversed_; }
    bool had_copier_header() const noexcept { return copier_header_; }
    bool is_us_region() const noexcept { return us_region_; }

    CartridgeState state() const noexcept { return state_; }
    void set_state(const CartridgeState& st) noexcept { state_ = st; }
    const std::array<uint8_t, kBramSize>& bram() const noexcept { return bram_; }
    std::array<uint8_t, kBramSize>& bram_mut() noexcept { return bram_; }

private:
    void init_after_load() noexcept;

    std::vector<uint8_t> rom_;
    std::array<uint8_t, kBramSize> bram_{};
    size_t bank_count_ = 0;
    MapperType mapper_ = MapperType::Standard;
    bool bit_reversed_ = false;
    bool us_region_ = false;
    bool copier_header_ = false;
    bool bram_dirty_ = false;
    CartridgeState state_{};
};

}  // namespace dragon::pce
