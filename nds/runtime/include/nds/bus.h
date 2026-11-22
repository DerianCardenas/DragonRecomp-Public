#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "nds/cartridge.h"
#include "nds/cpu_state.h"
#include "nds/gx3d.h"
#include "nds/ppu2d.h"
#include "nds/spu.h"

namespace dragon::nds {

// NDS KEYINPUT (0x04000130, active-low) & EXTKEYIN (0x04000136, active-low)
constexpr uint16_t kBtnA = 1U << 0;
constexpr uint16_t kBtnB = 1U << 1;
constexpr uint16_t kBtnSelect = 1U << 2;
constexpr uint16_t kBtnStart = 1U << 3;
constexpr uint16_t kBtnRight = 1U << 4;
constexpr uint16_t kBtnLeft = 1U << 5;
constexpr uint16_t kBtnUp = 1U << 6;
constexpr uint16_t kBtnDown = 1U << 7;
constexpr uint16_t kBtnR = 1U << 8;
constexpr uint16_t kBtnL = 1U << 9;
constexpr uint16_t kBtnX = 1U << 10;
constexpr uint16_t kBtnY = 1U << 11;

// IRQ bits (shared definitions across ARM9/ARM7)
constexpr uint32_t kIrqVBlank = 1U << 0;
constexpr uint32_t kIrqHBlank = 1U << 1;
constexpr uint32_t kIrqVCount = 1U << 2;
constexpr uint32_t kIrqTimer0 = 1U << 3;
constexpr uint32_t kIrqTimer1 = 1U << 4;
constexpr uint32_t kIrqTimer2 = 1U << 5;
constexpr uint32_t kIrqTimer3 = 1U << 6;
constexpr uint32_t kIrqDma0 = 1U << 8;
constexpr uint32_t kIrqDma1 = 1U << 9;
constexpr uint32_t kIrqDma2 = 1U << 10;
constexpr uint32_t kIrqDma3 = 1U << 11;
constexpr uint32_t kIrqIpcSync = 1U << 16;
constexpr uint32_t kIrqIpcSendEmpty = 1U << 17;
constexpr uint32_t kIrqIpcRecvNotEmpty = 1U << 18;
constexpr uint32_t kIrqCardComplete = 1U << 19;
constexpr uint32_t kIrqGxFifo = 1U << 21;

class Bus {
public:
    static constexpr uint32_t kScreenWidth = 256U;
    static constexpr uint32_t kScreenHeight = 384U;  // Top 192 + Bottom 192
    static constexpr uint32_t kSingleHeight = 192U;
    static constexpr uint32_t kTotalScanlines = 263U;
    static constexpr uint32_t kArm9CyclesPerLine = 4260U;  // 67.03 MHz / (263 * 59.826)
    static constexpr uint32_t kArm7CyclesPerLine = 2130U;

    Bus();

    bool load_rom(const std::filesystem::path& path, std::string* err = nullptr);
    bool load_rom_bytes(const std::vector<uint8_t>& raw, std::string* err = nullptr);
    void reset(NdsCpuState& arm9, NdsCpuState& arm7) noexcept;

    // Input & Touchscreen
    void set_buttons(uint16_t pressed_mask) noexcept { buttons_ = pressed_mask; }
    void set_touch(bool down, uint8_t x, uint8_t y) noexcept {
        touch_down_ = down;
        touch_x_ = x;
        touch_y_ = y;
    }

    // Dual-CPU Memory Access
    uint8_t arm9_read8(uint32_t addr, const NdsCpuState* cpu = nullptr) noexcept;
    uint16_t arm9_read16(uint32_t addr, const NdsCpuState* cpu = nullptr) noexcept;
    uint32_t arm9_read32(uint32_t addr, const NdsCpuState* cpu = nullptr) noexcept;
    void arm9_write8(uint32_t addr, uint8_t val, NdsCpuState* cpu = nullptr) noexcept;
    void arm9_write16(uint32_t addr, uint16_t val, NdsCpuState* cpu = nullptr) noexcept;
    void arm9_write32(uint32_t addr, uint32_t val, NdsCpuState* cpu = nullptr) noexcept;

    uint8_t arm7_read8(uint32_t addr) noexcept;
    uint16_t arm7_read16(uint32_t addr) noexcept;
    uint32_t arm7_read32(uint32_t addr) noexcept;
    void arm7_write8(uint32_t addr, uint8_t val) noexcept;
    void arm7_write16(uint32_t addr, uint16_t val) noexcept;
    void arm7_write32(uint32_t addr, uint32_t val) noexcept;

    // VRAM / Palette / OAM helpers for PPU2D & GX3D
    uint8_t read_vram_bg_a(uint32_t offset) const noexcept;
    uint8_t read_vram_obj_a(uint32_t offset) const noexcept;
    uint8_t read_vram_bg_b(uint32_t offset) const noexcept;
    uint8_t read_vram_obj_b(uint32_t offset) const noexcept;
    uint8_t read_vram_tex(uint32_t offset) const noexcept;
    uint8_t read_vram_tex_pal(uint32_t offset) const noexcept;
    uint16_t read_pal16(bool engine_a, uint32_t offset) const noexcept;
    uint16_t read_bg_ext_pal16(bool engine_a, int slot, uint32_t pal_idx,
                               uint32_t color_idx) const noexcept;
    uint16_t read_obj_ext_pal16(bool engine_a, uint32_t pal_idx, uint32_t color_idx) const noexcept;
    uint16_t read_oam16(bool engine_a, uint32_t offset) const noexcept;

    // SPU memory read helper (ARM7 Main RAM / WRAM)
    uint8_t spu_read8(uint32_t addr) const noexcept;

    // Hardware stepping & IRQ
    void step_scanline(NdsCpuState& arm9, NdsCpuState& arm7) noexcept;
    void sync_arm9_cycles(uint64_t arm9_cycles, NdsCpuState& arm9) noexcept;

    void raise_arm9_irq(uint32_t bit) noexcept { arm9_if_ |= bit; }
    void raise_arm7_irq(uint32_t bit) noexcept { arm7_if_ |= bit; }
    bool arm9_irq_pending() const noexcept {
        return (arm9_ime_ & 1U) && ((arm9_ie_ & arm9_if_) != 0U);
    }
    bool arm7_irq_pending() const noexcept {
        return (arm7_ime_ & 1U) && ((arm7_ie_ & arm7_if_) != 0U);
    }
    uint32_t arm9_ie() const noexcept { return arm9_ie_; }
    uint32_t arm9_if() const noexcept { return arm9_if_; }
    uint32_t arm7_ie() const noexcept { return arm7_ie_; }
    uint32_t arm7_if() const noexcept { return arm7_if_; }
    void clear_arm9_if(uint32_t mask) noexcept { arm9_if_ &= ~mask; }
    void clear_arm7_if(uint32_t mask) noexcept { arm7_if_ &= ~mask; }

    // Framebuffer & Audio
    const std::array<uint32_t, kScreenWidth * kScreenHeight>& framebuffer() const noexcept {
        return combined_fb_;
    }
    bool top_is_engine_a() const noexcept { return (powcnt1_ & (1U << 15)) != 0U; }
    size_t drain_audio_samples(int16_t* dst, size_t max_frames) noexcept {
        return spu_.drain_samples(dst, max_frames);
    }

    // Accessors
    Cartridge& cartridge() noexcept { return cart_; }
    const Cartridge& cartridge() const noexcept { return cart_; }
    Engine2D& engine_a() noexcept { return engine_a_; }
    Engine2D& engine_b() noexcept { return engine_b_; }
    Gx3d& gx3d() noexcept { return gx3d_; }
    Spu& spu() noexcept { return spu_; }
    NdsCpuState* arm7_state() noexcept { return arm7_ptr_; }
    uint32_t vcount() const noexcept { return vcount_; }
    uint64_t frame_count() const noexcept { return frame_count_; }

    // Static recompiler dispatch table (ARM9)
    void register_recomp_functions(const NdsRecompFuncEntry* entries, size_t count);
    NdsRecompFunc lookup_recomp_function(uint32_t addr, bool thumb) const noexcept;
    void set_recomp_mode(bool enabled) noexcept { recomp_mode_ = enabled; }
    bool recomp_mode() const noexcept { return recomp_mode_; }

    using FrameCallback = void (*)(NdsCpuState* arm9, Bus* bus, void* user_data);
    void set_frame_callback(FrameCallback cb, void* user_data) noexcept {
        frame_cb_ = cb;
        frame_cb_user_ = user_data;
    }

    // Save states
    bool save_state(const NdsCpuState& arm9, const NdsCpuState& arm7,
                    const std::filesystem::path& path) const;
    bool load_state(NdsCpuState& arm9, NdsCpuState& arm7, const std::filesystem::path& path);

    // Direct access for tests
    uint8_t* main_ram() noexcept { return main_ram_.data(); }
    uint8_t* dtcm() noexcept { return dtcm_.data(); }
    uint8_t* itcm() noexcept { return itcm_.data(); }

private:
    Cartridge cart_;
    Engine2D engine_a_{true};
    Engine2D engine_b_{false};
    Gx3d gx3d_;
    Spu spu_;

    // Memory pools
    std::vector<uint8_t> main_ram_;      // 4 MB
    std::array<uint8_t, 32768> itcm_{};  // 32 KB
    std::array<uint8_t, 16384> dtcm_{};  // 16 KB
    std::array<uint8_t, 32768> shared_wram_{};
    std::array<uint8_t, 65536> arm7_wram_{};
    std::array<uint8_t, 2048> palette_ram_{};
    std::array<uint8_t, 2048> oam_{};

    // VRAM Banks A..I (total 656 KB)
    std::array<uint8_t, 131072> vram_a_{};
    std::array<uint8_t, 131072> vram_b_{};
    std::array<uint8_t, 131072> vram_c_{};
    std::array<uint8_t, 131072> vram_d_{};
    std::array<uint8_t, 65536> vram_e_{};
    std::array<uint8_t, 16384> vram_f_{};
    std::array<uint8_t, 16384> vram_g_{};
    std::array<uint8_t, 32768> vram_h_{};
    std::array<uint8_t, 16384> vram_i_{};
    std::array<uint8_t, 9> vramcnt_{};  // A,B,C,D,E,F,G,H,I
    uint8_t wramcnt_ = 0x03U;

    // Combined 256x384 framebuffer
    std::array<uint32_t, kScreenWidth * kScreenHeight> combined_fb_{};

    // Display & timing
    uint16_t dispstat9_ = 0;
    uint16_t dispstat7_ = 0;
    uint16_t vcount_ = 0;
    uint16_t powcnt1_ = 0x820FU;
    uint16_t exmemcnt_ = 0;
    uint8_t postflg9_ = 1U;
    uint8_t postflg7_ = 1U;
    uint64_t last_sync_arm9_ = 0;
    uint64_t scanline_cycle_acc_ = 0;
    uint64_t frame_count_ = 0;

    // Interrupts
    uint32_t arm9_ime_ = 0, arm9_ie_ = 0, arm9_if_ = 0;
    uint32_t arm7_ime_ = 0, arm7_ie_ = 0, arm7_if_ = 0;

    // IPC (IPCSYNC + 16-entry bidirectional FIFOs)
    uint16_t ipcsync9_ = 0;
    uint16_t ipcsync7_ = 0;
    uint16_t ipcfifocnt9_ = 0;
    uint16_t ipcfifocnt7_ = 0;
    std::array<uint32_t, 16> fifo_9to7_{};
    uint8_t fifo_9to7_r_ = 0, fifo_9to7_w_ = 0, fifo_9to7_cnt_ = 0;
    uint32_t fifo_9to7_last_ = 0;
    std::array<uint32_t, 16> fifo_7to9_{};
    uint8_t fifo_7to9_r_ = 0, fifo_7to9_w_ = 0, fifo_7to9_cnt_ = 0;
    uint32_t fifo_7to9_last_ = 0;

    // Hardware Math (ARM9)
    uint16_t divcnt_ = 0;
    int64_t div_numer_ = 0;
    int64_t div_denom_ = 0;
    int64_t div_result_ = 0;
    int64_t div_rem_ = 0;
    uint16_t sqrtcnt_ = 0;
    uint64_t sqrt_param_ = 0;
    uint32_t sqrt_result_ = 0;

    // DMA (4 channels per CPU)
    struct DmaChannel {
        uint32_t sad = 0;
        uint32_t dad = 0;
        uint32_t cnt = 0;
        uint32_t int_sad = 0;
        uint32_t int_dad = 0;
    };
    std::array<DmaChannel, 4> dma9_{};
    std::array<DmaChannel, 4> dma7_{};
    std::array<uint32_t, 4> dma9_fill_{};

    // Timers (4 channels per CPU)
    struct TimerChannel {
        uint16_t reload = 0;
        uint16_t counter = 0;
        uint16_t cnt = 0;
        uint32_t sub_cycles = 0;
    };
    std::array<TimerChannel, 4> tmr9_{};
    std::array<TimerChannel, 4> tmr7_{};

    // Input & Touchscreen + ARM7 SPI
    uint16_t buttons_ = 0;
    bool touch_down_ = false;
    uint8_t touch_x_ = 0;
    uint8_t touch_y_ = 0;
    uint16_t spicnt_ = 0;
    uint8_t spidata_ = 0;
    uint16_t tsc_pending_ = 0;
    uint8_t tsc_byte_ = 0;
    uint8_t fw_cmd_ = 0;
    uint32_t fw_pos_ = 0;
    uint32_t fw_addr_ = 0;
    uint8_t fw_status_ = 0;
    uint8_t pmic_cmd_ = 0;
    uint8_t pmic_pos_ = 0;
    std::array<uint8_t, 8> pmic_regs_{0x0DU, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U};
    std::vector<uint8_t> fw_flash_;  // 256 KB DS Firmware SPI Flash (NVRAM)
    uint8_t rtc_reg_ = 0;

    // Recompiler & callbacks
    NdsCpuState* arm7_ptr_ = nullptr;
    std::unordered_map<uint64_t, NdsRecompFunc> recomp_table_;
    std::array<uint8_t, 1024>
        ram_page_dirty_{};  // 4 KB pages of 4 MB Main RAM (SMC / BLZ / overlays)
    bool recomp_mode_ = false;
    bool trace_ipc_ = false;
    FrameCallback frame_cb_ = nullptr;
    void* frame_cb_user_ = nullptr;

    void update_div() noexcept;
    void update_sqrt() noexcept;
    void run_dma9(int ch, int mode_filter, NdsCpuState* cpu) noexcept;
    void run_dma7(int ch, int mode_filter) noexcept;
    void step_timers(std::array<TimerChannel, 4>& tmrs, uint32_t cycles, bool is_arm9) noexcept;
    void spi_transfer(uint8_t val) noexcept;
    void write_vram_lcdc(uint32_t offset, uint8_t val) noexcept;
    uint8_t read_vram_lcdc(uint32_t offset) const noexcept;
    void write_vram_region(uint32_t addr, uint8_t val) noexcept;
    uint8_t read_vram_region(uint32_t addr) const noexcept;
    void compose_screens() noexcept;
    void init_firmware_settings() noexcept;
};

}  // namespace dragon::nds
