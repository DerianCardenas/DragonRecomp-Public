#include "pce/bus.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace dragon::pce {

namespace {

constexpr uint8_t kStateMagic[8] = {'D', 'R', 'S', 'T', 'P', 'C', 'E', '1'};
constexpr uint32_t kStateVersion = 2U;

struct SerializedCpu {
    uint8_t a, x, y, s, p;
    uint16_t pc;
    uint8_t mpr[8];
    uint8_t last_mpr, speed_high, nmi_pending;
    uint64_t cycles;
};

}  // namespace

Bus::Bus() = default;

void Bus::reset(PceCpuState& cpu) noexcept {
    cart_.reset();
    vdc_.reset();
    psg_.reset();
    wram_.fill(0x00U);

    input_ = {};
    joy_sel_ = 0U;
    io_buffer_ = 0U;
    timer_reload_ = 0U;
    timer_counter_ = 0U;
    timer_enabled_ = 0U;
    timer_prescaler_ = 0U;
    irq_disable_ = 0U;
    irq_status_ = 0U;
    last_sync_cycles_ = 0U;
    fallback_count_ = 0U;
    stop_requested_ = false;

    std::memset(&cpu, 0, sizeof(cpu));
    cpu.s = 0xFFU;
    cpu.p = PCE_FLAG_I;  // Interrupts masked, T=0, D=0
    cpu.mpr[0] = 0xFFU;  // Hardware I/O
    cpu.mpr[1] = 0xF8U;  // Work RAM (Zero Page $2000, Stack $2100)
    cpu.mpr[2] = 0x00U;
    cpu.mpr[3] = 0x00U;
    cpu.mpr[4] = 0x00U;
    cpu.mpr[5] = 0x00U;
    cpu.mpr[6] = 0x00U;
    cpu.mpr[7] = 0x00U;  // Boot bank 0 ($E000-$FFFF)
    cpu.last_mpr = 0x00U;
    cpu.speed_high = 0U;  // HuC6280 boots in low speed; games execute CSH ($D4)
    cpu.next_event = Vdc::kCyclesPerScanline;
    cpu.bus = this;
    cpu.wram = wram_.data();
    cpu.rom = cart_.rom().empty() ? nullptr : cart_.rom().data();
    cpu.rom_size = static_cast<uint32_t>(cart_.rom().size());

    // Fetch RESET vector from $FFFE-$FFFF (bank 0)
    const uint8_t lo = read(cpu, PCE_VEC_RESET);
    const uint8_t hi = read(cpu, static_cast<uint16_t>(PCE_VEC_RESET + 1U));
    cpu.pc = static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8U));
}

uint8_t Bus::io_read(uint16_t offset) noexcept {
    const uint16_t region = offset & 0x1C00U;
    switch (region) {
        case 0x0000U:  // $0000-$03FF: HuC6270 VDC (ignore SGX VPC/VDC2 at $0008-$001F)
            if ((offset & 0x18U) == 0x00U) {
                return vdc_.read_vdc(static_cast<uint8_t>(offset & 0x03U));
            }
            return 0x00U;
        case 0x0400U:  // $0400-$07FF: HuC6260 VCE
            return vdc_.read_vce(static_cast<uint8_t>(offset & 0x07U));
        case 0x0800U:  // $0800-$0BFF: PSG (write-only -> returns io_buffer_)
            return io_buffer_;
        case 0x0C00U: {  // $0C00-$0FFF: Timer
            // During the underflow/reload transition window, HuC6280 7-bit counter wraps via 0x7F.
            uint8_t cnt = timer_counter_ & 0x7FU;
            if (timer_enabled_ != 0U && cnt == 0U && timer_prescaler_ >= 992U) {
                cnt = 0x7FU;
            }
            io_buffer_ = static_cast<uint8_t>((io_buffer_ & 0x80U) | cnt);
            return io_buffer_;
        }
        case 0x1000U: {  // $1000-$13FF: Joypad I/O
            // Bit 7: 1 (no CD-ROM attached), Bit 6: 1=Japan PCE / 0=US TG-16, Bits 5-4: 1
            uint8_t v = cart_.is_us_region() ? 0xBFU : 0xFFU;
            if ((joy_sel_ & 0x02U) != 0U) {
                // CLR asserted: low nibble reads 0x0
                v &= 0xF0U;
            } else if ((joy_sel_ & 0x01U) != 0U) {
                // SEL=1: D-pad (Bit 0=Up, Bit 1=Right, Bit 2=Down, Bit 3=Left; active-low)
                if (input_.up) v &= static_cast<uint8_t>(~0x01U);
                if (input_.right) v &= static_cast<uint8_t>(~0x02U);
                if (input_.down) v &= static_cast<uint8_t>(~0x04U);
                if (input_.left) v &= static_cast<uint8_t>(~0x08U);
            } else {
                // SEL=0: Buttons (Bit 0=I, Bit 1=II, Bit 2=Select, Bit 3=Run; active-low)
                if (input_.button1) v &= static_cast<uint8_t>(~0x01U);
                if (input_.button2) v &= static_cast<uint8_t>(~0x02U);
                if (input_.select) v &= static_cast<uint8_t>(~0x04U);
                if (input_.run) v &= static_cast<uint8_t>(~0x08U);
            }
            io_buffer_ = v;
            return v;
        }
        case 0x1400U: {  // $1400-$17FF: Interrupt Controller
            const uint8_t reg = static_cast<uint8_t>(offset & 0x03U);
            if (reg == 2U) {
                io_buffer_ = static_cast<uint8_t>((io_buffer_ & 0xF8U) | (irq_disable_ & 0x07U));
                return io_buffer_;
            }
            if (reg == 3U) {
                uint8_t st = static_cast<uint8_t>(irq_status_ & kTiqBit);
                if (vdc_.irq_pending()) {
                    st |= kIrq1Bit;
                }
                io_buffer_ = static_cast<uint8_t>((io_buffer_ & 0xF8U) | (st & 0x07U));
                return io_buffer_;
            }
            return io_buffer_;
        }
        default:
            return 0xFFU;
    }
}

void Bus::io_write(uint16_t offset, uint8_t value) noexcept {
    const uint16_t region = offset & 0x1C00U;
    switch (region) {
        case 0x0000U:  // $0000-$03FF: VDC (ignore SGX VPC/VDC2 at $0008-$001F)
            if ((offset & 0x18U) == 0x00U) {
                vdc_.write_vdc(static_cast<uint8_t>(offset & 0x03U), value);
            }
            break;
        case 0x0400U:  // $0400-$07FF: VCE
            vdc_.write_vce(static_cast<uint8_t>(offset & 0x07U), value);
            break;
        case 0x0800U:  // $0800-$0BFF: PSG
            io_buffer_ = value;
            psg_.write(static_cast<uint8_t>(offset & 0x0FU), value);
            break;
        case 0x0C00U:  // $0C00-$0FFF: Timer
            io_buffer_ = value;
            if ((offset & 0x01U) == 0U) {
                timer_reload_ = value & 0x7FU;
            } else {
                const uint8_t prev = timer_enabled_;
                timer_enabled_ = value & 0x01U;
                if (prev == 0U && timer_enabled_ != 0U) {
                    timer_counter_ = timer_reload_;
                    timer_prescaler_ = 0U;
                }
            }
            break;
        case 0x1000U:  // $1000-$13FF: Joypad
            io_buffer_ = value;
            joy_sel_ = value & 0x03U;
            break;
        case 0x1400U: {  // $1400-$17FF: Interrupt Controller
            io_buffer_ = value;
            const uint8_t reg = static_cast<uint8_t>(offset & 0x03U);
            if (reg == 2U) {
                irq_disable_ = value & 0x07U;
            } else if (reg == 3U) {
                // Any write to $1403 acknowledges Timer IRQ (TIQ)
                irq_status_ &= static_cast<uint8_t>(~kTiqBit);
            }
            break;
        }
        default:
            break;
    }
}

uint8_t Bus::read(const PceCpuState& cpu, uint16_t addr) noexcept {
    const uint8_t bank = cpu.mpr[addr >> 13U];
    const uint16_t off = addr & 0x1FFFU;

    if (bank < 0x80U) {
        return cart_.read_bank(bank, off);
    }
    if (bank == 0xFFU) {
        return io_read(off);
    }
    if (bank >= 0xF8U && bank <= 0xFBU) {
        return wram_[static_cast<size_t>(bank - 0xF8U) * 8192U + off];
    }
    if (bank == 0xF7U) {
        return cart_.read_bram(off);
    }
    return 0xFFU;
}

void Bus::write(PceCpuState& cpu, uint16_t addr, uint8_t value) noexcept {
    const uint8_t bank = cpu.mpr[addr >> 13U];
    const uint16_t off = addr & 0x1FFFU;

    if (bank == 0xFFU) {
        io_write(off, value);
        return;
    }
    if (bank >= 0xF8U && bank <= 0xFBU) {
        wram_[static_cast<size_t>(bank - 0xF8U) * 8192U + off] = value;
        return;
    }
    if (bank < 0x80U) {
        cart_.write_bank(bank, off, value);
        return;
    }
    if (bank == 0xF7U) {
        cart_.write_bram(off, value);
    }
}

void Bus::write_vdc_direct(uint8_t port, uint8_t value) noexcept {
    vdc_.write_vdc(port, value);
}

void Bus::step_timer(uint32_t cpu_cycles) noexcept {
    if (timer_enabled_ == 0U) {
        return;
    }
    // Timer ticks once every 1024 high-speed CPU cycles (~6.992 kHz)
    timer_prescaler_ += cpu_cycles;
    while (timer_prescaler_ >= 1024U) {
        timer_prescaler_ -= 1024U;
        if (timer_counter_ == 0U) {
            timer_counter_ = timer_reload_;
            irq_status_ |= kTiqBit;
        } else {
            --timer_counter_;
        }
    }
}

uint8_t Bus::pending_irqs() const noexcept {
    uint8_t live = static_cast<uint8_t>(irq_status_ & kTiqBit);
    if (vdc_.irq_pending()) {
        live |= kIrq1Bit;
    }
    return static_cast<uint8_t>(live & static_cast<uint8_t>(~irq_disable_) & 0x07U);
}

void Bus::sync_hardware(PceCpuState& cpu) noexcept {
    if (cpu.cycles > last_sync_cycles_) {
        const uint64_t delta = cpu.cycles - last_sync_cycles_;
        last_sync_cycles_ = cpu.cycles;
        const uint32_t step_cyc = static_cast<uint32_t>(delta);
        vdc_.step(step_cyc);
        psg_.step(step_cyc);
        step_timer(step_cyc);
    }
    cpu.next_event = cpu.cycles + vdc_.cycles_until_next_scanline();
    if (frame_callback_ != nullptr && vdc_.consume_frame_ready()) {
        frame_callback_(&cpu, this, frame_callback_user_);
    }
}

PceRecompFunc Bus::lookup_recomp(uint8_t bank, uint16_t addr) const noexcept {
    if (dispatch_table_ == nullptr || dispatch_count_ == 0U) {
        return nullptr;
    }
    // Binary search on sorted (bank, addr) table
    size_t lo = 0U;
    size_t hi = dispatch_count_;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2U;
        const auto& e = dispatch_table_[mid];
        if (e.bank < bank || (e.bank == bank && e.addr < addr)) {
            lo = mid + 1U;
        } else if (e.bank > bank || e.addr > addr) {
            hi = mid;
        } else {
            return e.func;
        }
    }
    return nullptr;
}

void Bus::record_fallback(uint8_t bank, uint16_t addr, uint16_t from_pc) noexcept {
    ++fallback_count_;
    if (log_fallbacks_) {
        std::fprintf(stderr, "FALLBACK bank=%02X addr=%04X from=%04X\n",
                     static_cast<unsigned>(bank), static_cast<unsigned>(addr),
                     static_cast<unsigned>(from_pc));
    }
}

bool Bus::save_state(const std::filesystem::path& path, const PceCpuState& cpu) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    SerializedCpu sc{};
    sc.a = cpu.a; sc.x = cpu.x; sc.y = cpu.y; sc.s = cpu.s; sc.p = cpu.p;
    sc.pc = cpu.pc;
    std::memcpy(sc.mpr, cpu.mpr, sizeof(sc.mpr));
    sc.last_mpr = cpu.last_mpr;
    sc.speed_high = cpu.speed_high;
    sc.nmi_pending = cpu.nmi_pending;
    sc.cycles = cpu.cycles;

    const CartridgeState cart_st = cart_.state();
    const VdcState vdc_st = vdc_.state();
    const PsgState psg_st = psg_.state();

    out.write(reinterpret_cast<const char*>(kStateMagic), sizeof(kStateMagic));
    out.write(reinterpret_cast<const char*>(&kStateVersion), sizeof(kStateVersion));
    out.write(reinterpret_cast<const char*>(&sc), sizeof(sc));
    out.write(reinterpret_cast<const char*>(wram_.data()), static_cast<std::streamsize>(wram_.size()));
    out.write(reinterpret_cast<const char*>(&cart_st), sizeof(cart_st));
    out.write(reinterpret_cast<const char*>(cart_.bram().data()),
              static_cast<std::streamsize>(cart_.bram().size()));
    out.write(reinterpret_cast<const char*>(&vdc_st), sizeof(vdc_st));
    out.write(reinterpret_cast<const char*>(vdc_.vram().data()),
              static_cast<std::streamsize>(vdc_.vram().size() * sizeof(uint16_t)));
    out.write(reinterpret_cast<const char*>(vdc_.satb().data()),
              static_cast<std::streamsize>(vdc_.satb().size() * sizeof(uint16_t)));
    out.write(reinterpret_cast<const char*>(vdc_.palette().data()),
              static_cast<std::streamsize>(vdc_.palette().size() * sizeof(uint16_t)));
    out.write(reinterpret_cast<const char*>(&psg_st), sizeof(psg_st));
    out.write(reinterpret_cast<const char*>(&joy_sel_), sizeof(joy_sel_));
    out.write(reinterpret_cast<const char*>(&io_buffer_), sizeof(io_buffer_));
    out.write(reinterpret_cast<const char*>(&timer_reload_), sizeof(timer_reload_));
    out.write(reinterpret_cast<const char*>(&timer_counter_), sizeof(timer_counter_));
    out.write(reinterpret_cast<const char*>(&timer_enabled_), sizeof(timer_enabled_));
    out.write(reinterpret_cast<const char*>(&timer_prescaler_), sizeof(timer_prescaler_));
    out.write(reinterpret_cast<const char*>(&irq_disable_), sizeof(irq_disable_));
    out.write(reinterpret_cast<const char*>(&irq_status_), sizeof(irq_status_));
    out.write(reinterpret_cast<const char*>(&last_sync_cycles_), sizeof(last_sync_cycles_));
    return out.good();
}

bool Bus::load_state(const std::filesystem::path& path, PceCpuState& cpu) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    uint8_t magic[8]{};
    uint32_t ver = 0;
    in.read(reinterpret_cast<char*>(magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&ver), sizeof(ver));
    if (!in.good() || std::memcmp(magic, kStateMagic, sizeof(magic)) != 0 || ver != kStateVersion) {
        return false;
    }

    SerializedCpu sc{};
    CartridgeState cart_st{};
    VdcState vdc_st{};
    PsgState psg_st{};

    in.read(reinterpret_cast<char*>(&sc), sizeof(sc));
    in.read(reinterpret_cast<char*>(wram_.data()), static_cast<std::streamsize>(wram_.size()));
    in.read(reinterpret_cast<char*>(&cart_st), sizeof(cart_st));
    in.read(reinterpret_cast<char*>(cart_.bram_mut().data()),
            static_cast<std::streamsize>(cart_.bram().size()));
    in.read(reinterpret_cast<char*>(&vdc_st), sizeof(vdc_st));
    in.read(reinterpret_cast<char*>(vdc_.vram_mut().data()),
            static_cast<std::streamsize>(vdc_.vram().size() * sizeof(uint16_t)));
    in.read(reinterpret_cast<char*>(vdc_.satb_mut().data()),
            static_cast<std::streamsize>(vdc_.satb().size() * sizeof(uint16_t)));
    in.read(reinterpret_cast<char*>(vdc_.palette_mut().data()),
            static_cast<std::streamsize>(vdc_.palette().size() * sizeof(uint16_t)));
    in.read(reinterpret_cast<char*>(&psg_st), sizeof(psg_st));
    in.read(reinterpret_cast<char*>(&joy_sel_), sizeof(joy_sel_));
    in.read(reinterpret_cast<char*>(&io_buffer_), sizeof(io_buffer_));
    in.read(reinterpret_cast<char*>(&timer_reload_), sizeof(timer_reload_));
    in.read(reinterpret_cast<char*>(&timer_counter_), sizeof(timer_counter_));
    in.read(reinterpret_cast<char*>(&timer_enabled_), sizeof(timer_enabled_));
    in.read(reinterpret_cast<char*>(&timer_prescaler_), sizeof(timer_prescaler_));
    in.read(reinterpret_cast<char*>(&irq_disable_), sizeof(irq_disable_));
    in.read(reinterpret_cast<char*>(&irq_status_), sizeof(irq_status_));
    in.read(reinterpret_cast<char*>(&last_sync_cycles_), sizeof(last_sync_cycles_));
    if (!in.good()) {
        return false;
    }

    cpu.a = sc.a; cpu.x = sc.x; cpu.y = sc.y; cpu.s = sc.s; cpu.p = sc.p;
    cpu.pc = sc.pc;
    std::memcpy(cpu.mpr, sc.mpr, sizeof(cpu.mpr));
    cpu.last_mpr = sc.last_mpr;
    cpu.speed_high = sc.speed_high;
    cpu.nmi_pending = sc.nmi_pending;
    cpu.cycles = sc.cycles;
    cpu.next_event = cpu.cycles + Vdc::kCyclesPerScanline;

    cart_.set_state(cart_st);
    vdc_.set_state(vdc_st);
    psg_.set_state(psg_st);
    return true;
}

}  // namespace dragon::pce
