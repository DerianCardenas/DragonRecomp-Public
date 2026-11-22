#include "ps1/bios_hle.h"

#include <algorithm>
#include <cstring>

#include "ps1/runtime.h"

namespace dragon::ps1 {

namespace {

std::string read_ram_cstr(Ps1Bus& bus, uint32_t addr, size_t max_len = 256U) {
    std::string out;
    for (size_t i = 0; i < max_len; ++i) {
        const uint8_t c = bus.read8(addr + static_cast<uint32_t>(i));
        if (c == 0U)
            break;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

}  // namespace

Ps1BiosHle::Ps1BiosHle() {
    reset();
}

void Ps1BiosHle::reset() {
    events_.clear();
    files_.clear();
    pad_buf1_addr_ = 0;
    pad_buf2_addr_ = 0;
    pad_started_ = false;
    calls_handled_ = 0;
    console_output_.clear();
}

void Ps1BiosHle::on_vblank(Ps1Runtime& rt) {
    Ps1Bus& bus = rt.bus();
    // Update BIOS PAD buffer if InitPAD + StartPAD was used
    if (pad_started_ && pad_buf1_addr_ != 0U) {
        const uint16_t btns_low = static_cast<uint16_t>(~bus.pad_buttons());
        bus.write8(pad_buf1_addr_ + 0U, 0x00U);  // Status OK
        bus.write8(pad_buf1_addr_ + 1U, 0x41U);  // Digital pad ID
        bus.write8(pad_buf1_addr_ + 2U, static_cast<uint8_t>(btns_low & 0xFFU));
        bus.write8(pad_buf1_addr_ + 3U, static_cast<uint8_t>(btns_low >> 8U));
    }
    // Deliver enabled VBlank events (class 0xF2000003 or RCnt3)
    for (auto& ev : events_) {
        if (ev.open && ev.enabled && (ev.cls == 0xF2000003U || ev.cls == 0xF0000001U)) {
            ev.delivered = true;
        }
    }
}

void Ps1BiosHle::handle_syscall(Ps1Runtime& rt, Ps1CpuState& cpu, uint32_t code) {
    (void)rt;
    (void)code;
    ++calls_handled_;
    const uint32_t fn = cpu.gpr[4];  // $a0
    switch (fn) {
        case 1U:  // EnterCriticalSection: disable interrupts in COP0 SR, return old state
            cpu.gpr[2] = ((cpu.cop0[12] & 0x0401U) == 0x0401U) ? 1U : 0U;
            cpu.cop0[12] &= ~0x0401U;
            break;
        case 2U:  // ExitCriticalSection: re-enable interrupts
            cpu.cop0[12] |= 0x0401U;
            cpu.gpr[2] = 0U;
            break;
        default:
            cpu.gpr[2] = 0U;
            break;
    }
}

void Ps1BiosHle::handle_vector(Ps1Runtime& rt, Ps1CpuState& cpu, uint32_t vector_addr) {
    ++calls_handled_;
    const uint32_t vec = vector_addr & 0x1FFFFFFFU;
    const uint32_t fn = cpu.gpr[9] & 0xFFU;  // $t1 holds BIOS function index
    if (vec == 0x000000A0U)
        handle_a_table(rt, cpu, fn);
    else if (vec == 0x000000B0U)
        handle_b_table(rt, cpu, fn);
    else if (vec == 0x000000C0U)
        handle_c_table(rt, cpu, fn);
}

void Ps1BiosHle::handle_a_table(Ps1Runtime& rt, Ps1CpuState& cpu, uint32_t fn) {
    Ps1Bus& bus = rt.bus();
    const uint32_t a0 = cpu.gpr[4];
    const uint32_t a1 = cpu.gpr[5];
    const uint32_t a2 = cpu.gpr[6];
    uint32_t& v0 = cpu.gpr[2];

    switch (fn) {
        case 0x00: {  // FileOpen(name, mode)
            std::string name = read_ram_cstr(bus, a0);
            const auto colon = name.find(':');
            if (colon != std::string::npos)
                name = name.substr(colon + 1U);
            const Ps1IsoEntry* ent = rt.image().find_iso_entry(name);
            if (!ent || ent->is_dir) {
                v0 = 0xFFFFFFFFU;
                return;
            }
            BiosFile bf{};
            bf.fd = static_cast<int>(files_.size()) + 3;
            bf.open = true;
            bf.iso_lba = ent->lba;
            bf.size = ent->size;
            bf.cursor = 0U;
            files_.push_back(bf);
            v0 = static_cast<uint32_t>(bf.fd);
            return;
        }
        case 0x01: {  // FileSeek(fd, offset, seektype)
            for (auto& f : files_) {
                if (f.fd == static_cast<int>(a0) && f.open) {
                    int64_t next = static_cast<int64_t>(f.cursor);
                    const int64_t off = static_cast<int32_t>(a1);
                    if (a2 == 0U)
                        next = off;
                    else if (a2 == 1U)
                        next += off;
                    else if (a2 == 2U)
                        next = static_cast<int64_t>(f.size) + off;
                    f.cursor = static_cast<uint32_t>(std::clamp<int64_t>(next, 0, f.size));
                    v0 = f.cursor;
                    return;
                }
            }
            v0 = 0xFFFFFFFFU;
            return;
        }
        case 0x02: {  // FileRead(fd, dst, len)
            for (auto& f : files_) {
                if (f.fd == static_cast<int>(a0) && f.open) {
                    const uint32_t avail = (f.cursor < f.size) ? (f.size - f.cursor) : 0U;
                    const uint32_t n = std::min(avail, a2);
                    if (n > 0U) {
                        Ps1IsoEntry tmp{"", f.iso_lba, f.size, false};
                        std::vector<uint8_t> buf(n);
                        if (rt.image().read_disc_file(tmp, f.cursor, buf.data(), n)) {
                            for (uint32_t i = 0; i < n; ++i)
                                bus.write8(a1 + i, buf[i]);
                            f.cursor += n;
                            v0 = n;
                            return;
                        }
                    }
                    v0 = 0U;
                    return;
                }
            }
            v0 = 0xFFFFFFFFU;
            return;
        }
        case 0x04: {  // FileClose(fd)
            for (auto& f : files_) {
                if (f.fd == static_cast<int>(a0) && f.open) {
                    f.open = false;
                    v0 = a0;
                    return;
                }
            }
            v0 = 0xFFFFFFFFU;
            return;
        }
        case 0x17: {  // strcmp(s1, s2)
            const std::string s1 = read_ram_cstr(bus, a0);
            const std::string s2 = read_ram_cstr(bus, a1);
            const int cmp = s1.compare(s2);
            v0 = static_cast<uint32_t>((cmp < 0) ? -1 : (cmp > 0 ? 1 : 0));
            return;
        }
        case 0x1B: {  // strlen(s)
            v0 = static_cast<uint32_t>(read_ram_cstr(bus, a0, 4096U).size());
            return;
        }
        case 0x2A: {  // memcpy(dst, src, len)
            for (uint32_t i = 0; i < a2; ++i)
                bus.write8(a0 + i, bus.read8(a1 + i));
            v0 = a0;
            return;
        }
        case 0x2B: {  // memset(dst, fill, len)
            for (uint32_t i = 0; i < a2; ++i)
                bus.write8(a0 + i, static_cast<uint8_t>(a1 & 0xFFU));
            v0 = a0;
            return;
        }
        case 0x3C: {  // putchar(c)
            console_output_.push_back(static_cast<char>(a0 & 0xFFU));
            v0 = a0 & 0xFFU;
            return;
        }
        case 0x3F: {  // printf(fmt, ...)
            console_output_ += read_ram_cstr(bus, a0, 256U);
            v0 = 0U;
            return;
        }
        case 0x44:  // FlushCache
        case 0x54:  // CdInit
        case 0x70:  // _bu_init
        case 0x71:  // CdInit
        case 0x72:  // CdRemove
            v0 = 1U;
            return;
        default:
            v0 = 0U;
            return;
    }
}

void Ps1BiosHle::handle_b_table(Ps1Runtime& rt, Ps1CpuState& cpu, uint32_t fn) {
    const uint32_t a0 = cpu.gpr[4];
    const uint32_t a1 = cpu.gpr[5];
    const uint32_t a2 = cpu.gpr[6];
    const uint32_t a3 = cpu.gpr[7];
    uint32_t& v0 = cpu.gpr[2];

    switch (fn) {
        case 0x07: {  // DeliverEvent(class, spec)
            for (auto& ev : events_) {
                if (ev.open && ev.enabled && ev.cls == a0 && ev.spec == a1)
                    ev.delivered = true;
            }
            v0 = 0U;
            return;
        }
        case 0x08: {  // OpenEvent(class, spec, mode, func)
            BiosEvent ev{};
            ev.handle = 0xF1000000U + static_cast<uint32_t>(events_.size());
            ev.cls = a0;
            ev.spec = a1;
            ev.mode = a2;
            ev.handler_pc = a3;
            ev.open = true;
            ev.enabled = false;
            ev.delivered = false;
            events_.push_back(ev);
            v0 = ev.handle;
            return;
        }
        case 0x09: {  // CloseEvent(handle)
            for (auto& ev : events_)
                if (ev.handle == a0)
                    ev.open = false;
            v0 = 1U;
            return;
        }
        case 0x0A: {  // WaitEvent(handle)
            for (auto& ev : events_) {
                if (ev.handle == a0 && ev.open) {
                    if (ev.delivered) {
                        ev.delivered = false;
                        v0 = 1U;
                        return;
                    }
                    cpu.vblank_yield = 1U;
                    v0 = 1U;
                    return;
                }
            }
            v0 = 1U;
            return;
        }
        case 0x0B: {  // TestEvent(handle)
            for (auto& ev : events_) {
                if (ev.handle == a0 && ev.open && ev.delivered) {
                    ev.delivered = false;
                    v0 = 1U;
                    return;
                }
            }
            v0 = 0U;
            return;
        }
        case 0x0C: {  // EnableEvent(handle)
            for (auto& ev : events_)
                if (ev.handle == a0 && ev.open)
                    ev.enabled = true;
            v0 = 1U;
            return;
        }
        case 0x0D: {  // DisableEvent(handle)
            for (auto& ev : events_)
                if (ev.handle == a0 && ev.open)
                    ev.enabled = false;
            v0 = 1U;
            return;
        }
        case 0x12:  // InitPAD(buf1, siz1, buf2, siz2)
            pad_buf1_addr_ = a0;
            pad_buf2_addr_ = a2;
            v0 = 1U;
            return;
        case 0x13:  // StartPAD()
            pad_started_ = true;
            v0 = 1U;
            return;
        case 0x14:  // StopPAD()
            pad_started_ = false;
            v0 = 0U;
            return;
        case 0x17:  // ReturnFromException
            cpu.cop0[12] = (cpu.cop0[12] & ~0x0FU) | ((cpu.cop0[12] >> 2U) & 0x0FU);
            v0 = 0U;
            return;
        case 0x32:  // FileOpen -> forward to A(0x00)
            handle_a_table(rt, cpu, 0x00U);
            return;
        case 0x33:  // FileSeek -> forward to A(0x01)
            handle_a_table(rt, cpu, 0x01U);
            return;
        case 0x34:  // FileRead -> forward to A(0x02)
            handle_a_table(rt, cpu, 0x02U);
            return;
        case 0x36:  // FileClose -> forward to A(0x04)
            handle_a_table(rt, cpu, 0x04U);
            return;
        case 0x3D:  // putchar
            handle_a_table(rt, cpu, 0x3CU);
            return;
        case 0x5B:  // ChangeClearPAD
            v0 = 0U;
            return;
        default:
            v0 = 0U;
            return;
    }
}

void Ps1BiosHle::handle_c_table(Ps1Runtime& rt, Ps1CpuState& cpu, uint32_t fn) {
    (void)rt;
    (void)fn;
    cpu.gpr[2] = 0U;
}

}  // namespace dragon::ps1
