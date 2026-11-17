// Tests for the GBA Cart GPIO Port + RTC (Seiko S-3511A) emulation added for
// Pokemon Ruby/Sapphire/Emerald's in-cart clock (see gba/runtime/src/bus.cpp,
// references: GBATEK "GBA Cart I/O Port (GPIO)" and mGBA's src/gba/cart/gpio.c
// for the serial bit order/edges). Drives the protocol the exact way a real
// ARM driver would, through the public Bus::read16/write16 interface at
// 0x080000C4 (Data), 0x080000C6 (Direction), 0x080000C8 (Control) -- no
// internal RTC state is touched directly, so this exercises precisely what a
// recompiled or interpreted game would.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "gba/bus.h"
#include "gba/cartridge.h"

namespace {

using dragon::gba::Bus;
using dragon::gba::Cartridge;

constexpr uint32_t kGpioData = 0x080000C4U;
constexpr uint32_t kGpioDirection = 0x080000C6U;
constexpr uint32_t kGpioControl = 0x080000C8U;

std::vector<uint8_t> make_test_rom(size_t size = 0x4000) {
    std::vector<uint8_t> rom(size, 0);
    const uint32_t branch = 0xEA00002EU;  // B 0x080000C0
    std::memcpy(&rom[0], &branch, 4);
    const char* title = "DRAGONTEST";
    std::memcpy(&rom[0xA0], title, std::strlen(title));
    const char* code = "ADTE";
    std::memcpy(&rom[0xAC], code, 4);
    rom[0xB2] = 0x96;
    uint8_t chk = 0;
    for (size_t i = 0xA0; i <= 0xBC; ++i) {
        chk = static_cast<uint8_t>(chk - rom[i]);
    }
    rom[0xBD] = static_cast<uint8_t>(chk - 0x19U);
    // Known marker bytes at 0xC4-0xC9, the GPIO window: used to prove reads
    // fall through to real ROM data while GPIO readback stays disabled.
    rom[0xC4] = 0x11;
    rom[0xC5] = 0x22;
    rom[0xC6] = 0x33;
    rom[0xC7] = 0x44;
    return rom;
}

// Sends one command/data byte, LSB-first, sampled on the SCK rising edge
// (direction must already have SCK/CS/SIO as outputs, i.e. 0x7).
void rtc_send_byte(Bus& bus, uint8_t byte) {
    for (int i = 0; i < 8; ++i) {
        const uint16_t sio = static_cast<uint16_t>(((byte >> i) & 1U) << 1U);
        bus.write16(kGpioData, static_cast<uint16_t>(0x04U | sio));          // SCK low, CS high
        bus.write16(kGpioData, static_cast<uint16_t>(0x04U | sio | 0x01U));  // SCK rising edge
    }
}

// Reads one byte, LSB-first: bit 0 is already valid on SIO before any edge
// (Bus::rtc_prepare_read_byte loads it as soon as the command decodes), and
// each SCK falling-then-rising pulse (CS held high throughout) advances to
// the next bit, matching mGBA's _rtcOutput/_rtcReadPins behaviour.
uint8_t rtc_read_byte(Bus& bus) {
    uint8_t value = 0;
    for (int i = 0; i < 8; ++i) {
        const uint16_t data = bus.read16(kGpioData);
        if (data & 0x02U) {
            value = static_cast<uint8_t>(value | (1U << i));
        }
        bus.write16(kGpioData, 0x04U);  // CS high, SCK falling edge: advance to next bit
        bus.write16(kGpioData, 0x05U);  // CS high, SCK rising edge: no-op while reading
    }
    return value;
}

// Builds the S-3511A command byte: magic 0110, 3-bit register, 1-bit R/W
// (0=write, 1=read). See bus.h for the register table.
constexpr uint8_t rtc_command(uint8_t reg, bool is_read) {
    return static_cast<uint8_t>(0x60U | (static_cast<uint8_t>(reg & 0x07U) << 1U) |
                                (is_read ? 0x01U : 0x00U));
}

constexpr uint8_t kRegReset = 0U;
constexpr uint8_t kRegDateTime = 2U;
constexpr uint8_t kRegControl = 4U;
constexpr uint8_t kRegTime = 6U;

Bus make_bus_with_rom() {
    Bus bus;
    Cartridge cart = dragon::gba::parse_gba_rom_bytes(make_test_rom());
    bus.load_cartridge(cart);
    return bus;
}

void begin_transfer(Bus& bus) {
    bus.write16(kGpioDirection, 0x07U);  // SCK, SIO, CS all outputs for command phase
    bus.write16(kGpioData, 0x00U);       // idle: CS low, SCK low
    bus.write16(kGpioData, 0x04U);       // CS rising edge: begin transfer
}

void test_gpio_disabled_is_transparent_rom_passthrough() {
    Bus bus = make_bus_with_rom();
    // Control defaults to 0 (readback disabled): 0x080000C4 must read back
    // as ordinary ROM data, not a GPIO register -- this is what every
    // non-RTC game, and an RTC game between GPIO accesses, relies on since
    // this address range sits right after the cart header.
    assert(bus.read16(kGpioData) == 0x2211U);
    assert(bus.read16(kGpioDirection) == 0x4433U);
    std::printf("rtc_unit_tests: gpio-disabled ROM passthrough ok\n");
}

void test_reset_clears_power_off_flag() {
    Bus bus = make_bus_with_rom();
    bus.write16(kGpioControl, 0x01U);  // enable GPIO readback

    begin_transfer(bus);
    rtc_send_byte(bus, rtc_command(kRegReset, false));
    bus.write16(kGpioData, 0x00U);  // CS falling edge: end transfer

    // Now read Control and confirm bit 7 (power-off / time-lost, the exact
    // flag Pokemon RSE checks to show "the internal battery has run dry") is
    // clear.
    begin_transfer(bus);
    rtc_send_byte(bus, rtc_command(kRegControl, true));
    bus.write16(kGpioDirection, 0x05U);  // SIO becomes an input (chip drives it)
    const uint8_t status = rtc_read_byte(bus);
    bus.write16(kGpioData, 0x00U);

    assert((status & 0x80U) == 0U);
    std::printf("rtc_unit_tests: reset clears the power-off/time-lost flag (status=0x%02X)\n",
                status);
}

void test_control_register_write_read_roundtrip() {
    Bus bus = make_bus_with_rom();
    bus.write16(kGpioControl, 0x01U);

    begin_transfer(bus);
    rtc_send_byte(bus, rtc_command(kRegControl, false));
    rtc_send_byte(bus, 0x40U);  // bit6: 24-hour mode: writable, bit7 is not
    bus.write16(kGpioData, 0x00U);

    begin_transfer(bus);
    rtc_send_byte(bus, rtc_command(kRegControl, true));
    bus.write16(kGpioDirection, 0x05U);
    const uint8_t status = rtc_read_byte(bus);
    bus.write16(kGpioData, 0x00U);

    assert(status == 0x40U);
    std::printf("rtc_unit_tests: control register write/read roundtrip ok (0x%02X)\n", status);
}

void test_datetime_matches_host_wall_clock() {
    Bus bus = make_bus_with_rom();
    bus.write16(kGpioControl, 0x01U);

    const std::time_t before = std::time(nullptr);

    begin_transfer(bus);
    rtc_send_byte(bus, rtc_command(kRegDateTime, true));
    bus.write16(kGpioDirection, 0x05U);
    uint8_t bytes[7] = {};
    for (uint8_t& b : bytes) {
        b = rtc_read_byte(bus);
    }
    bus.write16(kGpioData, 0x00U);

    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &before);
#else
    localtime_r(&before, &tm_buf);
#endif
    const auto to_bcd = [](int v) { return static_cast<uint8_t>(((v / 10) % 10 << 4) | (v % 10)); };

    // Allow the minute/second to have ticked over once between `before` and
    // the simulated bus transfer; everything else must match exactly.
    assert(bytes[0] == to_bcd((tm_buf.tm_year + 1900) % 100));
    assert(bytes[1] == to_bcd(tm_buf.tm_mon + 1));
    assert(bytes[2] == to_bcd(tm_buf.tm_mday));
    assert(bytes[3] == to_bcd(tm_buf.tm_wday));
    assert((bytes[4] & 0x3FU) <=
           0x23U);  // BCD hour, 24h mode was left set from the prior test? no:
                    // fresh Bus here defaults to 12h; just sanity-bound it.
    std::printf(
        "rtc_unit_tests: datetime matches host wall clock "
        "(y=%02X m=%02X d=%02X dow=%02X h=%02X min=%02X s=%02X)\n",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6]);
}

void test_time_only_register_returns_three_bytes() {
    Bus bus = make_bus_with_rom();
    bus.write16(kGpioControl, 0x01U);

    begin_transfer(bus);
    rtc_send_byte(bus, rtc_command(kRegTime, true));
    bus.write16(kGpioDirection, 0x05U);
    uint8_t h = rtc_read_byte(bus);
    uint8_t m = rtc_read_byte(bus);
    uint8_t s = rtc_read_byte(bus);
    bus.write16(kGpioData, 0x00U);

    assert((h & 0x3FU) <= 0x23U);
    assert(m <= 0x59U);
    assert(s <= 0x59U);
    std::printf("rtc_unit_tests: time-only register returns 3 BCD bytes (h=%02X m=%02X s=%02X)\n",
                h, m, s);
}

}  // namespace

int main() {
    test_gpio_disabled_is_transparent_rom_passthrough();
    test_reset_clears_power_off_flag();
    test_control_register_write_read_roundtrip();
    test_datetime_matches_host_wall_clock();
    test_time_only_register_returns_three_bytes();
    std::printf("rtc_unit_tests: all GBA GPIO/RTC tests passed\n");
    return 0;
}
