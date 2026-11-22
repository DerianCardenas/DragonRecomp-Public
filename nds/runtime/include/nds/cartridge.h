#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dragon::nds {

struct NdsHeader {
    std::string title;
    std::string game_code;
    std::string maker_code;
    uint8_t unit_code = 0;  // 0x00=NDS, 0x02=NDS+DSi, 0x03=DSi

    uint32_t arm9_rom_offset = 0;
    uint32_t arm9_entry_addr = 0;
    uint32_t arm9_ram_addr = 0;
    uint32_t arm9_size = 0;

    uint32_t arm7_rom_offset = 0;
    uint32_t arm7_entry_addr = 0;
    uint32_t arm7_ram_addr = 0;
    uint32_t arm7_size = 0;

    uint32_t fnt_offset = 0;
    uint32_t fnt_size = 0;
    uint32_t fat_offset = 0;
    uint32_t fat_size = 0;

    uint32_t arm9_overlay_offset = 0;
    uint32_t arm9_overlay_size = 0;
    uint32_t arm7_overlay_offset = 0;
    uint32_t arm7_overlay_size = 0;

    uint32_t icon_banner_offset = 0;
    uint32_t total_used_rom_size = 0;
    uint32_t header_size = 0x200U;
};

struct OverlayEntry {
    uint32_t overlay_id = 0;
    uint32_t ram_addr = 0;
    uint32_t ram_size = 0;
    uint32_t bss_size = 0;
    uint32_t sinit_start = 0;
    uint32_t sinit_end = 0;
    uint32_t file_id = 0;
    uint32_t rom_offset = 0;
    uint32_t rom_size = 0;
};

class Cartridge {
public:
    bool load_from_file(const std::filesystem::path& path, std::string* err = nullptr);
    bool load_from_bytes(const std::vector<uint8_t>& raw, std::string* err = nullptr);
    void reset() noexcept;

    bool is_loaded() const noexcept { return !rom_.empty(); }
    const NdsHeader& header() const noexcept { return header_; }
    const std::vector<uint8_t>& rom() const noexcept { return rom_; }
    const std::vector<OverlayEntry>& arm9_overlays() const noexcept { return arm9_overlays_; }
    uint32_t chip_id() const noexcept { return chip_id_; }

    uint8_t read_rom8(uint32_t offset) const noexcept;
    uint32_t read_rom32(uint32_t offset) const noexcept;

    // DS Cartridge I/O (AUXSPICNT, AUXSPIDATA, ROMCTRL, CARDCMD, CARDDATA)
    uint16_t read_auxspicnt() const noexcept { return auxspicnt_; }
    void write_auxspicnt(uint16_t val) noexcept;
    uint8_t read_auxspidata() const noexcept { return spi_out_; }
    void write_auxspidata(uint8_t val) noexcept;

    uint32_t read_romctrl() const noexcept { return romctrl_; }
    void write_romctrl(uint32_t val) noexcept;
    void write_cardcmd(int byte_idx, uint8_t val) noexcept;
    uint8_t read_cardcmd(int byte_idx) const noexcept { return cardcmd_[byte_idx & 7]; }
    uint32_t read_carddata() noexcept;
    bool has_data_ready() const noexcept { return (romctrl_ & (1U << 23)) != 0U; }
    bool consume_transfer_complete_irq() noexcept;

    // Save persistence (.sav)
    bool load_save_file(const std::filesystem::path& path);
    bool save_to_file(const std::filesystem::path& path) const;
    bool save_dirty() const noexcept { return save_dirty_; }

private:
    std::vector<uint8_t> rom_;
    NdsHeader header_{};
    std::vector<OverlayEntry> arm9_overlays_;
    uint32_t chip_id_ = 0x00001FC2U;

    // ROM command transfer state
    uint16_t auxspicnt_ = 0;
    uint32_t romctrl_ = 0;
    std::array<uint8_t, 8> cardcmd_{};
    std::vector<uint32_t> transfer_buf_;
    size_t transfer_pos_ = 0;
    bool transfer_irq_pending_ = false;

    // SPI Flash/EEPROM Save (512 KB default Flash)
    std::vector<uint8_t> save_mem_;
    uint8_t spi_cmd_ = 0;
    uint32_t spi_pos_ = 0;
    uint32_t spi_addr_ = 0;
    uint8_t spi_status_ = 0;
    uint8_t spi_out_ = 0xFFU;
    bool spi_write_enable_ = false;
    bool has_ir_ = false;
    bool ir_passthrough_ = false;
    bool save_dirty_ = false;

    bool parse_header(std::string* err);
    void parse_overlays();
    void execute_card_command() noexcept;
};

}  // namespace dragon::nds
