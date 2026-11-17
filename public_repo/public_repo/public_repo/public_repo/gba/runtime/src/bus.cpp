#include "gba/bus.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

#include "dragon/save_state.h"

namespace dragon::gba {

namespace {

void write_u32_le(uint8_t* dst, uint32_t val) noexcept {
    dst[0] = static_cast<uint8_t>(val & 0xFFU);
    dst[1] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
    dst[2] = static_cast<uint8_t>((val >> 16U) & 0xFFU);
    dst[3] = static_cast<uint8_t>((val >> 24U) & 0xFFU);
}

uint16_t read_u16_le(const uint8_t* src) noexcept {
    return static_cast<uint16_t>(src[0] | (static_cast<uint16_t>(src[1]) << 8U));
}

uint32_t read_u32_le(const uint8_t* src) noexcept {
    return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8U) |
           (static_cast<uint32_t>(src[2]) << 16U) | (static_cast<uint32_t>(src[3]) << 24U);
}

}  // namespace

Bus::Bus() {
    reset();
}

void Bus::init_bios() noexcept {
    bios_.fill(0U);
    // Hardware exception vectors & standard BIOS IRQ wrapper ($00000128 - $0000013C)
    write_u32_le(&bios_[0x000], 0xEA000018U);  // Reset: B $0068
    write_u32_le(&bios_[0x008], 0xEA00004CU);  // SWI: B $0140
    write_u32_le(&bios_[0x018], 0xEA000042U);  // IRQ: B $0128

    // BIOS IRQ entry/exit sequence (see GBATEK BIOS Interrupt Handling)
    write_u32_le(&bios_[0x128], 0xE92D500FU);  // STMFD SP!, {R0-R3, R12, LR}
    write_u32_le(&bios_[0x12C], 0xE3A00301U);  // MOV R0, #0x04000000
    write_u32_le(&bios_[0x130], 0xE28FE000U);  // ADD LR, PC, #0 (-> $0138)
    write_u32_le(&bios_[0x134], 0xE510F004U);  // LDR PC, [R0, #-4] ([03FFFFFC])
    write_u32_le(&bios_[0x138], 0xE8BD500FU);  // LDMFD SP!, {R0-R3, R12, LR}
    write_u32_le(&bios_[0x13C], 0xE25EF004U);  // SUBS PC, LR, #4

    // BIOS SWI return stub
    write_u32_le(&bios_[0x140], 0xE1B0F00EU);  // MOVS PC, LR
}

void Bus::reset() {
    init_bios();
    ewram_.fill(0U);
    iwram_.fill(0U);
    save_ram_.fill(0xFFU);
    eeprom_.fill(0xFFU);

    flash_state_ = FlashState::Ready;
    flash_id_mode_ = false;
    flash_bank_ = 0U;

    eeprom_bit_buffer_.clear();
    eeprom_read_address_ = 0U;
    eeprom_read_bits_remaining_ = 0U;
    eeprom_addr_bits_ = 6U;

    ppu_.reset();
    apu_.reset();

    dma_.fill(DmaChannel{});
    timers_.fill(Timer{});

    keyinput_ = 0x03FFU;
    keycnt_ = 0U;
    ie_ = 0U;
    if_ = 0U;
    ime_ = 0U;
    waitcnt_ = 0U;
    postflg_ = 0U;
    haltcnt_ = 0U;
    acknowledged_irqs_ = 0U;
    last_sync_cycles_ = 0ULL;
    last_bios_read_ = 0xE129F000U;
    logged_fallbacks_.clear();
}

void Bus::load_cartridge(const Cartridge& cart) {
    reset();
    rom_ = cart.rom;
    save_type_ = cart.save_type;
}

void Bus::set_keypad(uint16_t pressed_mask) noexcept {
    keyinput_ = static_cast<uint16_t>((~pressed_mask) & 0x03FFU);
    if (keycnt_ & 0x4000U) {
        const uint16_t mask = keycnt_ & 0x03FFU;
        if (keycnt_ & 0x8000U) {  // Logical AND
            if ((pressed_mask & mask) == mask && mask != 0U) {
                if_ |= kIrqKeypad;
            }
        } else {  // Logical OR
            if ((pressed_mask & mask) != 0U) {
                if_ |= kIrqKeypad;
            }
        }
    }
}

uint8_t Bus::read_backup8(uint32_t addr) const noexcept {
    const uint32_t offset = addr & 0xFFFFU;
    if (save_type_ == SaveType::Flash64K || save_type_ == SaveType::Flash128K) {
        if (flash_id_mode_) {
            // Sanyo LE26FV10N1TS (128K: 0x1362) or Macronix MX29L512 (64K: 0x1CC2)
            if (offset == 0x0000U) {
                return (save_type_ == SaveType::Flash128K) ? 0x62U : 0xC2U;
            }
            if (offset == 0x0001U) {
                return (save_type_ == SaveType::Flash128K) ? 0x13U : 0x1CU;
            }
        }
        const uint32_t bank_offset = (save_type_ == SaveType::Flash128K)
                                         ? (static_cast<uint32_t>(flash_bank_) * 0x10000U)
                                         : 0U;
        return save_ram_[bank_offset + offset];
    }
    return save_ram_[offset];
}

void Bus::write_backup8(uint32_t addr, uint8_t value) noexcept {
    const uint32_t offset = addr & 0xFFFFU;
    if (save_type_ == SaveType::Sram || save_type_ == SaveType::None) {
        save_ram_[offset] = value;
        return;
    }

    if (save_type_ == SaveType::Flash64K || save_type_ == SaveType::Flash128K) {
        switch (flash_state_) {
            case FlashState::Ready:
                if (offset == 0x5555U && value == 0xAAU) {
                    flash_state_ = FlashState::Cmd1;
                }
                break;
            case FlashState::Cmd1:
                if (offset == 0x2AAAU && value == 0x55U) {
                    flash_state_ = FlashState::Cmd2;
                } else {
                    flash_state_ = FlashState::Ready;
                }
                break;
            case FlashState::Cmd2:
                if (offset == 0x5555U) {
                    if (value == 0x90U) {
                        flash_id_mode_ = true;
                        flash_state_ = FlashState::Ready;
                    } else if (value == 0xF0U) {
                        flash_id_mode_ = false;
                        flash_state_ = FlashState::Ready;
                    } else if (value == 0x80U) {
                        flash_state_ = FlashState::Erase1;
                    } else if (value == 0xA0U) {
                        flash_state_ = FlashState::WriteByte;
                    } else if (value == 0xB0U && save_type_ == SaveType::Flash128K) {
                        flash_state_ = FlashState::BankSwitch;
                    } else {
                        flash_state_ = FlashState::Ready;
                    }
                } else {
                    flash_state_ = FlashState::Ready;
                }
                break;
            case FlashState::Erase1:
                if (offset == 0x5555U && value == 0xAAU) {
                    flash_state_ = FlashState::Erase2;
                } else {
                    flash_state_ = FlashState::Ready;
                }
                break;
            case FlashState::Erase2:
                if (offset == 0x2AAAU && value == 0x55U) {
                    // Wait for final erase command (0x10 chip erase or 0x30 sector erase)
                } else if (offset == 0x5555U && value == 0x10U) {
                    save_ram_.fill(0xFFU);
                    flash_state_ = FlashState::Ready;
                } else if (value == 0x30U) {
                    const uint32_t bank_base = (save_type_ == SaveType::Flash128K)
                                                   ? (static_cast<uint32_t>(flash_bank_) * 0x10000U)
                                                   : 0U;
                    const uint32_t sector_base = bank_base + (offset & 0xF000U);
                    std::fill_n(&save_ram_[sector_base], 0x1000U, 0xFFU);
                    flash_state_ = FlashState::Ready;
                } else {
                    flash_state_ = FlashState::Ready;
                }
                break;
            case FlashState::WriteByte: {
                const uint32_t bank_base = (save_type_ == SaveType::Flash128K)
                                               ? (static_cast<uint32_t>(flash_bank_) * 0x10000U)
                                               : 0U;
                save_ram_[bank_base + offset] &= value;
                flash_state_ = FlashState::Ready;
                break;
            }
            case FlashState::BankSwitch:
                if (offset == 0x0000U) {
                    flash_bank_ = value & 1U;
                }
                flash_state_ = FlashState::Ready;
                break;
        }
    }
}

uint16_t Bus::read_eeprom16() noexcept {
    if (eeprom_read_bits_remaining_ == 0U) {
        return 1U;  // Ready bit = 1
    }
    --eeprom_read_bits_remaining_;
    if (eeprom_read_bits_remaining_ >= 64U) {
        return 0U;  // 4 dummy bits before 64 data bits
    }
    const uint32_t bit_idx = 63U - eeprom_read_bits_remaining_;
    const uint32_t byte_offset = (eeprom_read_address_ * 8U + (bit_idx >> 3U)) & 0x1FFFU;
    const uint8_t bit_shift = 7U - (bit_idx & 7U);
    return static_cast<uint16_t>((eeprom_[byte_offset] >> bit_shift) & 1U);
}

void Bus::write_eeprom16(uint16_t value) noexcept {
    eeprom_bit_buffer_.push_back(static_cast<uint8_t>(value & 1U));
}

// --- GBA Cart GPIO + RTC (Seiko S-3511A) -----------------------------------
// See bus.h for the GBATEK/mGBA references this follows.

uint16_t Bus::gpio_read16(uint32_t offset) noexcept {
    if ((gpio_control_ & 1U) == 0U) {
        // Readback disabled: 0x080000C4-0x080000C9 is just ordinary ROM data
        // (it sits right after the 192-byte cart header, so real code/data
        // can legitimately live there when a game never touches GPIO, or
        // between the RTC-aware game's own GPIO accesses).
        return (offset + 1U < rom_.size()) ? read_u16_le(&rom_[offset]) : 0U;
    }
    switch (offset) {
        case 0xC4U:
            return gpio_data_read();
        case 0xC6U:
            return gpio_direction_;
        case 0xC8U:
            return gpio_control_;
        default:
            return 0U;
    }
}

void Bus::gpio_write16(uint32_t offset, uint16_t value) noexcept {
    // Direction/Control latch writes regardless of the Control readback
    // bit (only reads are gated); Data additionally drives the RTC state
    // machine below.
    switch (offset) {
        case 0xC4U:
            gpio_set_data(static_cast<uint16_t>(value & 0x0FU));
            break;
        case 0xC6U:
            gpio_direction_ = static_cast<uint16_t>(value & 0x0FU);
            break;
        case 0xC8U:
            gpio_control_ = static_cast<uint16_t>(value & 0x01U);
            break;
        default:
            break;
    }
}

uint16_t Bus::gpio_data_read() const noexcept {
    // Bits configured as output (Direction=1) read back the game's own
    // latch; bits configured as input (Direction=0) read back whatever the
    // external device (the RTC chip, for SIO/bit1) is currently driving.
    uint16_t value = gpio_data_;
    if ((gpio_direction_ & 0x02U) == 0U) {
        value = static_cast<uint16_t>((value & ~0x02U) | (rtc_sio_out_ ? 0x02U : 0U));
    }
    return value;
}

void Bus::gpio_set_data(uint16_t new_value) noexcept {
    const bool old_cs = (gpio_data_ & 0x04U) != 0U;
    const bool old_sck = (gpio_data_ & 0x01U) != 0U;
    const bool new_cs = (new_value & 0x04U) != 0U;
    const bool new_sck = (new_value & 0x01U) != 0U;
    const bool sio_bit = (new_value & 0x02U) != 0U;

    gpio_data_ = new_value;

    if (!old_cs && new_cs) {
        rtc_begin_transfer();
    } else if (old_cs && !new_cs) {
        rtc_phase_ = RtcPhase::Idle;
    }

    if (new_cs) {
        if (!old_sck && new_sck) {
            rtc_clock_write_bit(sio_bit);
        } else if (old_sck && !new_sck) {
            rtc_clock_read_bit();
        }
    }
}

void Bus::rtc_begin_transfer() noexcept {
    rtc_phase_ = RtcPhase::Command;
    rtc_bits_read_ = 0U;
    rtc_shift_reg_ = 0U;
}

void Bus::rtc_decode_command(uint8_t command_byte) noexcept {
    rtc_bits_read_ = 0U;
    rtc_shift_reg_ = 0U;

    const uint8_t magic = static_cast<uint8_t>((command_byte >> 4U) & 0x0FU);
    if (magic != 0x06U) {
        // Not a valid S-3511A command frame (0110 RRR W); ignore until the
        // next CS pulse rather than guessing.
        rtc_phase_ = RtcPhase::Idle;
        return;
    }

    rtc_command_reg_ = static_cast<uint8_t>((command_byte >> 1U) & 0x07U);
    const bool is_read = (command_byte & 0x01U) != 0U;

    static constexpr uint8_t kBytesForRegister[8] = {
        0U,  // 0: Reset
        0U,  // 1: unused
        7U,  // 2: Date/Time
        0U,  // 3: Force IRQ (unimplemented, no data phase)
        1U,  // 4: Control / Status
        0U,  // 5: unused
        3U,  // 6: Time
        0U,  // 7: unused
    };
    rtc_bytes_remaining_ = kBytesForRegister[rtc_command_reg_];
    rtc_byte_index_ = 0U;

    if (rtc_command_reg_ == 0U) {
        // Reset: clear Control, including the power-off/time-lost flag, so
        // the game sees a healthy clock instead of the "internal battery is
        // depleted" warning.
        rtc_control_ = 0U;
    }
    if (rtc_command_reg_ == 2U || rtc_command_reg_ == 6U) {
        rtc_latch_current_time();
    }

    if (rtc_bytes_remaining_ == 0U) {
        rtc_phase_ = RtcPhase::Idle;
        return;
    }

    if (is_read) {
        rtc_phase_ = RtcPhase::TransferRead;
        rtc_prepare_read_byte();
    } else {
        rtc_phase_ = RtcPhase::TransferWrite;
    }
}

void Bus::rtc_clock_write_bit(bool bit) noexcept {
    if (rtc_phase_ == RtcPhase::Command) {
        rtc_shift_reg_ = static_cast<uint8_t>(rtc_shift_reg_ | (bit ? (1U << rtc_bits_read_) : 0U));
        ++rtc_bits_read_;
        if (rtc_bits_read_ == 8U) {
            rtc_decode_command(rtc_shift_reg_);
        }
        return;
    }
    if (rtc_phase_ != RtcPhase::TransferWrite) {
        return;
    }
    rtc_shift_reg_ = static_cast<uint8_t>(rtc_shift_reg_ | (bit ? (1U << rtc_bits_read_) : 0U));
    ++rtc_bits_read_;
    if (rtc_bits_read_ == 8U) {
        rtc_consume_written_byte(rtc_shift_reg_);
        rtc_shift_reg_ = 0U;
        rtc_bits_read_ = 0U;
        if (rtc_bytes_remaining_ > 0U) {
            --rtc_bytes_remaining_;
        }
        if (rtc_bytes_remaining_ == 0U) {
            rtc_phase_ = RtcPhase::Idle;
        }
    }
}

void Bus::rtc_clock_read_bit() noexcept {
    if (rtc_phase_ != RtcPhase::TransferRead) {
        return;
    }
    ++rtc_bits_read_;
    if (rtc_bits_read_ == 8U) {
        rtc_bits_read_ = 0U;
        ++rtc_byte_index_;
        if (rtc_bytes_remaining_ > 0U) {
            --rtc_bytes_remaining_;
        }
        if (rtc_bytes_remaining_ == 0U) {
            rtc_phase_ = RtcPhase::Idle;
            return;
        }
        rtc_prepare_read_byte();
        return;
    }
    rtc_sio_out_ = ((rtc_shift_reg_ >> rtc_bits_read_) & 1U) != 0U;
}

void Bus::rtc_prepare_read_byte() noexcept {
    rtc_shift_reg_ = rtc_current_register_byte(rtc_byte_index_);
    rtc_bits_read_ = 0U;
    rtc_sio_out_ = (rtc_shift_reg_ & 1U) != 0U;
}

uint8_t Bus::rtc_current_register_byte(uint8_t index) const noexcept {
    switch (rtc_command_reg_) {
        case 2U:  // Date/Time: year,month,day,dow,hour,min,sec
            return (index < rtc_datetime_.size()) ? rtc_datetime_[index] : 0U;
        case 4U:  // Control / Status
            return rtc_control_;
        case 6U:  // Time: hour,min,sec (bytes 4..6 of the Date/Time layout)
            return (index < 3U) ? rtc_datetime_[4U + index] : 0U;
        default:
            return 0U;
    }
}

void Bus::rtc_consume_written_byte(uint8_t byte_value) noexcept {
    if (rtc_command_reg_ == 4U) {
        // Bit 7 (power-off/time-lost) is read-only on the real chip, cleared
        // by Reset; a game write to it has no effect here either.
        rtc_control_ = static_cast<uint8_t>(byte_value & 0x7FU);
    }
    // Date/Time and Time writes are accepted on the wire but not applied:
    // the clock always tracks the host's wall time instead (matches how
    // every mainstream GBA emulator treats this, since there is no reason to
    // let a game set the recompiled game's clock away from real time).
}

void Bus::rtc_latch_current_time() noexcept {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif

    const auto to_bcd = [](int v) -> uint8_t {
        v = v % 100;
        if (v < 0) {
            v += 100;
        }
        return static_cast<uint8_t>(((v / 10) << 4U) | (v % 10));
    };

    rtc_datetime_[0] = to_bcd(tm_buf.tm_year + 1900 - 2000);  // 00h-99h = 2000-2099
    rtc_datetime_[1] = to_bcd(tm_buf.tm_mon + 1);
    rtc_datetime_[2] = to_bcd(tm_buf.tm_mday);
    rtc_datetime_[3] = to_bcd(tm_buf.tm_wday);  // 0=Sunday..6=Saturday, matches tm_wday

    const bool hour24 = (rtc_control_ & 0x40U) != 0U;  // bit6: 1=24h, 0=12h
    int hour = tm_buf.tm_hour;
    if (!hour24) {
        hour = hour % 12;
        if (hour == 0) {
            hour = 12;
        }
    }
    rtc_datetime_[4] = to_bcd(hour);
    rtc_datetime_[5] = to_bcd(tm_buf.tm_min);
    rtc_datetime_[6] = to_bcd(tm_buf.tm_sec);
}

uint8_t Bus::read_io8(uint32_t offset) noexcept {
    const uint16_t hw = read_io16(offset & ~1U);
    return (offset & 1U) ? static_cast<uint8_t>((hw >> 8U) & 0xFFU)
                         : static_cast<uint8_t>(hw & 0xFFU);
}

uint16_t Bus::read_io16(uint32_t offset) noexcept {
    offset &= 0xFFEU;
    if (offset < 0x060U) {
        return ppu_.read_io16(offset);
    }
    if (offset >= 0x060U && offset < 0x0B0U) {
        return apu_.read_io16(offset);
    }
    if (offset >= 0x0B0U && offset < 0x0E0U) {
        const uint32_t ch = (offset - 0x0B0U) / 12U;
        const uint32_t reg = (offset - 0x0B0U) % 12U;
        if (reg == 8U) {
            return 0U;  // DMAxCNT_L is write-only
        }
        if (reg == 10U) {
            return dma_[ch].cnt_h;
        }
        return 0U;
    }
    if (offset >= 0x100U && offset < 0x110U) {
        const uint32_t tm = (offset - 0x100U) >> 2U;
        if ((offset & 2U) == 0U) {
            return timers_[tm].counter;
        }
        return timers_[tm].cnt_h;
    }

    switch (offset) {
        case 0x130:
            return keyinput_;
        case 0x132:
            return keycnt_;
        case 0x200:
            return ie_;
        case 0x202:
            return if_;
        case 0x204:
            return waitcnt_;
        case 0x208:
            return ime_;
        case 0x300:
            return postflg_;
        default:
            return 0U;
    }
}

void Bus::write_io8(uint32_t offset, uint8_t value) noexcept {
    if (offset == 0x300U) {
        postflg_ = value;
        return;
    }
    if (offset == 0x301U) {
        haltcnt_ = value;
        return;
    }
    if (offset >= 0x060U && offset < 0x0B0U) {
        apu_.write_io8(offset, value);
        return;
    }
    if (offset == 0x202U) {
        write_io16(0x202U, static_cast<uint16_t>(value));
        return;
    }
    if (offset == 0x203U) {
        write_io16(0x202U, static_cast<uint16_t>(value) << 8U);
        return;
    }
    const uint32_t aligned = offset & ~1U;
    uint16_t cur = 0U;
    if (aligned >= 0x0B0U && aligned < 0x0E0U) {
        const uint32_t ch = (aligned - 0x0B0U) / 12U;
        const uint32_t reg = (aligned - 0x0B0U) % 12U;
        const auto& d = dma_[ch];
        switch (reg) {
            case 0:
                cur = static_cast<uint16_t>(d.sad & 0xFFFFU);
                break;
            case 2:
                cur = static_cast<uint16_t>((d.sad >> 16U) & 0xFFFFU);
                break;
            case 4:
                cur = static_cast<uint16_t>(d.dad & 0xFFFFU);
                break;
            case 6:
                cur = static_cast<uint16_t>((d.dad >> 16U) & 0xFFFFU);
                break;
            case 8:
                cur = d.cnt_l;
                break;
            case 10:
                cur = d.cnt_h;
                break;
            default:
                break;
        }
    } else if (aligned >= 0x100U && aligned < 0x110U && (aligned & 2U) == 0U) {
        const uint32_t tm = (aligned - 0x100U) >> 2U;
        cur = timers_[tm].reload;
    } else {
        cur = read_io16(aligned);
    }
    if (offset & 1U) {
        cur = static_cast<uint16_t>((cur & 0x00FFU) | (static_cast<uint16_t>(value) << 8U));
    } else {
        cur = static_cast<uint16_t>((cur & 0xFF00U) | value);
    }
    write_io16(aligned, cur);
}

void Bus::write_io16(uint32_t offset, uint16_t value) noexcept {
    offset &= 0xFFEU;
    if (offset < 0x060U) {
        ppu_.write_io16(offset, value);
        return;
    }
    if (offset >= 0x060U && offset < 0x0B0U) {
        apu_.write_io16(offset, value);
        return;
    }
    if (offset >= 0x0B0U && offset < 0x0E0U) {
        const uint32_t ch = (offset - 0x0B0U) / 12U;
        const uint32_t reg = (offset - 0x0B0U) % 12U;
        auto& d = dma_[ch];
        switch (reg) {
            case 0:
                d.sad = (d.sad & 0xFFFF0000U) | value;
                break;
            case 2:
                d.sad = (d.sad & 0x0000FFFFU) | (static_cast<uint32_t>(value) << 16U);
                break;
            case 4:
                d.dad = (d.dad & 0xFFFF0000U) | value;
                break;
            case 6:
                d.dad = (d.dad & 0x0000FFFFU) | (static_cast<uint32_t>(value) << 16U);
                break;
            case 8:
                d.cnt_l = value;
                break;
            case 10: {
                const bool was_enabled = (d.cnt_h & 0x8000U) != 0U;
                d.cnt_h = value;
                const bool now_enabled = (value & 0x8000U) != 0U;
                if (!was_enabled && now_enabled) {
                    d.internal_src = d.sad;
                    d.internal_dst = d.dad;
                    d.internal_count = d.cnt_l;
                    const uint8_t timing = (value >> 12U) & 3U;
                    if (timing == 0U) {
                        execute_dma_channel(static_cast<int>(ch));
                    }
                }
                break;
            }
            default:
                break;
        }
        return;
    }
    if (offset >= 0x100U && offset < 0x110U) {
        const uint32_t tm = (offset - 0x100U) >> 2U;
        auto& t = timers_[tm];
        if ((offset & 2U) == 0U) {
            t.reload = value;
        } else {
            const bool was_enabled = (t.cnt_h & 0x0080U) != 0U;
            t.cnt_h = value;
            const bool now_enabled = (value & 0x0080U) != 0U;
            if (!was_enabled && now_enabled) {
                t.counter = t.reload;
                t.prescale_accum = 0U;
            }
        }
        return;
    }

    switch (offset) {
        case 0x132:
            keycnt_ = value;
            break;
        case 0x200:
            ie_ = value;
            break;
        case 0x202:
            // Writing 1s to IF acknowledges (clears) those interrupt request flags
            if_ &= static_cast<uint16_t>(~value);
            acknowledged_irqs_ |= value;
            break;
        case 0x204:
            waitcnt_ = value;
            break;
        case 0x208:
            ime_ = value & 1U;
            break;
        case 0x300:
            postflg_ = static_cast<uint8_t>(value & 0xFFU);
            haltcnt_ = static_cast<uint8_t>((value >> 8U) & 0xFFU);
            break;
        default:
            break;
    }
}

uint8_t Bus::read8(uint32_t addr) noexcept {
    const uint32_t region = (addr >> 24U) & 0xFFU;
    switch (region) {
        case 0x00:
            if (addr < 0x4000U) {
                return bios_[addr];
            }
            return static_cast<uint8_t>((last_bios_read_ >> ((addr & 3U) * 8U)) & 0xFFU);
        case 0x02:
            return ewram_[addr & 0x3FFFFU];
        case 0x03:
            return iwram_[addr & 0x7FFFU];
        case 0x04:
            if ((addr & 0xFFFFFFU) < 0x400U) {
                return read_io8(addr & 0x3FFU);
            }
            return 0U;
        case 0x05:
            return ppu_.read_palette8(addr & 0x3FFU);
        case 0x06:
            return ppu_.read_vram8(addr & 0x1FFFFU);
        case 0x07:
            return ppu_.read_oam8(addr & 0x3FFU);
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D: {
            const uint32_t offset = addr & 0x01FFFFFFU;
            if (region == 0x08U && gpio_region(offset & ~1U)) {
                const uint16_t half = gpio_read16(offset & ~1U);
                return (offset & 1U) ? static_cast<uint8_t>((half >> 8U) & 0xFFU)
                                     : static_cast<uint8_t>(half & 0xFFU);
            }
            if (offset < rom_.size()) {
                return rom_[offset];
            }
            // GBA ROM out-of-bounds returns open-bus prefetch halfword ((addr >> 1) & 0xFFFF)
            const uint16_t ob = static_cast<uint16_t>((addr >> 1U) & 0xFFFFU);
            return (addr & 1U) ? static_cast<uint8_t>((ob >> 8U) & 0xFFU)
                               : static_cast<uint8_t>(ob & 0xFFU);
        }
        case 0x0E:
        case 0x0F:
            return read_backup8(addr);
        default:
            return 0U;
    }
}

uint16_t Bus::read16(uint32_t addr) noexcept {
    addr &= ~1U;
    const uint32_t region = (addr >> 24U) & 0xFFU;
    switch (region) {
        case 0x00:
            if (addr < 0x4000U) {
                return read_u16_le(&bios_[addr]);
            }
            return static_cast<uint16_t>((last_bios_read_ >> ((addr & 2U) * 8U)) & 0xFFFFU);
        case 0x02:
            return read_u16_le(&ewram_[addr & 0x3FFFEU]);
        case 0x03:
            return read_u16_le(&iwram_[addr & 0x7FFEU]);
        case 0x04:
            if ((addr & 0xFFFFFFU) < 0x400U) {
                return read_io16(addr & 0x3FEU);
            }
            return 0U;
        case 0x05:
            return ppu_.read_palette16(addr & 0x3FEU);
        case 0x06:
            return ppu_.read_vram16(addr & 0x1FFFEU);
        case 0x07:
            return ppu_.read_oam16(addr & 0x3FEU);
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D: {
            if (region == 0x0DU && save_type_ == SaveType::Eeprom &&
                (addr >= 0x0DFFFF00U || rom_.size() <= 0x01000000U)) {
                const uint32_t offset = addr & 0x01FFFFFFU;
                if (offset >= rom_.size()) {
                    return read_eeprom16();
                }
            }
            const uint32_t offset = addr & 0x01FFFFFEU;
            if (region == 0x08U && gpio_region(offset)) {
                return gpio_read16(offset);
            }
            if (offset + 1U < rom_.size()) {
                return read_u16_le(&rom_[offset]);
            }
            return static_cast<uint16_t>((addr >> 1U) & 0xFFFFU);
        }
        case 0x0E:
        case 0x0F: {
            const uint8_t b = read_backup8(addr);
            return static_cast<uint16_t>(b | (static_cast<uint16_t>(b) << 8U));
        }
        default:
            return 0U;
    }
}

uint32_t Bus::read32(uint32_t addr) noexcept {
    addr &= ~3U;
    const uint32_t region = (addr >> 24U) & 0xFFU;
    switch (region) {
        case 0x00:
            if (addr < 0x4000U) {
                last_bios_read_ = read_u32_le(&bios_[addr]);
                return last_bios_read_;
            }
            return last_bios_read_;
        case 0x02:
            return read_u32_le(&ewram_[addr & 0x3FFFCU]);
        case 0x03:
            return read_u32_le(&iwram_[addr & 0x7FFCU]);
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D: {
            const uint32_t offset = addr & 0x01FFFFFCU;
            const bool is_gpio_word =
                region == 0x08U && (gpio_region(offset) || gpio_region(offset + 2U));
            if (!is_gpio_word && offset + 3U < rom_.size()) {
                return read_u32_le(&rom_[offset]);
            }
            const uint16_t lo = read16(addr);
            const uint16_t hi = read16(addr + 2U);
            return static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16U);
        }
        default: {
            const uint16_t lo = read16(addr);
            const uint16_t hi = read16(addr + 2U);
            return static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16U);
        }
    }
}

void Bus::write8(uint32_t addr, uint8_t value) noexcept {
    const uint32_t region = (addr >> 24U) & 0xFFU;
    switch (region) {
        case 0x02:
            ewram_[addr & 0x3FFFFU] = value;
            break;
        case 0x03:
            iwram_[addr & 0x7FFFU] = value;
            break;
        case 0x04:
            if ((addr & 0xFFFFFFU) < 0x400U) {
                write_io8(addr & 0x3FFU, value);
            }
            break;
        case 0x05: {
            // 8-bit writes to Palette RAM duplicate the byte across the halfword
            const uint16_t dup =
                static_cast<uint16_t>(value | (static_cast<uint16_t>(value) << 8U));
            ppu_.write_palette16(addr & 0x3FEU, dup);
            break;
        }
        case 0x06: {
            // 8-bit writes to BG VRAM duplicate the byte across the halfword; OBJ VRAM ignores
            // 8-bit writes
            const uint32_t vram_off = addr & 0x1FFFFU;
            const uint32_t bg_limit = ((ppu_.dispcnt() & 7U) >= 3U) ? 0x14000U : 0x10000U;
            if (vram_off < bg_limit) {
                const uint16_t dup =
                    static_cast<uint16_t>(value | (static_cast<uint16_t>(value) << 8U));
                ppu_.write_vram16(vram_off & ~1U, dup);
            }
            break;
        }
        case 0x07:
            // 8-bit writes to OAM are ignored on GBA hardware
            break;
        case 0x08: {
            const uint32_t offset = addr & 0x01FFFFFFU;
            const uint32_t half_offset = offset & ~1U;
            if (gpio_region(half_offset)) {
                // Real GPIO hardware is accessed 16-bit-wide; an 8-bit write
                // updates just its half of the register (read-modify-write).
                uint16_t half = gpio_read16(half_offset);
                if (offset & 1U) {
                    half = static_cast<uint16_t>((half & 0x00FFU) |
                                                 (static_cast<uint16_t>(value) << 8U));
                } else {
                    half = static_cast<uint16_t>((half & 0xFF00U) | value);
                }
                gpio_write16(half_offset, half);
            }
            break;
        }
        case 0x0E:
        case 0x0F:
            write_backup8(addr, value);
            break;
        default:
            break;
    }
}

void Bus::write16(uint32_t addr, uint16_t value) noexcept {
    addr &= ~1U;
    const uint32_t region = (addr >> 24U) & 0xFFU;
    switch (region) {
        case 0x02: {
            const uint32_t off = addr & 0x3FFFEU;
            ewram_[off] = static_cast<uint8_t>(value & 0xFFU);
            ewram_[off + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
            break;
        }
        case 0x03: {
            const uint32_t off = addr & 0x7FFEU;
            iwram_[off] = static_cast<uint8_t>(value & 0xFFU);
            iwram_[off + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
            break;
        }
        case 0x04:
            if ((addr & 0xFFFFFFU) < 0x400U) {
                write_io16(addr & 0x3FEU, value);
            }
            break;
        case 0x05:
            ppu_.write_palette16(addr & 0x3FEU, value);
            break;
        case 0x06:
            ppu_.write_vram16(addr & 0x1FFFEU, value);
            break;
        case 0x07:
            ppu_.write_oam16(addr & 0x3FEU, value);
            break;
        case 0x08: {
            const uint32_t offset = addr & 0x01FFFFFEU;
            if (gpio_region(offset)) {
                gpio_write16(offset, value);
            }
            break;
        }
        case 0x0D:
            if (save_type_ == SaveType::Eeprom) {
                write_eeprom16(value);
            }
            break;
        case 0x0E:
        case 0x0F:
            write_backup8(addr, static_cast<uint8_t>(value & 0xFFU));
            break;
        default:
            break;
    }
}

void Bus::write32(uint32_t addr, uint32_t value) noexcept {
    addr &= ~3U;
    const uint32_t region = (addr >> 24U) & 0xFFU;
    if (region == 0x02U) {
        write_u32_le(&ewram_[addr & 0x3FFFCU], value);
        return;
    }
    if (region == 0x03U) {
        write_u32_le(&iwram_[addr & 0x7FFCU], value);
        return;
    }
    if (addr == 0x040000A0U) {
        apu_.write_fifo32(0, value);
        return;
    }
    if (addr == 0x040000A4U) {
        apu_.write_fifo32(1, value);
        return;
    }
    write16(addr, static_cast<uint16_t>(value & 0xFFFFU));
    write16(addr + 2U, static_cast<uint16_t>((value >> 16U) & 0xFFFFU));
}

void Bus::execute_dma_channel(int ch) noexcept {
    auto& d = dma_[ch];
    const bool is_32bit = (d.cnt_h & 0x0400U) != 0U;
    const uint8_t dst_ctrl = (d.cnt_h >> 5U) & 3U;
    const uint8_t src_ctrl = (d.cnt_h >> 7U) & 3U;
    const uint8_t timing = (d.cnt_h >> 12U) & 3U;

    uint32_t count = d.internal_count;
    if (count == 0U) {
        count = (ch == 3) ? 0x10000U : 0x4000U;
    }

    // Special Sound FIFO DMA (DMA1 / DMA2 with timing == 3) transfers 4 words (16 bytes) fixed to
    // FIFO
    if (timing == 3U && (ch == 1 || ch == 2)) {
        for (int i = 0; i < 4; ++i) {
            const uint32_t word = read32(d.internal_src);
            if (d.internal_dst == 0x040000A0U) {
                apu_.write_fifo32(0, word);
            } else if (d.internal_dst == 0x040000A4U) {
                apu_.write_fifo32(1, word);
            }
            if (src_ctrl == 0U) {
                d.internal_src += 4U;
            } else if (src_ctrl == 1U) {
                d.internal_src -= 4U;
            }
        }
        if (d.cnt_h & 0x4000U) {
            if_ |= static_cast<uint16_t>(kIrqDma0 << ch);
        }
        return;
    }

    const bool is_eeprom_write = (ch == 3) && !is_32bit && ((d.internal_dst >> 24U) == 0x0DU) &&
                                 (d.internal_dst >= 0x0DFFFF00U || rom_.size() <= 0x01000000U);
    if (is_eeprom_write) {
        save_type_ = SaveType::Eeprom;
        eeprom_bit_buffer_.clear();
    }

    const int32_t step = is_32bit ? 4 : 2;
    const int32_t src_delta = (src_ctrl == 0U) ? step : ((src_ctrl == 1U) ? -step : 0);
    const int32_t dst_delta =
        (dst_ctrl == 0U || dst_ctrl == 3U) ? step : ((dst_ctrl == 1U) ? -step : 0);

    for (uint32_t i = 0; i < count; ++i) {
        if (is_32bit) {
            write32(d.internal_dst, read32(d.internal_src));
        } else {
            write16(d.internal_dst, read16(d.internal_src));
        }
        d.internal_src = static_cast<uint32_t>(static_cast<int32_t>(d.internal_src) + src_delta);
        d.internal_dst = static_cast<uint32_t>(static_cast<int32_t>(d.internal_dst) + dst_delta);
    }

    if (is_eeprom_write && eeprom_bit_buffer_.size() >= 9U) {
        // Decode EEPROM bitstream command: [1, 1] = Set read address; [1, 0] = Write 64 bits
        const uint8_t cmd = (eeprom_bit_buffer_[0] << 1U) | eeprom_bit_buffer_[1];
        if (cmd == 3U) {
            const size_t addr_bits =
                eeprom_bit_buffer_.size() - 3U;  // minus 2 cmd bits and 1 stop bit
            eeprom_addr_bits_ = (addr_bits >= 14U) ? 14U : 6U;
            uint32_t addr_val = 0U;
            for (size_t b = 0; b < eeprom_addr_bits_ && (2U + b) < eeprom_bit_buffer_.size(); ++b) {
                addr_val = (addr_val << 1U) | eeprom_bit_buffer_[2U + b];
            }
            eeprom_read_address_ = addr_val & 0x3FFU;
            eeprom_read_bits_remaining_ = 68U;  // 4 dummy bits + 64 data bits
        } else if (cmd == 2U && eeprom_bit_buffer_.size() >= (2U + eeprom_addr_bits_ + 64U)) {
            uint32_t addr_val = 0U;
            for (size_t b = 0; b < eeprom_addr_bits_; ++b) {
                addr_val = (addr_val << 1U) | eeprom_bit_buffer_[2U + b];
            }
            addr_val &= 0x3FFU;
            const size_t data_start = 2U + eeprom_addr_bits_;
            for (size_t byte_i = 0; byte_i < 8U; ++byte_i) {
                uint8_t byte_val = 0U;
                for (size_t bit_i = 0; bit_i < 8U; ++bit_i) {
                    byte_val = static_cast<uint8_t>(
                        (byte_val << 1U) | eeprom_bit_buffer_[data_start + byte_i * 8U + bit_i]);
                }
                eeprom_[(addr_val * 8U + byte_i) & 0x1FFFU] = byte_val;
            }
        }
        eeprom_bit_buffer_.clear();
    }

    if (d.cnt_h & 0x4000U) {
        if_ |= static_cast<uint16_t>(kIrqDma0 << ch);
    }

    if ((d.cnt_h & 0x0200U) && timing != 0U) {
        d.internal_count = d.cnt_l;
        if (dst_ctrl == 3U) {
            d.internal_dst = d.dad;
        }
    } else {
        d.cnt_h &= ~0x8000U;
    }
}

void Bus::check_and_run_dma(uint8_t timing_mode, uint8_t fifo_req_mask) noexcept {
    for (int ch = 0; ch < 4; ++ch) {
        const auto& d = dma_[ch];
        if ((d.cnt_h & 0x8000U) == 0U) {
            continue;
        }
        const uint8_t ch_timing = (d.cnt_h >> 12U) & 3U;
        if (ch_timing != timing_mode) {
            continue;
        }
        if (timing_mode == 3U) {
            if (ch == 1 || ch == 2) {
                if ((d.internal_dst == 0x040000A0U && (fifo_req_mask & 1U)) ||
                    (d.internal_dst == 0x040000A4U && (fifo_req_mask & 2U))) {
                    execute_dma_channel(ch);
                }
            }
        } else {
            execute_dma_channel(ch);
        }
    }
}

void Bus::trigger_timer_overflow(int idx) noexcept {
    auto& t = timers_[idx];
    t.counter = t.reload;
    if (t.cnt_h & 0x0040U) {
        if_ |= static_cast<uint16_t>(kIrqTimer0 << idx);
    }

    if (idx == 0 || idx == 1) {
        const uint8_t fifo_req = apu_.on_timer_overflow(idx);
        if (fifo_req != 0U) {
            check_and_run_dma(3U, fifo_req);
        }
    }

    // Cascade to next timer if next timer is enabled and has count-up (bit 2) set
    if (idx < 3) {
        auto& next_t = timers_[idx + 1];
        if ((next_t.cnt_h & 0x0084U) == 0x0084U) {
            if (next_t.counter == 0xFFFFU) {
                trigger_timer_overflow(idx + 1);
            } else {
                ++next_t.counter;
            }
        }
    }
}

void Bus::step_timers(uint32_t cycles) noexcept {
    static constexpr uint32_t kPrescaleShifts[4] = {0U, 6U, 8U, 10U};  // 1, 64, 256, 1024
    for (int i = 0; i < 4; ++i) {
        auto& t = timers_[i];
        // Skip disabled or cascade-driven timers
        if ((t.cnt_h & 0x0080U) == 0U || (t.cnt_h & 0x0004U) != 0U) {
            continue;
        }
        const uint32_t shift = kPrescaleShifts[t.cnt_h & 3U];
        t.prescale_accum += cycles;
        uint32_t ticks = t.prescale_accum >> shift;
        t.prescale_accum &= (1U << shift) - 1U;

        while (ticks > 0U) {
            const uint32_t to_overflow = 0x10000U - t.counter;
            if (ticks >= to_overflow) {
                ticks -= to_overflow;
                trigger_timer_overflow(i);
            } else {
                t.counter = static_cast<uint16_t>(t.counter + ticks);
                ticks = 0U;
            }
        }
    }
}

void Bus::sync_hardware(uint64_t cpu_cycles) noexcept {
    if (cpu_cycles <= last_sync_cycles_) {
        return;
    }
    uint32_t delta = static_cast<uint32_t>(cpu_cycles - last_sync_cycles_);
    last_sync_cycles_ = cpu_cycles;

    // Step in fine-grained chunks so timers, DirectSound FIFO DMA, and PPU stay tightly interleaved
    while (delta > 0U) {
        const uint32_t step_c = std::min<uint32_t>(delta, 64U);
        delta -= step_c;

        step_timers(step_c);
        const Ppu::StepEvents pev = ppu_.step(step_c);
        if (pev.irq_flags != 0U) {
            if_ |= pev.irq_flags;
        }
        if (pev.entered_hblank) {
            check_and_run_dma(2U);
        }
        if (pev.entered_vblank) {
            check_and_run_dma(1U);
        }
        apu_.step(step_c);
    }
}

RecompFunc Bus::lookup_function(uint32_t addr, uint8_t thumb) const noexcept {
    if (!dispatch_table_ || dispatch_count_ == 0U) {
        return nullptr;
    }
    const uint32_t norm_addr = normalize_rom_addr(addr & (thumb ? ~1U : ~3U));
    size_t lo = 0U;
    size_t hi = dispatch_count_;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2U;
        const auto& entry = dispatch_table_[mid];
        if (entry.addr < norm_addr || (entry.addr == norm_addr && entry.thumb < thumb)) {
            lo = mid + 1U;
        } else if (entry.addr > norm_addr || (entry.addr == norm_addr && entry.thumb > thumb)) {
            hi = mid;
        } else {
            return entry.func;
        }
    }
    return nullptr;
}

void Bus::log_fallback(uint32_t addr, uint8_t thumb, uint32_t from_pc) {
    const uint32_t norm_addr = normalize_rom_addr(addr & (thumb ? ~1U : ~3U));
    // Only log ROM addresses ($08000000-$09FFFFFF) for static recompilation learning;
    // RAM ($02/$03) and BIOS ($00) execution is normal dynamic code.
    if (norm_addr < 0x08000000U || norm_addr >= 0x0A000000U) {
        return;
    }
    const uint32_t rom_off = norm_addr - 0x08000000U;
    if (rom_off >= rom_.size()) {
        return;
    }
    if (logged_fallbacks_.insert({norm_addr, thumb}).second) {
        std::fprintf(stderr, "FALLBACK addr=$%08X mode=%02X from=$%08X\n", norm_addr, thumb,
                     from_pc);
        std::fflush(stderr);
    }
}

bool Bus::load_battery_ram(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    if (save_type_ == SaveType::Eeprom) {
        in.read(reinterpret_cast<char*>(eeprom_.data()),
                static_cast<std::streamsize>(eeprom_.size()));
    } else {
        in.read(reinterpret_cast<char*>(save_ram_.data()),
                static_cast<std::streamsize>(save_ram_.size()));
    }
    return in.good() || in.eof();
}

bool Bus::save_battery_ram(const std::filesystem::path& path) const {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    if (save_type_ == SaveType::Eeprom) {
        out.write(reinterpret_cast<const char*>(eeprom_.data()),
                  static_cast<std::streamsize>(eeprom_.size()));
    } else {
        const size_t sz = (save_type_ == SaveType::Flash128K) ? 0x20000U : 0x10000U;
        out.write(reinterpret_cast<const char*>(save_ram_.data()),
                  static_cast<std::streamsize>(sz));
    }
    return out.good();
}

bool Bus::save_state(const CpuState& cpu, const std::filesystem::path& path) const {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    HardwareStatePayload hw{};
    hw.dma = dma_;
    hw.timers = timers_;
    hw.keyinput = keyinput_;
    hw.keycnt = keycnt_;
    hw.ie = ie_;
    hw.if_ = if_;
    hw.ime = ime_;
    hw.waitcnt = waitcnt_;
    hw.postflg = postflg_;
    hw.haltcnt = haltcnt_;
    hw.acknowledged_irqs = acknowledged_irqs_;
    hw.last_sync_cycles = last_sync_cycles_;
    ppu_.save_state(hw.ppu);

    dragon::SaveStateHeader hdr;
    std::memcpy(hdr.system, "GBA ", 4);
    hdr.version = 2;
    hdr.payload_size =
        static_cast<uint32_t>(sizeof(CpuState) + ewram_.size() + iwram_.size() + save_ram_.size() +
                              eeprom_.size() + ppu_.palette_ram().size() + ppu_.vram().size() +
                              ppu_.oam().size() + sizeof(HardwareStatePayload));
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(&cpu), sizeof(CpuState));
    out.write(reinterpret_cast<const char*>(ewram_.data()),
              static_cast<std::streamsize>(ewram_.size()));
    out.write(reinterpret_cast<const char*>(iwram_.data()),
              static_cast<std::streamsize>(iwram_.size()));
    out.write(reinterpret_cast<const char*>(save_ram_.data()),
              static_cast<std::streamsize>(save_ram_.size()));
    out.write(reinterpret_cast<const char*>(eeprom_.data()),
              static_cast<std::streamsize>(eeprom_.size()));
    out.write(reinterpret_cast<const char*>(ppu_.palette_ram().data()),
              static_cast<std::streamsize>(ppu_.palette_ram().size()));
    out.write(reinterpret_cast<const char*>(ppu_.vram().data()),
              static_cast<std::streamsize>(ppu_.vram().size()));
    out.write(reinterpret_cast<const char*>(ppu_.oam().data()),
              static_cast<std::streamsize>(ppu_.oam().size()));
    out.write(reinterpret_cast<const char*>(&hw), sizeof(HardwareStatePayload));
    return out.good();
}

bool Bus::load_state(CpuState& cpu, const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    dragon::SaveStateHeader hdr{};
    in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!in.good()) {
        return false;
    }
    if (hdr.is_valid("GBA ", 2)) {
        void* saved_bus = cpu.bus;
        in.read(reinterpret_cast<char*>(&cpu), sizeof(CpuState));
        cpu.bus = saved_bus;
        in.read(reinterpret_cast<char*>(ewram_.data()),
                static_cast<std::streamsize>(ewram_.size()));
        in.read(reinterpret_cast<char*>(iwram_.data()),
                static_cast<std::streamsize>(iwram_.size()));
        in.read(reinterpret_cast<char*>(save_ram_.data()),
                static_cast<std::streamsize>(save_ram_.size()));
        in.read(reinterpret_cast<char*>(eeprom_.data()),
                static_cast<std::streamsize>(eeprom_.size()));
        in.read(reinterpret_cast<char*>(ppu_.palette_ram().data()),
                static_cast<std::streamsize>(ppu_.palette_ram().size()));
        in.read(reinterpret_cast<char*>(ppu_.vram().data()),
                static_cast<std::streamsize>(ppu_.vram().size()));
        in.read(reinterpret_cast<char*>(ppu_.oam().data()),
                static_cast<std::streamsize>(ppu_.oam().size()));

        HardwareStatePayload hw{};
        in.read(reinterpret_cast<char*>(&hw), sizeof(HardwareStatePayload));
        if (in.good()) {
            dma_ = hw.dma;
            timers_ = hw.timers;
            keyinput_ = hw.keyinput;
            keycnt_ = hw.keycnt;
            ie_ = hw.ie;
            if_ = hw.if_;
            ime_ = hw.ime;
            waitcnt_ = hw.waitcnt;
            postflg_ = hw.postflg;
            haltcnt_ = hw.haltcnt;
            acknowledged_irqs_ = hw.acknowledged_irqs;
            last_sync_cycles_ = hw.last_sync_cycles;
            ppu_.load_state(hw.ppu);
        }
        return in.good();
    } else if (hdr.is_valid("GBA ", 1)) {
        // Backward compatibility fallback for v1 state
        void* saved_bus = cpu.bus;
        in.read(reinterpret_cast<char*>(&cpu), sizeof(CpuState));
        cpu.bus = saved_bus;
        in.read(reinterpret_cast<char*>(ewram_.data()),
                static_cast<std::streamsize>(ewram_.size()));
        in.read(reinterpret_cast<char*>(iwram_.data()),
                static_cast<std::streamsize>(iwram_.size()));
        in.read(reinterpret_cast<char*>(save_ram_.data()),
                static_cast<std::streamsize>(save_ram_.size()));
        in.read(reinterpret_cast<char*>(eeprom_.data()),
                static_cast<std::streamsize>(eeprom_.size()));
        in.read(reinterpret_cast<char*>(ppu_.palette_ram().data()),
                static_cast<std::streamsize>(ppu_.palette_ram().size()));
        in.read(reinterpret_cast<char*>(ppu_.vram().data()),
                static_cast<std::streamsize>(ppu_.vram().size()));
        in.read(reinterpret_cast<char*>(ppu_.oam().data()),
                static_cast<std::streamsize>(ppu_.oam().size()));
        last_sync_cycles_ = cpu.cycles;
        return in.good();
    }
    return false;
}

}  // namespace dragon::gba
