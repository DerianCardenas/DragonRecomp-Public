#include "studio/rom_detector.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

#include "dragon/file_io.h"
#include "dragon/sha1.h"

namespace dragon::studio {

namespace {

std::string clean_title_from_path(const std::filesystem::path& path) {
    std::string stem = path.stem().string();
    std::string out;
    for (char c : stem) {
        if (c == '_' || c == '-') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string sanitize_internal_title(std::string_view raw) {
    std::string out;
    for (char c : raw) {
        if (c >= 32 && c <= 126) {
            out.push_back(c);
        }
    }
    // Trim trailing whitespace
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

}  // namespace

RomInfo detect_rom(const std::filesystem::path& path) {
    RomInfo info;
    info.file_path = std::filesystem::absolute(path);

    if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) {
        return info;
    }

    const auto data = dragon::read_binary_file(path);
    if (data.empty()) {
        return info;
    }

    info.file_size = data.size();

    // 1. Check iNES (NES) header
    if (data.size() >= 16 && data[0] == 'N' && data[1] == 'E' && data[2] == 'S' &&
        static_cast<uint8_t>(data[3]) == 0x1A) {
        info.platform = RomPlatform::NES;
        info.prg_size_kb = static_cast<size_t>(data[4]) * 16U;
        info.chr_size_kb = static_cast<size_t>(data[5]) * 8U;
        info.battery = (data[6] & 0x02) != 0;
        info.mapper = (data[7] & 0xF0) | (data[6] >> 4);
        info.sha1 = dragon::sha1_hex(data);
        info.clean_name = clean_title_from_path(path);
        info.internal_title = info.clean_name;
        return info;
    }

    // 1.5. Check Sega Master System / Game Gear ("TMR SEGA" header or .sms/.gg extension)
    {
        const std::string ext_chk = path.extension().string();
        const bool is_sms_ext = (ext_chk == ".sms" || ext_chk == ".SMS");
        const bool is_gg_ext = (ext_chk == ".gg" || ext_chk == ".GG");
        const size_t sms_off = (data.size() > 512U && (data.size() % 16384U) == 512U) ? 512U : 0U;
        const uint8_t* sms_rom = data.data() + sms_off;
        const size_t sms_sz = data.size() - sms_off;

        bool has_tmr_sega = false;
        uint8_t region_code = 0;
        uint16_t sms_csum = 0;
        constexpr size_t kTmrOffsets[3] = {0x7FF0U, 0x3FF0U, 0x1FF0U};
        for (size_t off : kTmrOffsets) {
            if (off + 16U <= sms_sz && std::memcmp(sms_rom + off, "TMR SEGA", 8) == 0) {
                has_tmr_sega = true;
                sms_csum = static_cast<uint16_t>(
                    sms_rom[off + 0x0AU] | (static_cast<uint16_t>(sms_rom[off + 0x0BU]) << 8U));
                region_code = static_cast<uint8_t>((sms_rom[off + 0x0FU] >> 4U) & 0x0FU);
                break;
            }
        }

        if (has_tmr_sega || is_sms_ext || is_gg_ext) {
            const bool is_gg =
                is_gg_ext || (has_tmr_sega && region_code >= 5U && region_code <= 7U);
            info.platform = is_gg ? RomPlatform::GameGear : RomPlatform::MasterSystem;
            info.rom_size_kb = sms_sz / 1024U;
            info.checksum = sms_csum;
            info.smc_header = (sms_off != 0U);
            info.sha1 = dragon::sha1_hex(std::span<const uint8_t>(sms_rom, sms_sz));
            info.clean_name = clean_title_from_path(path);
            info.internal_title = info.clean_name;
            return info;
        }
    }

    // 1.6. Check PC Engine / TurboGrafx-16 (.pce / .sgx HuCard)
    {
        const std::string ext_chk = path.extension().string();
        if (ext_chk == ".pce" || ext_chk == ".PCE" || ext_chk == ".sgx" || ext_chk == ".SGX") {
            const size_t pce_off =
                (data.size() >= 8192U + 512U && (data.size() % 8192U) == 512U) ? 512U : 0U;
            const uint8_t* pce_rom = data.data() + pce_off;
            const size_t pce_sz = data.size() - pce_off;
            info.platform = RomPlatform::PCEngine;
            info.rom_size_kb = pce_sz / 1024U;
            info.smc_header = (pce_off != 0U);
            info.sha1 = dragon::sha1_hex(std::span<const uint8_t>(pce_rom, pce_sz));
            info.clean_name = clean_title_from_path(path);
            info.internal_title = info.clean_name;
            return info;
        }
    }

    // 1.7. Check Sega Genesis / Mega Drive (.md, .bin, .gen, .smd)
    {
        const std::string ext_chk = path.extension().string();
        const bool is_md_ext =
            (ext_chk == ".md" || ext_chk == ".MD" || ext_chk == ".gen" || ext_chk == ".GEN" ||
             ext_chk == ".smd" || ext_chk == ".SMD" || ext_chk == ".bin" || ext_chk == ".BIN");
        const bool has_sega_hdr =
            (data.size() >= 0x200U && std::memcmp(data.data() + 0x100, "SEGA", 4) == 0);
        const bool is_smd_hdr =
            (data.size() >= 512U + 16384U && ((data.size() - 512U) % 16384U) == 0U &&
             data[8] == 0xAAU && data[9] == 0xBBU);
        if (has_sega_hdr || is_smd_hdr ||
            (is_md_ext && ext_chk != ".bin" && ext_chk != ".BIN" && data.size() >= 0x200U)) {
            info.platform = RomPlatform::Genesis;
            info.rom_size_kb = (is_smd_hdr ? (data.size() - 512U) : data.size()) / 1024U;
            info.smc_header = is_smd_hdr;
            if (has_sega_hdr) {
                info.checksum =
                    static_cast<uint16_t>((static_cast<uint16_t>(data[0x18E]) << 8U) | data[0x18F]);
                std::string_view raw_title(reinterpret_cast<const char*>(data.data() + 0x150), 48);
                info.internal_title = sanitize_internal_title(raw_title);
                if (info.internal_title.empty()) {
                    std::string_view dom_title(reinterpret_cast<const char*>(data.data() + 0x120),
                                               48);
                    info.internal_title = sanitize_internal_title(dom_title);
                }
            }
            if (info.internal_title.empty()) {
                info.internal_title = clean_title_from_path(path);
            }
            info.clean_name = clean_title_from_path(path);
            info.sha1 = dragon::sha1_hex(std::span<const uint8_t>(data.data(), data.size()));
            return info;
        }
    }

    // 2. Check SNES header
    // Check if 512-byte SMC header is present
    size_t smc_offset = 0;
    if (data.size() % 1024 == 512) {
        smc_offset = 512;
        info.smc_header = true;
    }

    size_t rom_size = data.size() - smc_offset;
    const uint8_t* rom = data.data() + smc_offset;

    auto check_snes_header = [&](size_t offset, const char* mode_name) -> bool {
        if (rom_size < offset + 0x30) {
            return false;
        }
        const uint16_t csum = static_cast<uint16_t>(rom[offset + 0x1E]) |
                              (static_cast<uint16_t>(rom[offset + 0x1F]) << 8);
        const uint16_t comp = static_cast<uint16_t>(rom[offset + 0x1C]) |
                              (static_cast<uint16_t>(rom[offset + 0x1D]) << 8);
        if (static_cast<uint16_t>(csum ^ comp) == 0xFFFFU && csum != 0) {
            info.platform = RomPlatform::SNES;
            info.map_mode_str = mode_name;
            info.checksum = csum;
            const uint8_t map_byte = rom[offset + 0x15];
            info.fast_rom = (map_byte & 0x10) != 0;

            std::string_view raw_title(reinterpret_cast<const char*>(rom + offset), 21);
            info.internal_title = sanitize_internal_title(raw_title);
            if (info.internal_title.empty()) {
                info.internal_title = clean_title_from_path(path);
            }
            info.clean_name = info.internal_title;
            return true;
        }
        return false;
    };

    if (check_snes_header(0x7FC0, "lorom") || check_snes_header(0xFFC0, "hirom") ||
        check_snes_header(0x40FFC0, "exhirom")) {
        info.sha1 = dragon::sha1_hex(std::span<const uint8_t>(rom, rom_size));
        return info;
    }

    if (smc_offset == 0 && (data.size() % 0x8000) == 1024 && data.size() >= 0x8400) {
        smc_offset = 1024;
        rom_size = data.size() - smc_offset;
        rom = data.data() + smc_offset;
        if (check_snes_header(0x7FC0, "lorom") || check_snes_header(0xFFC0, "hirom") ||
            check_snes_header(0x40FFC0, "exhirom")) {
            info.smc_header = true;
            info.sha1 = dragon::sha1_hex(std::span<const uint8_t>(rom, rom_size));
            return info;
        }
        smc_offset = 0;
        rom_size = data.size();
        rom = data.data();
    }

    // Fallback detection by file extension (.sfc / .smc)
    const std::string ext = path.extension().string();
    if (ext == ".sfc" || ext == ".smc" || ext == ".SFC" || ext == ".SMC") {
        info.platform = RomPlatform::SNES;
        info.map_mode_str = "lorom";
        info.sha1 = dragon::sha1_hex(std::span<const uint8_t>(rom, rom_size));
        info.clean_name = clean_title_from_path(path);
        info.internal_title = info.clean_name;
        return info;
    }

    // 3. Check Game Boy Advance header ($0000-$00BF)
    if (data.size() >= 0xC0) {
        const bool is_gba_ext = (ext == ".gba" || ext == ".GBA" || ext == ".agb" || ext == ".AGB");
        const bool fixed_96 = (data[0xB2] == 0x96U);
        const bool arm_branch = (data[0x03] == 0xEAU);
        uint8_t gba_chk = 0;
        for (size_t i = 0xA0; i <= 0xBC; ++i) {
            gba_chk = static_cast<uint8_t>(gba_chk - data[i]);
        }
        gba_chk = static_cast<uint8_t>(gba_chk - 0x19U);
        const bool gba_csum_match = (gba_chk == data[0xBD]);

        if (is_gba_ext || (fixed_96 && arm_branch && gba_csum_match)) {
            info.platform = RomPlatform::GameBoyAdvance;
            std::string_view raw_title(reinterpret_cast<const char*>(data.data() + 0xA0), 12);
            info.internal_title = sanitize_internal_title(raw_title);
            if (info.internal_title.empty()) {
                info.internal_title = clean_title_from_path(path);
            }
            info.clean_name = info.internal_title;

            std::string_view raw_code(reinterpret_cast<const char*>(data.data() + 0xAC), 4);
            info.game_code = sanitize_internal_title(raw_code);
            info.rom_size_kb = data.size() / 1024U;

            std::string_view rom_sv(reinterpret_cast<const char*>(data.data()), data.size());
            if (rom_sv.find("FLASH1M_V") != std::string_view::npos) {
                info.save_type_str = "FLASH 128K";
                info.battery = true;
            } else if (rom_sv.find("FLASH512_V") != std::string_view::npos ||
                       rom_sv.find("FLASH_V") != std::string_view::npos) {
                info.save_type_str = "FLASH 64K";
                info.battery = true;
            } else if (rom_sv.find("EEPROM_V") != std::string_view::npos) {
                info.save_type_str = "EEPROM";
                info.battery = true;
            } else if (rom_sv.find("SRAM_V") != std::string_view::npos ||
                       rom_sv.find("SRAM_F_V") != std::string_view::npos) {
                info.save_type_str = "SRAM 64K";
                info.battery = true;
            } else {
                info.save_type_str = "SRAM / Auto";
            }

            info.sha1 = dragon::sha1_hex(data);
            return info;
        }
    }

    // 4. Check Game Boy / Game Boy Color header ($0100-$014F)
    if (data.size() >= 0x0150) {
        // Nintendo Logo at 0x0104 - 0x0133
        static constexpr uint8_t kGbLogo[48] = {
            0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
            0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
            0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
            0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E};
        const bool logo_match = (std::memcmp(data.data() + 0x0104, kGbLogo, sizeof(kGbLogo)) == 0);
        uint8_t csum = 0;
        for (size_t i = 0x0134; i <= 0x014C; ++i) {
            csum = static_cast<uint8_t>(csum - data[i] - 1U);
        }
        const bool csum_match = (csum == data[0x014D]);
        const bool is_gb_ext = (ext == ".gb" || ext == ".GB" || ext == ".gbc" || ext == ".GBC");

        if (logo_match || csum_match || is_gb_ext) {
            const uint8_t cgb_flag = data[0x0143];
            const bool is_cgb =
                (cgb_flag == 0x80 || cgb_flag == 0xC0 || ext == ".gbc" || ext == ".GBC");
            info.platform = is_cgb ? RomPlatform::GameBoyColor : RomPlatform::GameBoy;
            info.cgb_flag = is_cgb;

            // Title: 0x0134 to 0x0143
            const size_t title_len = (cgb_flag & 0x80) ? 15 : 16;
            std::string_view raw_title(reinterpret_cast<const char*>(data.data() + 0x0134),
                                       title_len);
            info.internal_title = sanitize_internal_title(raw_title);
            if (info.internal_title.empty()) {
                info.internal_title = clean_title_from_path(path);
            }
            info.clean_name = info.internal_title;

            // ROM Size: 32 KiB << byte
            const uint8_t rom_size_byte = data[0x0148];
            info.rom_size_kb = (rom_size_byte <= 8) ? (32U << rom_size_byte) : (data.size() / 1024);

            // RAM Size
            const uint8_t ram_size_byte = data[0x0149];
            switch (ram_size_byte) {
                case 0:
                    info.ram_size_kb = 0;
                    break;
                case 1:
                    info.ram_size_kb = 2;
                    break;
                case 2:
                    info.ram_size_kb = 8;
                    break;
                case 3:
                    info.ram_size_kb = 32;
                    break;
                case 4:
                    info.ram_size_kb = 128;
                    break;
                case 5:
                    info.ram_size_kb = 64;
                    break;
                default:
                    info.ram_size_kb = 0;
                    break;
            }

            // Cartridge / MBC type
            const uint8_t cart_type = data[0x0147];
            switch (cart_type) {
                case 0x00:
                    info.mbc_type_str = "ROM ONLY";
                    break;
                case 0x01:
                    info.mbc_type_str = "MBC1";
                    break;
                case 0x02:
                    info.mbc_type_str = "MBC1+RAM";
                    break;
                case 0x03:
                    info.mbc_type_str = "MBC1+RAM+BATTERY";
                    info.battery = true;
                    break;
                case 0x05:
                    info.mbc_type_str = "MBC2";
                    break;
                case 0x06:
                    info.mbc_type_str = "MBC2+BATTERY";
                    info.battery = true;
                    break;
                case 0x08:
                    info.mbc_type_str = "ROM+RAM";
                    break;
                case 0x09:
                    info.mbc_type_str = "ROM+RAM+BATTERY";
                    info.battery = true;
                    break;
                case 0x0F:
                    info.mbc_type_str = "MBC3+TIMER+BATTERY";
                    info.battery = true;
                    break;
                case 0x10:
                    info.mbc_type_str = "MBC3+TIMER+RAM+BATTERY";
                    info.battery = true;
                    break;
                case 0x11:
                    info.mbc_type_str = "MBC3";
                    break;
                case 0x12:
                    info.mbc_type_str = "MBC3+RAM";
                    break;
                case 0x13:
                    info.mbc_type_str = "MBC3+RAM+BATTERY";
                    info.battery = true;
                    break;
                case 0x19:
                    info.mbc_type_str = "MBC5";
                    break;
                case 0x1A:
                    info.mbc_type_str = "MBC5+RAM";
                    break;
                case 0x1B:
                    info.mbc_type_str = "MBC5+RAM+BATTERY";
                    info.battery = true;
                    break;
                default:
                    info.mbc_type_str = "MBC";
                    break;
            }

            info.sha1 = dragon::sha1_hex(data);
            return info;
        }
    }

    return info;
}

}  // namespace dragon::studio
