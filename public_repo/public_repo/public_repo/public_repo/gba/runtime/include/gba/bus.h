#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <utility>
#include <vector>

#include "gba/apu.h"
#include "gba/cartridge.h"
#include "gba/cpu_state.h"
#include "gba/ppu.h"

namespace dragon::gba {

// Active-low GBA keypad button bits (1 = pressed in our input mask, inverted in KEYINPUT)
constexpr uint16_t kButtonA = 1U << 0;
constexpr uint16_t kButtonB = 1U << 1;
constexpr uint16_t kButtonSelect = 1U << 2;
constexpr uint16_t kButtonStart = 1U << 3;
constexpr uint16_t kButtonRight = 1U << 4;
constexpr uint16_t kButtonLeft = 1U << 5;
constexpr uint16_t kButtonUp = 1U << 6;
constexpr uint16_t kButtonDown = 1U << 7;
constexpr uint16_t kButtonR = 1U << 8;
constexpr uint16_t kButtonL = 1U << 9;

// Hardware Interrupt bitmasks (IE / IF)
constexpr uint16_t kIrqVBlank = 1U << 0;
constexpr uint16_t kIrqHBlank = 1U << 1;
constexpr uint16_t kIrqVCount = 1U << 2;
constexpr uint16_t kIrqTimer0 = 1U << 3;
constexpr uint16_t kIrqTimer1 = 1U << 4;
constexpr uint16_t kIrqTimer2 = 1U << 5;
constexpr uint16_t kIrqTimer3 = 1U << 6;
constexpr uint16_t kIrqSerial = 1U << 7;
constexpr uint16_t kIrqDma0 = 1U << 8;
constexpr uint16_t kIrqDma1 = 1U << 9;
constexpr uint16_t kIrqDma2 = 1U << 10;
constexpr uint16_t kIrqDma3 = 1U << 11;
constexpr uint16_t kIrqKeypad = 1U << 12;
constexpr uint16_t kIrqGamePak = 1U << 13;

class Bus {
public:
    using FrameCallback = void (*)(CpuState* cpu, Bus* bus, void* user_data);

    Bus();

    void reset();
    void load_cartridge(const Cartridge& cart);

    uint8_t read8(uint32_t addr) noexcept;
    uint16_t read16(uint32_t addr) noexcept;
    uint32_t read32(uint32_t addr) noexcept;

    void write8(uint32_t addr, uint8_t value) noexcept;
    void write16(uint32_t addr, uint16_t value) noexcept;
    void write32(uint32_t addr, uint32_t value) noexcept;

    void sync_hardware(uint64_t cpu_cycles) noexcept;

    void request_irq(uint16_t mask) noexcept { if_ |= mask; }
    uint16_t ie() const noexcept { return ie_; }
    uint16_t irf() const noexcept { return if_; }
    uint16_t ime() const noexcept { return ime_; }
    bool has_pending_irq() const noexcept {
        return (ime_ & 1U) != 0U && (ie_ & if_ & 0x3FFFU) != 0U;
    }
    bool has_wake_irq() const noexcept { return (ie_ & if_ & 0x3FFFU) != 0U; }

    uint16_t consume_acknowledged_irqs() noexcept {
        const uint16_t v = acknowledged_irqs_;
        acknowledged_irqs_ = 0U;
        return v;
    }

    void set_keypad(uint16_t pressed_mask) noexcept;
    uint16_t keyinput() const noexcept { return keyinput_; }

    Ppu& ppu() noexcept { return ppu_; }
    const Ppu& ppu() const noexcept { return ppu_; }
    Apu& apu() noexcept { return apu_; }
    const Apu& apu() const noexcept { return apu_; }

    void set_frame_callback(FrameCallback cb, void* user_data) noexcept {
        frame_cb_ = cb;
        frame_cb_user_ = user_data;
    }
    void on_frame_complete(CpuState* cpu) noexcept {
        if (frame_cb_) {
            frame_cb_(cpu, this, frame_cb_user_);
        }
    }

    void set_dispatch_table(const RecompFuncEntry* table, size_t count) noexcept {
        dispatch_table_ = table;
        dispatch_count_ = count;
    }
    RecompFunc lookup_function(uint32_t addr, uint8_t thumb) const noexcept;
    void log_fallback(uint32_t addr, uint8_t thumb, uint32_t from_pc);

    bool has_battery() const noexcept { return save_type_ != SaveType::None; }
    bool load_battery_ram(const std::filesystem::path& path);
    bool save_battery_ram(const std::filesystem::path& path) const;

    bool save_state(const CpuState& cpu, const std::filesystem::path& path) const;
    bool load_state(CpuState& cpu, const std::filesystem::path& path);

    struct DmaChannel {
        uint32_t sad = 0;
        uint32_t dad = 0;
        uint16_t cnt_l = 0;
        uint16_t cnt_h = 0;
        uint32_t internal_src = 0;
        uint32_t internal_dst = 0;
        uint32_t internal_count = 0;
    };

    struct Timer {
        uint16_t counter = 0;
        uint16_t reload = 0;
        uint16_t cnt_h = 0;
        uint32_t prescale_accum = 0;
    };

    struct HardwareStatePayload {
        std::array<DmaChannel, 4> dma{};
        std::array<Timer, 4> timers{};
        uint16_t keyinput = 0;
        uint16_t keycnt = 0;
        uint16_t ie = 0;
        uint16_t if_ = 0;
        uint16_t ime = 0;
        uint16_t waitcnt = 0;
        uint8_t postflg = 0;
        uint8_t haltcnt = 0;
        uint16_t acknowledged_irqs = 0;
        uint64_t last_sync_cycles = 0;
        Ppu::StatePayload ppu{};
    };

    static uint32_t normalize_rom_addr(uint32_t addr) noexcept {
        const uint32_t region = (addr >> 24U) & 0xFFU;
        if (region >= 0x08U && region <= 0x0DU) {
            return 0x08000000U | (addr & 0x01FFFFFFU);
        }
        return addr;
    }

private:
    enum class FlashState : uint8_t {
        Ready,
        Cmd1,
        Cmd2,
        Erase1,
        Erase2,
        WriteByte,
        BankSwitch,
    };

    uint8_t read_io8(uint32_t offset) noexcept;
    uint16_t read_io16(uint32_t offset) noexcept;
    void write_io8(uint32_t offset, uint8_t value) noexcept;
    void write_io16(uint32_t offset, uint16_t value) noexcept;

    uint8_t read_backup8(uint32_t addr) const noexcept;
    void write_backup8(uint32_t addr, uint8_t value) noexcept;

    uint16_t read_eeprom16() noexcept;
    void write_eeprom16(uint16_t value) noexcept;

    // GBA Cart I/O Port (GPIO) + Real-Time Clock (Seiko S-3511A), used by
    // Pokemon Ruby/Sapphire/Emerald (and Boktai) for the in-cart clock.
    // References: GBATEK "GBA Cart I/O Port (GPIO)" (problemkaputt.de/gbatek.htm)
    // for the register map and command-byte bit meaning; GBATEK leaves the
    // exact serial bit order/edges ambiguous, so the clocking below mirrors
    // mGBA's production implementation (github.com/mgba-emu/mgba,
    // src/gba/cart/gpio.c: _rtcReadPins / _rtcBeginCommand / _rtcProcessByte /
    // _rtcOutput) instead of guessing: bits clocked LSB-first, write bits
    // sampled on the SCK rising edge, read bits presented on the SCK falling
    // edge.
    bool gpio_region(uint32_t offset) const noexcept { return offset >= 0xC4U && offset <= 0xC9U; }
    uint16_t gpio_read16(uint32_t offset) noexcept;
    void gpio_write16(uint32_t offset, uint16_t value) noexcept;
    uint16_t gpio_data_read() const noexcept;
    void gpio_set_data(uint16_t new_value) noexcept;

    void rtc_begin_transfer() noexcept;
    void rtc_clock_write_bit(bool bit) noexcept;
    void rtc_clock_read_bit() noexcept;
    void rtc_decode_command(uint8_t command_byte) noexcept;
    void rtc_consume_written_byte(uint8_t byte_value) noexcept;
    void rtc_prepare_read_byte() noexcept;
    uint8_t rtc_current_register_byte(uint8_t index) const noexcept;
    void rtc_latch_current_time() noexcept;

    void step_timers(uint32_t cycles) noexcept;
    void trigger_timer_overflow(int idx) noexcept;

    void check_and_run_dma(uint8_t timing_mode, uint8_t fifo_req_mask = 0) noexcept;
    void execute_dma_channel(int ch) noexcept;

    void init_bios() noexcept;

    std::array<uint8_t, 0x4000> bios_{};
    std::array<uint8_t, 0x40000> ewram_{};
    std::array<uint8_t, 0x8000> iwram_{};
    std::vector<uint8_t> rom_;
    std::array<uint8_t, 0x20000> save_ram_{};  // Up to 128 KiB Flash / 64 KiB SRAM
    std::array<uint8_t, 0x2000> eeprom_{};     // Up to 8 KiB EEPROM

    SaveType save_type_ = SaveType::Sram;
    FlashState flash_state_ = FlashState::Ready;
    bool flash_id_mode_ = false;
    uint8_t flash_bank_ = 0;

    // EEPROM serial state
    std::vector<uint8_t> eeprom_bit_buffer_;
    uint32_t eeprom_read_address_ = 0;
    uint8_t eeprom_read_bits_remaining_ = 0;
    uint8_t eeprom_addr_bits_ = 6;  // 6 bits (512B) or 14 bits (8KB)

    // GPIO (Data/Direction/Control) + RTC serial state. See the method
    // declarations above for references.
    uint16_t gpio_data_ = 0U;
    uint16_t gpio_direction_ = 0U;
    uint16_t gpio_control_ = 0U;

    enum class RtcPhase : uint8_t { Idle, Command, TransferWrite, TransferRead };
    RtcPhase rtc_phase_ = RtcPhase::Idle;
    uint8_t rtc_bits_read_ = 0U;
    uint8_t rtc_shift_reg_ = 0U;
    uint8_t rtc_command_reg_ = 0U;  // 0=Reset,2=DateTime,3=ForceIRQ,4=Control,6=Time
    uint8_t rtc_bytes_remaining_ = 0U;
    uint8_t rtc_byte_index_ = 0U;
    bool rtc_sio_out_ = false;  // chip-driven SIO level while reading
    uint8_t rtc_control_ = 0U;  // status/control register (bit6=24h, bit7=power-off/lost)
    std::array<uint8_t, 7> rtc_datetime_{};  // BCD: year,month,day,dow,hour,min,sec

    Ppu ppu_;
    Apu apu_;

    std::array<DmaChannel, 4> dma_{};
    std::array<Timer, 4> timers_{};

    uint16_t keyinput_ = 0x03FFU;
    uint16_t keycnt_ = 0U;
    uint16_t ie_ = 0U;
    uint16_t if_ = 0U;
    uint16_t ime_ = 0U;
    uint16_t waitcnt_ = 0U;
    uint8_t postflg_ = 0U;
    uint8_t haltcnt_ = 0U;
    uint16_t acknowledged_irqs_ = 0U;

    uint64_t last_sync_cycles_ = 0ULL;
    uint32_t last_bios_read_ = 0xE129F000U;

    FrameCallback frame_cb_ = nullptr;
    void* frame_cb_user_ = nullptr;

    const RecompFuncEntry* dispatch_table_ = nullptr;
    size_t dispatch_count_ = 0;
    std::set<std::pair<uint32_t, uint8_t>> logged_fallbacks_;
};

}  // namespace dragon::gba
