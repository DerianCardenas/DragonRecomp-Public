#include "sms/cartridge.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <span>

#include "dragon/sha1.h"

namespace dragon::sms {

namespace {

bool matches_tmr_sega(const std::vector<uint8_t>& rom, size_t offset) noexcept {
    if (offset + 16U > rom.size()) {
        return false;
    }
    return std::memcmp(rom.data() + offset, "TMR SEGA", 8) == 0;
}

std::string lower_ext(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

}  // namespace

bool Cartridge::load_from_file(const std::filesystem::path& path, std::string* err) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        if (err) *err = "No se pudo abrir la ROM SMS/GG: " + path.string();
        return false;
    }
    const auto sz = in.tellg();
    if (sz <= 0) {
        if (err) *err = "Archivo ROM vacio: " + path.string();
        return false;
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), sz)) {
        if (err) *err = "Error leyendo ROM: " + path.string();
        return false;
    }

    const SystemType hint =
        (lower_ext(path) == ".gg") ? SystemType::GameGear : SystemType::MasterSystem;
    if (!load_from_bytes(std::move(bytes), hint)) {
        if (err) *err = "ROM SMS/GG invalida: " + path.string();
        return false;
    }
    return true;
}

bool Cartridge::load_from_bytes(std::vector<uint8_t> bytes, SystemType hint,
                                MapperType mapper_override, bool use_mapper_override) {
    if (bytes.empty()) {
        return false;
    }

    // Strip optional 512-byte copier header (common on legacy dumps)
    if (bytes.size() > 512U && (bytes.size() % kBankSize) == 512U) {
        bytes.erase(bytes.begin(), bytes.begin() + 512);
    }

    // Compute SHA-1 over clean ROM image
    sha1_hex_ = dragon::sha1_hex(std::span<const uint8_t>(bytes.data(), bytes.size()));

    // Pad ROM to at least 16 KiB and a multiple of 16 KiB so bank math is always branchless
    const size_t padded_size =
        std::max(kBankSize, ((bytes.size() + kBankSize - 1U) / kBankSize) * kBankSize);
    bytes.resize(padded_size, 0xFFU);

    rom_ = std::move(bytes);
    bank_count_ = rom_.size() / kBankSize;
    system_type_ = hint;
    sram_.fill(0x00U);
    sram_dirty_ = false;

    parse_header();

    if (use_mapper_override) {
        mapper_type_ = mapper_override;
    } else {
        detect_mapper();
    }

    reset();
    return true;
}

void Cartridge::parse_header() {
    header_ = {};
    constexpr uint16_t kOffsets[3] = {0x7FF0U, 0x3FF0U, 0x1FF0U};
    for (uint16_t off : kOffsets) {
        if (matches_tmr_sega(rom_, off)) {
            header_.has_tmr_sega = true;
            header_.header_offset = off;
            header_.checksum = static_cast<uint16_t>(rom_[off + 0x0AU] |
                                                     (static_cast<uint16_t>(rom_[off + 0x0BU]) << 8U));
            header_.product_code =
                static_cast<uint32_t>(rom_[off + 0x0CU]) |
                (static_cast<uint32_t>(rom_[off + 0x0DU]) << 8U) |
                (static_cast<uint32_t>(rom_[off + 0x0EU] & 0xF0U) << 12U);
            header_.version = static_cast<uint8_t>(rom_[off + 0x0EU] & 0x0FU);
            header_.region_code = static_cast<uint8_t>((rom_[off + 0x0FU] >> 4U) & 0x0FU);
            header_.rom_size_code = static_cast<uint8_t>(rom_[off + 0x0FU] & 0x0FU);

            // Region codes 5, 6, 7 identify Game Gear cartridges
            if (header_.region_code >= 5U && header_.region_code <= 7U) {
                system_type_ = SystemType::GameGear;
            }
            break;
        }
    }
}

void Cartridge::detect_mapper() {
    mapper_type_ = MapperType::Sega;

    // Codemasters header ($7FE0-$7FE9, https://www.smspower.org/Development/CodemastersHeader):
    // $7FE0 holds the 16 KiB bank count; $7FE6-$7FE7 holds the 16-bit word-sum checksum and
    // $7FE8-$7FE9 holds its two's-complement negation (sum + inv == 0 mod 65536).
    // Export Codemasters ROMs (e.g. Micro Machines) also include "TMR SEGA" at $7FF0.
    if (rom_.size() >= 0x8000U) {
        const uint8_t cm_banks = rom_[0x7FE0U];
        const uint16_t cm_sum =
            static_cast<uint16_t>(rom_[0x7FE6U] | (static_cast<uint16_t>(rom_[0x7FE7U]) << 8U));
        const uint16_t cm_inv =
            static_cast<uint16_t>(rom_[0x7FE8U] | (static_cast<uint16_t>(rom_[0x7FE9U]) << 8U));
        if (cm_banks == bank_count_ && cm_sum != 0U && cm_sum != 0xFFFFU &&
            static_cast<uint16_t>(cm_sum + cm_inv) == 0U) {
            mapper_type_ = MapperType::Codemasters;
        }
    }
}

void Cartridge::reset() noexcept {
    state_.ram_control = 0U;
    state_.slot_banks[0] = 0U;
    state_.slot_banks[1] = static_cast<uint8_t>(bank_count_ > 1U ? 1U : 0U);
    state_.slot_banks[2] = static_cast<uint8_t>(bank_count_ > 2U ? 2U : 0U);
    state_.cm_ram_enable = 0U;
}

uint8_t Cartridge::current_bank(uint16_t addr) const noexcept {
    if (addr < 0x0400U && mapper_type_ == MapperType::Sega) {
        return 0U;
    }
    if (addr < 0x4000U) {
        return static_cast<uint8_t>(state_.slot_banks[0] % bank_count_);
    }
    if (addr < 0x8000U) {
        return static_cast<uint8_t>(state_.slot_banks[1] % bank_count_);
    }
    if (addr < 0xC000U) {
        return static_cast<uint8_t>(state_.slot_banks[2] % bank_count_);
    }
    return 0U;
}

uint8_t Cartridge::read(uint16_t addr) const noexcept {
    if (rom_.empty()) {
        return 0xFFU;
    }

    // Sega Mapper: first 1 KiB ($0000-$03FF) is always fixed to ROM offset 0
    if (addr < 0x0400U && mapper_type_ == MapperType::Sega) {
        return rom_[addr];
    }

    if (addr < 0x4000U) {
        const size_t bank = state_.slot_banks[0] % bank_count_;
        return rom_[bank * kBankSize + addr];
    }
    if (addr < 0x8000U) {
        const size_t bank = state_.slot_banks[1] % bank_count_;
        return rom_[bank * kBankSize + (addr - 0x4000U)];
    }
    if (addr < 0xC000U) {
        if (mapper_type_ == MapperType::Sega && (state_.ram_control & 0x08U) != 0U) {
            const size_t ram_bank = (state_.ram_control & 0x04U) ? 1U : 0U;
            return sram_[ram_bank * kBankSize + (addr - 0x8000U)];
        }
        if (mapper_type_ == MapperType::Codemasters && state_.cm_ram_enable != 0U &&
            addr >= 0xA000U) {
            return sram_[addr - 0xA000U];
        }
        const size_t bank = state_.slot_banks[2] % bank_count_;
        return rom_[bank * kBankSize + (addr - 0x8000U)];
    }
    return 0xFFU;
}

void Cartridge::write(uint16_t addr, uint8_t value) noexcept {
    if (mapper_type_ == MapperType::Sega) {
        if (addr >= 0x8000U && addr < 0xC000U && (state_.ram_control & 0x08U) != 0U) {
            const size_t ram_bank = (state_.ram_control & 0x04U) ? 1U : 0U;
            sram_[ram_bank * kBankSize + (addr - 0x8000U)] = value;
            sram_dirty_ = true;
        }
        return;
    }

    if (mapper_type_ == MapperType::Codemasters) {
        if (addr == 0x0000U) {
            state_.slot_banks[0] = value;
        } else if (addr == 0x4000U) {
            state_.cm_ram_enable = (value & 0x80U) ? 1U : 0U;
            state_.slot_banks[1] = static_cast<uint8_t>(value & 0x7FU);
        } else if (addr == 0x8000U) {
            state_.slot_banks[2] = value;
        } else if (state_.cm_ram_enable != 0U && addr >= 0xA000U && addr < 0xC000U) {
            sram_[addr - 0xA000U] = value;
            sram_dirty_ = true;
        }
        return;
    }

    if (mapper_type_ == MapperType::Korean) {
        if (addr == 0xA000U) {
            state_.slot_banks[2] = value;
        }
    }
}

void Cartridge::write_sega_mapper(uint16_t addr, uint8_t value) noexcept {
    if (mapper_type_ != MapperType::Sega) {
        return;
    }
    switch (addr) {
        case 0xFFFCU:
            state_.ram_control = value;
            break;
        case 0xFFFDU:
            state_.slot_banks[0] = value;
            break;
        case 0xFFFEU:
            state_.slot_banks[1] = value;
            break;
        case 0xFFFFU:
            state_.slot_banks[2] = value;
            break;
        default:
            break;
    }
}

bool Cartridge::load_sram(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    in.read(reinterpret_cast<char*>(sram_.data()), static_cast<std::streamsize>(sram_.size()));
    sram_dirty_ = false;
    return true;
}

bool Cartridge::save_sram(const std::filesystem::path& path) const {
    if (!sram_dirty_) {
        return true;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(sram_.data()),
              static_cast<std::streamsize>(sram_.size()));
    return out.good();
}

}  // namespace dragon::sms
