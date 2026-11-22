#include "ps1/bus.h"

#include <algorithm>
#include <cstring>

#include "ps1/loader.h"

namespace dragon::ps1 {

namespace {

inline uint32_t mask_region(uint32_t addr) noexcept {
    // KUSEG (0x00000000), KSEG0 (0x80000000), KSEG1 (0xA0000000) -> physical
    if (addr >= 0xA0000000U && addr < 0xC0000000U)
        return addr & 0x1FFFFFFFU;
    if (addr >= 0x80000000U && addr < 0xA0000000U)
        return addr & 0x1FFFFFFFU;
    return addr;
}

}  // namespace

Ps1Bus::Ps1Bus() : main_ram_(kMainRamSize, 0U), bios_(kBiosSize, 0U) {
    reset();
}

void Ps1Bus::reset() {
    std::fill(main_ram_.begin(), main_ram_.end(), uint8_t{0});
    scratchpad_.fill(0U);
    std::fill(bios_.begin(), bios_.end(), uint8_t{0});
    gpu_.reset();
    spu_.reset();
    cdrom_.reset();

    i_stat_ = 0;
    i_mask_ = 0;
    for (auto& d : dma_)
        d = DmaChannel{};
    dpcr_ = 0x07654321U;
    dicr_ = 0;
    for (auto& t : timers_)
        t = Timer{};
    pad_buttons_ = 0;
    sio_ctrl_ = 0;
    sio_mode_ = 0;
    sio_baud_ = 0x0088;
    sio_rx_byte_ = 0xFF;
    sio_rx_ready_ = false;
    sio_step_ = 0;
    sio_device_ = 0;

    install_hle_bios_trampolines();
}

void Ps1Bus::install_hle_bios_trampolines() noexcept {
    // Standard PS1 BIOS vector trampolines at 0x000000A0, 0x000000B0, 0x000000C0:
    // Both interpreter and recompiled code intercept PC == 0x800000A0/B0/C0 directly,
    // and we also place `jr $ra; nop` as safe fallback instructions in RAM.
    auto put_stub = [&](uint32_t addr) {
        write32(addr, 0x03E00008U);       // jr $ra
        write32(addr + 4U, 0x00000000U);  // nop
    };
    put_stub(0x000000A0U);
    put_stub(0x000000B0U);
    put_stub(0x000000C0U);
    // Exception handler at 0x80000080: rfe; jr $k0; nop (or intercept)
    write32(0x00000080U, 0x42000010U);  // rfe
    write32(0x00000084U, 0x03400008U);  // jr $k0
    write32(0x00000088U, 0x00000000U);  // nop
}

void Ps1Bus::load_image(const LoadedPs1Image& img) {
    reset();
    cdrom_.attach_image(&img);

    const uint32_t base = mask_region(img.load_addr) & (kMainRamSize - 1U);
    const size_t copy_n = std::min<size_t>(img.exe_body.size(), kMainRamSize - base);
    if (copy_n > 0U) {
        std::memcpy(main_ram_.data() + base, img.exe_body.data(), copy_n);
    }
}

uint8_t* Ps1Bus::ram_ptr(uint32_t addr) noexcept {
    const uint32_t p = mask_region(addr);
    if (p < 0x00800000U) {
        return main_ram_.data() + (p & (kMainRamSize - 1U));
    }
    if (p >= 0x1F800000U && p < 0x1F800400U) {
        return scratchpad_.data() + (p - 0x1F800000U);
    }
    return nullptr;
}

const uint8_t* Ps1Bus::ram_ptr(uint32_t addr) const noexcept {
    const uint32_t p = mask_region(addr);
    if (p < 0x00800000U) {
        return main_ram_.data() + (p & (kMainRamSize - 1U));
    }
    if (p >= 0x1F800000U && p < 0x1F800400U) {
        return scratchpad_.data() + (p - 0x1F800000U);
    }
    return nullptr;
}

void Ps1Bus::request_irq(uint32_t irq_bit) noexcept {
    i_stat_ |= (irq_bit & 0x07FFU);
}

void Ps1Bus::on_vblank() noexcept {
    gpu_.toggle_odd_line();
    request_irq(kPs1IrqVBlank);
    // Timer 1 VBlank sync
    if ((timers_[1].mode & 0x0100U) != 0U) {
        timers_[1].counter = (timers_[1].counter + 1U) & 0xFFFFU;
    }
}

void Ps1Bus::step_timers(uint32_t cpu_cycles) noexcept {
    for (int i = 0; i < 3; ++i) {
        auto& t = timers_[static_cast<size_t>(i)];
        uint32_t inc = cpu_cycles;
        if (i == 1 && (t.mode & 0x0100U) != 0U) {
            // HBlank clock (~2148 CPU cycles per scanline)
            t.div_accum += cpu_cycles;
            inc = t.div_accum / 2148U;
            t.div_accum %= 2148U;
        } else if (i == 2 && (t.mode & 0x0200U) != 0U) {
            // System clock / 8
            t.div_accum += cpu_cycles;
            inc = t.div_accum >> 3U;
            t.div_accum &= 7U;
        }
        if (inc == 0U)
            continue;

        const uint32_t prev = t.counter;
        uint32_t next = prev + inc;
        const bool reset_on_target = (t.mode & 0x0008U) != 0U;
        if (t.target != 0U && prev <= t.target && next >= t.target) {
            t.mode |= (1U << 11U);  // Reached target
            if ((t.mode & 0x0010U) != 0U)
                request_irq(kPs1IrqTimer0 << i);
            if (reset_on_target)
                next %= (static_cast<uint32_t>(t.target) + 1U);
        }
        if (next > 0xFFFFU) {
            t.mode |= (1U << 12U);  // Reached 0xFFFF
            if ((t.mode & 0x0020U) != 0U)
                request_irq(kPs1IrqTimer0 << i);
            next &= 0xFFFFU;
        }
        t.counter = next;
    }
}

void Ps1Bus::sio0_transfer(uint8_t tx) noexcept {
    // Standard PS1 Digital Pad protocol (Controller port 1)
    if (sio_step_ == 0) {
        sio_device_ = tx;
        sio_rx_byte_ = 0xFFU;
        if (tx == 0x01U) {  // Controller 1 selected
            sio_step_ = 1;
            request_irq(kPs1IrqSio0);
        }
    } else if (sio_device_ == 0x01U) {
        const uint16_t btns_active_low = static_cast<uint16_t>(~pad_buttons_);
        switch (sio_step_) {
            case 1:  // Command 0x42 = Read data -> reply ID low 0x41 (Digital Pad)
                sio_rx_byte_ = (tx == 0x42U) ? 0x41U : 0xFFU;
                sio_step_ = (tx == 0x42U) ? 2 : 0;
                break;
            case 2:  // Reply ID high 0x5A
                sio_rx_byte_ = 0x5AU;
                sio_step_ = 3;
                break;
            case 3:  // Buttons low byte
                sio_rx_byte_ = static_cast<uint8_t>(btns_active_low & 0xFFU);
                sio_step_ = 4;
                break;
            case 4:  // Buttons high byte
                sio_rx_byte_ = static_cast<uint8_t>(btns_active_low >> 8U);
                sio_step_ = 0;
                break;
            default:
                sio_rx_byte_ = 0xFFU;
                sio_step_ = 0;
                break;
        }
        request_irq(kPs1IrqSio0);
    } else {
        sio_rx_byte_ = 0xFFU;
        sio_step_ = 0;
    }
    sio_rx_ready_ = true;
}

void Ps1Bus::run_dma(int ch) noexcept {
    auto& d = dma_[static_cast<size_t>(ch)];
    const bool to_dev = (d.chcr & 1U) != 0U;
    const int32_t step = ((d.chcr >> 1U) & 1U) ? -4 : 4;
    const uint32_t sync = (d.chcr >> 9U) & 3U;

    uint32_t addr = d.madr & 0x001FFFFCU;

    if (ch == 6) {
        // OTC (Ordering Table Clear) — reverse linked list in Main RAM
        uint32_t words = d.bcr & 0xFFFFU;
        if (words == 0U)
            words = 0x10000U;
        while (words > 0U) {
            const uint32_t next = (words == 1U) ? 0x00FFFFFFU : ((addr - 4U) & 0x001FFFFCU);
            write32(addr, next);
            addr = (addr - 4U) & 0x001FFFFCU;
            --words;
        }
    } else if (ch == 2) {
        // GPU DMA
        if (sync == 2U) {
            // Linked-list mode (Ordering Table -> GP0)
            uint32_t safety = 0;
            while (safety++ < 65536U) {
                const uint32_t hdr = read32(addr);
                const uint32_t count = hdr >> 24U;
                for (uint32_t i = 1U; i <= count; ++i) {
                    gpu_.write_gp0(read32((addr + i * 4U) & 0x001FFFFCU));
                }
                if ((hdr & 0x00800000U) != 0U)
                    break;
                addr = hdr & 0x001FFFFCU;
            }
        } else {
            // Block / Burst mode
            uint32_t words =
                (sync == 1U) ? ((d.bcr & 0xFFFFU) * ((d.bcr >> 16U) & 0xFFFFU)) : (d.bcr & 0xFFFFU);
            if (words == 0U)
                words = 0x10000U;
            for (uint32_t i = 0; i < words; ++i) {
                if (to_dev) {
                    gpu_.write_gp0(read32(addr));
                } else {
                    write32(addr, gpu_.read_gpuread());
                }
                addr = (addr + static_cast<uint32_t>(step)) & 0x001FFFFCU;
            }
        }
    } else if (ch == 3 && !to_dev) {
        // CD-ROM -> RAM
        uint32_t words = d.bcr & 0xFFFFU;
        if (words == 0U)
            words = 0x10000U;
        for (uint32_t i = 0; i < words; ++i) {
            write32(addr, cdrom_.dma_read32());
            addr = (addr + static_cast<uint32_t>(step)) & 0x001FFFFCU;
        }
    } else if (ch == 4) {
        // SPU DMA (RAM <-> SPU RAM)
        uint32_t words =
            (sync == 1U) ? ((d.bcr & 0xFFFFU) * ((d.bcr >> 16U) & 0xFFFFU)) : (d.bcr & 0xFFFFU);
        for (uint32_t i = 0; i < words; ++i) {
            if (to_dev) {
                const uint32_t w = read32(addr);
                spu_.dma_write16(static_cast<uint16_t>(w & 0xFFFFU));
                spu_.dma_write16(static_cast<uint16_t>(w >> 16U));
            } else {
                const uint16_t lo = spu_.dma_read16();
                const uint16_t hi = spu_.dma_read16();
                write32(addr, static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16U));
            }
            addr = (addr + static_cast<uint32_t>(step)) & 0x001FFFFCU;
        }
    }

    // Clear busy/trigger bits
    d.chcr &= ~((1U << 24U) | (1U << 28U));
    // Trigger DMA IRQ if enabled in DICR
    if (((dicr_ >> (16U + ch)) & 1U) != 0U) {
        dicr_ |= (1U << (24U + ch));
        if ((dicr_ & (1U << 23U)) != 0U) {
            dicr_ |= (1U << 31U);
            request_irq(kPs1IrqDma);
        }
    }
}

uint8_t Ps1Bus::read8(uint32_t addr) noexcept {
    const uint32_t p = mask_region(addr);
    if (p < 0x00800000U)
        return main_ram_[p & (kMainRamSize - 1U)];
    if (p >= 0x1F800000U && p < 0x1F800400U)
        return scratchpad_[p - 0x1F800000U];
    if (p >= 0x1FC00000U && p < 0x1FC80000U)
        return bios_[p - 0x1FC00000U];
    if (p >= 0x1F801800U && p <= 0x1F801803U)
        return cdrom_.read8(p);
    if (p == 0x1F801040U) {
        sio_rx_ready_ = false;
        return sio_rx_byte_;
    }
    const uint32_t w = read32(p & ~3U);
    return static_cast<uint8_t>((w >> ((p & 3U) * 8U)) & 0xFFU);
}

uint16_t Ps1Bus::read16(uint32_t addr) noexcept {
    const uint32_t p = mask_region(addr) & ~1U;
    if (p < 0x00800000U) {
        const uint32_t o = p & (kMainRamSize - 1U);
        return static_cast<uint16_t>(main_ram_[o]) |
               (static_cast<uint16_t>(main_ram_[o + 1U]) << 8U);
    }
    if (p >= 0x1F800000U && p < 0x1F800400U) {
        const uint32_t o = p - 0x1F800000U;
        return static_cast<uint16_t>(scratchpad_[o]) |
               (static_cast<uint16_t>(scratchpad_[o + 1U]) << 8U);
    }
    if (p >= 0x1FC00000U && p < 0x1FC80000U) {
        const uint32_t o = p - 0x1FC00000U;
        return static_cast<uint16_t>(bios_[o]) | (static_cast<uint16_t>(bios_[o + 1U]) << 8U);
    }
    if (p >= 0x1F801C00U && p < 0x1F802000U)
        return spu_.read16(p);
    if (p == 0x1F801044U) {
        // SIO0 STAT: TX ready 1/2 + RX ready when byte present
        return static_cast<uint16_t>(0x0005U | (sio_rx_ready_ ? 0x0002U : 0U));
    }
    if (p == 0x1F801048U)
        return sio_mode_;
    if (p == 0x1F80104AU)
        return sio_ctrl_;
    if (p == 0x1F80104EU)
        return sio_baud_;
    const uint32_t w = read32(p & ~3U);
    return static_cast<uint16_t>((w >> ((p & 2U) * 8U)) & 0xFFFFU);
}

uint32_t Ps1Bus::read32(uint32_t addr) noexcept {
    const uint32_t p = mask_region(addr) & ~3U;
    if (p < 0x00800000U) {
        const uint32_t o = p & (kMainRamSize - 1U);
        return static_cast<uint32_t>(main_ram_[o]) |
               (static_cast<uint32_t>(main_ram_[o + 1U]) << 8U) |
               (static_cast<uint32_t>(main_ram_[o + 2U]) << 16U) |
               (static_cast<uint32_t>(main_ram_[o + 3U]) << 24U);
    }
    if (p >= 0x1F800000U && p < 0x1F800400U) {
        const uint32_t o = p - 0x1F800000U;
        return static_cast<uint32_t>(scratchpad_[o]) |
               (static_cast<uint32_t>(scratchpad_[o + 1U]) << 8U) |
               (static_cast<uint32_t>(scratchpad_[o + 2U]) << 16U) |
               (static_cast<uint32_t>(scratchpad_[o + 3U]) << 24U);
    }
    if (p >= 0x1FC00000U && p < 0x1FC80000U) {
        const uint32_t o = p - 0x1FC00000U;
        return static_cast<uint32_t>(bios_[o]) | (static_cast<uint32_t>(bios_[o + 1U]) << 8U) |
               (static_cast<uint32_t>(bios_[o + 2U]) << 16U) |
               (static_cast<uint32_t>(bios_[o + 3U]) << 24U);
    }

    // I/O registers
    if (p == 0x1F801070U)
        return i_stat_;
    if (p == 0x1F801074U)
        return i_mask_;

    if (p >= 0x1F801080U && p < 0x1F8010F0U) {
        const size_t ch = (p - 0x1F801080U) >> 4U;
        const uint32_t reg = p & 0x0FU;
        if (reg == 0x0U)
            return dma_[ch].madr;
        if (reg == 0x4U)
            return dma_[ch].bcr;
        if (reg == 0x8U)
            return dma_[ch].chcr;
        return 0U;
    }
    if (p == 0x1F8010F0U)
        return dpcr_;
    if (p == 0x1F8010F4U)
        return dicr_;

    if (p >= 0x1F801100U && p < 0x1F801130U) {
        const size_t idx = (p - 0x1F801100U) >> 4U;
        const uint32_t reg = p & 0x0FU;
        if (reg == 0x0U)
            return timers_[idx].counter;
        if (reg == 0x4U) {
            const uint32_t m = timers_[idx].mode;
            timers_[idx].mode &= ~0x1800U;  // Clear reached-target / overflow on read
            return m;
        }
        if (reg == 0x8U)
            return timers_[idx].target;
        return 0U;
    }

    if (p == 0x1F801810U)
        return gpu_.read_gpuread();
    if (p == 0x1F801814U)
        return gpu_.read_gpustat();
    if (p >= 0x1F801C00U && p < 0x1F802000U) {
        return static_cast<uint32_t>(spu_.read16(p)) |
               (static_cast<uint32_t>(spu_.read16(p + 2U)) << 16U);
    }
    return 0U;
}

void Ps1Bus::write8(uint32_t addr, uint8_t val) noexcept {
    const uint32_t p = mask_region(addr);
    if (p < 0x00800000U) {
        main_ram_[p & (kMainRamSize - 1U)] = val;
        return;
    }
    if (p >= 0x1F800000U && p < 0x1F800400U) {
        scratchpad_[p - 0x1F800000U] = val;
        return;
    }
    if (p >= 0x1F801800U && p <= 0x1F801803U) {
        cdrom_.write8(p, val);
        if (cdrom_.has_pending_irq())
            request_irq(kPs1IrqCdrom);
        return;
    }
    if (p == 0x1F801040U) {
        sio0_transfer(val);
        return;
    }
}

void Ps1Bus::write16(uint32_t addr, uint16_t val) noexcept {
    const uint32_t p = mask_region(addr) & ~1U;
    if (p < 0x00800000U) {
        const uint32_t o = p & (kMainRamSize - 1U);
        main_ram_[o] = static_cast<uint8_t>(val & 0xFFU);
        main_ram_[o + 1U] = static_cast<uint8_t>(val >> 8U);
        return;
    }
    if (p >= 0x1F800000U && p < 0x1F800400U) {
        const uint32_t o = p - 0x1F800000U;
        scratchpad_[o] = static_cast<uint8_t>(val & 0xFFU);
        scratchpad_[o + 1U] = static_cast<uint8_t>(val >> 8U);
        return;
    }
    if (p >= 0x1F801C00U && p < 0x1F802000U) {
        spu_.write16(p, val);
        return;
    }
    if (p == 0x1F801048U) {
        sio_mode_ = val;
        return;
    }
    if (p == 0x1F80104AU) {
        sio_ctrl_ = val;
        if ((val & 0x0040U) != 0U || (val & 0x0002U) == 0U)
            sio_step_ = 0;
        return;
    }
    if (p == 0x1F80104EU) {
        sio_baud_ = val;
        return;
    }
    if (p == 0x1F801070U) {
        i_stat_ &= val;
        return;
    }
    if (p == 0x1F801074U) {
        i_mask_ = val & 0x07FFU;
        return;
    }
    if (p >= 0x1F801100U && p < 0x1F801130U) {
        write32(p, val);
        return;
    }
}

void Ps1Bus::write32(uint32_t addr, uint32_t val) noexcept {
    const uint32_t p = mask_region(addr) & ~3U;
    if (p < 0x00800000U) {
        const uint32_t o = p & (kMainRamSize - 1U);
        main_ram_[o] = static_cast<uint8_t>(val & 0xFFU);
        main_ram_[o + 1U] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
        main_ram_[o + 2U] = static_cast<uint8_t>((val >> 16U) & 0xFFU);
        main_ram_[o + 3U] = static_cast<uint8_t>((val >> 24U) & 0xFFU);
        return;
    }
    if (p >= 0x1F800000U && p < 0x1F800400U) {
        const uint32_t o = p - 0x1F800000U;
        scratchpad_[o] = static_cast<uint8_t>(val & 0xFFU);
        scratchpad_[o + 1U] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
        scratchpad_[o + 2U] = static_cast<uint8_t>((val >> 16U) & 0xFFU);
        scratchpad_[o + 3U] = static_cast<uint8_t>((val >> 24U) & 0xFFU);
        return;
    }

    if (p == 0x1F801070U) {
        i_stat_ &= val;
        return;
    }
    if (p == 0x1F801074U) {
        i_mask_ = val & 0x07FFU;
        return;
    }

    if (p >= 0x1F801080U && p < 0x1F8010F0U) {
        const int ch = static_cast<int>((p - 0x1F801080U) >> 4U);
        const uint32_t reg = p & 0x0FU;
        if (reg == 0x0U)
            dma_[static_cast<size_t>(ch)].madr = val & 0x00FFFFFFU;
        else if (reg == 0x4U)
            dma_[static_cast<size_t>(ch)].bcr = val;
        else if (reg == 0x8U) {
            dma_[static_cast<size_t>(ch)].chcr = val;
            if ((val & (1U << 24U)) != 0U) {
                run_dma(ch);
            }
        }
        return;
    }
    if (p == 0x1F8010F0U) {
        dpcr_ = val;
        return;
    }
    if (p == 0x1F8010F4U) {
        const uint32_t ack = (val >> 24U) & 0x7FU;
        dicr_ = (dicr_ & 0x7F000000U & ~(ack << 24U)) | (val & 0x00FFFFFFU);
        if ((dicr_ & 0x00800000U) != 0U && ((dicr_ >> 24U) & (dicr_ >> 16U) & 0x7FU) != 0U) {
            dicr_ |= 0x80000000U;
        } else {
            dicr_ &= ~0x80000000U;
        }
        return;
    }

    if (p >= 0x1F801100U && p < 0x1F801130U) {
        const size_t idx = (p - 0x1F801100U) >> 4U;
        const uint32_t reg = p & 0x0FU;
        if (reg == 0x0U)
            timers_[idx].counter = val & 0xFFFFU;
        else if (reg == 0x4U) {
            timers_[idx].mode = static_cast<uint16_t>(val & 0xFFFFU);
            timers_[idx].counter = 0U;
        } else if (reg == 0x8U)
            timers_[idx].target = static_cast<uint16_t>(val & 0xFFFFU);
        return;
    }

    if (p == 0x1F801810U) {
        gpu_.write_gp0(val);
        return;
    }
    if (p == 0x1F801814U) {
        gpu_.write_gp1(val);
        return;
    }
    if (p >= 0x1F801C00U && p < 0x1F802000U) {
        spu_.write16(p, static_cast<uint16_t>(val & 0xFFFFU));
        spu_.write16(p + 2U, static_cast<uint16_t>(val >> 16U));
        return;
    }
}

}  // namespace dragon::ps1
