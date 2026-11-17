#include "gba/cartridge.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <span>
#include <stdexcept>

#include "dragon/file_io.h"
#include "dragon/sha1.h"

namespace dragon::gba {

namespace {

bool contains_ascii_tag(std::span<const uint8_t> data, std::string_view tag) noexcept {
    if (data.size() < tag.size()) {
        return false;
    }
    const auto* begin = data.data();
    const auto* end = data.data() + data.size();
    const auto* it = std::search(begin, end, reinterpret_cast<const uint8_t*>(tag.data()),
                                 reinterpret_cast<const uint8_t*>(tag.data() + tag.size()));
    return it != end;
}

SaveType detect_save_type_from_rom(std::span<const uint8_t> data) noexcept {
    if (contains_ascii_tag(data, "FLASH1M_V")) {
        return SaveType::Flash128K;
    }
    if (contains_ascii_tag(data, "FLASH512_V") || contains_ascii_tag(data, "FLASH_V")) {
        return SaveType::Flash64K;
    }
    if (contains_ascii_tag(data, "EEPROM_V")) {
        return SaveType::Eeprom;
    }
    if (contains_ascii_tag(data, "SRAM_V") || contains_ascii_tag(data, "SRAM_F_V")) {
        return SaveType::Sram;
    }
    // Default to SRAM so homebrew and untagged titles can still read/write $0E000000
    return SaveType::Sram;
}

std::string sanitize_ascii(const uint8_t* ptr, size_t max_len) {
    std::string out;
    for (size_t i = 0; i < max_len; ++i) {
        const uint8_t c = ptr[i];
        if (c == 0) {
            break;
        }
        if (c >= 32 && c <= 126) {
            out.push_back(static_cast<char>(c));
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

}  // namespace

const char* save_type_to_string(SaveType type) noexcept {
    switch (type) {
        case SaveType::None:
            return "NONE";
        case SaveType::Sram:
            return "SRAM_64K";
        case SaveType::Flash64K:
            return "FLASH_64K";
        case SaveType::Flash128K:
            return "FLASH_128K";
        case SaveType::Eeprom:
            return "EEPROM";
    }
    return "SRAM_64K";
}

Cartridge parse_gba_rom_bytes(std::vector<uint8_t> data, std::string_view fallback_title) {
    if (data.size() < 4U) {
        throw std::runtime_error("GBA ROM file is too small (< 4 bytes)");
    }

    Cartridge cart;
    cart.sha1 = dragon::sha1_hex(data);
    cart.rom_size = static_cast<uint32_t>(data.size());
    cart.save_type = detect_save_type_from_rom(data);

    if (data.size() >= 0xC0U) {
        cart.title = sanitize_ascii(data.data() + 0xA0U, 12U);
        cart.game_code = sanitize_ascii(data.data() + 0xACU, 4U);
        cart.maker_code = sanitize_ascii(data.data() + 0xB0U, 2U);
        cart.header_checksum = data[0xBDU];

        uint8_t chk = 0U;
        for (size_t i = 0xA0U; i <= 0xBCU; ++i) {
            chk = static_cast<uint8_t>(chk - data[i]);
        }
        chk = static_cast<uint8_t>(chk - 0x19U);
        cart.checksum_valid = (chk == cart.header_checksum);
    }

    if (cart.title.empty()) {
        cart.title = std::string(fallback_title.empty() ? "GBA Game" : fallback_title);
    }

    cart.rom = std::move(data);
    return cart;
}

Cartridge load_gba_rom(const std::filesystem::path& path, std::string_view expected_sha1) {
    auto data = dragon::read_binary_file(path);
    Cartridge cart = parse_gba_rom_bytes(std::move(data), path.stem().string());

    if (!expected_sha1.empty()) {
        std::string exp_lower(expected_sha1);
        std::transform(exp_lower.begin(), exp_lower.end(), exp_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (cart.sha1 != exp_lower) {
            throw std::runtime_error("GBA ROM SHA-1 mismatch: expected " + exp_lower + ", got " +
                                     cart.sha1);
        }
    }

    return cart;
}

}  // namespace dragon::gba
