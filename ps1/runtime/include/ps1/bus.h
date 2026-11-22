#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ps1/cdrom.h"
#include "ps1/gpu.h"
#include "ps1/spu.h"

namespace dragon::ps1 {

struct LoadedPs1Image;

// PS1 Digital Pad button bits (active-low in SIO0; stored active-high here for clarity)
enum Ps1PadButton : uint16_t {
    kPs1PadSelect = 1U << 0U,
    kPs1PadL3 = 1U << 1U,
    kPs1PadR3 = 1U << 2U,
    kPs1PadStart = 1U << 3U,
    kPs1PadUp = 1U << 4U,
    kPs1PadRight = 1U << 5U,
    kPs1PadDown = 1U << 6U,
    kPs1PadLeft = 1U << 7U,
    kPs1PadL2 = 1U << 8U,
    kPs1PadR2 = 1U << 9U,
    kPs1PadL1 = 1U << 10U,
    kPs1PadR1 = 1U << 11U,
    kPs1PadTriangle = 1U << 12U,
    kPs1PadCircle = 1U << 13U,
    kPs1PadCross = 1U << 14U,
    kPs1PadSquare = 1U << 15U,
};

// PS1 Hardware IRQ bits (I_STAT / I_MASK at 0x1F801070 / 0x1F801074)
enum Ps1IrqBit : uint32_t {
    kPs1IrqVBlank = 1U << 0U,
    kPs1IrqGpu = 1U << 1U,
    kPs1IrqCdrom = 1U << 2U,
    kPs1IrqDma = 1U << 3U,
    kPs1IrqTimer0 = 1U << 4U,
    kPs1IrqTimer1 = 1U << 5U,
    kPs1IrqTimer2 = 1U << 6U,
    kPs1IrqSio0 = 1U << 7U,
    kPs1IrqSio1 = 1U << 8U,
    kPs1IrqSpu = 1U << 9U,
};

class Ps1Bus {
public:
    static constexpr uint32_t kMainRamSize = 2U * 1024U * 1024U;  // 2 MB
    static constexpr uint32_t kScratchpadSize = 1024U;            // 1 KB
    static constexpr uint32_t kBiosSize = 512U * 1024U;           // 512 KB

    Ps1Bus();
    void reset();
    void load_image(const LoadedPs1Image& img);

    uint8_t read8(uint32_t addr) noexcept;
    uint16_t read16(uint32_t addr) noexcept;
    uint32_t read32(uint32_t addr) noexcept;

    void write8(uint32_t addr, uint8_t val) noexcept;
    void write16(uint32_t addr, uint16_t val) noexcept;
    void write32(uint32_t addr, uint32_t val) noexcept;

    void request_irq(uint32_t irq_bit) noexcept;
    bool irq_pending() const noexcept { return (i_stat_ & i_mask_) != 0U; }
    uint32_t i_stat() const noexcept { return i_stat_; }
    uint32_t i_mask() const noexcept { return i_mask_; }

    void set_pad_buttons(uint16_t active_high_mask) noexcept { pad_buttons_ = active_high_mask; }
    uint16_t pad_buttons() const noexcept { return pad_buttons_; }

    void step_timers(uint32_t cpu_cycles) noexcept;
    void on_vblank() noexcept;

    Ps1Gpu& gpu() noexcept { return gpu_; }
    const Ps1Gpu& gpu() const noexcept { return gpu_; }
    Ps1Spu& spu() noexcept { return spu_; }
    const Ps1Spu& spu() const noexcept { return spu_; }
    Ps1Cdrom& cdrom() noexcept { return cdrom_; }

    uint8_t* main_ram() noexcept { return main_ram_.data(); }
    const uint8_t* main_ram() const noexcept { return main_ram_.data(); }
    uint8_t* scratchpad() noexcept { return scratchpad_.data(); }
    const uint8_t* scratchpad() const noexcept { return scratchpad_.data(); }

    // Direct RAM pointer helper for BIOS HLE string/buffer access
    uint8_t* ram_ptr(uint32_t addr) noexcept;
    const uint8_t* ram_ptr(uint32_t addr) const noexcept;

private:
    struct DmaChannel {
        uint32_t madr = 0;
        uint32_t bcr = 0;
        uint32_t chcr = 0;
    };

    struct Timer {
        uint32_t counter = 0;
        uint16_t mode = 0;
        uint16_t target = 0;
        uint32_t div_accum = 0;
    };

    void install_hle_bios_trampolines() noexcept;
    void run_dma(int ch) noexcept;
    void sio0_transfer(uint8_t tx) noexcept;

    std::vector<uint8_t> main_ram_;
    std::array<uint8_t, kScratchpadSize> scratchpad_{};
    std::vector<uint8_t> bios_;

    Ps1Gpu gpu_;
    Ps1Spu spu_;
    Ps1Cdrom cdrom_;

    // Interrupt Controller
    uint32_t i_stat_ = 0;
    uint32_t i_mask_ = 0;

    // DMA Controller
    std::array<DmaChannel, 7> dma_{};
    uint32_t dpcr_ = 0x07654321U;
    uint32_t dicr_ = 0;

    // Timers 0..2
    std::array<Timer, 3> timers_{};

    // SIO0 Pad state machine
    uint16_t pad_buttons_ = 0;
    uint16_t sio_ctrl_ = 0;
    uint16_t sio_mode_ = 0;
    uint16_t sio_baud_ = 0x0088;
    uint8_t sio_rx_byte_ = 0xFF;
    bool sio_rx_ready_ = false;
    int sio_step_ = 0;
    uint8_t sio_device_ = 0;
};

}  // namespace dragon::ps1
