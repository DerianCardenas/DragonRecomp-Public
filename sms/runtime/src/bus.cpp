#include "sms/bus.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace dragon::sms {

namespace {

constexpr uint8_t kStateMagic[8] = {'D', 'R', 'S', 'T', 'S', 'M', 'S', '1'};
constexpr uint32_t kStateVersion = 1U;

struct SerializedCpu {
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t a2, f2, b2, c2, d2, e2, h2, l2;
    uint16_t ix, iy, sp, pc;
    uint8_t i, r, iff1, iff2, im, ei_delay, halted, nmi_pending;
    uint64_t cycles;
};

}  // namespace

Bus::Bus() = default;

void Bus::reset(SmsCpuState& cpu) noexcept {
    cart_.reset();
    const bool is_gg = (cart_.system_type() == SystemType::GameGear);
    vdp_.reset(is_gg);
    psg_.reset();
    wram_.fill(0x00U);

    input_ = {};
    prev_pause_ = false;
    mem_control_ = 0xABU;
    io_control_ = 0xFFU;
    gg_regs_ = {0xC0U, 0x7FU, 0xFFU, 0x00U, 0xFFU, 0x00U};
    last_sync_cycles_ = 0;
    fallback_count_ = 0;
    stop_requested_ = false;

    std::memset(&cpu, 0, sizeof(cpu));
    cpu.sp = 0xDFF0U;  // Standard post-BIOS stack pointer
    cpu.pc = 0x0000U;
    cpu.im = 1U;
    cpu.next_event = Vdp::kCyclesPerScanline;
    cpu.bus = this;
    cpu.wram = wram_.data();
    cpu.rom = cart_.rom().empty() ? nullptr : cart_.rom().data();
    cpu.rom_size = static_cast<uint32_t>(cart_.rom().size());
}

uint8_t Bus::read(uint16_t addr) const noexcept {
    if (addr < 0xC000U) {
        return cart_.read(addr);
    }
    return wram_[addr & 0x1FFFU];
}

void Bus::write(uint16_t addr, uint8_t value) noexcept {
    if (addr < 0xC000U) {
        cart_.write(addr, value);
        return;
    }
    wram_[addr & 0x1FFFU] = value;
    if (addr >= 0xFFFCU) {
        cart_.write_sega_mapper(addr, value);
    }
}

uint8_t Bus::io_read(uint8_t port) noexcept {
    // Game Gear dedicated I/O ports $00-$06
    if (cart_.system_type() == SystemType::GameGear && port <= 0x06U) {
        if (port == 0x00U) {
            // Bit 7: Start button (0 = pressed, 1 = released), Bit 6: 1 = Export, Bit 5: 0 = NTSC
            uint8_t val = 0xC0U;
            if (input_.pause_or_start) {
                val &= static_cast<uint8_t>(~0x80U);
            }
            return val;
        }
        if (port <= 0x05U) {
            return gg_regs_[port];
        }
        return 0xFFU;
    }

    // Standard SMS partial address decoding on bits A7, A6, A0
    const uint8_t region = static_cast<uint8_t>(port & 0xC1U);
    switch (region) {
        case 0x40U:  // $40-$7F even ($7E): VDP V-Counter
            return vdp_.read_v_counter();
        case 0x41U:  // $40-$7F odd ($7F): VDP H-Counter
            return vdp_.read_h_counter();
        case 0x80U:  // $80-$BF even ($BE): VDP Data Port
            return vdp_.read_data();
        case 0x81U:  // $80-$BF odd ($BF): VDP Status Port
            return vdp_.read_status();
        case 0xC0U: {  // $C0-$FF even ($DC): Joypad Port A (active low)
            uint8_t v = 0xFFU;
            if (input_.up) v &= static_cast<uint8_t>(~0x01U);
            if (input_.down) v &= static_cast<uint8_t>(~0x02U);
            if (input_.left) v &= static_cast<uint8_t>(~0x04U);
            if (input_.right) v &= static_cast<uint8_t>(~0x08U);
            if (input_.button1) v &= static_cast<uint8_t>(~0x10U);
            if (input_.button2) v &= static_cast<uint8_t>(~0x20U);
            if (input_.p2_up) v &= static_cast<uint8_t>(~0x40U);
            if (input_.p2_down) v &= static_cast<uint8_t>(~0x80U);
            return v;
        }
        case 0xC1U: {  // $C0-$FF odd ($DD): Joypad Port B + TH/TR pins (active low)
            uint8_t v = 0xFFU;
            if (input_.p2_left) v &= static_cast<uint8_t>(~0x01U);
            if (input_.p2_right) v &= static_cast<uint8_t>(~0x02U);
            if (input_.p2_button1) v &= static_cast<uint8_t>(~0x04U);
            if (input_.p2_button2) v &= static_cast<uint8_t>(~0x08U);
            // Export region detection reads back TH outputs when set to output mode (bits 1 & 3 = 0)
            if ((io_control_ & 0x02U) == 0U && (io_control_ & 0x20U) == 0U) {
                v &= static_cast<uint8_t>(~0x40U);  // P1 TH low
            }
            if ((io_control_ & 0x08U) == 0U && (io_control_ & 0x80U) == 0U) {
                v &= static_cast<uint8_t>(~0x80U);  // P2 TH low
            }
            return v;
        }
        default:
            return 0xFFU;
    }
}

void Bus::io_write(uint8_t port, uint8_t value) noexcept {
    if (cart_.system_type() == SystemType::GameGear && port <= 0x06U) {
        if (port == 0x06U) {
            psg_.write_stereo(value);
        } else if (port >= 0x01U && port <= 0x05U) {
            gg_regs_[port] = value;
        }
        return;
    }

    const uint8_t region = static_cast<uint8_t>(port & 0xC1U);
    switch (region) {
        case 0x00U:  // $00-$3F even ($3E): Memory Control
            mem_control_ = value;
            break;
        case 0x01U:  // $00-$3F odd ($3F): I/O Port Control
            io_control_ = value;
            break;
        case 0x40U:  // $40-$7F ($7E / $7F): SN76489 PSG
        case 0x41U:
            psg_.write(value);
            break;
        case 0x80U:  // $80-$BF even ($BE): VDP Data
            vdp_.write_data(value);
            break;
        case 0x81U:  // $80-$BF odd ($BF): VDP Control
            vdp_.write_control(value);
            break;
        default:
            break;
    }
}

void Bus::sync_hardware(SmsCpuState& cpu) noexcept {
    if (cpu.cycles > last_sync_cycles_) {
        const uint64_t delta = cpu.cycles - last_sync_cycles_;
        last_sync_cycles_ = cpu.cycles;
        const uint32_t step_cyc = static_cast<uint32_t>(delta);
        vdp_.step(step_cyc);
        psg_.step(step_cyc);
    }
    cpu.next_event = cpu.cycles + Vdp::kCyclesPerScanline;
    if (frame_callback_ != nullptr && vdp_.consume_frame_ready()) {
        frame_callback_(&cpu, this, frame_callback_user_);
    }
}

void Bus::set_input(const InputState& in, SmsCpuState& cpu) noexcept {
    input_ = in;
    // On Master System, pressing Pause triggers a rising-edge Z80 NMI ($0066).
    // On Game Gear, Start is polled via port $00 bit 7 instead.
    if (cart_.system_type() == SystemType::MasterSystem && in.pause_or_start && !prev_pause_) {
        cpu.nmi_pending = 1U;
    }
    prev_pause_ = in.pause_or_start;
}

SmsRecompFunc Bus::lookup_recomp(uint8_t bank, uint16_t addr) const noexcept {
    if (!dispatch_table_ || dispatch_count_ == 0U) {
        return nullptr;
    }
    // Table is sorted by (bank, addr) by smsrecomp emitter -> O(log N) binary search
    size_t lo = 0;
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
    if (log_fallbacks_ && fallback_count_ <= 256U) {
        std::fprintf(stderr, "FALLBACK bank=%02X addr=%04X from=%04X\n",
                     static_cast<unsigned>(bank), static_cast<unsigned>(addr),
                     static_cast<unsigned>(from_pc));
    }
}

bool Bus::save_state(const std::filesystem::path& path, const SmsCpuState& cpu) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    SerializedCpu sc{};
    sc.a = cpu.a;   sc.f = cpu.f;   sc.b = cpu.b;   sc.c = cpu.c;
    sc.d = cpu.d;   sc.e = cpu.e;   sc.h = cpu.h;   sc.l = cpu.l;
    sc.a2 = cpu.a2; sc.f2 = cpu.f2; sc.b2 = cpu.b2; sc.c2 = cpu.c2;
    sc.d2 = cpu.d2; sc.e2 = cpu.e2; sc.h2 = cpu.h2; sc.l2 = cpu.l2;
    sc.ix = cpu.ix; sc.iy = cpu.iy; sc.sp = cpu.sp; sc.pc = cpu.pc;
    sc.i = cpu.i;   sc.r = cpu.r;
    sc.iff1 = cpu.iff1; sc.iff2 = cpu.iff2; sc.im = cpu.im;
    sc.ei_delay = cpu.ei_delay; sc.halted = cpu.halted; sc.nmi_pending = cpu.nmi_pending;
    sc.cycles = cpu.cycles;

    const auto cart_st = cart_.state();
    const auto& vdp_st = vdp_.state();
    const auto& psg_st = psg_.state();

    out.write(reinterpret_cast<const char*>(kStateMagic), sizeof(kStateMagic));
    out.write(reinterpret_cast<const char*>(&kStateVersion), sizeof(kStateVersion));
    out.write(reinterpret_cast<const char*>(&sc), sizeof(sc));
    out.write(reinterpret_cast<const char*>(wram_.data()), static_cast<std::streamsize>(wram_.size()));
    out.write(reinterpret_cast<const char*>(&cart_st), sizeof(cart_st));
    out.write(reinterpret_cast<const char*>(cart_.sram().data()),
              static_cast<std::streamsize>(cart_.sram().size()));
    out.write(reinterpret_cast<const char*>(&vdp_st), sizeof(vdp_st));
    out.write(reinterpret_cast<const char*>(&psg_st), sizeof(psg_st));
    out.write(reinterpret_cast<const char*>(&mem_control_), sizeof(mem_control_));
    out.write(reinterpret_cast<const char*>(&io_control_), sizeof(io_control_));
    out.write(reinterpret_cast<const char*>(gg_regs_.data()),
              static_cast<std::streamsize>(gg_regs_.size()));
    out.write(reinterpret_cast<const char*>(&last_sync_cycles_), sizeof(last_sync_cycles_));
    return out.good();
}

bool Bus::load_state(const std::filesystem::path& path, SmsCpuState& cpu) {
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
    VdpState vdp_st{};
    PsgState psg_st{};

    in.read(reinterpret_cast<char*>(&sc), sizeof(sc));
    in.read(reinterpret_cast<char*>(wram_.data()), static_cast<std::streamsize>(wram_.size()));
    in.read(reinterpret_cast<char*>(&cart_st), sizeof(cart_st));
    in.read(reinterpret_cast<char*>(cart_.sram_mut().data()),
            static_cast<std::streamsize>(cart_.sram().size()));
    in.read(reinterpret_cast<char*>(&vdp_st), sizeof(vdp_st));
    in.read(reinterpret_cast<char*>(&psg_st), sizeof(psg_st));
    in.read(reinterpret_cast<char*>(&mem_control_), sizeof(mem_control_));
    in.read(reinterpret_cast<char*>(&io_control_), sizeof(io_control_));
    in.read(reinterpret_cast<char*>(gg_regs_.data()),
            static_cast<std::streamsize>(gg_regs_.size()));
    in.read(reinterpret_cast<char*>(&last_sync_cycles_), sizeof(last_sync_cycles_));
    if (!in.good()) {
        return false;
    }

    cpu.a = sc.a;   cpu.f = sc.f;   cpu.b = sc.b;   cpu.c = sc.c;
    cpu.d = sc.d;   cpu.e = sc.e;   cpu.h = sc.h;   cpu.l = sc.l;
    cpu.a2 = sc.a2; cpu.f2 = sc.f2; cpu.b2 = sc.b2; cpu.c2 = sc.c2;
    cpu.d2 = sc.d2; cpu.e2 = sc.e2; cpu.h2 = sc.h2; cpu.l2 = sc.l2;
    cpu.ix = sc.ix; cpu.iy = sc.iy; cpu.sp = sc.sp; cpu.pc = sc.pc;
    cpu.i = sc.i;   cpu.r = sc.r;
    cpu.iff1 = sc.iff1; cpu.iff2 = sc.iff2; cpu.im = sc.im;
    cpu.ei_delay = sc.ei_delay; cpu.halted = sc.halted; cpu.nmi_pending = sc.nmi_pending;
    cpu.cycles = sc.cycles;
    cpu.next_event = cpu.cycles + Vdp::kCyclesPerScanline;

    cart_.set_state(cart_st);
    vdp_.set_state(vdp_st);
    psg_.set_state(psg_st);
    return true;
}

}  // namespace dragon::sms
