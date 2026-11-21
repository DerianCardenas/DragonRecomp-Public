#pragma once

// Sega Genesis / Mega Drive Zilog Z80A Sound Coprocessor (3.579545 MHz).
// Owns 8 KiB Sound RAM ($0000-$1FFF), YM2612 ($4000-$4003), 9-bit serial 68K bank register
// ($6000), SN76489 PSG ($7F11), and 32 KiB 68K banking window ($8000-$FFFF).
// Reference: Zilog Z80 User Manual (UM0080), docs/GENESIS_SPEC.md §1.

#include <array>
#include <cstdint>

namespace dragon::md {

class Bus;

class MdZ80 {
public:
    MdZ80() noexcept;

    void reset() noexcept;

    // Bus control lines from M68000 ($A11100 BUSREQ, $A11200 RESET)
    void set_bus_request(bool requested) noexcept;
    void set_reset_line(bool in_reset) noexcept;
    bool is_bus_granted() const noexcept { return bus_requested_ || in_reset_; }
    bool is_running() const noexcept { return !bus_requested_ && !in_reset_; }
    bool is_in_reset() const noexcept { return in_reset_; }

    // 68K access to Z80 RAM ($A00000-$A01FFF) when bus is granted
    uint8_t read_ram(uint16_t offset) const noexcept { return ram_[offset & 0x1FFFU]; }
    void write_ram(uint16_t offset, uint8_t value) noexcept { ram_[offset & 0x1FFFU] = value; }

    // Trigger V-Int IRQ from VDP (scanline 224)
    void assert_irq(bool level) noexcept { irq_line_ = level; }

    // Advance Z80 by `z80_cycles` T-cycles
    void step(uint32_t z80_cycles, Bus* bus) noexcept;

    uint32_t bank_address() const noexcept { return static_cast<uint32_t>(bank_reg_) << 15U; }
    uint16_t pc() const noexcept { return pc_; }

    const std::array<uint8_t, 8192>& ram() const noexcept { return ram_; }
    std::array<uint8_t, 8192>& ram_mut() noexcept { return ram_; }

    struct Snapshot {
        std::array<uint8_t, 8192> ram{};
        uint8_t a = 0, f = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
        uint8_t a2 = 0, f2 = 0, b2 = 0, c2 = 0, d2 = 0, e2 = 0, h2 = 0, l2 = 0;
        uint16_t ix = 0, iy = 0, sp = 0x2000, pc = 0;
        uint8_t i = 0, r = 0, iff1 = 0, iff2 = 0, im = 0, ei_delay = 0, halted = 0;
        uint16_t bank_reg = 0;
        uint8_t bus_requested = 0;
        uint8_t in_reset = 1;
        uint8_t irq_line = 0;
    };

    void save_snapshot(Snapshot& s) const noexcept;
    void load_snapshot(const Snapshot& s) noexcept;

private:
    uint8_t mem_read(uint16_t addr, Bus* bus) noexcept;
    void mem_write(uint16_t addr, uint8_t val, Bus* bus) noexcept;
    uint16_t mem_read16(uint16_t addr, Bus* bus) noexcept;
    void mem_write16(uint16_t addr, uint16_t val, Bus* bus) noexcept;
    uint32_t exec_one(Bus* bus) noexcept;
    uint32_t exec_cb(Bus* bus) noexcept;
    uint32_t exec_ed(Bus* bus) noexcept;
    uint32_t exec_ixiy(uint16_t& reg, Bus* bus) noexcept;

    std::array<uint8_t, 8192> ram_{};
    uint8_t a_ = 0, f_ = 0, b_ = 0, c_ = 0, d_ = 0, e_ = 0, h_ = 0, l_ = 0;
    uint8_t a2_ = 0, f2_ = 0, b2_ = 0, c2_ = 0, d2_ = 0, e2_ = 0, h2_ = 0, l2_ = 0;
    uint16_t ix_ = 0, iy_ = 0, sp_ = 0x2000U, pc_ = 0;
    uint8_t i_ = 0, r_ = 0;
    uint8_t iff1_ = 0, iff2_ = 0, im_ = 0, ei_delay_ = 0, halted_ = 0;

    uint16_t bank_reg_ = 0;  // 9-bit serial bank register
    bool bus_requested_ = false;
    bool in_reset_ = true;  // Genesis boots with Z80 held in reset until 68K releases it
    bool irq_line_ = false;
    int32_t cycle_debt_ = 0;
};

}  // namespace dragon::md
