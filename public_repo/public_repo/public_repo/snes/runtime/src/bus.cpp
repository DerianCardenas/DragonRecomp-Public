#include "snes/bus.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "dragon/file_io.h"
#include "dragon/save_state.h"
#include "snes/interpreter.h"
#include "snes/opcodes.h"

namespace dragon::snes {

namespace {

// B-bus register address offsets for DMA transfer modes 0..7 across transfer byte index (i & 3).
// Reference: https://snes.nesdev.org/wiki/DMA_registers#DMAPn_($43n0)
constexpr std::array<std::array<uint8_t, 4>, 8> kDmaBOffsets = {{
    {{0U, 0U, 0U, 0U}},  // Mode 0: 1 register write once (p)
    {{0U, 1U, 0U, 1U}},  // Mode 1: 2 registers write once (p, p+1)
    {{0U, 0U, 0U, 0U}},  // Mode 2: 1 register write twice (p, p)
    {{0U, 0U, 1U, 1U}},  // Mode 3: 2 registers write twice each (p, p, p+1, p+1)
    {{0U, 1U, 2U, 3U}},  // Mode 4: 4 registers write once each (p, p+1, p+2, p+3)
    {{0U, 1U, 0U, 1U}},  // Mode 5: 2 registers write twice alternate (p, p+1, p, p+1)
    {{0U, 0U, 0U, 0U}},  // Mode 6: 1 register write twice (same as mode 2)
    {{0U, 0U, 1U, 1U}},  // Mode 7: 2 registers write twice each (same as mode 3)
}};

}  // namespace

Bus::Bus() noexcept {
    sram_.resize(kDefaultSramSize, 0U);
    reset();
}

void Bus::reset() noexcept {
    wram_.fill(0U);
    wram_addr_ = 0U;

    wr_mpya_ = 0xFFU;
    wr_mpyb_ = 0xFFU;
    wr_diva_ = 0xFFFFU;
    wr_divb_ = 0xFFU;
    rd_div_ = 0U;
    rd_mpy_ = 0U;

    ppu_.reset();
    apu_.reset();
    nmitimen_ = 0U;
    hdmaen_ = 0U;
    fastrom_enabled_ = false;
    rdnmi_flag_ = false;
    timeup_flag_ = false;
    nmi_requested_ = false;
    irq_requested_ = false;
    auto_joypad_busy_ = false;
    htime_ = 0x01FFU;
    vtime_ = 0x01FFU;
    joy1_state_ = 0U;
    joy2_state_ = 0U;
    joy1_data_ = 0U;
    joy2_data_ = 0U;
    joy1_shift_ = 0U;
    joy2_shift_ = 0U;
    joy_strobe_ = false;
    hdma_active_.fill(false);
    hdma_do_transfer_.fill(false);
    hdma_repeat_.fill(false);
    hdma_line_counter_.fill(0U);
    in_nmi_ = false;
    in_irq_ = false;
    in_context_switch_ = false;
    next_scanline_cycle_ = 1364ULL;
    current_scanline_ = 0U;
    current_frame_ = 0ULL;
    open_bus_ = 0U;

    for (auto& ch : dma_) {
        ch = DmaChannel{};
    }
    total_dma_master_cycles_ = 0ULL;
    reset_fallbacks();
}

void Bus::load_cartridge(const Cartridge& cart) noexcept {
    reset();
    map_mode_ = cart.map_mode;
    has_battery_ = cart.has_battery;
    rom_ = cart.rom;
    const size_t desired_sram = (cart.sram_size_bytes > 0U)
                                    ? static_cast<size_t>(cart.sram_size_bytes)
                                    : (cart.has_battery ? kDefaultSramSize : 0U);
    sram_.assign(desired_sram, 0U);
}

void Bus::load_rom(std::span<const uint8_t> rom, MapMode mode) noexcept {
    reset();
    map_mode_ = mode;
    rom_.assign(rom.begin(), rom.end());
    sram_.assign(kDefaultSramSize, 0U);
}

void Bus::attach_cpu(CpuState* cpu) noexcept {
    cpu_ = cpu;
    if (cpu_) {
        cpu_->bus = this;
        cpu_->next_event = cpu_->master_cycles + 357368ULL;
    }
}

int64_t Bus::map_sram_address(uint8_t bank, uint16_t offset) const noexcept {
    if (sram_.empty()) {
        return -1;
    }
    const uint8_t eff_bank = static_cast<uint8_t>(bank & 0x7FU);
    if (map_mode_ == MapMode::LoROM) {
        if (eff_bank >= 0x70U && eff_bank <= 0x7DU && offset < 0x8000U) {
            const size_t idx =
                (static_cast<size_t>(eff_bank - 0x70U) * 0x8000U + offset) % sram_.size();
            return static_cast<int64_t>(idx);
        }
    } else {
        // HiROM / ExHiROM: $20-$3F:6000-$7FFF and $A0-$BF:6000-$7FFF
        if (eff_bank >= 0x20U && eff_bank <= 0x3FU && offset >= 0x6000U && offset < 0x8000U) {
            const size_t idx =
                (static_cast<size_t>(eff_bank - 0x20U) * 0x2000U + (offset - 0x6000U)) %
                sram_.size();
            return static_cast<int64_t>(idx);
        }
    }
    return -1;
}

uint8_t Bus::read_b_bus(uint8_t reg) noexcept {
    if (reg == 0x80U) {
        // $2180: WMDATA — read from WRAM at 17-bit address and post-increment
        const uint8_t val = wram_[wram_addr_ & 0x1FFFFU];
        wram_addr_ = (wram_addr_ + 1U) & 0x1FFFFU;
        return val;
    }
    if (reg < 0x40U) {
        return ppu_.read_register(reg);
    }
    if (reg >= 0x40U && reg < 0x80U) {
        apu_.catchup(cpu_ ? cpu_->master_cycles : 0ULL);
        return apu_.read_port(static_cast<uint8_t>(reg & 0x03U));
    }
    return open_bus_;
}

void Bus::write_b_bus(uint8_t reg, uint8_t value) noexcept {
    if (reg == 0x80U) {
        // $2180: WMDATA — write to WRAM at 17-bit address and post-increment
        wram_[wram_addr_ & 0x1FFFFU] = value;
        wram_addr_ = (wram_addr_ + 1U) & 0x1FFFFU;
        return;
    }
    if (reg == 0x81U) {
        wram_addr_ = (wram_addr_ & 0x1FF00U) | static_cast<uint32_t>(value);
        return;
    }
    if (reg == 0x82U) {
        wram_addr_ = (wram_addr_ & 0x100FFU) | (static_cast<uint32_t>(value) << 8U);
        return;
    }
    if (reg == 0x83U) {
        wram_addr_ = (wram_addr_ & 0x0FFFFU) | (static_cast<uint32_t>(value & 0x01U) << 16U);
        return;
    }
    if (reg < 0x40U) {
        ppu_.write_register(reg, value);
        return;
    }
    if (reg >= 0x40U && reg < 0x80U) {
        apu_.write_port(static_cast<uint8_t>(reg & 0x03U), value);
        apu_.catchup(cpu_ ? cpu_->master_cycles : 0ULL);
        return;
    }
}

void Bus::execute_dma(uint8_t channels_mask) noexcept {
    uint64_t dma_cycles = 0ULL;
    for (size_t ch = 0U; ch < 8U; ++ch) {
        if ((channels_mask & (1U << ch)) == 0U) {
            continue;
        }
        DmaChannel& d = dma_[ch];
        const uint32_t count = (d.das == 0U) ? 0x10000U : static_cast<uint32_t>(d.das);
        const uint8_t mode = static_cast<uint8_t>(d.dmap & 0x07U);
        const bool fixed = (d.dmap & 0x08U) != 0U;
        const bool decrement = (d.dmap & 0x10U) != 0U;
        const bool b_to_a = (d.dmap & 0x80U) != 0U;

        for (uint32_t i = 0U; i < count; ++i) {
            const uint32_t a_addr = (static_cast<uint32_t>(d.a1b) << 16U) | d.a1t;
            const uint8_t b_reg = static_cast<uint8_t>(d.bbad + kDmaBOffsets[mode][i & 0x03U]);

            if (!b_to_a) {
                const uint8_t val = read(a_addr);
                write_b_bus(b_reg, val);
            } else {
                const uint8_t val = read_b_bus(b_reg);
                write(a_addr, val);
            }

            if (!fixed) {
                if (decrement) {
                    d.a1t = static_cast<uint16_t>(d.a1t - 1U);
                } else {
                    d.a1t = static_cast<uint16_t>(d.a1t + 1U);
                }
            }
            d.das = static_cast<uint16_t>(d.das - 1U);
        }

        dma_cycles += 8ULL * static_cast<uint64_t>(count);
    }

    total_dma_master_cycles_ += dma_cycles;
    if (cpu_) {
        cpu_->master_cycles += dma_cycles;
    }
}

uint8_t Bus::read_io(uint16_t offset) noexcept {
    if (offset >= 0x2100U && offset <= 0x21FFU) {
        return read_b_bus(static_cast<uint8_t>(offset & 0xFFU));
    }
    if (offset == 0x4016U) {
        if (joy_strobe_) {
            joy1_shift_ = joy1_state_;
        }
        const uint8_t bit = static_cast<uint8_t>((joy1_shift_ & 0x8000U) != 0U ? 1U : 0U);
        if (!joy_strobe_) {
            joy1_shift_ = static_cast<uint16_t>((joy1_shift_ << 1U) | 0x0001U);
        }
        return bit;
    }
    if (offset == 0x4017U) {
        if (joy_strobe_) {
            joy2_shift_ = joy2_state_;
        }
        const uint8_t bit = static_cast<uint8_t>((joy2_shift_ & 0x8000U) != 0U ? 1U : 0U);
        if (!joy_strobe_) {
            joy2_shift_ = static_cast<uint16_t>((joy2_shift_ << 1U) | 0x0001U);
        }
        return bit;
    }
    switch (offset) {
        case 0x4210U: {  // RDNMI
            const uint8_t v = static_cast<uint8_t>((rdnmi_flag_ ? 0x80U : 0x00U) | 0x02U);
            rdnmi_flag_ = false;
            return v;
        }
        case 0x4211U: {  // TIMEUP
            const uint8_t v = static_cast<uint8_t>(timeup_flag_ ? 0x80U : 0x00U);
            timeup_flag_ = false;
            irq_requested_ = false;
            return v;
        }
        case 0x4212U: {  // HVBJOY
            uint8_t status = 0U;
            if (cpu_) {
                const uint64_t frame_cyc = cpu_->master_cycles % 357368ULL;
                const uint64_t scanline = frame_cyc / 1364ULL;
                const uint64_t hdot_cyc = frame_cyc % 1364ULL;
                if (scanline >= 225ULL) {
                    status |= 0x80U;
                }
                if (hdot_cyc >= 1096ULL) {
                    status |= 0x40U;
                }
            }
            if (auto_joypad_busy_) {
                status |= 0x01U;
            }
            return status;
        }
        case 0x4214U:  // RDDIVL
            return static_cast<uint8_t>(rd_div_ & 0xFFU);
        case 0x4215U:  // RDDIVH
            return static_cast<uint8_t>(rd_div_ >> 8U);
        case 0x4216U:  // RDMPYL
            return static_cast<uint8_t>(rd_mpy_ & 0xFFU);
        case 0x4217U:  // RDMPYH
            return static_cast<uint8_t>(rd_mpy_ >> 8U);
        case 0x4218U:
            return static_cast<uint8_t>(joy1_data_ & 0xFFU);
        case 0x4219U:
            return static_cast<uint8_t>((joy1_data_ >> 8U) & 0xFFU);
        case 0x421AU:
            return static_cast<uint8_t>(joy2_data_ & 0xFFU);
        case 0x421BU:
            return static_cast<uint8_t>((joy2_data_ >> 8U) & 0xFFU);
        case 0x421CU:
        case 0x421DU:
        case 0x421EU:
        case 0x421FU:
            return 0x00U;
        default:
            break;
    }

    if (offset >= 0x4300U && offset <= 0x437FU) {
        const size_t ch = (offset >> 4U) & 0x07U;
        const DmaChannel& d = dma_[ch];
        switch (offset & 0x0FU) {
            case 0x00U:
                return d.dmap;
            case 0x01U:
                return d.bbad;
            case 0x02U:
                return static_cast<uint8_t>(d.a1t & 0xFFU);
            case 0x03U:
                return static_cast<uint8_t>(d.a1t >> 8U);
            case 0x04U:
                return d.a1b;
            case 0x05U:
                return static_cast<uint8_t>(d.das & 0xFFU);
            case 0x06U:
                return static_cast<uint8_t>(d.das >> 8U);
            case 0x07U:
                return d.dasb;
            case 0x08U:
                return static_cast<uint8_t>(d.a2a & 0xFFU);
            case 0x09U:
                return static_cast<uint8_t>(d.a2a >> 8U);
            case 0x0AU:
                return d.ntrl;
            case 0x0BU:
            case 0x0FU:
                return d.unused;
            default:
                return open_bus_;
        }
    }

    return open_bus_;
}

void Bus::write_io(uint16_t offset, uint8_t value) noexcept {
    if (offset >= 0x2100U && offset <= 0x21FFU) {
        write_b_bus(static_cast<uint8_t>(offset & 0xFFU), value);
        return;
    }
    if (offset == 0x4016U) {
        const bool new_strobe = ((value & 0x01U) != 0U);
        if (joy_strobe_ || new_strobe) {
            joy1_shift_ = joy1_state_;
            joy2_shift_ = joy2_state_;
        }
        joy_strobe_ = new_strobe;
        return;
    }
    switch (offset) {
        case 0x4200U:  // NMITIMEN
            nmitimen_ = value;
            return;
        case 0x4202U:  // WRMPYA
            wr_mpya_ = value;
            return;
        case 0x4203U:  // WRMPYB — triggers 8x8 -> 16 unsigned multiplication
            wr_mpyb_ = value;
            rd_mpy_ = static_cast<uint16_t>(static_cast<uint16_t>(wr_mpya_) *
                                            static_cast<uint16_t>(wr_mpyb_));
            rd_div_ = wr_mpyb_;
            return;
        case 0x4204U:  // WRDIVL
            wr_diva_ = static_cast<uint16_t>((wr_diva_ & 0xFF00U) | value);
            return;
        case 0x4205U:  // WRDIVH
            wr_diva_ =
                static_cast<uint16_t>((wr_diva_ & 0x00FFU) | (static_cast<uint16_t>(value) << 8U));
            return;
        case 0x4206U:  // WRDIVB — triggers 16/8 -> 16 unsigned division + remainder
            wr_divb_ = value;
            if (wr_divb_ == 0U) {
                rd_div_ = 0xFFFFU;
                rd_mpy_ = wr_diva_;
            } else {
                rd_div_ = static_cast<uint16_t>(wr_diva_ / wr_divb_);
                rd_mpy_ = static_cast<uint16_t>(wr_diva_ % wr_divb_);
            }
            return;
        case 0x4207U:  // HTIMEL
            htime_ = static_cast<uint16_t>((htime_ & 0x0100U) | value);
            return;
        case 0x4208U:  // HTIMEH
            htime_ = static_cast<uint16_t>((htime_ & 0x00FFU) |
                                           ((static_cast<uint16_t>(value) & 0x01U) << 8U));
            return;
        case 0x4209U:  // VTIMEL
            vtime_ = static_cast<uint16_t>((vtime_ & 0x0100U) | value);
            return;
        case 0x420AU:  // VTIMEH
            vtime_ = static_cast<uint16_t>((vtime_ & 0x00FFU) |
                                           ((static_cast<uint16_t>(value) & 0x01U) << 8U));
            return;
        case 0x420BU:  // MDMAEN — triggers general-purpose DMA
            if (value != 0U) {
                execute_dma(value);
            }
            return;
        case 0x420CU:  // HDMAEN
            hdmaen_ = value;
            return;
        case 0x420DU:  // MEMSEL
            fastrom_enabled_ = (value & 0x01U) != 0U;
            return;
        default:
            break;
    }

    if (offset >= 0x4300U && offset <= 0x437FU) {
        const size_t ch = (offset >> 4U) & 0x07U;
        DmaChannel& d = dma_[ch];
        switch (offset & 0x0FU) {
            case 0x00U:
                d.dmap = value;
                break;
            case 0x01U:
                d.bbad = value;
                break;
            case 0x02U:
                d.a1t = static_cast<uint16_t>((d.a1t & 0xFF00U) | value);
                break;
            case 0x03U:
                d.a1t =
                    static_cast<uint16_t>((d.a1t & 0x00FFU) | (static_cast<uint16_t>(value) << 8U));
                break;
            case 0x04U:
                d.a1b = value;
                break;
            case 0x05U:
                d.das = static_cast<uint16_t>((d.das & 0xFF00U) | value);
                break;
            case 0x06U:
                d.das =
                    static_cast<uint16_t>((d.das & 0x00FFU) | (static_cast<uint16_t>(value) << 8U));
                break;
            case 0x07U:
                d.dasb = value;
                break;
            case 0x08U:
                d.a2a = static_cast<uint16_t>((d.a2a & 0xFF00U) | value);
                break;
            case 0x09U:
                d.a2a =
                    static_cast<uint16_t>((d.a2a & 0x00FFU) | (static_cast<uint16_t>(value) << 8U));
                break;
            case 0x0AU:
                d.ntrl = value;
                break;
            case 0x0BU:
            case 0x0FU:
                d.unused = value;
                break;
            default:
                break;
        }
    }
}

uint8_t Bus::peek(uint32_t addr24) const noexcept {
    const uint8_t bank = static_cast<uint8_t>((addr24 >> 16U) & 0xFFU);
    const uint16_t offset = static_cast<uint16_t>(addr24 & 0xFFFFU);

    if (bank == 0x7EU || bank == 0x7FU) {
        return wram_[(static_cast<size_t>(bank - 0x7EU) << 16U) | offset];
    }

    const uint8_t low_bank = static_cast<uint8_t>(bank & 0x7FU);
    if (low_bank <= 0x3FU) {
        if (offset < 0x2000U) {
            return wram_[offset];
        }
        if (offset >= 0x6000U && offset < 0x8000U) {
            const int64_t sram_idx = map_sram_address(bank, offset);
            if (sram_idx >= 0) {
                return sram_[static_cast<size_t>(sram_idx)];
            }
        }
    }

    const int64_t sram_idx = map_sram_address(bank, offset);
    if (sram_idx >= 0) {
        return sram_[static_cast<size_t>(sram_idx)];
    }

    const int64_t rom_idx = map_rom_address(map_mode_, rom_.size(), bank, offset);
    if (rom_idx >= 0 && static_cast<size_t>(rom_idx) < rom_.size()) {
        return rom_[static_cast<size_t>(rom_idx)];
    }

    return open_bus_;
}

uint8_t Bus::read(uint32_t addr24) noexcept {
    const uint8_t bank = static_cast<uint8_t>((addr24 >> 16U) & 0xFFU);
    const uint16_t offset = static_cast<uint16_t>(addr24 & 0xFFFFU);

    if (bank == 0x7EU || bank == 0x7FU) {
        open_bus_ = wram_[(static_cast<size_t>(bank - 0x7EU) << 16U) | offset];
        return open_bus_;
    }

    const uint8_t low_bank = static_cast<uint8_t>(bank & 0x7FU);
    if (low_bank <= 0x3FU) {
        if (offset < 0x2000U) {
            open_bus_ = wram_[offset];
            return open_bus_;
        }
        if (offset >= 0x2000U && offset < 0x6000U) {
            open_bus_ = read_io(offset);
            return open_bus_;
        }
    }

    const int64_t sram_idx = map_sram_address(bank, offset);
    if (sram_idx >= 0) {
        open_bus_ = sram_[static_cast<size_t>(sram_idx)];
        return open_bus_;
    }

    const int64_t rom_idx = map_rom_address(map_mode_, rom_.size(), bank, offset);
    if (rom_idx >= 0 && static_cast<size_t>(rom_idx) < rom_.size()) {
        open_bus_ = rom_[static_cast<size_t>(rom_idx)];
        return open_bus_;
    }

    open_bus_ = (bank != 0U) ? bank : static_cast<uint8_t>(offset >> 8U);
    return open_bus_;
}

void Bus::write(uint32_t addr24, uint8_t value) noexcept {
    open_bus_ = value;
    const uint8_t bank = static_cast<uint8_t>((addr24 >> 16U) & 0xFFU);
    const uint16_t offset = static_cast<uint16_t>(addr24 & 0xFFFFU);

    if (bank == 0x7EU || bank == 0x7FU) {
        wram_[(static_cast<size_t>(bank - 0x7EU) << 16U) | offset] = value;
        return;
    }

    const uint8_t low_bank = static_cast<uint8_t>(bank & 0x7FU);
    if (low_bank <= 0x3FU) {
        if (offset < 0x2000U) {
            wram_[offset] = value;
            return;
        }
        if (offset >= 0x2000U && offset < 0x6000U) {
            write_io(offset, value);
            return;
        }
    }

    const int64_t sram_idx = map_sram_address(bank, offset);
    if (sram_idx >= 0) {
        sram_[static_cast<size_t>(sram_idx)] = value;
        return;
    }
}

void Bus::write_direct(uint32_t addr24, uint8_t value) noexcept {
    const uint8_t bank = static_cast<uint8_t>((addr24 >> 16U) & 0xFFU);
    const uint16_t offset = static_cast<uint16_t>(addr24 & 0xFFFFU);

    if (bank == 0x7EU || bank == 0x7FU) {
        wram_[(static_cast<size_t>(bank - 0x7EU) << 16U) | offset] = value;
        return;
    }
    if ((bank & 0x7FU) <= 0x3FU && offset < 0x2000U) {
        wram_[offset] = value;
        return;
    }
    const int64_t sram_idx = map_sram_address(bank, offset);
    if (sram_idx >= 0) {
        sram_[static_cast<size_t>(sram_idx)] = value;
        return;
    }
    if (rom_.empty()) {
        rom_.assign(0x10000U, 0U);
    }
    const int64_t rom_idx = map_rom_address(map_mode_, rom_.size(), bank, offset);
    if (rom_idx >= 0 && static_cast<size_t>(rom_idx) < rom_.size()) {
        rom_[static_cast<size_t>(rom_idx)] = value;
    }
}

void Bus::load_direct(uint32_t start_addr24, std::span<const uint8_t> data) noexcept {
    for (size_t i = 0U; i < data.size(); ++i) {
        write_direct(static_cast<uint32_t>(start_addr24 + i), data[i]);
    }
}

RecompFunc Bus::lookup_exact_function(uint8_t bank, uint16_t addr,
                                      uint8_t mode_flags) const noexcept {
    if (!dispatch_entries_ || dispatch_count_ == 0U) {
        return nullptr;
    }
    size_t lo = 0U;
    size_t hi = dispatch_count_;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2U;
        const RecompFuncEntry& e = dispatch_entries_[mid];
        if (e.bank < bank || (e.bank == bank && e.addr < addr) ||
            (e.bank == bank && e.addr == addr && e.mode_flags < mode_flags)) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    if (lo < dispatch_count_) {
        const RecompFuncEntry& e = dispatch_entries_[lo];
        if (e.bank == bank && e.addr == addr && e.mode_flags == mode_flags) {
            return e.func;
        }
    }
    return nullptr;
}

RecompFunc Bus::lookup_function(uint8_t bank, uint16_t addr, uint8_t mode_flags) const noexcept {
    // WRAM (banks $7E-$7F) is dynamic RAM, never static recompiled ROM code
    if (bank == 0x7EU || bank == 0x7FU) {
        return nullptr;
    }
    if (RecompFunc fn = lookup_exact_function(bank, addr, mode_flags)) {
        return fn;
    }
    // Check mirror banks ($00-$3F <-> $80-$BF, and HiROM $C0-$FF) when accessing upper half ROM
    if (addr >= 0x8000U) {
        if (bank <= 0x3FU || (bank >= 0x80U && bank <= 0xBFU)) {
            if (RecompFunc fn =
                    lookup_exact_function(static_cast<uint8_t>(bank ^ 0x80U), addr, mode_flags)) {
                return fn;
            }
        }
        if (map_mode_ != MapMode::LoROM) {
            if (RecompFunc fn =
                    lookup_exact_function(static_cast<uint8_t>(bank | 0xC0U), addr, mode_flags)) {
                return fn;
            }
            if (RecompFunc fn =
                    lookup_exact_function(static_cast<uint8_t>(bank & 0x3FU), addr, mode_flags)) {
                return fn;
            }
        }
    }
    return nullptr;
}

bool Bus::is_stack_trick_target(uint8_t bank, uint16_t addr) const noexcept {
    for (size_t i = 0U; i < stack_trick_count_; ++i) {
        const uint32_t t = stack_trick_targets_[i];
        if (t > 0xFFFFU) {
            if (static_cast<uint8_t>(t >> 16U) == bank &&
                static_cast<uint16_t>(t & 0xFFFFU) == addr) {
                return true;
            }
        } else if (static_cast<uint16_t>(t) == addr) {
            return true;
        }
    }
    return false;
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

void Bus::init_hdma_frame() noexcept {
    for (size_t ch = 0; ch < 8U; ++ch) {
        if ((hdmaen_ & (1U << ch)) != 0U) {
            dma_[ch].a2a = dma_[ch].a1t;
            dma_[ch].ntrl = 0U;
            hdma_line_counter_[ch] = 0U;
            hdma_repeat_[ch] = false;
            hdma_active_[ch] = true;
            hdma_do_transfer_[ch] = false;
        } else {
            hdma_active_[ch] = false;
            hdma_do_transfer_[ch] = false;
        }
    }
}

void Bus::step_hdma_scanline(uint16_t scanline) noexcept {
    if (scanline >= 225U) {
        return;
    }
    static constexpr std::array<uint8_t, 8> kBytesPerMode = {{1, 2, 2, 4, 4, 4, 2, 4}};

    for (size_t ch = 0; ch < 8U; ++ch) {
        if ((hdmaen_ & (1U << ch)) == 0U || !hdma_active_[ch]) {
            continue;
        }
        DmaChannel& d = dma_[ch];
        const bool indirect = (d.dmap & 0x40U) != 0U;
        const uint8_t mode = d.dmap & 0x07U;
        const uint8_t transfer_bytes = kBytesPerMode[mode];

        if (hdma_line_counter_[ch] == 0U) {
            const uint32_t table_addr = (static_cast<uint32_t>(d.a1b) << 16U) | d.a2a;
            const uint8_t line_byte = read(table_addr);
            d.a2a++;
            if (line_byte == 0U) {
                hdma_active_[ch] = false;
                continue;
            }
            d.ntrl = line_byte;
            hdma_repeat_[ch] = (line_byte > 0x80U);
            hdma_line_counter_[ch] = (line_byte == 0x80U) ? 128U : (line_byte & 0x7FU);

            if (indirect) {
                const uint8_t lo = read((static_cast<uint32_t>(d.a1b) << 16U) | d.a2a);
                d.a2a++;
                const uint8_t hi = read((static_cast<uint32_t>(d.a1b) << 16U) | d.a2a);
                d.a2a++;
                d.das = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
            }
            hdma_do_transfer_[ch] = true;
        } else {
            hdma_do_transfer_[ch] = hdma_repeat_[ch];
        }

        if (hdma_do_transfer_[ch]) {
            for (uint8_t b = 0; b < transfer_bytes; ++b) {
                uint8_t val = 0U;
                if (!indirect) {
                    const uint32_t src_addr = (static_cast<uint32_t>(d.a1b) << 16U) | d.a2a;
                    val = read(src_addr);
                    d.a2a++;
                } else {
                    const uint32_t src_addr = (static_cast<uint32_t>(d.dasb) << 16U) | d.das;
                    val = read(src_addr);
                    d.das++;
                }
                const uint8_t reg = static_cast<uint8_t>(d.bbad + kDmaBOffsets[mode][b & 3U]);
                write_b_bus(reg, val);
            }
        }

        if (hdma_line_counter_[ch] > 0U) {
            hdma_line_counter_[ch]--;
        }
    }
}

void Bus::on_frame_start() noexcept {
    init_hdma_frame();
}

void Bus::on_scanline(uint16_t scanline) noexcept {
    if (scanline == 0U) {
        init_hdma_frame();
    }
    if (scanline < 225U) {
        step_hdma_scanline(scanline);
    }
    if (scanline < 224U) {
        ppu_.render_scanline(scanline);
    } else if (scanline == 225U) {
        ppu_.on_vblank_start();
        rdnmi_flag_ = true;
        if ((nmitimen_ & 0x80U) != 0U) {
            nmi_requested_ = true;
        }
        if ((nmitimen_ & 0x01U) != 0U) {
            joy1_data_ = joy1_state_;
            joy2_data_ = joy2_state_;
            auto_joypad_busy_ = true;
        }
    } else if (scanline == 227U) {
        auto_joypad_busy_ = false;
    } else if (scanline >= 261U) {
        rdnmi_flag_ = false;
    }

    // Check timer IRQ (bits 4-5 of NMITIMEN: bit 5 = V timer)
    if ((nmitimen_ & 0x20U) != 0U && scanline == vtime_) {
        timeup_flag_ = true;
        irq_requested_ = true;
    }

    apu_.catchup(cpu_ ? cpu_->master_cycles : (static_cast<uint64_t>(scanline + 1U) * 1364ULL));
}

void Bus::sync_scanlines(CpuState* cpu) noexcept {
    if (!cpu) {
        return;
    }
    while (cpu->master_cycles >= next_scanline_cycle_) {
        on_scanline(current_scanline_);
        if (current_scanline_ == 261U) {
            current_frame_++;
            if (frame_callback_) {
                frame_callback_(frame_user_data_, current_frame_);
            }
            if (max_frames_ > 0ULL && current_frame_ >= max_frames_) {
                cpu->halted = 1U;
            }
        }
        current_scanline_ = static_cast<uint16_t>((current_scanline_ + 1U) % 262U);
        next_scanline_cycle_ += 1364ULL;
    }
}

void Bus::reset_fallbacks() noexcept {
    fallback_bitmap_.fill(0U);
    fallback_count_ = 0U;
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

struct SnesStatePayloadV1 {
    uint16_t a;
    uint16_t x;
    uint16_t y;
    uint16_t s;
    uint16_t pc;
    uint16_t d;
    uint8_t db;
    uint8_t pb;
    uint8_t c;
    uint8_t v;
    uint8_t i;
    uint8_t d_flag;
    uint8_t m;
    uint8_t xf;
    uint8_t e;
    uint16_t n;
    uint16_t z;
    uint64_t master_cycles;
    uint64_t next_event;
    uint8_t halted;
    uint8_t waiting;

    std::array<uint8_t, Bus::kWramSize> wram;
    uint32_t sram_size;
    uint32_t wram_addr;
    uint8_t wr_mpya;
    uint8_t wr_mpyb;
    uint16_t wr_diva;
    std::array<DmaChannel, 8> dma;
    uint8_t hdmaen;
    uint8_t nmitimen;
    uint8_t vtime;
    uint16_t htime;
    bool rdnmi_flag;
    bool timeup_flag;
    bool nmi_requested;
    bool irq_requested;
    uint16_t current_scanline;
    uint64_t next_scanline_cycle;

    Ppu::Snapshot ppu;
    Apu::Snapshot apu;
};

struct SnesStatePayloadV2 {
    uint16_t a;
    uint16_t x;
    uint16_t y;
    uint16_t s;
    uint16_t pc;
    uint16_t d;
    uint8_t db;
    uint8_t pb;
    uint8_t c;
    uint8_t v;
    uint8_t i;
    uint8_t d_flag;
    uint8_t m;
    uint8_t xf;
    uint8_t e;
    uint16_t n;
    uint16_t z;
    uint64_t master_cycles;
    uint64_t next_event;
    uint8_t halted;
    uint8_t waiting;

    std::array<uint8_t, Bus::kWramSize> wram;
    uint32_t sram_size;
    uint32_t wram_addr;
    uint8_t wr_mpya;
    uint8_t wr_mpyb;
    uint16_t wr_diva;
    std::array<DmaChannel, 8> dma;
    uint8_t hdmaen;
    uint8_t nmitimen;
    uint8_t vtime;
    uint16_t htime;
    bool rdnmi_flag;
    bool timeup_flag;
    bool nmi_requested;
    bool irq_requested;
    uint16_t current_scanline;
    uint64_t next_scanline_cycle;
    uint16_t joy1_data;
    uint16_t joy2_data;
    uint16_t joy1_shift;
    uint16_t joy2_shift;
    bool joy_strobe;
    bool auto_joypad_busy;

    Ppu::Snapshot ppu;
    Apu::Snapshot apu;
};

bool Bus::save_state(const CpuState& cpu, const std::filesystem::path& path) const noexcept {
    SnesStatePayloadV2 payload{};
    payload.a = cpu.a;
    payload.x = cpu.x;
    payload.y = cpu.y;
    payload.s = cpu.s;
    payload.pc = cpu.pc;
    payload.d = cpu.d;
    payload.db = cpu.db;
    payload.pb = cpu.pb;
    payload.c = cpu.c;
    payload.v = cpu.v;
    payload.i = cpu.i;
    payload.d_flag = cpu.d_flag;
    payload.m = cpu.m;
    payload.xf = cpu.xf;
    payload.e = cpu.e;
    payload.n = cpu.n;
    payload.z = cpu.z;
    payload.master_cycles = cpu.master_cycles;
    payload.next_event = cpu.next_event;
    payload.halted = cpu.halted;
    payload.waiting = cpu.waiting;

    payload.wram = wram_;
    payload.sram_size = static_cast<uint32_t>(sram_.size());
    payload.wram_addr = wram_addr_;
    payload.wr_mpya = wr_mpya_;
    payload.wr_mpyb = wr_mpyb_;
    payload.wr_diva = wr_diva_;
    payload.dma = dma_;
    payload.hdmaen = hdmaen_;
    payload.nmitimen = nmitimen_;
    payload.vtime = vtime_;
    payload.htime = htime_;
    payload.rdnmi_flag = rdnmi_flag_;
    payload.timeup_flag = timeup_flag_;
    payload.nmi_requested = nmi_requested_;
    payload.irq_requested = irq_requested_;
    payload.current_scanline = current_scanline_;
    payload.next_scanline_cycle = next_scanline_cycle_;
    payload.joy1_data = joy1_data_;
    payload.joy2_data = joy2_data_;
    payload.joy1_shift = joy1_shift_;
    payload.joy2_shift = joy2_shift_;
    payload.joy_strobe = joy_strobe_;
    payload.auto_joypad_busy = auto_joypad_busy_;

    ppu_.save_snapshot(payload.ppu);
    apu_.save_snapshot(payload.apu);

    dragon::SaveStateHeader hdr;
    std::memcpy(hdr.system, "SNES", 4);
    hdr.version = 2;
    hdr.payload_size = sizeof(SnesStatePayloadV2) + payload.sram_size;

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

    if (hdr.is_valid("SNES", 2)) {
        if (buffer.size() < sizeof(hdr) + sizeof(SnesStatePayloadV2)) {
            return false;
        }
        SnesStatePayloadV2 payload;
        std::memcpy(&payload, buffer.data() + sizeof(hdr), sizeof(payload));
        if (buffer.size() < sizeof(hdr) + sizeof(payload) + payload.sram_size) {
            return false;
        }

        cpu.a = payload.a;
        cpu.x = payload.x;
        cpu.y = payload.y;
        cpu.s = payload.s;
        cpu.pc = payload.pc;
        cpu.d = payload.d;
        cpu.db = payload.db;
        cpu.pb = payload.pb;
        cpu.c = payload.c;
        cpu.v = payload.v;
        cpu.i = payload.i;
        cpu.d_flag = payload.d_flag;
        cpu.m = payload.m;
        cpu.xf = payload.xf;
        cpu.e = payload.e;
        cpu.n = payload.n;
        cpu.z = payload.z;
        cpu.master_cycles = payload.master_cycles;
        cpu.next_event = payload.next_event;
        cpu.halted = payload.halted;
        cpu.waiting = payload.waiting;

        wram_ = payload.wram;
        if (payload.sram_size > 0) {
            sram_.resize(payload.sram_size);
            std::memcpy(sram_.data(), buffer.data() + sizeof(hdr) + sizeof(payload),
                        payload.sram_size);
        }
        wram_addr_ = payload.wram_addr;
        wr_mpya_ = payload.wr_mpya;
        wr_mpyb_ = payload.wr_mpyb;
        wr_diva_ = payload.wr_diva;
        dma_ = payload.dma;
        hdmaen_ = payload.hdmaen;
        nmitimen_ = payload.nmitimen;
        vtime_ = payload.vtime;
        htime_ = payload.htime;
        rdnmi_flag_ = payload.rdnmi_flag;
        timeup_flag_ = payload.timeup_flag;
        nmi_requested_ = payload.nmi_requested;
        irq_requested_ = payload.irq_requested;
        current_scanline_ = payload.current_scanline;
        next_scanline_cycle_ = payload.next_scanline_cycle;
        joy1_data_ = payload.joy1_data;
        joy2_data_ = payload.joy2_data;
        joy1_shift_ = payload.joy1_shift;
        joy2_shift_ = payload.joy2_shift;
        joy_strobe_ = payload.joy_strobe;
        auto_joypad_busy_ = payload.auto_joypad_busy;

        ppu_.load_snapshot(payload.ppu);
        apu_.load_snapshot(payload.apu);

        return true;
    } else if (hdr.is_valid("SNES", 1)) {
        if (buffer.size() < sizeof(hdr) + sizeof(SnesStatePayloadV1)) {
            return false;
        }
        SnesStatePayloadV1 payload;
        std::memcpy(&payload, buffer.data() + sizeof(hdr), sizeof(payload));
        if (buffer.size() < sizeof(hdr) + sizeof(payload) + payload.sram_size) {
            return false;
        }

        cpu.a = payload.a;
        cpu.x = payload.x;
        cpu.y = payload.y;
        cpu.s = payload.s;
        cpu.pc = payload.pc;
        cpu.d = payload.d;
        cpu.db = payload.db;
        cpu.pb = payload.pb;
        cpu.c = payload.c;
        cpu.v = payload.v;
        cpu.i = payload.i;
        cpu.d_flag = payload.d_flag;
        cpu.m = payload.m;
        cpu.xf = payload.xf;
        cpu.e = payload.e;
        cpu.n = payload.n;
        cpu.z = payload.z;
        cpu.master_cycles = payload.master_cycles;
        cpu.next_event = payload.next_event;
        cpu.halted = payload.halted;
        cpu.waiting = payload.waiting;

        wram_ = payload.wram;
        if (payload.sram_size > 0) {
            sram_.resize(payload.sram_size);
            std::memcpy(sram_.data(), buffer.data() + sizeof(hdr) + sizeof(payload),
                        payload.sram_size);
        }
        wram_addr_ = payload.wram_addr;
        wr_mpya_ = payload.wr_mpya;
        wr_mpyb_ = payload.wr_mpyb;
        wr_diva_ = payload.wr_diva;
        dma_ = payload.dma;
        hdmaen_ = payload.hdmaen;
        nmitimen_ = payload.nmitimen;
        vtime_ = payload.vtime;
        htime_ = payload.htime;
        rdnmi_flag_ = payload.rdnmi_flag;
        timeup_flag_ = payload.timeup_flag;
        nmi_requested_ = payload.nmi_requested;
        irq_requested_ = payload.irq_requested;
        current_scanline_ = payload.current_scanline;
        next_scanline_cycle_ = payload.next_scanline_cycle;
        joy1_data_ = 0;
        joy2_data_ = 0;
        joy1_shift_ = 0;
        joy2_shift_ = 0;
        joy_strobe_ = false;
        auto_joypad_busy_ = false;

        ppu_.load_snapshot(payload.ppu);
        apu_.load_snapshot(payload.apu);

        return true;
    }

    return false;
}

}  // namespace dragon::snes

extern "C" {

uint8_t bus_read(CpuState* cpu, uint32_t addr) {
    auto* bus = static_cast<dragon::snes::Bus*>(cpu->bus);
    return bus->read(addr);
}

void bus_write(CpuState* cpu, uint32_t addr, uint8_t value) {
    auto* bus = static_cast<dragon::snes::Bus*>(cpu->bus);
    bus->write(addr, value);
}

void runtime_poll(CpuState* cpu) {
    if (!cpu) {
        return;
    }
    auto* bus = static_cast<dragon::snes::Bus*>(cpu->bus);
    if (!bus) {
        cpu->next_event = cpu->master_cycles + 1364ULL;
        return;
    }

    bus->sync_scanlines(cpu);
    if (cpu->halted) {
        return;
    }

    if (!bus->in_nmi() && bus->nmi_requested()) {
        cpu->waiting = 0U;
        bus->acknowledge_nmi();
        bus->set_in_nmi(true);
        const uint8_t saved_pb = cpu->pb;
        const uint16_t saved_pc = cpu->pc;
        const uint16_t saved_s = cpu->s;
        if (!cpu->e) {
            cpu_push8(cpu, cpu->pb);
            cpu_push16(cpu, cpu->pc);
            cpu_push8(cpu, cpu_pack_flags(cpu, 0U));
            cpu->i = 1U;
            cpu->d_flag = 0U;
            const uint16_t nmi_vector = bus->read16(0x00FFEAU);
            cpu->master_cycles += 8ULL * 4ULL;
            runtime_dispatch(cpu, 0x00, nmi_vector, snes_addr24(saved_pb, saved_pc));
        } else {
            cpu_push16(cpu, cpu->pc);
            cpu_push8(cpu, cpu_pack_flags(cpu, 0U));
            cpu->i = 1U;
            cpu->d_flag = 0U;
            const uint16_t nmi_vector = bus->read16(0x00FFFAU);
            cpu->master_cycles += 7ULL * 4ULL;
            runtime_dispatch(cpu, 0x00, nmi_vector, snes_addr24(saved_pb, saved_pc));
        }
        bus->set_in_nmi(false);
        if (!bus->in_context_switch() &&
            (cpu->pb != saved_pb || cpu->pc != saved_pc || cpu->s != saved_s)) {
            bus->set_in_context_switch(true);
            while (!cpu->halted &&
                   (cpu->pb != saved_pb || cpu->pc != saved_pc || cpu->s != saved_s)) {
                if (cpu->waiting) {
                    cpu->master_cycles = bus->next_scanline_cycle();
                    runtime_poll(cpu);
                    continue;
                }
                dragon::snes::cpu_step(*cpu);
                runtime_poll(cpu);
            }
            bus->set_in_context_switch(false);
        }
        cpu->next_event = bus->next_scanline_cycle();
        return;
    }

    if (!bus->in_irq() && cpu->i == 0U && bus->irq_requested()) {
        cpu->waiting = 0U;
        bus->acknowledge_irq();
        bus->set_in_irq(true);
        const uint8_t saved_pb = cpu->pb;
        const uint16_t saved_pc = cpu->pc;
        const uint16_t saved_s = cpu->s;
        if (!cpu->e) {
            cpu_push8(cpu, cpu->pb);
            cpu_push16(cpu, cpu->pc);
            cpu_push8(cpu, cpu_pack_flags(cpu, 0U));
            cpu->i = 1U;
            cpu->d_flag = 0U;
            const uint16_t irq_vector = bus->read16(0x00FFEEU);
            cpu->master_cycles += 8ULL * 4ULL;
            runtime_dispatch(cpu, 0x00, irq_vector, snes_addr24(saved_pb, saved_pc));
        } else {
            cpu_push16(cpu, cpu->pc);
            cpu_push8(cpu, cpu_pack_flags(cpu, 0U));
            cpu->i = 1U;
            cpu->d_flag = 0U;
            const uint16_t irq_vector = bus->read16(0x00FFFEU);
            cpu->master_cycles += 7ULL * 4ULL;
            runtime_dispatch(cpu, 0x00, irq_vector, snes_addr24(saved_pb, saved_pc));
        }
        bus->set_in_irq(false);
        if (!bus->in_context_switch() &&
            (cpu->pb != saved_pb || cpu->pc != saved_pc || cpu->s != saved_s)) {
            bus->set_in_context_switch(true);
            while (!cpu->halted &&
                   (cpu->pb != saved_pb || cpu->pc != saved_pc || cpu->s != saved_s)) {
                if (cpu->waiting) {
                    cpu->master_cycles = bus->next_scanline_cycle();
                    runtime_poll(cpu);
                    continue;
                }
                dragon::snes::cpu_step(*cpu);
                runtime_poll(cpu);
            }
            bus->set_in_context_switch(false);
        }
        cpu->next_event = bus->next_scanline_cycle();
        return;
    } else if (cpu->waiting && bus->irq_requested()) {
        cpu->waiting = 0U;
    }

    cpu->next_event = bus->next_scanline_cycle();
}

void runtime_wai(CpuState* cpu) {
    if (!cpu) {
        return;
    }
    auto* bus = static_cast<dragon::snes::Bus*>(cpu->bus);
    if (!bus) {
        return;
    }
    cpu->waiting = 1U;
    while (cpu->waiting && !cpu->halted) {
        cpu->master_cycles = bus->next_scanline_cycle();
        runtime_poll(cpu);
    }
}

void runtime_dispatch(CpuState* cpu, uint8_t bank, uint16_t target, uint32_t from_addr) {
    auto* bus = static_cast<dragon::snes::Bus*>(cpu->bus);
    cpu->pb = bank;
    cpu->pc = target;

    const uint8_t mflags = cpu_mode_flags(cpu);
    if (RecompFunc fn = bus->lookup_function(bank, target, mflags)) {
        fn(cpu);
        return;
    }

    if (bus->should_log_fallback(target)) {
        std::fprintf(stderr, "FALLBACK addr=$%02X:%04X mode=%02X from=$%06X\n", bank, target,
                     mflags, from_addr & 0xFFFFFFU);
        std::fflush(stderr);
    }

    uint16_t entry_s = cpu->s;
    while (!cpu->halted) {
        if (cpu->waiting) {
            cpu->master_cycles = bus->next_scanline_cycle();
            runtime_poll(cpu);
            continue;
        }
        const uint32_t cur_addr = snes_addr24(cpu->pb, cpu->pc);
        const uint8_t op = bus->peek(cur_addr);
        const dragon::snes::Mnemonic mnem = dragon::snes::get_opcode_info(op).mnemonic;
        dragon::snes::cpu_step(*cpu);
        if (mnem == dragon::snes::Mnemonic::TXS || mnem == dragon::snes::Mnemonic::TCS) {
            entry_s = cpu->s;
        }
        runtime_poll(cpu);

        if (mnem == dragon::snes::Mnemonic::RTI) {
            return;
        }
        if (mnem == dragon::snes::Mnemonic::RTS) {
            if (!bus->is_stack_trick_target(bank, target) && cpu->s >= entry_s + 2U) {
                return;
            }
        }
        if (mnem == dragon::snes::Mnemonic::RTL) {
            if (cpu->s >= entry_s + 3U) {
                return;
            }
        }

        if (RecompFunc fn = bus->lookup_function(cpu->pb, cpu->pc, cpu_mode_flags(cpu))) {
            fn(cpu);
            if (cpu->halted || cpu->s > entry_s) {
                return;
            }
            continue;
        }
    }
}

}  // extern "C"
