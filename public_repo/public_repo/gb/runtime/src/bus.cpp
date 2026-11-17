#include "gb/bus.h"

#include <algorithm>
#include <cstring>

#include "dragon/file_io.h"
#include "dragon/save_state.h"

namespace dragon::gb {

Bus::Bus() noexcept {
    flat_ram_.fill(0);
    vram_bank0_.fill(0);
    vram_bank1_.fill(0);
    wram_bank0_.fill(0);
    wram_banks_.fill(0);
    oam_.fill(0);
    hram_.fill(0);
}

void Bus::load_cartridge(const Cartridge& cart) noexcept {
    rom_ = cart.rom;
    mbc_ = cart.mbc;
    has_battery_ = cart.has_battery;
    is_cgb_ = (cart.cgb_mode != CgbSupport::DmgOnly);
    ppu_.set_cgb(is_cgb_);

    if (cart.ram_size > 0) {
        sram_.assign(cart.ram_size, 0);
    } else {
        sram_.clear();
    }

    rom_bank_ = 1;
    ram_bank_ = 0;
    ram_enabled_ = false;
    mbc1_mode_ = 0;
    flat_ram_mode_ = false;
    last_ppu_sync_cycles_ = 0;
    last_apu_sync_cycles_ = 0;
    apu_.reset();

    hdma_src_ = 0;
    hdma_dst_ = 0;
    hdma5_ = 0xFF;
    hdma_active_ = false;

    rtc_s_ = 0;
    rtc_m_ = 0;
    rtc_h_ = 0;
    rtc_dl_ = 0;
    rtc_dh_ = 0;
    rtc_select_ = 0;
    rtc_latch_prev_ = 0xFF;
    latched_rtc_s_ = 0;
    latched_rtc_m_ = 0;
    latched_rtc_h_ = 0;
    latched_rtc_dl_ = 0;
    latched_rtc_dh_ = 0;
    rtc_cycles_ = 0;
}

void Bus::load_rom(std::span<const uint8_t> rom) noexcept {
    rom_.assign(rom.begin(), rom.end());
    rom_bank_ = 1;
    ram_bank_ = 0;
    ram_enabled_ = false;
    flat_ram_mode_ = false;
}

void Bus::load_direct(uint16_t addr, std::span<const uint8_t> bytes) noexcept {
    if (flat_ram_mode_) {
        for (size_t i = 0; i < bytes.size() && (addr + i) < 65536; ++i) {
            flat_ram_[addr + i] = bytes[i];
        }
    } else {
        for (size_t i = 0; i < bytes.size(); ++i) {
            write((uint16_t)(addr + i), bytes[i]);
        }
    }
}

uint8_t Bus::peek(uint16_t addr) const noexcept {
    if (flat_ram_mode_) {
        return flat_ram_[addr];
    }

    // ROM Bank 00 ($0000-$3FFF)
    if (addr < 0x4000) {
        if (rom_.empty())
            return 0xFF;
        uint32_t offset = addr;
        if (mbc_ == MbcType::Mbc1 && mbc1_mode_ == 1) {
            offset |= (static_cast<uint32_t>(ram_bank_) << 19);
        }
        offset %= rom_.size();
        return rom_[offset];
    }

    // Switchable ROM Bank ($4000-$7FFF)
    if (addr < 0x8000) {
        if (rom_.empty())
            return 0xFF;
        uint32_t bank = rom_bank_;
        if (mbc_ == MbcType::Mbc1 && mbc1_mode_ == 1) {
            bank |= (static_cast<uint32_t>(ram_bank_) << 5);
        }
        uint32_t offset = (bank * 0x4000U) + (addr - 0x4000U);
        offset %= rom_.size();
        return rom_[offset];
    }

    // VRAM ($8000-$9FFF)
    if (addr < 0xA000) {
        uint16_t offset = addr - 0x8000U;
        return (is_cgb_ && vram_bank_idx_ == 1) ? vram_bank1_[offset] : vram_bank0_[offset];
    }

    // External RAM / RTC ($A000-$BFFF)
    if (addr < 0xC000) {
        if (!ram_enabled_)
            return 0xFF;
        if (mbc_ == MbcType::Mbc3 && rtc_select_ >= 0x08 && rtc_select_ <= 0x0C) {
            switch (rtc_select_) {
                case 0x08:
                    return latched_rtc_s_;
                case 0x09:
                    return latched_rtc_m_;
                case 0x0A:
                    return latched_rtc_h_;
                case 0x0B:
                    return latched_rtc_dl_;
                case 0x0C:
                    return latched_rtc_dh_;
            }
        }
        if (sram_.empty())
            return 0xFF;
        uint32_t offset = (static_cast<uint32_t>(ram_bank_) * 0x2000U) + (addr - 0xA000U);
        offset %= sram_.size();
        return sram_[offset];
    }

    // WRAM Bank 0 ($C000-$CFFF)
    if (addr < 0xD000) {
        return wram_bank0_[addr - 0xC000U];
    }

    // WRAM Banks 1..7 ($D000-$DFFF)
    if (addr < 0xE000) {
        uint32_t bank = (is_cgb_ && wram_bank_idx_ > 0) ? (wram_bank_idx_ - 1U) : 0U;
        return wram_banks_[(bank * 0x1000U) + (addr - 0xD000U)];
    }

    // Echo RAM ($E000-$FDFF) -> mirror of $C000-$DDFF
    if (addr < 0xFE00) {
        return peek((uint16_t)(addr - 0x2000U));
    }

    // OAM ($FE00-$FE9F)
    if (addr < 0xFEA0) {
        return oam_[addr - 0xFE00U];
    }

    // Prohibited ($FEA0-$FEFF)
    if (addr < 0xFF00) {
        return 0xFF;
    }

    // I/O Registers ($FF00-$FF7F)
    if (addr < 0xFF80) {
        switch (addr) {
            case 0xFF00:
                return joyp_;
            case 0xFF01:
                return sb_;
            case 0xFF02:
                return sc_ | 0x7EU;
            case 0xFF04:
                return static_cast<uint8_t>(div_counter_ >> 8U);
            case 0xFF05:
                return tima_;
            case 0xFF06:
                return tma_;
            case 0xFF07:
                return tac_ | 0xF8U;
            case 0xFF0F:
                return if_reg_ | 0xE0U;
            case 0xFF10:
            case 0xFF11:
            case 0xFF12:
            case 0xFF13:
            case 0xFF14:
            case 0xFF16:
            case 0xFF17:
            case 0xFF18:
            case 0xFF19:
            case 0xFF1A:
            case 0xFF1B:
            case 0xFF1C:
            case 0xFF1D:
            case 0xFF1E:
            case 0xFF20:
            case 0xFF21:
            case 0xFF22:
            case 0xFF23:
            case 0xFF24:
            case 0xFF25:
            case 0xFF26:
            case 0xFF30:
            case 0xFF31:
            case 0xFF32:
            case 0xFF33:
            case 0xFF34:
            case 0xFF35:
            case 0xFF36:
            case 0xFF37:
            case 0xFF38:
            case 0xFF39:
            case 0xFF3A:
            case 0xFF3B:
            case 0xFF3C:
            case 0xFF3D:
            case 0xFF3E:
            case 0xFF3F:
                return apu_.read_register(addr);
            case 0xFF40:
            case 0xFF41:
            case 0xFF42:
            case 0xFF43:
            case 0xFF44:
            case 0xFF45:
            case 0xFF47:
            case 0xFF48:
            case 0xFF49:
            case 0xFF4A:
            case 0xFF4B:
            case 0xFF68:
            case 0xFF69:
            case 0xFF6A:
            case 0xFF6B:
                return ppu_.read_register(addr);
            case 0xFF4D:
                return is_cgb_ ? (key1_ | 0x7EU) : 0xFFU;
            case 0xFF4F:
                return is_cgb_ ? (vram_bank_idx_ | 0xFEU) : 0xFFU;
            case 0xFF51:
                return is_cgb_ ? static_cast<uint8_t>(hdma_src_ >> 8U) : 0xFFU;
            case 0xFF52:
                return is_cgb_ ? static_cast<uint8_t>(hdma_src_ & 0xFFU) : 0xFFU;
            case 0xFF53:
                return is_cgb_ ? static_cast<uint8_t>(hdma_dst_ >> 8U) : 0xFFU;
            case 0xFF54:
                return is_cgb_ ? static_cast<uint8_t>(hdma_dst_ & 0xFFU) : 0xFFU;
            case 0xFF55:
                if (!is_cgb_)
                    return 0xFFU;
                return hdma_active_ ? (hdma5_ & 0x7FU) : 0xFFU;
            case 0xFF70:
                return is_cgb_ ? (wram_bank_idx_ | 0xF8U) : 0xFFU;
            default:
                return 0xFF;
        }
    }

    // HRAM ($FF80-$FFFE)
    if (addr < 0xFFFF) {
        return hram_[addr - 0xFF80U];
    }

    // IE ($FFFF)
    return ie_reg_;
}

uint8_t Bus::read(uint16_t addr) noexcept {
    return peek(addr);
}

void Bus::write(uint16_t addr, uint8_t value) noexcept {
    if (flat_ram_mode_) {
        flat_ram_[addr] = value;
        if (addr == 0xFF0FU)
            if_reg_ = value & 0x1FU;
        if (addr == 0xFFFFU)
            ie_reg_ = value;
        return;
    }

    // MBC Control Registers ($0000-$7FFF)
    if (addr < 0x2000) {
        // RAM Enable
        if (mbc_ == MbcType::Mbc1 || mbc_ == MbcType::Mbc3 || mbc_ == MbcType::Mbc5) {
            ram_enabled_ = ((value & 0x0FU) == 0x0AU);
        } else if (mbc_ == MbcType::Mbc2) {
            if ((addr & 0x0100U) == 0) {
                ram_enabled_ = ((value & 0x0FU) == 0x0AU);
            }
        }
        return;
    }

    if (addr < 0x4000) {
        // ROM Bank Number
        if (mbc_ == MbcType::Mbc1) {
            uint8_t b = value & 0x1FU;
            if (b == 0)
                b = 1;
            rom_bank_ = (rom_bank_ & 0x60U) | b;
        } else if (mbc_ == MbcType::Mbc3) {
            uint8_t b = value & 0x7FU;
            if (b == 0)
                b = 1;
            rom_bank_ = b;
        } else if (mbc_ == MbcType::Mbc5) {
            if (addr < 0x3000) {
                rom_bank_ = (rom_bank_ & 0x100U) | value;
            } else {
                rom_bank_ = (rom_bank_ & 0x0FFU) | ((static_cast<uint16_t>(value & 0x01U)) << 8U);
            }
        } else if (mbc_ == MbcType::Mbc2) {
            if ((addr & 0x0100U) != 0) {
                uint8_t b = value & 0x0FU;
                if (b == 0)
                    b = 1;
                rom_bank_ = b;
            }
        }
        return;
    }

    if (addr < 0x6000) {
        // RAM Bank Number / Upper ROM bits / RTC Register Select
        if (mbc_ == MbcType::Mbc1) {
            ram_bank_ = value & 0x03U;
        } else if (mbc_ == MbcType::Mbc3) {
            if (value <= 0x03U) {
                ram_bank_ = value;
                rtc_select_ = 0;
            } else if (value >= 0x08U && value <= 0x0CU) {
                rtc_select_ = value;
            }
        } else if (mbc_ == MbcType::Mbc5) {
            ram_bank_ = value & 0x0FU;
        }
        return;
    }

    if (addr < 0x8000) {
        // Banking Mode Select / RTC Latch
        if (mbc_ == MbcType::Mbc1) {
            mbc1_mode_ = value & 0x01U;
        } else if (mbc_ == MbcType::Mbc3) {
            // Latch sequence: 0x00 then 0x01
            if (rtc_latch_prev_ == 0x00 && value == 0x01) {
                latched_rtc_s_ = rtc_s_;
                latched_rtc_m_ = rtc_m_;
                latched_rtc_h_ = rtc_h_;
                latched_rtc_dl_ = rtc_dl_;
                latched_rtc_dh_ = rtc_dh_;
            }
            rtc_latch_prev_ = value;
        }
        return;
    }

    // VRAM ($8000-$9FFF)
    if (addr < 0xA000) {
        uint16_t offset = addr - 0x8000U;
        if (is_cgb_ && vram_bank_idx_ == 1) {
            vram_bank1_[offset] = value;
        } else {
            vram_bank0_[offset] = value;
        }
        return;
    }

    // External RAM / RTC ($A000-$BFFF)
    if (addr < 0xC000) {
        if (!ram_enabled_)
            return;
        if (mbc_ == MbcType::Mbc3 && rtc_select_ >= 0x08 && rtc_select_ <= 0x0C) {
            switch (rtc_select_) {
                case 0x08:
                    rtc_s_ = value;
                    latched_rtc_s_ = value;
                    break;
                case 0x09:
                    rtc_m_ = value;
                    latched_rtc_m_ = value;
                    break;
                case 0x0A:
                    rtc_h_ = value;
                    latched_rtc_h_ = value;
                    break;
                case 0x0B:
                    rtc_dl_ = value;
                    latched_rtc_dl_ = value;
                    break;
                case 0x0C:
                    rtc_dh_ = value;
                    latched_rtc_dh_ = value;
                    break;
            }
            return;
        }
        if (sram_.empty())
            return;
        uint32_t offset = (static_cast<uint32_t>(ram_bank_) * 0x2000U) + (addr - 0xA000U);
        offset %= sram_.size();
        sram_[offset] = value;
        return;
    }

    // WRAM Bank 0 ($C000-$CFFF)
    if (addr < 0xD000) {
        wram_bank0_[addr - 0xC000U] = value;
        return;
    }

    // WRAM Banks 1..7 ($D000-$DFFF)
    if (addr < 0xE000) {
        uint32_t bank = (is_cgb_ && wram_bank_idx_ > 0) ? (wram_bank_idx_ - 1U) : 0U;
        wram_banks_[(bank * 0x1000U) + (addr - 0xD000U)] = value;
        return;
    }

    // Echo RAM ($E000-$FDFF) -> mirror of $C000-$DDFF
    if (addr < 0xFE00) {
        write((uint16_t)(addr - 0x2000U), value);
        return;
    }

    // OAM ($FE00-$FE9F)
    if (addr < 0xFEA0) {
        oam_[addr - 0xFE00U] = value;
        return;
    }

    // Prohibited ($FEA0-$FEFF)
    if (addr < 0xFF00) {
        return;
    }

    // I/O Registers ($FF00-$FF7F)
    if (addr < 0xFF80) {
        switch (addr) {
            case 0xFF00:
                joyp_ = (joyp_ & 0x0FU) | (value & 0x30U);
                update_joyp();
                break;
            case 0xFF01:
                sb_ = value;
                break;
            case 0xFF02:
                sc_ = value;
                // Blargg test ROMs: writing 0x81 starts transfer with internal clock
                if ((value & 0x81U) == 0x81U) {
                    serial_out_.push_back(static_cast<char>(sb_));
                    sc_ &= ~0x80U;         // transfer complete
                    request_interrupt(3);  // serial interrupt
                }
                break;
            case 0xFF04:
                div_counter_ = 0;
                break;
            case 0xFF05:
                tima_ = value;
                break;
            case 0xFF06:
                tma_ = value;
                break;
            case 0xFF07:
                tac_ = value & 0x07U;
                break;
            case 0xFF0F:
                if_reg_ = value & 0x1FU;
                break;
            case 0xFF10:
            case 0xFF11:
            case 0xFF12:
            case 0xFF13:
            case 0xFF14:
            case 0xFF16:
            case 0xFF17:
            case 0xFF18:
            case 0xFF19:
            case 0xFF1A:
            case 0xFF1B:
            case 0xFF1C:
            case 0xFF1D:
            case 0xFF1E:
            case 0xFF20:
            case 0xFF21:
            case 0xFF22:
            case 0xFF23:
            case 0xFF24:
            case 0xFF25:
            case 0xFF26:
            case 0xFF30:
            case 0xFF31:
            case 0xFF32:
            case 0xFF33:
            case 0xFF34:
            case 0xFF35:
            case 0xFF36:
            case 0xFF37:
            case 0xFF38:
            case 0xFF39:
            case 0xFF3A:
            case 0xFF3B:
            case 0xFF3C:
            case 0xFF3D:
            case 0xFF3E:
            case 0xFF3F:
                apu_.write_register(addr, value);
                break;
            case 0xFF40:
            case 0xFF41:
            case 0xFF42:
            case 0xFF43:
            case 0xFF44:
            case 0xFF45:
            case 0xFF47:
            case 0xFF48:
            case 0xFF49:
            case 0xFF4A:
            case 0xFF4B:
            case 0xFF68:
            case 0xFF69:
            case 0xFF6A:
            case 0xFF6B:
                ppu_.write_register(addr, value, this);
                break;
            case 0xFF46: {
                // OAM DMA ($FF46)
                uint16_t src = static_cast<uint16_t>(value) << 8U;
                for (uint16_t i = 0; i < 0xA0; ++i) {
                    oam_[i] = read((uint16_t)(src + i));
                }
                break;
            }
            case 0xFF4D:
                if (is_cgb_) {
                    key1_ = (key1_ & 0x80U) | (value & 0x01U);
                }
                break;
            case 0xFF4F:
                if (is_cgb_) {
                    vram_bank_idx_ = value & 0x01U;
                }
                break;
            case 0xFF51:
                if (is_cgb_) {
                    hdma_src_ = (hdma_src_ & 0x00FFU) | (static_cast<uint16_t>(value) << 8U);
                }
                break;
            case 0xFF52:
                if (is_cgb_) {
                    hdma_src_ = (hdma_src_ & 0xFF00U) | (value & 0xF0U);
                }
                break;
            case 0xFF53:
                if (is_cgb_) {
                    hdma_dst_ =
                        (hdma_dst_ & 0x00FFU) | (static_cast<uint16_t>(value & 0x1FU) << 8U);
                }
                break;
            case 0xFF54:
                if (is_cgb_) {
                    hdma_dst_ = (hdma_dst_ & 0xFF00U) | (value & 0xF0U);
                }
                break;
            case 0xFF55:
                if (is_cgb_) {
                    trigger_hdma(value);
                }
                break;
            case 0xFF70:
                if (is_cgb_) {
                    wram_bank_idx_ = value & 0x07U;
                    if (wram_bank_idx_ == 0)
                        wram_bank_idx_ = 1;
                }
                break;
            default:
                break;
        }
        return;
    }

    // HRAM ($FF80-$FFFE)
    if (addr < 0xFFFF) {
        hram_[addr - 0xFF80U] = value;
        return;
    }

    // IE ($FFFF)
    ie_reg_ = value;
}

void Bus::update_joyp() noexcept {
    uint8_t low_nibble = 0x0FU;
    // Bit 4 low: select direction keys
    if ((joyp_ & 0x10U) == 0) {
        if (joypad_state_ & kButtonRight)
            low_nibble &= ~0x01U;
        if (joypad_state_ & kButtonLeft)
            low_nibble &= ~0x02U;
        if (joypad_state_ & kButtonUp)
            low_nibble &= ~0x04U;
        if (joypad_state_ & kButtonDown)
            low_nibble &= ~0x08U;
    }
    // Bit 5 low: select action buttons
    if ((joyp_ & 0x20U) == 0) {
        if (joypad_state_ & kButtonA)
            low_nibble &= ~0x01U;
        if (joypad_state_ & kButtonB)
            low_nibble &= ~0x02U;
        if (joypad_state_ & kButtonSelect)
            low_nibble &= ~0x04U;
        if (joypad_state_ & kButtonStart)
            low_nibble &= ~0x08U;
    }
    joyp_ = (joyp_ & 0xF0U) | low_nibble;
}

void Bus::step_timers(uint32_t t_cycles) noexcept {
    step_rtc(t_cycles);
    div_counter_ += static_cast<uint16_t>(t_cycles);

    if (tac_ & 0x04U) {  // Timer enabled
        static constexpr uint16_t kClocks[4] = {1024, 16, 64, 256};
        uint16_t period = kClocks[tac_ & 0x03U];

        // Increment TIMA based on divider counter edges
        // Simplified accurate model: accumulate cycles
        uint32_t remaining = t_cycles;
        while (remaining > 0) {
            uint32_t step = std::min<uint32_t>(remaining, period);
            remaining -= step;
            if ((div_counter_ % period) < step) {
                if (tima_ == 0xFFU) {
                    tima_ = tma_;
                    request_interrupt(2);  // Timer interrupt
                } else {
                    ++tima_;
                }
            }
        }
    }
}

void Bus::sync_ppu(uint64_t cpu_cycles) noexcept {
    if (flat_ram_mode_)
        return;
    if (cpu_cycles > last_ppu_sync_cycles_) {
        uint32_t delta = static_cast<uint32_t>(cpu_cycles - last_ppu_sync_cycles_);
        last_ppu_sync_cycles_ = cpu_cycles;
        ppu_.advance(delta, this);
    }
}

void Bus::sync_apu(uint64_t cpu_cycles) noexcept {
    if (flat_ram_mode_)
        return;
    if (cpu_cycles > last_apu_sync_cycles_) {
        uint32_t delta = static_cast<uint32_t>(cpu_cycles - last_apu_sync_cycles_);
        last_apu_sync_cycles_ = cpu_cycles;
        apu_.advance(delta);
    }
}

void Bus::step_rtc(uint32_t t_cycles) noexcept {
    if (mbc_ != MbcType::Mbc3)
        return;
    if (rtc_dh_ & 0x40U)
        return;  // Timer halted

    rtc_cycles_ += t_cycles;
    while (rtc_cycles_ >= 4194304U) {
        rtc_cycles_ -= 4194304U;
        if (++rtc_s_ >= 60U) {
            rtc_s_ = 0;
            if (++rtc_m_ >= 60U) {
                rtc_m_ = 0;
                if (++rtc_h_ >= 24U) {
                    rtc_h_ = 0;
                    uint16_t days = static_cast<uint16_t>(rtc_dl_) |
                                    (static_cast<uint16_t>(rtc_dh_ & 0x01U) << 8U);
                    ++days;
                    if (days >= 512U) {
                        days = 0;
                        rtc_dh_ |= 0x80U;  // Day counter carry
                    }
                    rtc_dl_ = static_cast<uint8_t>(days & 0xFFU);
                    rtc_dh_ = (rtc_dh_ & ~0x01U) | static_cast<uint8_t>((days >> 8U) & 0x01U);
                }
            }
        }
    }
}

void Bus::step_hdma() noexcept {
    if (!is_cgb_ || !hdma_active_)
        return;

    for (uint16_t i = 0; i < 16; ++i) {
        uint16_t src = (hdma_src_ & 0xFFF0U) + i;
        uint16_t dst = 0x8000U | (((hdma_dst_ & 0x1FF0U) + i) & 0x1FFFU);
        write(dst, read(src));
    }
    hdma_src_ += 16U;
    hdma_dst_ += 16U;
    if (hdma5_ == 0) {
        hdma_active_ = false;
        hdma5_ = 0xFFU;
    } else {
        --hdma5_;
    }
}

void Bus::trigger_hdma(uint8_t value) noexcept {
    if (hdma_active_) {
        if ((value & 0x80U) == 0) {
            // Cancel active HDMA
            hdma_active_ = false;
            hdma5_ |= 0x80U;
            return;
        }
    }

    uint16_t blocks = (value & 0x7FU) + 1U;
    bool is_hblank = (value & 0x80U) != 0;

    if (!is_hblank) {
        // Immediate transfer (GDMA)
        for (uint16_t b = 0; b < blocks; ++b) {
            for (uint16_t i = 0; i < 16; ++i) {
                uint16_t src = (hdma_src_ & 0xFFF0U) + (b * 16U) + i;
                uint16_t dst = 0x8000U | (((hdma_dst_ & 0x1FF0U) + (b * 16U) + i) & 0x1FFFU);
                write(dst, read(src));
            }
        }
        hdma_src_ += blocks * 16U;
        hdma_dst_ += blocks * 16U;
        hdma5_ = 0xFFU;
        hdma_active_ = false;
    } else {
        // HBlank DMA
        hdma_active_ = true;
        hdma5_ = value & 0x7FU;
    }
}

void Bus::on_frame_complete(CpuState* cpu) noexcept {
    if (frame_cb_) {
        frame_cb_(cpu, this, frame_user_data_);
    }
}

RecompFunc Bus::lookup_function(uint16_t addr, uint8_t bank) const noexcept {
    if (!dispatch_entries_ || dispatch_count_ == 0) {
        return nullptr;
    }
    const uint8_t eff_bank = (addr < 0x4000U) ? 0U : bank;
    size_t lo = 0;
    size_t hi = dispatch_count_;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const RecompFuncEntry& e = dispatch_entries_[mid];
        if (e.bank < eff_bank || (e.bank == eff_bank && e.addr < addr)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < dispatch_count_ && dispatch_entries_[lo].bank == eff_bank &&
        dispatch_entries_[lo].addr == addr) {
        return dispatch_entries_[lo].func;
    }
    return nullptr;
}

void Bus::log_fallback(uint16_t addr, uint8_t bank, uint16_t from_pc) {
    if (addr >= 0x8000U) {
        return;
    }
    const uint8_t eff_bank = (addr < 0x4000U) ? 0U : bank;
    if (logged_fallbacks_.insert({addr, eff_bank}).second) {
        std::fprintf(stderr, "FALLBACK addr=$%04X bank=$%02X from=$%04X\n", addr, eff_bank,
                     from_pc);
        std::fflush(stderr);
    }
}

bool Bus::load_battery_ram(const std::filesystem::path& path) noexcept {
    auto data = dragon::try_read_binary_file(path);
    if (data.empty() || sram_.empty()) {
        return false;
    }
    const size_t to_copy = std::min(data.size(), sram_.size());
    std::memcpy(sram_.data(), data.data(), to_copy);
    return true;
}

bool Bus::save_battery_ram(const std::filesystem::path& path) const noexcept {
    if (sram_.empty()) {
        return false;
    }
    return dragon::try_write_binary_file(path, sram_);
}

struct GbStatePayloadV1 {
    uint8_t a, f, b, c, d, e, h, l;
    uint16_t sp, pc;
    uint8_t ime, ime_delay, halted, halt_bug, double_speed;
    uint64_t cycles, next_event;

    std::array<uint8_t, 0x2000> vram_bank0;
    std::array<uint8_t, 0x2000> vram_bank1;
    std::array<uint8_t, 0x1000> wram_bank0;
    std::array<uint8_t, 0x7000> wram_banks;
    std::array<uint8_t, 0x00A0> oam;
    std::array<uint8_t, 0x007F> hram;
    uint32_t sram_size;

    bool ram_enabled;
    uint16_t rom_bank;
    uint8_t ram_bank;
    uint8_t mbc1_mode;

    uint8_t rtc_s, rtc_m, rtc_h, rtc_dl, rtc_dh, rtc_select;
    uint8_t rtc_latch_prev, latched_rtc_s, latched_rtc_m, latched_rtc_h, latched_rtc_dl;

    uint16_t div_counter;
    uint8_t tima, tma, tac;
    uint8_t if_reg, ie_reg;
    uint8_t joypad_state;

    uint8_t vram_bank_idx;
    uint8_t wram_bank_idx;
    uint8_t key1;

    Ppu::Snapshot ppu;
    Apu::Snapshot apu;
};

struct GbStatePayloadV2 {
    uint8_t a, f, b, c, d, e, h, l;
    uint16_t sp, pc;
    uint8_t ime, ime_delay, halted, halt_bug, double_speed;
    uint64_t cycles, next_event;

    std::array<uint8_t, 0x2000> vram_bank0;
    std::array<uint8_t, 0x2000> vram_bank1;
    std::array<uint8_t, 0x1000> wram_bank0;
    std::array<uint8_t, 0x7000> wram_banks;
    std::array<uint8_t, 0x00A0> oam;
    std::array<uint8_t, 0x007F> hram;
    uint32_t sram_size;

    bool ram_enabled;
    uint16_t rom_bank;
    uint8_t ram_bank;
    uint8_t mbc1_mode;

    uint8_t rtc_s, rtc_m, rtc_h, rtc_dl, rtc_dh, rtc_select;
    uint8_t rtc_latch_prev, latched_rtc_s, latched_rtc_m, latched_rtc_h, latched_rtc_dl, latched_rtc_dh;
    uint32_t rtc_cycles;

    uint16_t div_counter;
    uint8_t tima, tma, tac;
    uint8_t if_reg, ie_reg;
    uint8_t joypad_state;

    uint8_t vram_bank_idx;
    uint8_t wram_bank_idx;
    uint8_t key1;

    bool is_cgb;
    uint16_t hdma_src;
    uint16_t hdma_dst;
    uint8_t hdma5;
    bool hdma_active;

    Ppu::Snapshot ppu;
    Apu::Snapshot apu;
};

bool Bus::save_state(const CpuState& cpu, const std::filesystem::path& path) const noexcept {
    GbStatePayloadV2 payload{};
    payload.a = cpu.a;
    payload.f = cpu.f;
    payload.b = cpu.b;
    payload.c = cpu.c;
    payload.d = cpu.d;
    payload.e = cpu.e;
    payload.h = cpu.h;
    payload.l = cpu.l;
    payload.sp = cpu.sp;
    payload.pc = cpu.pc;
    payload.ime = cpu.ime;
    payload.ime_delay = cpu.ime_delay;
    payload.halted = cpu.halted;
    payload.halt_bug = cpu.halt_bug;
    payload.double_speed = cpu.double_speed;
    payload.cycles = cpu.cycles;
    payload.next_event = cpu.next_event;

    payload.vram_bank0 = vram_bank0_;
    payload.vram_bank1 = vram_bank1_;
    payload.wram_bank0 = wram_bank0_;
    payload.wram_banks = wram_banks_;
    payload.oam = oam_;
    payload.hram = hram_;
    payload.sram_size = static_cast<uint32_t>(sram_.size());

    payload.ram_enabled = ram_enabled_;
    payload.rom_bank = rom_bank_;
    payload.ram_bank = ram_bank_;
    payload.mbc1_mode = mbc1_mode_;

    payload.rtc_s = rtc_s_;
    payload.rtc_m = rtc_m_;
    payload.rtc_h = rtc_h_;
    payload.rtc_dl = rtc_dl_;
    payload.rtc_dh = rtc_dh_;
    payload.rtc_select = rtc_select_;
    payload.rtc_latch_prev = rtc_latch_prev_;
    payload.latched_rtc_s = latched_rtc_s_;
    payload.latched_rtc_m = latched_rtc_m_;
    payload.latched_rtc_h = latched_rtc_h_;
    payload.latched_rtc_dl = latched_rtc_dl_;
    payload.latched_rtc_dh = latched_rtc_dh_;
    payload.rtc_cycles = rtc_cycles_;

    payload.div_counter = div_counter_;
    payload.tima = tima_;
    payload.tma = tma_;
    payload.tac = tac_;
    payload.if_reg = if_reg_;
    payload.ie_reg = ie_reg_;
    payload.joypad_state = joypad_state_;

    payload.vram_bank_idx = vram_bank_idx_;
    payload.wram_bank_idx = wram_bank_idx_;
    payload.key1 = key1_;

    payload.is_cgb = is_cgb_;
    payload.hdma_src = hdma_src_;
    payload.hdma_dst = hdma_dst_;
    payload.hdma5 = hdma5_;
    payload.hdma_active = hdma_active_;

    ppu_.save_snapshot(payload.ppu);
    apu_.save_snapshot(payload.apu);

    dragon::SaveStateHeader hdr;
    std::memcpy(hdr.system, "GB  ", 4);
    hdr.version = 2;
    hdr.payload_size = sizeof(GbStatePayloadV2) + payload.sram_size;

    std::vector<uint8_t> buffer(sizeof(hdr) + sizeof(payload) + payload.sram_size);
    std::memcpy(buffer.data(), &hdr, sizeof(hdr));
    std::memcpy(buffer.data() + sizeof(hdr), &payload, sizeof(payload));
    if (payload.sram_size > 0 && !sram_.empty()) {
        std::memcpy(buffer.data() + sizeof(hdr) + sizeof(payload), sram_.data(), payload.sram_size);
    }

    return dragon::try_write_binary_file(path, buffer);
}

bool Bus::load_state(CpuState& cpu, const std::filesystem::path& path) noexcept {
    auto buffer = dragon::try_read_binary_file(path);
    if (buffer.size() < sizeof(dragon::SaveStateHeader)) {
        return false;
    }
    dragon::SaveStateHeader hdr;
    std::memcpy(&hdr, buffer.data(), sizeof(hdr));

    if (hdr.is_valid("GB  ", 2)) {
        if (buffer.size() < sizeof(hdr) + sizeof(GbStatePayloadV2)) {
            return false;
        }
        GbStatePayloadV2 payload;
        std::memcpy(&payload, buffer.data() + sizeof(hdr), sizeof(payload));
        if (buffer.size() < sizeof(hdr) + sizeof(payload) + payload.sram_size) {
            return false;
        }

        cpu.a = payload.a;
        cpu.f = payload.f;
        cpu.b = payload.b;
        cpu.c = payload.c;
        cpu.d = payload.d;
        cpu.e = payload.e;
        cpu.h = payload.h;
        cpu.l = payload.l;
        cpu.sp = payload.sp;
        cpu.pc = payload.pc;
        cpu.ime = payload.ime;
        cpu.ime_delay = payload.ime_delay;
        cpu.halted = payload.halted;
        cpu.halt_bug = payload.halt_bug;
        cpu.double_speed = payload.double_speed;
        cpu.cycles = payload.cycles;
        cpu.next_event = payload.next_event;

        vram_bank0_ = payload.vram_bank0;
        vram_bank1_ = payload.vram_bank1;
        wram_bank0_ = payload.wram_bank0;
        wram_banks_ = payload.wram_banks;
        oam_ = payload.oam;
        hram_ = payload.hram;
        if (payload.sram_size > 0) {
            sram_.resize(payload.sram_size);
            std::memcpy(sram_.data(), buffer.data() + sizeof(hdr) + sizeof(payload), payload.sram_size);
        }

        ram_enabled_ = payload.ram_enabled;
        rom_bank_ = payload.rom_bank;
        ram_bank_ = payload.ram_bank;
        mbc1_mode_ = payload.mbc1_mode;

        rtc_s_ = payload.rtc_s;
        rtc_m_ = payload.rtc_m;
        rtc_h_ = payload.rtc_h;
        rtc_dl_ = payload.rtc_dl;
        rtc_dh_ = payload.rtc_dh;
        rtc_select_ = payload.rtc_select;
        rtc_latch_prev_ = payload.rtc_latch_prev;
        latched_rtc_s_ = payload.latched_rtc_s;
        latched_rtc_m_ = payload.latched_rtc_m;
        latched_rtc_h_ = payload.latched_rtc_h;
        latched_rtc_dl_ = payload.latched_rtc_dl;
        latched_rtc_dh_ = payload.latched_rtc_dh;
        rtc_cycles_ = payload.rtc_cycles;

        div_counter_ = payload.div_counter;
        tima_ = payload.tima;
        tma_ = payload.tma;
        tac_ = payload.tac;
        if_reg_ = payload.if_reg;
        ie_reg_ = payload.ie_reg;
        joypad_state_ = payload.joypad_state;

        vram_bank_idx_ = payload.vram_bank_idx;
        wram_bank_idx_ = payload.wram_bank_idx;
        key1_ = payload.key1;

        is_cgb_ = payload.is_cgb;
        hdma_src_ = payload.hdma_src;
        hdma_dst_ = payload.hdma_dst;
        hdma5_ = payload.hdma5;
        hdma_active_ = payload.hdma_active;

        ppu_.set_cgb(is_cgb_);
        ppu_.load_snapshot(payload.ppu);
        apu_.load_snapshot(payload.apu);

        return true;
    } else if (hdr.is_valid("GB  ", 1)) {
        if (buffer.size() < sizeof(hdr) + sizeof(GbStatePayloadV1)) {
            return false;
        }
        GbStatePayloadV1 payload;
        std::memcpy(&payload, buffer.data() + sizeof(hdr), sizeof(payload));
        if (buffer.size() < sizeof(hdr) + sizeof(payload) + payload.sram_size) {
            return false;
        }

        cpu.a = payload.a;
        cpu.f = payload.f;
        cpu.b = payload.b;
        cpu.c = payload.c;
        cpu.d = payload.d;
        cpu.e = payload.e;
        cpu.h = payload.h;
        cpu.l = payload.l;
        cpu.sp = payload.sp;
        cpu.pc = payload.pc;
        cpu.ime = payload.ime;
        cpu.ime_delay = payload.ime_delay;
        cpu.halted = payload.halted;
        cpu.halt_bug = payload.halt_bug;
        cpu.double_speed = payload.double_speed;
        cpu.cycles = payload.cycles;
        cpu.next_event = payload.next_event;

        vram_bank0_ = payload.vram_bank0;
        vram_bank1_ = payload.vram_bank1;
        wram_bank0_ = payload.wram_bank0;
        wram_banks_ = payload.wram_banks;
        oam_ = payload.oam;
        hram_ = payload.hram;
        if (payload.sram_size > 0) {
            sram_.resize(payload.sram_size);
            std::memcpy(sram_.data(), buffer.data() + sizeof(hdr) + sizeof(payload), payload.sram_size);
        }

        ram_enabled_ = payload.ram_enabled;
        rom_bank_ = payload.rom_bank;
        ram_bank_ = payload.ram_bank;
        mbc1_mode_ = payload.mbc1_mode;

        rtc_s_ = payload.rtc_s;
        rtc_m_ = payload.rtc_m;
        rtc_h_ = payload.rtc_h;
        rtc_dl_ = payload.rtc_dl;
        rtc_dh_ = payload.rtc_dh;
        rtc_select_ = payload.rtc_select;
        rtc_latch_prev_ = payload.rtc_latch_prev;
        latched_rtc_s_ = payload.latched_rtc_s;
        latched_rtc_m_ = payload.latched_rtc_m;
        latched_rtc_h_ = payload.latched_rtc_h;
        latched_rtc_dl_ = payload.latched_rtc_dl;

        div_counter_ = payload.div_counter;
        tima_ = payload.tima;
        tma_ = payload.tma;
        tac_ = payload.tac;
        if_reg_ = payload.if_reg;
        ie_reg_ = payload.ie_reg;
        joypad_state_ = payload.joypad_state;

        vram_bank_idx_ = payload.vram_bank_idx;
        wram_bank_idx_ = payload.wram_bank_idx;
        key1_ = payload.key1;

        ppu_.load_snapshot(payload.ppu);
        apu_.load_snapshot(payload.apu);

        return true;
    }

    return false;
}

}  // namespace dragon::gb
