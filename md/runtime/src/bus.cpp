#include "md/bus.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "dragon/save_state.h"
#include "md/interpreter.h"

namespace dragon::md {

namespace {

constexpr uint32_t kMdStateVersion = 1U;

struct MdStatePayload {
    uint32_t d[8]{};
    uint32_t a[8]{};
    uint32_t usp = 0;
    uint32_t ssp = 0;
    uint32_t pc = 0;
    uint16_t sr = 0;
    uint8_t stopped = 0;
    uint8_t pad = 0;
    uint64_t cycles = 0;
    std::array<uint8_t, 65536> wram{};
    Vdp::Snapshot vdp{};
    MdZ80::Snapshot z80{};
    dragon::sms::PsgState psg{};
    uint8_t sram_enabled = 0;
    uint8_t sram_wp = 0;
    std::array<uint8_t, 8> ssf2_banks{};
};

}  // namespace

Bus::Bus() noexcept {
    wram_.fill(0);
    ctrl_[0] = ControllerPort{};
    ctrl_[1] = ControllerPort{};
}

bool Bus::load_rom(const std::filesystem::path& path, std::string* err) {
    return cart_.load_from_file(path, err);
}

bool Bus::load_rom_bytes(const std::vector<uint8_t>& raw, std::string* err) {
    return cart_.load_from_bytes(raw, err);
}

void Bus::reset(MdCpuState& cpu) noexcept {
    wram_.fill(0);
    cart_.reset();
    vdp_.reset(cart_.header().pal_only);
    ym2612_.reset();
    psg_.reset();
    z80_.reset();
    z80_.set_reset_line(true);
    z80_.set_bus_request(false);
    ctrl_[0] = ControllerPort{};
    ctrl_[1] = ControllerPort{};
    last_sync_cycles_ = 0;
    z80_cycle_rem_ = 0;
    cpu_reset(cpu, *this);
}

void Bus::set_controller_state(int port, uint16_t buttons) noexcept {
    if (port == 0 || port == 1) {
        ctrl_[port].buttons = buttons;
    }
}

uint8_t Bus::read_io(uint32_t reg) noexcept {
    switch (reg) {
        case 0x00U:
            // Hardware version: Bit 7=Overseas (1=USA/EUR), Bit 6=PAL(1)/NTSC(0), Bit 5=No SCD(1)
            return vdp_.is_pal() ? 0xE0U : 0xA0U;
        case 0x01U:
        case 0x02U: {
            const int idx = (reg == 0x01U) ? 0 : 1;
            const auto& cp = ctrl_[idx];
            const uint16_t b = cp.buttons;
            // TH pin (bit 6) is pulled HIGH when configured as input (ctrl bit 6 == 0)
            const bool th = ((cp.ctrl & 0x40U) == 0U) || ((cp.data & 0x40U) != 0U);
            uint8_t out = static_cast<uint8_t>(th ? 0x40U : 0x00U);

            if (six_button_ && !th && cp.th_step == 3U) {
                // Phase 5 (3rd TH=0): 6-button signature (bits 0..3 = 0, bit 4 = A, bit 5 = Start)
                if (!(b & kBtnA))
                    out |= 0x10U;
                if (!(b & kBtnStart))
                    out |= 0x20U;
            } else if (six_button_ && th && cp.th_step == 3U) {
                // Phase 6 (3rd TH=1): extended buttons Z(0), Y(1), X(2), Mode(3), B(4), C(5)
                if (!(b & kBtnZ))
                    out |= 0x01U;
                if (!(b & kBtnY))
                    out |= 0x02U;
                if (!(b & kBtnX))
                    out |= 0x04U;
                if (!(b & kBtnMode))
                    out |= 0x08U;
                if (!(b & kBtnB))
                    out |= 0x10U;
                if (!(b & kBtnC))
                    out |= 0x20U;
            } else if (six_button_ && !th && cp.th_step == 4U) {
                // Phase 7 (4th TH=0): bits 0..3 = 1, bit 4 = A, bit 5 = Start
                out |= 0x0FU;
                if (!(b & kBtnA))
                    out |= 0x10U;
                if (!(b & kBtnStart))
                    out |= 0x20U;
            } else if (th) {
                // Standard TH=1: Up(0), Down(1), Left(2), Right(3), B(4), C(5)
                if (!(b & kBtnUp))
                    out |= 0x01U;
                if (!(b & kBtnDown))
                    out |= 0x02U;
                if (!(b & kBtnLeft))
                    out |= 0x04U;
                if (!(b & kBtnRight))
                    out |= 0x08U;
                if (!(b & kBtnB))
                    out |= 0x10U;
                if (!(b & kBtnC))
                    out |= 0x20U;
            } else {
                // Standard TH=0: Up(0), Down(1), 0(2), 0(3), A(4), Start(5)
                if (!(b & kBtnUp))
                    out |= 0x01U;
                if (!(b & kBtnDown))
                    out |= 0x02U;
                if (!(b & kBtnA))
                    out |= 0x10U;
                if (!(b & kBtnStart))
                    out |= 0x20U;
            }
            return out;
        }
        case 0x03U:
            return 0x7FU;  // Extension port (unconnected)
        case 0x04U:
            return ctrl_[0].ctrl;
        case 0x05U:
            return ctrl_[1].ctrl;
        default:
            return 0x00U;
    }
}

void Bus::write_io(uint32_t reg, uint8_t value) noexcept {
    if (reg == 0x01U || reg == 0x02U) {
        const int idx = (reg == 0x01U) ? 0 : 1;
        auto& cp = ctrl_[idx];
        const bool old_th = ((cp.ctrl & 0x40U) == 0U) || ((cp.data & 0x40U) != 0U);
        cp.data = value;
        const bool new_th = ((cp.ctrl & 0x40U) == 0U) || ((cp.data & 0x40U) != 0U);
        if (old_th && !new_th) {
            // Falling edge (1 -> 0) advances the 6-button state machine (1..4)
            cp.th_step = (cp.th_step < 4U) ? static_cast<uint8_t>(cp.th_step + 1U) : 1U;
            cp.th_timeout = 12000U;  // ~1.5 ms reset window
        } else if (!old_th && new_th && cp.th_step == 4U) {
            cp.th_step = 0U;
        }
    } else if (reg == 0x04U || reg == 0x05U) {
        const int idx = (reg == 0x04U) ? 0 : 1;
        auto& cp = ctrl_[idx];
        const bool old_th = ((cp.ctrl & 0x40U) == 0U) || ((cp.data & 0x40U) != 0U);
        cp.ctrl = value;
        const bool new_th = ((cp.ctrl & 0x40U) == 0U) || ((cp.data & 0x40U) != 0U);
        if (old_th && !new_th) {
            cp.th_step = (cp.th_step < 4U) ? static_cast<uint8_t>(cp.th_step + 1U) : 1U;
            cp.th_timeout = 12000U;
        } else if (!old_th && new_th && cp.th_step == 4U) {
            cp.th_step = 0U;
        }
    }
}

uint8_t Bus::read8(uint32_t addr) noexcept {
    const uint32_t a = addr & 0x00FFFFFFU;
    if (flat_ram_mode_) {
        return wram_[a & 0xFFFFU];
    }
    if (a < 0x400000U) {
        return cart_.read8(a);
    }
    if (a >= 0xE00000U) {
        return wram_[a & 0xFFFFU];
    }
    if (a >= 0xA00000U && a < 0xA10000U) {
        const uint16_t z_off = static_cast<uint16_t>(a & 0xFFFFU);
        if (z_off < 0x4000U) {
            return z80_.read_ram(z_off);
        }
        if (z_off < 0x6000U) {
            return ym2612_.read_status();
        }
        return 0xFFU;
    }
    if (a >= 0xA10000U && a < 0xA10020U) {
        return read_io((a & 0x1FU) >> 1U);
    }
    if (a == 0xA11100U) {
        // Bit 0 == 0 means 68K owns Z80 bus (BUSACK)
        return z80_.is_bus_granted() ? 0x00U : 0x01U;
    }
    if (a == 0xA11101U) {
        return 0x00U;
    }
    if (a >= 0xC00000U && a < 0xC00020U) {
        const uint16_t w = read16(a & ~1U);
        return (a & 1U) ? static_cast<uint8_t>(w & 0xFFU) : static_cast<uint8_t>(w >> 8U);
    }
    return 0xFFU;
}

uint16_t Bus::read16(uint32_t addr) noexcept {
    const uint32_t a = addr & 0x00FFFFFEU;
    if (flat_ram_mode_) {
        const uint32_t o = a & 0xFFFFU;
        return static_cast<uint16_t>((static_cast<uint16_t>(wram_[o]) << 8U) |
                                     wram_[(o + 1U) & 0xFFFFU]);
    }
    if (a < 0x400000U) {
        return cart_.read16(a);
    }
    if (a >= 0xE00000U) {
        const uint32_t o = a & 0xFFFFU;
        return static_cast<uint16_t>((static_cast<uint16_t>(wram_[o]) << 8U) |
                                     wram_[(o + 1U) & 0xFFFFU]);
    }
    if (a >= 0xC00000U && a < 0xC00020U) {
        const uint32_t p = a & 0x1EU;
        if (p == 0x00U || p == 0x02U) {
            return vdp_.read_data();
        }
        if (p == 0x04U || p == 0x06U) {
            return vdp_.read_control();
        }
        if (p == 0x08U || p == 0x0AU || p == 0x0CU || p == 0x0EU) {
            return vdp_.read_hv_counter();
        }
        return 0xFFFFU;
    }
    if (a == 0xA11100U) {
        return z80_.is_bus_granted() ? 0x0000U : 0x0100U;
    }
    if (a >= 0xA10000U && a < 0xA10020U) {
        const uint8_t v = read_io((a & 0x1FU) >> 1U);
        return static_cast<uint16_t>((static_cast<uint16_t>(v) << 8U) | v);
    }
    return static_cast<uint16_t>((static_cast<uint16_t>(read8(a)) << 8U) | read8(a + 1U));
}

uint32_t Bus::read32(uint32_t addr) noexcept {
    const uint32_t hi = read16(addr);
    const uint32_t lo = read16(addr + 2U);
    return (hi << 16U) | lo;
}

void Bus::write8(uint32_t addr, uint8_t value) noexcept {
    const uint32_t a = addr & 0x00FFFFFFU;
    if (flat_ram_mode_) {
        wram_[a & 0xFFFFU] = value;
        return;
    }
    if (a >= 0xE00000U) {
        wram_[a & 0xFFFFU] = value;
        return;
    }
    if (a < 0x400000U) {
        cart_.write8(a, value);
        return;
    }
    if (a >= 0xA00000U && a < 0xA10000U) {
        const uint16_t z_off = static_cast<uint16_t>(a & 0xFFFFU);
        if (z_off < 0x4000U) {
            z80_.write_ram(z_off, value);
        } else if (z_off < 0x6000U) {
            ym2612_.write(static_cast<uint8_t>(z_off & 0x03U), value);
        }
        return;
    }
    if (a >= 0xA10000U && a < 0xA10020U) {
        write_io((a & 0x1FU) >> 1U, value);
        return;
    }
    if (a == 0xA11100U) {
        z80_.set_bus_request((value & 0x01U) != 0U);
        return;
    }
    if (a == 0xA11200U) {
        z80_.set_reset_line((value & 0x01U) == 0U);
        return;
    }
    if (a >= 0xA130F0U && a <= 0xA130FFU) {
        cart_.write_time_reg(a, value);
        return;
    }
    if (a >= 0xC00000U && a < 0xC00020U) {
        const uint32_t p = a & 0x1FU;
        if (p == 0x11U || p == 0x13U || p == 0x15U || p == 0x17U) {
            psg_.write(value);
        } else if (p < 0x08U) {
            const uint16_t w = static_cast<uint16_t>((static_cast<uint16_t>(value) << 8U) | value);
            write16(a & ~1U, w);
        }
    }
}

void Bus::write16(uint32_t addr, uint16_t value) noexcept {
    const uint32_t a = addr & 0x00FFFFFEU;
    if (flat_ram_mode_) {
        const uint32_t o = a & 0xFFFFU;
        wram_[o] = static_cast<uint8_t>(value >> 8U);
        wram_[(o + 1U) & 0xFFFFU] = static_cast<uint8_t>(value & 0xFFU);
        return;
    }
    if (a >= 0xE00000U) {
        const uint32_t o = a & 0xFFFFU;
        wram_[o] = static_cast<uint8_t>(value >> 8U);
        wram_[(o + 1U) & 0xFFFFU] = static_cast<uint8_t>(value & 0xFFU);
        return;
    }
    if (a < 0x400000U) {
        cart_.write16(a, value);
        return;
    }
    if (a >= 0xC00000U && a < 0xC00020U) {
        const uint32_t p = a & 0x1EU;
        if (p == 0x00U || p == 0x02U) {
            vdp_.write_data(value, this);
        } else if (p == 0x04U || p == 0x06U) {
            vdp_.write_control(value, this);
        } else if (p == 0x10U || p == 0x12U || p == 0x14U || p == 0x16U) {
            psg_.write(static_cast<uint8_t>(value & 0xFFU));
        }
        return;
    }
    if (a == 0xA11100U) {
        z80_.set_bus_request((value & 0x0100U) != 0U);
        return;
    }
    if (a == 0xA11200U) {
        z80_.set_reset_line((value & 0x0100U) == 0U);
        return;
    }
    if (a >= 0xA10000U && a < 0xA10020U) {
        write_io((a & 0x1FU) >> 1U, static_cast<uint8_t>(value & 0xFFU));
        return;
    }
    if (a >= 0xA130F0U && a <= 0xA130FFU) {
        cart_.write_time_reg(a | 1U, static_cast<uint8_t>(value & 0xFFU));
        return;
    }
    if (a >= 0xA00000U && a < 0xA10000U) {
        // 68K 16-bit write to Z80 space writes the upper byte only on real hardware
        write8(a, static_cast<uint8_t>(value >> 8U));
    }
}

void Bus::write32(uint32_t addr, uint32_t value) noexcept {
    write16(addr, static_cast<uint16_t>(value >> 16U));
    write16(addr + 2U, static_cast<uint16_t>(value & 0xFFFFU));
}

void Bus::sync_hardware(uint64_t m68k_cycles) noexcept {
    if (flat_ram_mode_ || m68k_cycles <= last_sync_cycles_) {
        return;
    }
    const uint32_t delta = static_cast<uint32_t>(m68k_cycles - last_sync_cycles_);
    last_sync_cycles_ = m68k_cycles;

    vdp_.step(delta, this);
    ym2612_.step(delta);

    // Z80 clock = M68K_clock * 7 / 15
    const uint32_t z_num = delta * 7U + z80_cycle_rem_;
    const uint32_t z_cycles = z_num / 15U;
    z80_cycle_rem_ = z_num % 15U;
    if (z_cycles > 0U) {
        z80_.step(z_cycles, this);
        psg_.step(z_cycles);
    }

    // 6-button controller TH counter timeout
    for (int i = 0; i < 2; ++i) {
        if (ctrl_[i].th_timeout > 0U) {
            if (delta >= ctrl_[i].th_timeout) {
                ctrl_[i].th_timeout = 0;
                ctrl_[i].th_step = 0;
            } else {
                ctrl_[i].th_timeout -= delta;
            }
        }
    }
}

size_t Bus::drain_audio_samples(int16_t* dst, size_t max_frames) noexcept {
    const size_t fm_avail = ym2612_.available_frames();
    const size_t n = std::min(max_frames, fm_avail);
    if (n == 0U) {
        return 0U;
    }
    ym2612_.drain_samples(dst, n);

    std::array<int16_t, 2048> psg_buf{};
    size_t done = 0;
    while (done < n) {
        const size_t chunk = std::min<size_t>(n - done, psg_buf.size() / 2U);
        const size_t got = psg_.drain_samples(psg_buf.data(), chunk);
        for (size_t i = 0; i < got; ++i) {
            const int32_t l = static_cast<int32_t>(dst[(done + i) * 2U + 0U]) +
                              (static_cast<int32_t>(psg_buf[i * 2U + 0U]) >> 1);
            const int32_t r = static_cast<int32_t>(dst[(done + i) * 2U + 1U]) +
                              (static_cast<int32_t>(psg_buf[i * 2U + 1U]) >> 1);
            dst[(done + i) * 2U + 0U] = static_cast<int16_t>(std::clamp(l, -32768, 32767));
            dst[(done + i) * 2U + 1U] = static_cast<int16_t>(std::clamp(r, -32768, 32767));
        }
        done += chunk;
    }
    return n;
}

void Bus::register_recomp_functions(const MdRecompFuncEntry* entries, size_t count) {
    recomp_table_.clear();
    recomp_table_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        recomp_table_[entries[i].addr & 0x00FFFFFFU] = entries[i].func;
    }
}

MdRecompFunc Bus::lookup_function(uint32_t addr) const noexcept {
    const auto it = recomp_table_.find(addr & 0x00FFFFFFU);
    return (it != recomp_table_.end()) ? it->second : nullptr;
}

void Bus::log_fallback(uint32_t target, uint32_t from_pc) noexcept {
    if (fallbacks_.size() < 4096U) {
        fallbacks_.push_back({target & 0x00FFFFFFU, from_pc & 0x00FFFFFFU});
    }
}

void Bus::on_frame_complete(MdCpuState* cpu) noexcept {
    if (frame_cb_) {
        frame_cb_(cpu, this, frame_cb_user_);
    }
}

bool Bus::save_state(const MdCpuState& cpu, const std::filesystem::path& path) const {
    MdStatePayload p{};
    for (int i = 0; i < 8; ++i) {
        p.d[i] = cpu.d[i];
        p.a[i] = cpu.a[i];
    }
    p.usp = cpu.usp;
    p.ssp = cpu.ssp;
    p.pc = cpu.pc;
    p.sr = cpu.sr;
    p.stopped = cpu.stopped;
    p.cycles = cpu.cycles;
    p.wram = wram_;
    vdp_.save_snapshot(p.vdp);
    z80_.save_snapshot(p.z80);
    p.psg = psg_.state();
    p.sram_enabled = cart_.sram_enabled() ? 1U : 0U;
    p.sram_wp = cart_.sram_write_protect() ? 1U : 0U;
    p.ssf2_banks = cart_.ssf2_banks();

    std::vector<uint8_t> bytes(sizeof(MdStatePayload) + cart_.sram().size());
    std::memcpy(bytes.data(), &p, sizeof(MdStatePayload));
    if (!cart_.sram().empty()) {
        std::memcpy(bytes.data() + sizeof(MdStatePayload), cart_.sram().data(),
                    cart_.sram().size());
    }
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    dragon::SaveStateHeader hdr;
    std::memcpy(hdr.system, "MD  ", 4);
    hdr.version = kMdStateVersion;
    hdr.payload_size = static_cast<uint32_t>(bytes.size());
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

bool Bus::load_state(MdCpuState& cpu, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    dragon::SaveStateHeader hdr{};
    if (!in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
        return false;
    }
    if (!hdr.is_valid("MD  ", kMdStateVersion) || hdr.payload_size < sizeof(MdStatePayload)) {
        return false;
    }
    std::vector<uint8_t> bytes(hdr.payload_size);
    if (!in.read(reinterpret_cast<char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()))) {
        return false;
    }
    MdStatePayload p{};
    std::memcpy(&p, bytes.data(), sizeof(MdStatePayload));
    for (int i = 0; i < 8; ++i) {
        cpu.d[i] = p.d[i];
        cpu.a[i] = p.a[i];
    }
    cpu.usp = p.usp;
    cpu.ssp = p.ssp;
    cpu.pc = p.pc;
    cpu.sr = p.sr;
    cpu.stopped = p.stopped;
    cpu.cycles = p.cycles;
    cpu.next_event = cpu.cycles + Vdp::kCyclesPerScanline;
    wram_ = p.wram;
    vdp_.load_snapshot(p.vdp);
    z80_.load_snapshot(p.z80);
    psg_.set_state(p.psg);
    cart_.set_sram_enabled(p.sram_enabled != 0U);
    cart_.set_sram_write_protect(p.sram_wp != 0U);
    cart_.set_ssf2_banks(p.ssf2_banks);
    if (!cart_.sram().empty() && bytes.size() >= sizeof(MdStatePayload) + cart_.sram().size()) {
        std::memcpy(cart_.sram_mut().data(), bytes.data() + sizeof(MdStatePayload),
                    cart_.sram().size());
    }
    last_sync_cycles_ = cpu.cycles;
    return true;
}

}  // namespace dragon::md
