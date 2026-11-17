#include "gb/cartridge.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "dragon/file_io.h"
#include "dragon/sha1.h"

namespace dragon::gb {

Cartridge parse_gb_rom(std::span<const uint8_t> file_bytes, std::string_view expected_sha1) {
    if (file_bytes.size() < 0x0150) {
        throw std::runtime_error("ROM file too small for Game Boy header ($0100-$014F)");
    }

    Cartridge cart;
    cart.sha1 = dragon::sha1_hex(file_bytes);

    if (!expected_sha1.empty()) {
        std::string exp(expected_sha1);
        std::transform(exp.begin(), exp.end(), exp.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (cart.sha1 != exp) {
            throw std::runtime_error("ROM SHA-1 mismatch: expected " + exp + ", got " + cart.sha1);
        }
    }

    // Verify header checksum:
    // x = 0; for i = 0x0134 to 0x014C: x = x - bytes[i] - 1; assert (uint8_t)x == bytes[0x014D]
    uint8_t chk = 0;
    for (uint16_t addr = 0x0134; addr <= 0x014C; ++addr) {
        chk = (uint8_t)(chk - file_bytes[addr] - 1U);
    }
    cart.header_checksum = file_bytes[0x014D];
    // We record the checksum. Note: we do not throw on bad checksum to allow homebrew/test ROMs.

    // Extract title
    // On DMG, title is $0134-$0143 (16 bytes). On CGB, $0143 is CGB flag, and $013F-$0142 can be
    // manufacturer code.
    const uint8_t cgb_flag = file_bytes[0x0143];
    size_t title_len = (cgb_flag == 0x80 || cgb_flag == 0xC0) ? 15 : 16;
    std::string title_str;
    for (size_t i = 0; i < title_len; ++i) {
        uint8_t c = file_bytes[0x0134 + i];
        if (c == 0) {
            break;
        }
        if (c >= 0x20 && c <= 0x7E) {
            title_str.push_back(static_cast<char>(c));
        }
    }
    // Trim trailing spaces
    while (!title_str.empty() && title_str.back() == ' ') {
        title_str.pop_back();
    }
    cart.title = title_str.empty() ? "UNKNOWN_GB" : title_str;

    // CGB Mode
    if (cgb_flag == 0xC0) {
        cart.cgb_mode = CgbSupport::CgbOnly;
    } else if (cgb_flag == 0x80) {
        cart.cgb_mode = CgbSupport::CgbCompatible;
    } else {
        cart.cgb_mode = CgbSupport::DmgOnly;
    }

    // Cartridge Type / MBC ($0147)
    const uint8_t type = file_bytes[0x0147];
    switch (type) {
        case 0x00:
            cart.mbc = MbcType::RomOnly;
            break;
        case 0x01:  // MBC1
            cart.mbc = MbcType::Mbc1;
            break;
        case 0x02:  // MBC1+RAM
            cart.mbc = MbcType::Mbc1;
            cart.has_ram = true;
            break;
        case 0x03:  // MBC1+RAM+BATTERY
            cart.mbc = MbcType::Mbc1;
            cart.has_ram = true;
            cart.has_battery = true;
            break;
        case 0x05:  // MBC2
        case 0x06:  // MBC2+BATTERY
            cart.mbc = MbcType::Mbc2;
            cart.has_ram = true;
            cart.has_battery = (type == 0x06);
            break;
        case 0x0F:  // MBC3+TIMER+BATTERY
            cart.mbc = MbcType::Mbc3;
            cart.has_rtc = true;
            cart.has_battery = true;
            break;
        case 0x10:  // MBC3+TIMER+RAM+BATTERY
            cart.mbc = MbcType::Mbc3;
            cart.has_rtc = true;
            cart.has_ram = true;
            cart.has_battery = true;
            break;
        case 0x11:  // MBC3
            cart.mbc = MbcType::Mbc3;
            break;
        case 0x12:  // MBC3+RAM
            cart.mbc = MbcType::Mbc3;
            cart.has_ram = true;
            break;
        case 0x13:  // MBC3+RAM+BATTERY
            cart.mbc = MbcType::Mbc3;
            cart.has_ram = true;
            cart.has_battery = true;
            break;
        case 0x19:  // MBC5
            cart.mbc = MbcType::Mbc5;
            break;
        case 0x1A:  // MBC5+RAM
            cart.mbc = MbcType::Mbc5;
            cart.has_ram = true;
            break;
        case 0x1B:  // MBC5+RAM+BATTERY
            cart.mbc = MbcType::Mbc5;
            cart.has_ram = true;
            cart.has_battery = true;
            break;
        case 0x1C:  // MBC5+RUMBLE
            cart.mbc = MbcType::Mbc5;
            cart.has_rumble = true;
            break;
        case 0x1D:  // MBC5+RUMBLE+RAM
            cart.mbc = MbcType::Mbc5;
            cart.has_rumble = true;
            cart.has_ram = true;
            break;
        case 0x1E:  // MBC5+RUMBLE+RAM+BATTERY
            cart.mbc = MbcType::Mbc5;
            cart.has_rumble = true;
            cart.has_ram = true;
            cart.has_battery = true;
            break;
        default:
            cart.mbc = MbcType::Unsupported;
            break;
    }

    // ROM Size ($0148): 32 KiB << value
    const uint8_t rom_code = file_bytes[0x0148];
    if (rom_code <= 0x08) {
        cart.rom_size = 32768U << rom_code;
    } else {
        cart.rom_size = static_cast<uint32_t>(file_bytes.size());
    }

    // RAM Size ($0149)
    const uint8_t ram_code = file_bytes[0x0149];
    switch (ram_code) {
        case 0x00:
            cart.ram_size = (cart.mbc == MbcType::Mbc2) ? 512 : 0;
            break;
        case 0x01:  // 2 KiB (unused practically, but part of spec)
            cart.ram_size = 2048;
            cart.has_ram = true;
            break;
        case 0x02:  // 8 KiB (1 bank)
            cart.ram_size = 8192;
            cart.has_ram = true;
            break;
        case 0x03:  // 32 KiB (4 banks of 8 KiB)
            cart.ram_size = 32768;
            cart.has_ram = true;
            break;
        case 0x04:  // 128 KiB (16 banks of 8 KiB)
            cart.ram_size = 131072;
            cart.has_ram = true;
            break;
        case 0x05:  // 64 KiB (8 banks of 8 KiB)
            cart.ram_size = 65536;
            cart.has_ram = true;
            break;
        default:
            cart.ram_size = 0;
            break;
    }

    cart.rom.assign(file_bytes.begin(), file_bytes.end());
    return cart;
}

Cartridge load_gb_rom(const std::filesystem::path& path, std::string_view expected_sha1) {
    auto file_bytes = dragon::read_binary_file(path);
    return parse_gb_rom(file_bytes, expected_sha1);
}

}  // namespace dragon::gb
