#include "nes/bus.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "dragon/file_io.h"
#include "dragon/save_state.h"
#include "nes/interpreter.h"
#include "nes/opcodes.h"

namespace dragon::nes {

Bus::Bus() noexcept {
    apu_io_regs_.fill(0x00U);
    apu_.set_memory_reader(&Bus::apu_memory_read, this);
    ppu_.set_scanline_irq_callback(&Bus::on_ppu_scanline_a12, this, false);
}

uint8_t Bus::apu_memory_read(void* user_data, uint16_t addr) noexcept {
    auto* self = static_cast<Bus*>(user_data);
    return self ? self->peek(addr) : 0U;
}

void Bus::on_ppu_scanline_a12(void* user_data) noexcept {
    auto* self = static_cast<Bus*>(user_data);
    if (self && (self->mapper_ == 4U || self->mapper_ == 189U)) {
        self->clock_mmc3_scanline_irq();
    }
}

void Bus::load_cartridge(const Cartridge& cart) noexcept {
    ram_.fill(0U);
    prg_ram_.fill(0U);
    if (cart.has_trainer && cart.trainer.size() == 512) {
        std::memcpy(prg_ram_.data() + 0x1000, cart.trainer.data(), 512);
    }
    mapper_ = cart.mapper;
    cart_mirroring_ = cart.mirroring;
    has_battery_ = cart.has_battery;
    is_vs_system_ = cart.is_vs_system;
    coin_pulses_ = {0U, 0U};
    service_pulse_ = 0U;
    load_prg_rom(cart.prg_rom);
    ppu_.reset();
    ppu_.set_mirroring(cart.mirroring);
    if (is_vs_system_) {
        // Default Vs. PPU palette. Vs. games shipped with different RP2C04/RC2C03
        // chips whose 64-color tables differ (copy protection); the exact chip is
        // per-game and can be overridden with --palette. RP2C04-0001 (Vs0001) is
        // the common default and matches e.g. Vs. Battle City (MAME: rp2c04-0001).
        ppu_.set_palette(PpuPalette::Vs0001);
    }
    ppu_.load_chr(cart.chr_rom, cart.chr_rom.empty());
    apu_.reset();
    apu_.set_memory_reader(&Bus::apu_memory_read, this);
    reset_mapper_state();
    update_mapper_banks();
    last_ppu_cpu_cycles_ = cpu_ ? cpu_->cycles : 0ULL;
    in_nmi_ = false;
    in_irq_ = false;
    completed_frames_ = 0ULL;
}

void Bus::load_prg_rom(std::span<const uint8_t> prg) noexcept {
    prg_rom_small_ = {};
    if (prg.size() <= prg_rom_small_.size()) {
        if (!prg.empty()) {
            std::memcpy(prg_rom_small_.data(), prg.data(), prg.size());
        }
        prg_rom_ = prg_rom_small_.data();
        prg_rom_size_ = static_cast<uint32_t>(prg.size());
    } else {
        prg_rom_large_.assign(prg.begin(), prg.end());
        prg_rom_ = prg_rom_large_.data();
        prg_rom_size_ = static_cast<uint32_t>(prg_rom_large_.size());
    }
    reset_mapper_state();
    update_mapper_banks();
}

void Bus::set_mapper(uint16_t mapper) noexcept {
    mapper_ = mapper;
    reset_mapper_state();
    update_mapper_banks();
}

void Bus::reset_mapper_state() noexcept {
    mmc1_shift_reg_ = 0U;
    mmc1_shift_count_ = 0U;
    mmc1_control_ = 0x0CU;  // Mode 3: fix last 16 KiB bank at $C000, switch 16 KiB at $8000
    mmc1_chr0_ = 0U;
    mmc1_chr1_ = 0U;
    mmc1_prg_ = 0U;

    uxrom_prg_bank_ = 0U;
    cnrom_chr_bank_ = 0U;

    mmc3_bank_select_ = 0U;
    mmc3_bank_regs_ = {0U, 2U, 4U, 5U, 6U, 7U, 0U, 1U};
    mmc3_irq_latch_ = 0U;
    mmc3_irq_counter_ = 0U;
    mmc3_irq_reload_ = false;
    mmc3_irq_enabled_ = false;
    mmc3_irq_pending_ = false;

    mapper99_chr_bank_ = 0U;

    mapper189_prg_bank_ = 0U;

    mapper40_prg_bank_ = 0U;
    mapper40_irq_enabled_ = false;
    mapper40_irq_counter_ = 0U;
    mapper40_irq_pending_ = false;

    ppu_.set_scanline_irq_watch(mapper_ == 4U || mapper_ == 189U);
}

void Bus::set_prg_bank_8k(uint8_t slot_8k, uint32_t bank_8k) noexcept {
    if (slot_8k >= 4U || prg_rom_size_ < 0x2000U) {
        prg_bank_offsets_[slot_8k & 0x03U] = 0U;
        return;
    }
    const uint32_t num_8k_banks = prg_rom_size_ >> 13U;
    const uint32_t wrapped = (num_8k_banks > 0U) ? (bank_8k % num_8k_banks) : 0U;
    prg_bank_offsets_[slot_8k] = wrapped << 13U;
}

void Bus::set_prg_bank_16k(uint8_t slot_16k, uint32_t bank_16k) noexcept {
    const uint8_t base_slot = static_cast<uint8_t>((slot_16k & 0x01U) * 2U);
    const uint32_t base_8k = bank_16k * 2U;
    set_prg_bank_8k(base_slot, base_8k);
    set_prg_bank_8k(static_cast<uint8_t>(base_slot + 1U), base_8k + 1U);
}

void Bus::set_prg_bank_32k(uint32_t bank_32k) noexcept {
    const uint32_t base_8k = bank_32k * 4U;
    for (uint8_t i = 0U; i < 4U; ++i) {
        set_prg_bank_8k(i, base_8k + i);
    }
}

void Bus::update_mapper_banks() noexcept {
    const uint32_t num_16k = (prg_rom_size_ >= 0x4000U) ? (prg_rom_size_ >> 14U) : 1U;
    const uint32_t num_8k = (prg_rom_size_ >= 0x2000U) ? (prg_rom_size_ >> 13U) : 1U;

    switch (mapper_) {
        case 0U: {
            // Mapper 0 (NROM): 16 KiB mirrored or 32 KiB linear
            set_prg_bank_16k(0U, 0U);
            set_prg_bank_16k(1U, (num_16k > 1U) ? 1U : 0U);
            ppu_.set_chr_bank_8k(0U);
            break;
        }
        case 1U: {
            // Mapper 1 (MMC1 / SxROM): https://www.nesdev.org/wiki/MMC1
            switch (mmc1_control_ & 0x03U) {
                case 0U:
                    ppu_.set_mirroring(Mirroring::OneScreenLower);
                    break;
                case 1U:
                    ppu_.set_mirroring(Mirroring::OneScreenUpper);
                    break;
                case 2U:
                    ppu_.set_mirroring(Mirroring::Vertical);
                    break;
                case 3U:
                    ppu_.set_mirroring(Mirroring::Horizontal);
                    break;
            }

            const uint8_t prg_mode = static_cast<uint8_t>((mmc1_control_ >> 2U) & 0x03U);
            const uint32_t prg_bank = static_cast<uint32_t>(mmc1_prg_ & 0x0FU);
            if (prg_mode == 0U || prg_mode == 1U) {
                set_prg_bank_32k(prg_bank >> 1U);
            } else if (prg_mode == 2U) {
                set_prg_bank_16k(0U, 0U);
                set_prg_bank_16k(1U, prg_bank);
            } else {
                set_prg_bank_16k(0U, prg_bank);
                set_prg_bank_16k(1U, (num_16k > 0U) ? (num_16k - 1U) : 0U);
            }

            if ((mmc1_control_ & 0x10U) == 0U) {
                // 8 KiB CHR mode (low bit of chr0 ignored)
                ppu_.set_chr_bank_8k(static_cast<uint32_t>(mmc1_chr0_ >> 1U));
            } else {
                // Two 4 KiB CHR banks
                ppu_.set_chr_bank_4k(0U, mmc1_chr0_);
                ppu_.set_chr_bank_4k(1U, mmc1_chr1_);
            }
            break;
        }
        case 2U: {
            // Mapper 2 (UxROM): https://www.nesdev.org/wiki/UxROM
            set_prg_bank_16k(0U, uxrom_prg_bank_);
            set_prg_bank_16k(1U, (num_16k > 0U) ? (num_16k - 1U) : 0U);
            ppu_.set_chr_bank_8k(0U);
            break;
        }
        case 3U: {
            // Mapper 3 (CNROM): https://www.nesdev.org/wiki/CNROM
            set_prg_bank_16k(0U, 0U);
            set_prg_bank_16k(1U, (num_16k > 1U) ? 1U : 0U);
            ppu_.set_chr_bank_8k(cnrom_chr_bank_);
            break;
        }
        case 4U: {
            // Mapper 4 (MMC3 / TxROM): https://www.nesdev.org/wiki/MMC3
            const uint32_t last_8k = (num_8k > 0U) ? (num_8k - 1U) : 0U;
            const uint32_t second_last_8k = (num_8k > 1U) ? (num_8k - 2U) : 0U;
            const uint32_t r6 = static_cast<uint32_t>(mmc3_bank_regs_[6] & 0x3FU);
            const uint32_t r7 = static_cast<uint32_t>(mmc3_bank_regs_[7] & 0x3FU);

            if ((mmc3_bank_select_ & 0x40U) == 0U) {
                set_prg_bank_8k(0U, r6);
                set_prg_bank_8k(1U, r7);
                set_prg_bank_8k(2U, second_last_8k);
                set_prg_bank_8k(3U, last_8k);
            } else {
                set_prg_bank_8k(0U, second_last_8k);
                set_prg_bank_8k(1U, r7);
                set_prg_bank_8k(2U, r6);
                set_prg_bank_8k(3U, last_8k);
            }

            apply_mmc3_chr_banks();
            break;
        }
        case 189U: {
            // Mapper 189 (TXC/MMC3 variant): a single 32 KiB PRG bank occupies
            // $8000-$FFFF (MMC3 PRG registers are ignored); CHR banking is pure
            // MMC3. https://www.nesdev.org/wiki/INES_Mapper_189
            const uint32_t num_32k = (prg_rom_size_ >= 0x8000U) ? (prg_rom_size_ >> 15U) : 1U;
            const uint32_t wrapped = (num_32k > 0U) ? (mapper189_prg_bank_ % num_32k) : 0U;
            set_prg_bank_32k(wrapped);
            apply_mmc3_chr_banks();
            break;
        }
        case 99U: {
            set_prg_bank_16k(0U, 0U);
            set_prg_bank_16k(1U, (num_16k > 1U) ? (num_16k - 1U) : 0U);
            ppu_.set_chr_bank_8k(mapper99_chr_bank_);
            break;
        }
        case 40U: {
            // Mapper 40 (NTDEC 2722 / SMB2J pirate):
            // $8000-$9FFF: fixed Bank 4
            // $A000-$BFFF: fixed Bank 5
            // $C000-$DFFF: switchable Bank (0..7)
            // $E000-$FFFF: fixed Bank 7
            set_prg_bank_8k(0U, 4U);
            set_prg_bank_8k(1U, 5U);
            set_prg_bank_8k(2U, static_cast<uint32_t>(mapper40_prg_bank_ & 0x07U));
            set_prg_bank_8k(3U, 7U);
            ppu_.set_chr_bank_8k(0U);
            break;
        }
        default: {
            set_prg_bank_16k(0U, 0U);
            set_prg_bank_16k(1U, (num_16k > 1U) ? (num_16k - 1U) : 0U);
            ppu_.set_chr_bank_8k(0U);
            break;
        }
    }
}

void Bus::apply_mmc3_chr_banks() noexcept {
    if ((mmc3_bank_select_ & 0x80U) == 0U) {
        ppu_.set_chr_bank_2k(0U, mmc3_bank_regs_[0]);
        ppu_.set_chr_bank_2k(2U, mmc3_bank_regs_[1]);
        ppu_.set_chr_bank_1k(4U, mmc3_bank_regs_[2]);
        ppu_.set_chr_bank_1k(5U, mmc3_bank_regs_[3]);
        ppu_.set_chr_bank_1k(6U, mmc3_bank_regs_[4]);
        ppu_.set_chr_bank_1k(7U, mmc3_bank_regs_[5]);
    } else {
        ppu_.set_chr_bank_1k(0U, mmc3_bank_regs_[2]);
        ppu_.set_chr_bank_1k(1U, mmc3_bank_regs_[3]);
        ppu_.set_chr_bank_1k(2U, mmc3_bank_regs_[4]);
        ppu_.set_chr_bank_1k(3U, mmc3_bank_regs_[5]);
        ppu_.set_chr_bank_2k(4U, mmc3_bank_regs_[0]);
        ppu_.set_chr_bank_2k(6U, mmc3_bank_regs_[1]);
    }
}

void Bus::write_mapper(uint16_t addr, uint8_t value) noexcept {
    switch (mapper_) {
        case 0U:
            break;
        case 1U: {
            // Mapper 1 (MMC1) serial shift register ($8000-$FFFF)
            if ((value & 0x80U) != 0U) {
                mmc1_shift_reg_ = 0U;
                mmc1_shift_count_ = 0U;
                mmc1_control_ |= 0x0CU;
                update_mapper_banks();
            } else {
                mmc1_shift_reg_ =
                    static_cast<uint8_t>(mmc1_shift_reg_ | ((value & 0x01U) << mmc1_shift_count_));
                ++mmc1_shift_count_;
                if (mmc1_shift_count_ == 5U) {
                    const uint8_t reg_val = static_cast<uint8_t>(mmc1_shift_reg_ & 0x1FU);
                    switch ((addr >> 13U) & 0x03U) {
                        case 0U:
                            mmc1_control_ = reg_val;
                            break;
                        case 1U:
                            mmc1_chr0_ = reg_val;
                            break;
                        case 2U:
                            mmc1_chr1_ = reg_val;
                            break;
                        case 3U:
                            mmc1_prg_ = reg_val;
                            break;
                    }
                    mmc1_shift_reg_ = 0U;
                    mmc1_shift_count_ = 0U;
                    update_mapper_banks();
                }
            }
            break;
        }
        case 2U: {
            // Mapper 2 (UxROM)
            uxrom_prg_bank_ = value;
            update_mapper_banks();
            break;
        }
        case 3U: {
            // Mapper 3 (CNROM)
            cnrom_chr_bank_ = value;
            update_mapper_banks();
            break;
        }
        case 4U:
        case 189U: {
            // Mapper 4 (MMC3); mapper 189 shares the MMC3 register file for CHR,
            // mirroring and IRQ (its PRG bank regs are ignored by update_mapper_banks).
            const bool even = (addr & 0x01U) == 0U;
            switch (addr & 0xE000U) {
                case 0x8000U:
                    if (even) {
                        mmc3_bank_select_ = value;
                    } else {
                        mmc3_bank_regs_[mmc3_bank_select_ & 0x07U] = value;
                    }
                    update_mapper_banks();
                    break;
                case 0xA000U:
                    if (even && cart_mirroring_ != Mirroring::FourScreen) {
                        ppu_.set_mirroring(((value & 0x01U) != 0U) ? Mirroring::Horizontal
                                                                   : Mirroring::Vertical);
                    }
                    break;
                case 0xC000U:
                    if (even) {
                        mmc3_irq_latch_ = value;
                    } else {
                        mmc3_irq_reload_ = true;
                    }
                    break;
                case 0xE000U:
                    if (even) {
                        mmc3_irq_enabled_ = false;
                        mmc3_irq_pending_ = false;
                    } else {
                        mmc3_irq_enabled_ = true;
                    }
                    break;
            }
            break;
        }
        case 40U: {
            // Mapper 40 (NTDEC 2722): register mask $E000
            switch (addr & 0xE000U) {
                case 0x8000U:
                    // Disable and acknowledge IRQ
                    mapper40_irq_enabled_ = false;
                    mapper40_irq_pending_ = false;
                    mapper40_irq_counter_ = 0U;
                    break;
                case 0xA000U:
                    // Enable IRQ and reset 4096-cycle timer
                    mapper40_irq_enabled_ = true;
                    mapper40_irq_pending_ = false;
                    mapper40_irq_counter_ = 4096U;
                    break;
                case 0xC000U:
                    // Acknowledge IRQ
                    mapper40_irq_pending_ = false;
                    break;
                case 0xE000U:
                    // Select 8 KiB PRG bank for $C000-$DFFF
                    mapper40_prg_bank_ = static_cast<uint8_t>(value & 0x07U);
                    update_mapper_banks();
                    break;
            }
            break;
        }
        default:
            break;
    }
}

void Bus::clock_mmc3_scanline_irq() noexcept {
    if (mmc3_irq_counter_ == 0U || mmc3_irq_reload_) {
        mmc3_irq_counter_ = mmc3_irq_latch_;
        mmc3_irq_reload_ = false;
    } else {
        --mmc3_irq_counter_;
    }
    if (mmc3_irq_counter_ == 0U && mmc3_irq_enabled_) {
        mmc3_irq_pending_ = true;
    }
}

uint8_t Bus::current_prg_bank(uint16_t addr) const noexcept {
    if (addr < 0x8000U || prg_rom_size_ == 0U) {
        return 0U;
    }
    const uint8_t slot = static_cast<uint8_t>((addr >> 13U) & 0x03U);
    const uint32_t offset = prg_bank_offsets_[slot];
    if (mapper_ == 1U || mapper_ == 2U) {
        return static_cast<uint8_t>(offset >> 14U);
    }
    if (mapper_ == 4U) {
        return static_cast<uint8_t>(offset >> 13U);
    }
    if (mapper_ == 189U) {
        // Mapper 189 swaps a single 32 KiB window, so the bank id is 32K-granular.
        return static_cast<uint8_t>(offset >> 15U);
    }
    if (mapper_ == 40U) {
        if (addr < 0x8000U) {
            return 6U;
        }
        switch (slot) {
            case 0:
                return 4U;
            case 1:
                return 5U;
            case 2:
                return static_cast<uint8_t>(mapper40_prg_bank_ & 0x07U);
            default:
                return 7U;
        }
    }
    return 0U;
}

void Bus::set_controller(uint8_t port, uint8_t buttons) noexcept {
    if (port < 2U) {
        joypad_state_[port] = buttons;
        if (joypad_strobe_) {
            joypad_shift_[port] = buttons;
        }
    }
}

uint8_t Bus::controller(uint8_t port) const noexcept {
    return (port < 2U) ? joypad_state_[port] : 0U;
}

void Bus::attach_cpu(CpuState* cpu) noexcept {
    cpu_ = cpu;
    last_ppu_cpu_cycles_ = cpu ? cpu->cycles : 0ULL;
    if (cpu) {
        update_next_event(cpu);
    }
}

void Bus::sync_ppu(uint64_t cpu_cycles) noexcept {
    if (flat_ram_mode_) {
        last_ppu_cpu_cycles_ = cpu_cycles;
        return;
    }
    if (cpu_cycles > last_ppu_cpu_cycles_) {
        const uint64_t delta_cpu = cpu_cycles - last_ppu_cpu_cycles_;
        last_ppu_cpu_cycles_ = cpu_cycles;
        ppu_.step_dots(delta_cpu * 3ULL);
        apu_.step_cycles(delta_cpu);

        if (mapper_ == 40U && mapper40_irq_enabled_) {
            if (delta_cpu >= mapper40_irq_counter_) {
                const uint64_t rem = delta_cpu - mapper40_irq_counter_;
                if (!mapper40_irq_pending_) {
                    mapper40_irq_pending_ = true;
                    if (rem >= 4096ULL) {
                        mapper40_irq_pending_ = false;
                        mapper40_irq_counter_ = 0U;
                    } else {
                        mapper40_irq_counter_ = static_cast<uint32_t>(4096ULL - rem);
                    }
                } else {
                    mapper40_irq_pending_ = false;
                    mapper40_irq_counter_ = 0U;
                }
            } else {
                mapper40_irq_counter_ -= static_cast<uint32_t>(delta_cpu);
            }
        }
    }
}

void Bus::update_next_event(CpuState* cpu) noexcept {
    if (!cpu) {
        return;
    }
    if (flat_ram_mode_) {
        cpu->next_event = cpu->cycles + 29780ULL;
        return;
    }
    const uint32_t dots = ppu_.dots_until_next_event();
    if (dots == 0U || (cpu->i == 0U && irq_pending())) {
        cpu->next_event = cpu->cycles;
        return;
    }
    uint64_t cyc = (static_cast<uint64_t>(dots) + 2ULL) / 3ULL;
    if (cpu->i == 0U) {
        const uint32_t apu_cyc = apu_.cycles_until_next_irq();
        if (apu_cyc < cyc) {
            cyc = apu_cyc;
        }
        if (mapper_ == 40U && mapper40_irq_enabled_ && mapper40_irq_counter_ > 0U) {
            if (mapper40_irq_counter_ < cyc) {
                cyc = mapper40_irq_counter_;
            }
        }
    }
    cpu->next_event = cpu->cycles + ((cyc > 0ULL) ? cyc : 1ULL);
}

void Bus::on_frame_complete(CpuState* cpu) noexcept {
    ++completed_frames_;
    if (coin_pulses_[0] > 0U) {
        --coin_pulses_[0];
    }
    if (coin_pulses_[1] > 0U) {
        --coin_pulses_[1];
    }
    if (service_pulse_ > 0U) {
        --service_pulse_;
    }
    if (frame_callback_) {
        frame_callback_(cpu, this, frame_user_data_);
    }
    if (max_frames_ > 0ULL && completed_frames_ >= max_frames_ && cpu) {
        cpu->halted = 1U;
    }
}

uint8_t Bus::peek(uint16_t addr) const noexcept {
    if (flat_ram_mode_) {
        return flat_ram_[addr];
    }
    if (addr < 0x2000U) {
        return ram_[addr & 0x07FFU];
    }
    if (addr == 0x4015U) {
        return apu_.peek_status();
    }
    if (addr < 0x6000U) {
        return 0U;
    }
    if (addr < 0x8000U) {
        if (mapper_ == 40U && prg_rom_ && prg_rom_size_ >= 0xE000U) {
            return prg_rom_[6U * 0x2000U + (addr & 0x1FFFU)];
        }
        return prg_ram_[addr - 0x6000U];
    }
    if (prg_rom_size_ == 0U || !prg_rom_) {
        return 0U;
    }
    const uint8_t slot = static_cast<uint8_t>((addr >> 13U) & 0x03U);
    return prg_rom_[prg_bank_offsets_[slot] + (addr & 0x1FFFU)];
}

uint8_t Bus::read(uint16_t addr) noexcept {
    if (flat_ram_mode_) {
        return flat_ram_[addr];
    }
    if (addr < 0x2000U) {
        return ram_[addr & 0x07FFU];
    }
    if (addr < 0x4000U) {
        return ppu_.read_register(addr & 0x0007U);
    }
    if (addr < 0x4020U) {
        if (addr == 0x4016U || addr == 0x4017U) {
            const uint8_t port = static_cast<uint8_t>(addr - 0x4016U);
            if (joypad_strobe_) {
                joypad_shift_[port] = joypad_state_[port];
            }
            const uint8_t bit = static_cast<uint8_t>(joypad_shift_[port] & 0x01U);
            if (!joypad_strobe_) {
                joypad_shift_[port] = static_cast<uint8_t>((joypad_shift_[port] >> 1U) | 0x80U);
            }
            if (is_vs_system_) {
                uint8_t vs_val = bit;
                if (port == 0U) {
                    if (coin_pulses_[0] > 0U) {
                        vs_val |= 0x40U;  // Coin Slot 1 active (bit 6)
                    }
                    if (coin_pulses_[1] > 0U) {
                        vs_val |= 0x20U;  // Coin Slot 2 active (bit 5)
                    }
                    if (service_pulse_ > 0U) {
                        vs_val |= 0x04U;  // Service button (bit 2) — alt credit input
                    }
                    vs_val |= static_cast<uint8_t>((vs_dip_ & 0x03U) << 3U);
                } else {
                    vs_val |= static_cast<uint8_t>(vs_dip_ & 0xFCU);
                }
                return vs_val;
            }
            return static_cast<uint8_t>(0x40U | bit);
        }
        if (addr == 0x4015U) {
            return apu_.read_status();
        }
        return apu_io_regs_[addr & 0x001FU];
    }
    if (addr < 0x6000U) {
        return 0U;
    }
    if (addr < 0x8000U) {
        if (mapper_ == 40U && prg_rom_ && prg_rom_size_ >= 0xE000U) {
            return prg_rom_[6U * 0x2000U + (addr & 0x1FFFU)];
        }
        return prg_ram_[addr - 0x6000U];
    }
    if (prg_rom_size_ == 0U || !prg_rom_) {
        return 0U;
    }
    const uint8_t slot = static_cast<uint8_t>((addr >> 13U) & 0x03U);
    return prg_rom_[prg_bank_offsets_[slot] + (addr & 0x1FFFU)];
}

void Bus::write(uint16_t addr, uint8_t value) noexcept {
    if (flat_ram_mode_) {
        flat_ram_[addr] = value;
        if (addr < 0x2000U) {
            ram_[addr & 0x07FFU] = value;
        }
        return;
    }
    if (addr < 0x2000U) {
        ram_[addr & 0x07FFU] = value;
        return;
    }
    if (addr < 0x4000U) {
        ppu_.write_register(addr & 0x0007U, value);
        return;
    }
    if (addr < 0x4020U) {
        apu_io_regs_[addr & 0x001FU] = value;
        if (addr == 0x4014U) {
            const uint16_t base = static_cast<uint16_t>(value) << 8U;
            for (uint16_t i = 0U; i < 256U; ++i) {
                ppu_.write_oam_dma(static_cast<uint8_t>(i), peek(static_cast<uint16_t>(base + i)));
            }
            if (cpu_) {
                const uint64_t dma_cycles = 513ULL + (cpu_->cycles & 1ULL);
                cpu_->cycles += dma_cycles;
                sync_ppu(cpu_->cycles);
            } else {
                ppu_.step_dots(513ULL * 3ULL);
                apu_.step_cycles(513ULL);
            }
            return;
        }
        if (addr == 0x4016U) {
            const bool new_strobe = (value & 0x01U) != 0U;
            if (joypad_strobe_ || new_strobe) {
                joypad_shift_[0] = joypad_state_[0];
                joypad_shift_[1] = joypad_state_[1];
            }
            joypad_strobe_ = new_strobe;
            if (mapper_ == 99U) {
                mapper99_chr_bank_ = static_cast<uint32_t>((value >> 2U) & 0x01U);
                update_mapper_banks();
            }
            return;
        }
        if (addr <= 0x4013U || addr == 0x4015U || addr == 0x4017U) {
            apu_.write_register(addr, value);
        }
        return;
    }
    if (addr >= 0x4020U && addr < 0x6000U) {
        // Mapper 189 selects its 32 KiB PRG bank via a register in $4100-$5FFF.
        // Pirate boards vary in address/bit decoding, so follow the nesdev
        // generalization: OR the two data nibbles to form the bank number.
        // https://www.nesdev.org/wiki/INES_Mapper_189
        if (mapper_ == 189U) {
            mapper189_prg_bank_ = static_cast<uint8_t>((value >> 4U) | (value & 0x0FU));
            update_mapper_banks();
        }
        return;
    }
    if (addr >= 0x6000U && addr < 0x8000U) {
        if (mapper_ == 40U) {
            return;  // Mapper 40 Bank 6 in PRG ROM is read-only
        }
        prg_ram_[addr - 0x6000U] = value;
        return;
    }
    if (addr >= 0x8000U) {
        write_mapper(addr, value);
    }
}

void Bus::write_direct(uint16_t addr, uint8_t value) noexcept {
    if (flat_ram_mode_) {
        flat_ram_[addr] = value;
    }
    if (addr < 0x2000U) {
        ram_[addr & 0x07FFU] = value;
    } else if (addr >= 0x6000U && addr < 0x8000U) {
        prg_ram_[addr - 0x6000U] = value;
    } else if (addr >= 0x8000U) {
        prg_rom_small_[addr - 0x8000U] = value;
        if (prg_rom_size_ == 0U) {
            prg_rom_ = prg_rom_small_.data();
            prg_rom_size_ = 0x8000U;
            update_mapper_banks();
        }
    }
}

void Bus::load_direct(uint16_t start_addr, std::span<const uint8_t> data) noexcept {
    for (size_t i = 0; i < data.size(); ++i) {
        write_direct(static_cast<uint16_t>(start_addr + i), data[i]);
    }
}

RecompFunc Bus::lookup_exact_function(uint16_t addr, uint8_t bank) const noexcept {
    if (!dispatch_entries_ || dispatch_count_ == 0) {
        return nullptr;
    }
    size_t lo = 0;
    size_t hi = dispatch_count_;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const RecompFuncEntry& e = dispatch_entries_[mid];
        if (e.addr < addr || (e.addr == addr && e.bank < bank)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < dispatch_count_ && dispatch_entries_[lo].addr == addr &&
        dispatch_entries_[lo].bank == bank) {
        return dispatch_entries_[lo].func;
    }
    return nullptr;
}

RecompFunc Bus::lookup_function(uint16_t addr, uint8_t bank) const noexcept {
    if (RecompFunc fn = lookup_exact_function(addr, bank)) {
        return fn;
    }
    if (bank != kAnyBank) {
        return lookup_exact_function(addr, kAnyBank);
    }
    return nullptr;
}

bool Bus::should_log_fallback(uint16_t addr) noexcept {
    const size_t byte_idx = addr >> 3U;
    const uint8_t bit_mask = static_cast<uint8_t>(1U << (addr & 7U));
    if ((fallback_bitmap_[byte_idx] & bit_mask) == 0U) {
        fallback_bitmap_[byte_idx] |= bit_mask;
        ++fallback_count_;
        return log_fallbacks_;
    }
    return false;
}

void Bus::reset_fallbacks() noexcept {
    fallback_bitmap_.fill(0U);
    fallback_count_ = 0;
}

bool Bus::load_battery_ram(const std::filesystem::path& path) noexcept {
    auto data = dragon::try_read_binary_file(path);
    if (data.empty()) {
        return false;
    }
    const size_t to_copy = std::min(data.size(), prg_ram_.size());
    std::memcpy(prg_ram_.data(), data.data(), to_copy);
    return true;
}

bool Bus::save_battery_ram(const std::filesystem::path& path) const noexcept {
    return dragon::try_write_binary_file(path, prg_ram_);
}

struct NesStatePayload {
    uint8_t a;
    uint8_t x;
    uint8_t y;
    uint8_t s;
    uint16_t pc;
    uint8_t c;
    uint8_t v;
    uint8_t i;
    uint8_t d;
    uint8_t n;
    uint8_t z;
    uint64_t cycles;
    uint64_t next_event;

    std::array<uint8_t, 0x0800> ram;
    std::array<uint8_t, 0x2000> prg_ram;
    uint64_t last_ppu_cpu_cycles;
    bool in_nmi;
    bool in_irq;
    std::array<uint8_t, 2> joypad_state;
    std::array<uint8_t, 2> joypad_shift;
    bool joypad_strobe;

    uint16_t mapper;
    Mirroring cart_mirroring;
    std::array<uint32_t, 4> prg_bank_offsets;
    uint8_t mmc1_shift_reg;
    uint8_t mmc1_shift_count;
    uint8_t mmc1_control;
    uint8_t mmc1_chr0;
    uint8_t mmc1_chr1;
    uint8_t mmc1_prg;
    uint8_t uxrom_prg_bank;
    uint8_t cnrom_chr_bank;
    uint8_t mmc3_bank_select;
    std::array<uint8_t, 8> mmc3_bank_regs;
    uint8_t mmc3_irq_latch;
    uint8_t mmc3_irq_counter;
    bool mmc3_irq_reload;
    bool mmc3_irq_enabled;
    bool mmc3_irq_pending;
    uint8_t mapper189_prg_bank;
    uint8_t mapper40_prg_bank;
    bool mapper40_irq_enabled;
    uint32_t mapper40_irq_counter;
    bool mapper40_irq_pending;

    Ppu::Snapshot ppu;
    Apu::Snapshot apu;
};

bool Bus::save_state(const CpuState& cpu, const std::filesystem::path& path) const noexcept {
    NesStatePayload payload{};
    payload.a = cpu.a;
    payload.x = cpu.x;
    payload.y = cpu.y;
    payload.s = cpu.s;
    payload.pc = cpu.pc;
    payload.c = cpu.c;
    payload.v = cpu.v;
    payload.i = cpu.i;
    payload.d = cpu.d;
    payload.n = cpu.n;
    payload.z = cpu.z;
    payload.cycles = cpu.cycles;
    payload.next_event = cpu.next_event;

    payload.ram = ram_;
    payload.prg_ram = prg_ram_;
    payload.last_ppu_cpu_cycles = last_ppu_cpu_cycles_;
    payload.in_nmi = in_nmi_;
    payload.in_irq = in_irq_;
    payload.joypad_state = joypad_state_;
    payload.joypad_shift = joypad_shift_;
    payload.joypad_strobe = joypad_strobe_;

    payload.mapper = mapper_;
    payload.cart_mirroring = cart_mirroring_;
    payload.prg_bank_offsets = prg_bank_offsets_;
    payload.mmc1_shift_reg = mmc1_shift_reg_;
    payload.mmc1_shift_count = mmc1_shift_count_;
    payload.mmc1_control = mmc1_control_;
    payload.mmc1_chr0 = mmc1_chr0_;
    payload.mmc1_chr1 = mmc1_chr1_;
    payload.mmc1_prg = mmc1_prg_;
    payload.uxrom_prg_bank = uxrom_prg_bank_;
    payload.cnrom_chr_bank = cnrom_chr_bank_;
    payload.mmc3_bank_select = mmc3_bank_select_;
    payload.mmc3_bank_regs = mmc3_bank_regs_;
    payload.mmc3_irq_latch = mmc3_irq_latch_;
    payload.mmc3_irq_counter = mmc3_irq_counter_;
    payload.mmc3_irq_reload = mmc3_irq_reload_;
    payload.mmc3_irq_enabled = mmc3_irq_enabled_;
    payload.mmc3_irq_pending = mmc3_irq_pending_;
    payload.mapper189_prg_bank = mapper189_prg_bank_;
    payload.mapper40_prg_bank = mapper40_prg_bank_;
    payload.mapper40_irq_enabled = mapper40_irq_enabled_;
    payload.mapper40_irq_counter = mapper40_irq_counter_;
    payload.mapper40_irq_pending = mapper40_irq_pending_;

    ppu_.save_snapshot(payload.ppu);
    apu_.save_snapshot(payload.apu);

    dragon::SaveStateHeader hdr;
    std::memcpy(hdr.system, "NES ", 4);
    hdr.version = 1;
    hdr.payload_size = sizeof(NesStatePayload);

    std::vector<uint8_t> buffer(sizeof(hdr) + sizeof(payload));
    std::memcpy(buffer.data(), &hdr, sizeof(hdr));
    std::memcpy(buffer.data() + sizeof(hdr), &payload, sizeof(payload));

    return dragon::try_write_binary_file(path, buffer);
}

bool Bus::load_state(CpuState& cpu, const std::filesystem::path& path) noexcept {
    auto buffer = dragon::try_read_binary_file(path);
    if (buffer.size() < sizeof(dragon::SaveStateHeader) + sizeof(NesStatePayload)) {
        return false;
    }
    dragon::SaveStateHeader hdr;
    std::memcpy(&hdr, buffer.data(), sizeof(hdr));
    if (!hdr.is_valid("NES ", 1) || hdr.payload_size != sizeof(NesStatePayload)) {
        return false;
    }
    NesStatePayload payload;
    std::memcpy(&payload, buffer.data() + sizeof(hdr), sizeof(payload));

    cpu.a = payload.a;
    cpu.x = payload.x;
    cpu.y = payload.y;
    cpu.s = payload.s;
    cpu.pc = payload.pc;
    cpu.c = payload.c;
    cpu.v = payload.v;
    cpu.i = payload.i;
    cpu.d = payload.d;
    cpu.n = payload.n;
    cpu.z = payload.z;
    cpu.cycles = payload.cycles;
    cpu.next_event = payload.next_event;

    ram_ = payload.ram;
    prg_ram_ = payload.prg_ram;
    last_ppu_cpu_cycles_ = payload.last_ppu_cpu_cycles;
    in_nmi_ = payload.in_nmi;
    in_irq_ = payload.in_irq;
    joypad_state_ = payload.joypad_state;
    joypad_shift_ = payload.joypad_shift;
    joypad_strobe_ = payload.joypad_strobe;

    mapper_ = payload.mapper;
    cart_mirroring_ = payload.cart_mirroring;
    prg_bank_offsets_ = payload.prg_bank_offsets;
    mmc1_shift_reg_ = payload.mmc1_shift_reg;
    mmc1_shift_count_ = payload.mmc1_shift_count;
    mmc1_control_ = payload.mmc1_control;
    mmc1_chr0_ = payload.mmc1_chr0;
    mmc1_chr1_ = payload.mmc1_chr1;
    mmc1_prg_ = payload.mmc1_prg;
    uxrom_prg_bank_ = payload.uxrom_prg_bank;
    cnrom_chr_bank_ = payload.cnrom_chr_bank;
    mmc3_bank_select_ = payload.mmc3_bank_select;
    mmc3_bank_regs_ = payload.mmc3_bank_regs;
    mmc3_irq_latch_ = payload.mmc3_irq_latch;
    mmc3_irq_counter_ = payload.mmc3_irq_counter;
    mmc3_irq_reload_ = payload.mmc3_irq_reload;
    mmc3_irq_enabled_ = payload.mmc3_irq_enabled;
    mmc3_irq_pending_ = payload.mmc3_irq_pending;
    mapper189_prg_bank_ = payload.mapper189_prg_bank;
    mapper40_prg_bank_ = payload.mapper40_prg_bank;
    mapper40_irq_enabled_ = payload.mapper40_irq_enabled;
    mapper40_irq_counter_ = payload.mapper40_irq_counter;
    mapper40_irq_pending_ = payload.mapper40_irq_pending;

    ppu_.load_snapshot(payload.ppu);
    apu_.load_snapshot(payload.apu);

    return true;
}

}  // namespace dragon::nes

extern "C" {

uint8_t bus_read(CpuState* cpu, uint16_t addr) {
    auto* bus = static_cast<dragon::nes::Bus*>(cpu->bus);
    if (!bus->is_flat_ram() && addr >= 0x2000U && addr < 0x4020U) {
        bus->sync_ppu(cpu->cycles);
        const uint8_t val = bus->read(addr);
        bus->update_next_event(cpu);
        return val;
    }
    return bus->read(addr);
}

void bus_write(CpuState* cpu, uint16_t addr, uint8_t value) {
    auto* bus = static_cast<dragon::nes::Bus*>(cpu->bus);
    if (!bus->is_flat_ram() &&
        ((addr >= 0x2000U && addr < 0x4020U) || (addr >= 0x8000U && bus->mapper() != 0U))) {
        bus->sync_ppu(cpu->cycles);
        bus->write(addr, value);
        bus->update_next_event(cpu);
        return;
    }
    bus->write(addr, value);
}

void runtime_poll(CpuState* cpu) {
    auto* bus = static_cast<dragon::nes::Bus*>(cpu->bus);
    if (!bus || bus->is_flat_ram()) {
        cpu->next_event = cpu->cycles + 29780ULL;
        return;
    }

    bus->sync_ppu(cpu->cycles);

    if (bus->ppu().consume_frame_complete()) {
        bus->on_frame_complete(cpu);
        if (cpu->halted) {
            return;
        }
    }

    while (!cpu->halted && bus->ppu().consume_nmi()) {
        const bool prev_in_nmi = bus->in_nmi();
        bus->set_in_nmi(true);
        const uint16_t saved_pc = cpu->pc;
        cpu_push16(cpu, saved_pc);
        cpu_push8(cpu, cpu_pack_flags(cpu, 0U));
        cpu->i = 1U;
        const uint16_t nmi_addr = bus_read16(cpu, 0xFFFAU);
        cpu->cycles += 7ULL;
        bus->sync_ppu(cpu->cycles);
        bus->update_next_event(cpu);

        runtime_dispatch(cpu, nmi_addr, saved_pc);

        bus->set_in_nmi(prev_in_nmi);
        if (cpu->halted) {
            return;
        }
        bus->sync_ppu(cpu->cycles);
        if (bus->ppu().consume_frame_complete()) {
            bus->on_frame_complete(cpu);
            if (cpu->halted) {
                return;
            }
        }
    }

    if (!cpu->halted && !bus->in_nmi() && !bus->in_irq() && cpu->i == 0U && bus->irq_pending()) {
        bus->set_in_irq(true);
        const uint16_t saved_pc = cpu->pc;
        cpu_push16(cpu, saved_pc);
        cpu_push8(cpu, cpu_pack_flags(cpu, 0U));
        cpu->i = 1U;
        const uint16_t irq_addr = bus_read16(cpu, 0xFFFEU);
        cpu->cycles += 7ULL;
        bus->sync_ppu(cpu->cycles);
        bus->update_next_event(cpu);

        runtime_dispatch(cpu, irq_addr, saved_pc);

        bus->set_in_irq(false);
        if (cpu->halted) {
            return;
        }
        bus->sync_ppu(cpu->cycles);
        if (bus->ppu().consume_frame_complete()) {
            bus->on_frame_complete(cpu);
            if (cpu->halted) {
                return;
            }
        }
    }

    bus->update_next_event(cpu);
}

void runtime_dispatch(CpuState* cpu, uint16_t target, uint16_t from_pc) {
    auto* bus = static_cast<dragon::nes::Bus*>(cpu->bus);
    const uint8_t entry_s = cpu->s;
    cpu->pc = target;

    while (!cpu->halted) {
        const uint8_t pc_bank = bus->current_prg_bank(cpu->pc);
        if (RecompFunc fn = bus->lookup_function(cpu->pc, pc_bank)) {
            fn(cpu);
            if (cpu->halted || static_cast<int8_t>(cpu->s - entry_s) > 0) {
                return;
            }
            continue;
        }

        if (bus->should_log_fallback(cpu->pc)) {
            std::fprintf(stderr, "FALLBACK addr=$%04X bank=$%02X from=$%04X\n", cpu->pc, pc_bank,
                         from_pc);
            std::fflush(stderr);
        }

        const uint8_t op = bus->peek(cpu->pc);
        const dragon::nes::Mnemonic mnem = dragon::nes::get_opcode_info(op).mnemonic;
        dragon::nes::cpu_step(*cpu);

        if (cpu->cycles >= cpu->next_event) {
            runtime_poll(cpu);
            if (cpu->halted) {
                return;
            }
        }

        if (mnem == dragon::nes::Mnemonic::RTS || mnem == dragon::nes::Mnemonic::RTI) {
            if (static_cast<int8_t>(cpu->s - entry_s) > 0) {
                return;
            }
        }
    }
}

}  // extern "C"
