#include "nds/cartridge.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace dragon::nds {

namespace {

inline uint32_t read_le32(const std::vector<uint8_t>& buf, size_t off) noexcept {
    if (off + 4U > buf.size()) {
        return 0U;
    }
    return static_cast<uint32_t>(buf[off]) | (static_cast<uint32_t>(buf[off + 1U]) << 8U) |
           (static_cast<uint32_t>(buf[off + 2U]) << 16U) |
           (static_cast<uint32_t>(buf[off + 3U]) << 24U);
}

std::string read_ascii_trimmed(const std::vector<uint8_t>& buf, size_t off, size_t len) {
    if (off + len > buf.size()) {
        return {};
    }
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const uint8_t ch = buf[off + i];
        if (ch == 0U) {
            break;
        }
        if (ch >= 0x20U && ch <= 0x7EU) {
            s.push_back(static_cast<char>(ch));
        }
    }
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    return s;
}

}  // namespace

bool Cartridge::load_from_file(const std::filesystem::path& path, std::string* err) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        if (err)
            *err = "Cannot open NDS ROM file: " + path.string();
        return false;
    }
    const std::streamsize sz = in.tellg();
    if (sz < 0x200) {
        if (err)
            *err = "NDS ROM file too small (< 512 bytes)";
        return false;
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> raw(static_cast<size_t>(sz));
    if (!in.read(reinterpret_cast<char*>(raw.data()), sz)) {
        if (err)
            *err = "Failed reading NDS ROM file";
        return false;
    }
    return load_from_bytes(raw, err);
}

bool Cartridge::load_from_bytes(const std::vector<uint8_t>& raw, std::string* err) {
    if (raw.size() < 0x200U) {
        if (err)
            *err = "NDS ROM buffer too small";
        return false;
    }
    rom_ = raw;
    if (!parse_header(err)) {
        rom_.clear();
        return false;
    }
    parse_overlays();

    // Compute synthetic NDS chip ID based on ROM size (GBATEK)
    uint32_t mb = static_cast<uint32_t>((rom_.size() + 0xFFFFFU) >> 20U);
    uint8_t size_byte = 0x3FU;
    if (mb <= 1U)
        size_byte = 0x00U;
    else if (mb <= 8U)
        size_byte = 0x07U;
    else if (mb <= 16U)
        size_byte = 0x0FU;
    else if (mb <= 32U)
        size_byte = 0x1FU;
    else if (mb <= 64U)
        size_byte = 0x3FU;
    else if (mb <= 128U)
        size_byte = 0x7FU;
    else if (mb <= 256U)
        size_byte = 0xFEU;
    else
        size_byte = 0xFFU;
    chip_id_ = 0x000000C2U | (static_cast<uint32_t>(size_byte) << 8U);

    // Default 512 KB SPI Flash save memory (covers Pokemon D/P/Pt/HG/SS/B/W/B2/W2 & most DS games)
    save_mem_.assign(512U * 1024U, 0xFFU);
    has_ir_ = (!header_.game_code.empty() && header_.game_code[0] == 'I');
    ir_passthrough_ = false;
    save_dirty_ = false;
    reset();
    return true;
}

bool Cartridge::parse_header(std::string* err) {
    header_ = NdsHeader{};
    header_.title = read_ascii_trimmed(rom_, 0x000U, 12U);
    header_.game_code = read_ascii_trimmed(rom_, 0x00CU, 4U);
    header_.maker_code = read_ascii_trimmed(rom_, 0x010U, 2U);
    header_.unit_code = rom_[0x012U];

    header_.arm9_rom_offset = read_le32(rom_, 0x020U);
    header_.arm9_entry_addr = read_le32(rom_, 0x024U);
    header_.arm9_ram_addr = read_le32(rom_, 0x028U);
    header_.arm9_size = read_le32(rom_, 0x02CU);

    header_.arm7_rom_offset = read_le32(rom_, 0x030U);
    header_.arm7_entry_addr = read_le32(rom_, 0x034U);
    header_.arm7_ram_addr = read_le32(rom_, 0x038U);
    header_.arm7_size = read_le32(rom_, 0x03CU);

    header_.fnt_offset = read_le32(rom_, 0x040U);
    header_.fnt_size = read_le32(rom_, 0x044U);
    header_.fat_offset = read_le32(rom_, 0x048U);
    header_.fat_size = read_le32(rom_, 0x04CU);

    header_.arm9_overlay_offset = read_le32(rom_, 0x050U);
    header_.arm9_overlay_size = read_le32(rom_, 0x054U);
    header_.arm7_overlay_offset = read_le32(rom_, 0x058U);
    header_.arm7_overlay_size = read_le32(rom_, 0x05CU);

    header_.icon_banner_offset = read_le32(rom_, 0x068U);
    header_.total_used_rom_size = read_le32(rom_, 0x080U);
    header_.header_size = read_le32(rom_, 0x084U);

    if (header_.arm9_size == 0U ||
        static_cast<uint64_t>(header_.arm9_rom_offset) + header_.arm9_size > rom_.size()) {
        if (err)
            *err = "Invalid NDS header: ARM9 binary out of ROM bounds";
        return false;
    }
    if (header_.arm7_size == 0U ||
        static_cast<uint64_t>(header_.arm7_rom_offset) + header_.arm7_size > rom_.size()) {
        if (err)
            *err = "Invalid NDS header: ARM7 binary out of ROM bounds";
        return false;
    }
    return true;
}

void Cartridge::parse_overlays() {
    arm9_overlays_.clear();
    if (header_.arm9_overlay_size == 0U || header_.arm9_overlay_offset >= rom_.size()) {
        return;
    }
    const uint32_t count = header_.arm9_overlay_size / 32U;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t off = static_cast<size_t>(header_.arm9_overlay_offset) + i * 32U;
        if (off + 32U > rom_.size()) {
            break;
        }
        OverlayEntry e{};
        e.overlay_id = read_le32(rom_, off + 0U);
        e.ram_addr = read_le32(rom_, off + 4U);
        e.ram_size = read_le32(rom_, off + 8U);
        e.bss_size = read_le32(rom_, off + 12U);
        e.sinit_start = read_le32(rom_, off + 16U);
        e.sinit_end = read_le32(rom_, off + 20U);
        e.file_id = read_le32(rom_, off + 24U);

        // Resolve file_id in FAT
        const size_t fat_entry = static_cast<size_t>(header_.fat_offset) + e.file_id * 8U;
        if (header_.fat_size > 0U && fat_entry + 8U <= rom_.size()) {
            const uint32_t start = read_le32(rom_, fat_entry);
            const uint32_t end = read_le32(rom_, fat_entry + 4U);
            if (end >= start && end <= rom_.size()) {
                e.rom_offset = start;
                e.rom_size = end - start;
            }
        }
        arm9_overlays_.push_back(e);
    }
}

void Cartridge::reset() noexcept {
    auxspicnt_ = 0;
    romctrl_ = 0;
    cardcmd_.fill(0);
    transfer_buf_.clear();
    transfer_pos_ = 0;
    transfer_irq_pending_ = false;
    spi_cmd_ = 0;
    spi_pos_ = 0;
    spi_addr_ = 0;
    spi_status_ = 0;
    spi_out_ = 0xFFU;
    spi_write_enable_ = false;
    ir_passthrough_ = false;
}

uint8_t Cartridge::read_rom8(uint32_t offset) const noexcept {
    if (offset < rom_.size()) {
        return rom_[offset];
    }
    return 0xFFU;
}

uint32_t Cartridge::read_rom32(uint32_t offset) const noexcept {
    return read_le32(rom_, offset);
}

void Cartridge::write_auxspicnt(uint16_t val) noexcept {
    auxspicnt_ = val & 0xE043U;
}

void Cartridge::write_auxspidata(uint8_t val) noexcept {
    if (save_mem_.empty()) {
        spi_out_ = 0xFFU;
        return;
    }
    const bool hold = (auxspicnt_ & (1U << 6)) != 0U;

    // NTR-031 Infrared cartridges (Pokemon HG/SS/B/W/B2/W2, game_code 'Ixxx'):
    // First byte 0x00 enables passthrough to the underlying SPI Flash; 0x08 returns IR ID 0xAA.
    if (has_ir_ && !ir_passthrough_ && spi_pos_ == 0U) {
        if (val == 0x00U) {
            ir_passthrough_ = true;
            spi_out_ = 0x00U;
            if (!hold) {
                ir_passthrough_ = false;
            }
            return;
        }
        if (val == 0x08U) {
            spi_cmd_ = 0x08U;
            spi_out_ = 0x00U;
            spi_pos_ = 1U;
            if (!hold) {
                spi_pos_ = 0U;
            }
            return;
        }
    }
    if (has_ir_ && !ir_passthrough_ && spi_cmd_ == 0x08U) {
        spi_out_ = 0xAAU;
        if (!hold) {
            spi_pos_ = 0U;
            spi_cmd_ = 0U;
        }
        return;
    }

    if (spi_pos_ == 0U) {
        spi_cmd_ = val;
        spi_addr_ = 0;
        spi_out_ = 0xFFU;
        if (spi_cmd_ == 0x06U) {
            spi_write_enable_ = true;
            spi_status_ |= 0x02U;
        } else if (spi_cmd_ == 0x04U) {
            spi_write_enable_ = false;
            spi_status_ &= ~0x02U;
        }
        spi_pos_ = 1U;
    } else {
        switch (spi_cmd_) {
            case 0x05U:  // RDSR (Read Status Register)
                spi_out_ = spi_status_;
                break;
            case 0x9FU:  // RDID (JEDEC ID)
                spi_out_ = (spi_pos_ == 1U) ? 0x20U : (spi_pos_ == 2U ? 0x40U : 0x13U);
                break;
            case 0x03U:  // READ (24-bit address)
                if (spi_pos_ <= 3U) {
                    spi_addr_ = (spi_addr_ << 8U) | val;
                    spi_out_ = 0xFFU;
                } else {
                    spi_out_ = save_mem_[spi_addr_ % save_mem_.size()];
                    ++spi_addr_;
                }
                break;
            case 0x02U:  // PW / PP (Page Write/Program)
            case 0x0AU:  // PW (Flash Page Write)
                if (spi_pos_ <= 3U) {
                    spi_addr_ = (spi_addr_ << 8U) | val;
                } else if (spi_write_enable_) {
                    save_mem_[spi_addr_ % save_mem_.size()] = val;
                    ++spi_addr_;
                    save_dirty_ = true;
                }
                spi_out_ = 0xFFU;
                break;
            default:
                spi_out_ = 0xFFU;
                break;
        }
        ++spi_pos_;
    }
    if (!hold) {
        if (spi_cmd_ == 0x02U || spi_cmd_ == 0x0AU) {
            spi_write_enable_ = false;
            spi_status_ &= ~0x02U;
        }
        spi_pos_ = 0U;
        ir_passthrough_ = false;
    }
}

void Cartridge::write_cardcmd(int byte_idx, uint8_t val) noexcept {
    cardcmd_[byte_idx & 7] = val;
}

void Cartridge::write_romctrl(uint32_t val) noexcept {
    const bool start = (val & (1U << 31)) != 0U;
    romctrl_ = (val & 0x7F7FFFFFU);
    if (start) {
        execute_card_command();
    }
}

void Cartridge::execute_card_command() noexcept {
    transfer_buf_.clear();
    transfer_pos_ = 0;

    const uint32_t blk_field = (romctrl_ >> 24U) & 0x07U;
    size_t word_count = 0;
    if (blk_field == 0U) {
        word_count = 0;
    } else if (blk_field == 7U) {
        word_count = 1;  // 4 bytes
    } else {
        word_count = (0x100U << blk_field) / 4U;
    }

    const uint8_t cmd = cardcmd_[0];
    if (word_count > 0U) {
        transfer_buf_.resize(word_count, 0xFFFFFFFFU);
        if (cmd == 0xB8U || cmd == 0x90U) {
            // Get Chip ID
            for (size_t i = 0; i < word_count; ++i) {
                transfer_buf_[i] = chip_id_;
            }
        } else if (cmd == 0x00U) {
            // Read ROM Header (0x0000..0x0FFF)
            for (size_t i = 0; i < word_count; ++i) {
                transfer_buf_[i] = read_rom32(static_cast<uint32_t>(i * 4U));
            }
        } else if (cmd == 0xB7U) {
            // Unencrypted / Main Data Read: address in cardcmd[1..4] big-endian
            uint32_t addr = (static_cast<uint32_t>(cardcmd_[1]) << 24U) |
                            (static_cast<uint32_t>(cardcmd_[2]) << 16U) |
                            (static_cast<uint32_t>(cardcmd_[3]) << 8U) |
                            static_cast<uint32_t>(cardcmd_[4]);
            addr &= ~0x03U;
            // Hardware quirk: reads below 0x8000 redirect to 0x8000 + (addr & 0x1FF)
            if (addr < 0x8000U) {
                addr = 0x8000U + (addr & 0x1FFU);
            }
            for (size_t i = 0; i < word_count; ++i) {
                transfer_buf_[i] = read_rom32(addr + static_cast<uint32_t>(i * 4U));
            }
        } else if (cmd == 0x9FU) {
            // Dummy high-Z command -> all 0xFF
            std::fill(transfer_buf_.begin(), transfer_buf_.end(), 0xFFFFFFFFU);
        }
        romctrl_ |= (1U << 31) | (1U << 23);  // Busy + Data Word Ready
    } else {
        romctrl_ &= ~((1U << 31) | (1U << 23));
        if (auxspicnt_ & (1U << 14)) {
            transfer_irq_pending_ = true;
        }
    }
}

uint32_t Cartridge::read_carddata() noexcept {
    if (transfer_pos_ >= transfer_buf_.size()) {
        romctrl_ &= ~((1U << 31) | (1U << 23));
        return 0xFFFFFFFFU;
    }
    const uint32_t word = transfer_buf_[transfer_pos_++];
    if (transfer_pos_ >= transfer_buf_.size()) {
        romctrl_ &= ~((1U << 31) | (1U << 23));
        if (auxspicnt_ & (1U << 14)) {
            transfer_irq_pending_ = true;
        }
    }
    return word;
}

bool Cartridge::consume_transfer_complete_irq() noexcept {
    const bool p = transfer_irq_pending_;
    transfer_irq_pending_ = false;
    return p;
}

bool Cartridge::load_save_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    const std::streamsize sz = in.tellg();
    if (sz <= 0)
        return false;
    in.seekg(0, std::ios::beg);
    save_mem_.resize(static_cast<size_t>(sz));
    in.read(reinterpret_cast<char*>(save_mem_.data()), sz);
    save_dirty_ = false;
    return in.good();
}

bool Cartridge::save_to_file(const std::filesystem::path& path) const {
    if (save_mem_.empty())
        return false;
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(save_mem_.data()),
              static_cast<std::streamsize>(save_mem_.size()));
    return out.good();
}

}  // namespace dragon::nds
