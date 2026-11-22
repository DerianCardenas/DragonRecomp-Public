#include "psp/bus.h"

#include <algorithm>
#include <cstring>

#include "psp/loader.h"

namespace dragon::psp {

PspBus::PspBus() : vram_(kVramSize, 0), main_ram_(kMainRamSize, 0) {}

void PspBus::reset() {
    scratchpad_.fill(0);
    std::fill(vram_.begin(), vram_.end(), uint8_t{0});
    std::fill(main_ram_.begin(), main_ram_.end(), uint8_t{0});
    heap_cursor_ = 0x09000000U;
}

void PspBus::load_image(const LoadedPspImage& img) {
    reset();
    for (const auto& seg : img.segments) {
        if (seg.memsz == 0)
            continue;
        // Zero-fill memsz then copy initialized data
        uint32_t addr = seg.vaddr;
        for (uint32_t i = 0; i < seg.memsz; ++i) {
            write8(addr + i, i < seg.data.size() ? seg.data[i] : 0U);
        }
    }
    // Patch imported NID stubs: `jr $ra` (0x03E00008) followed by `syscall <code_idx>`
    for (const auto& imp : img.imports) {
        if (imp.stub_addr != 0) {
            const uint32_t jr_ra = 0x03E00008U;
            const uint32_t syscall_insn = 0x0000000CU | ((imp.syscall_code & 0xFFFFFU) << 6U);
            write32(imp.stub_addr, jr_ra);
            write32(imp.stub_addr + 4U, syscall_insn);
        }
    }
    set_heap_base(img.image_end);
}

uint8_t* PspBus::ptr(uint32_t addr) noexcept {
    const uint32_t phys = normalize_addr(addr);
    if (phys >= kMainRamBase && phys < kMainRamBase + kMainRamSize) {
        return main_ram_.data() + (phys - kMainRamBase);
    }
    if (phys >= kVramBase && phys < kVramBase + 0x00800000U) {
        // 2 MB VRAM mirrored 4x across 0x04000000..0x047FFFFF
        return vram_.data() + ((phys - kVramBase) & (kVramSize - 1U));
    }
    if (phys >= kScratchpadBase && phys < kScratchpadBase + kScratchpadSize) {
        return scratchpad_.data() + (phys - kScratchpadBase);
    }
    return nullptr;
}

const uint8_t* PspBus::ptr(uint32_t addr) const noexcept {
    const uint32_t phys = normalize_addr(addr);
    if (phys >= kMainRamBase && phys < kMainRamBase + kMainRamSize) {
        return main_ram_.data() + (phys - kMainRamBase);
    }
    if (phys >= kVramBase && phys < kVramBase + 0x00800000U) {
        return vram_.data() + ((phys - kVramBase) & (kVramSize - 1U));
    }
    if (phys >= kScratchpadBase && phys < kScratchpadBase + kScratchpadSize) {
        return scratchpad_.data() + (phys - kScratchpadBase);
    }
    return nullptr;
}

uint8_t PspBus::read8(uint32_t addr) const noexcept {
    const uint8_t* p = ptr(addr);
    return p ? *p : 0U;
}

uint16_t PspBus::read16(uint32_t addr) const noexcept {
    const uint32_t aligned = addr & ~1U;
    const uint8_t* p = ptr(aligned);
    if (!p)
        return 0U;
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8U);
}

uint32_t PspBus::read32(uint32_t addr) const noexcept {
    const uint32_t aligned = addr & ~3U;
    const uint8_t* p = ptr(aligned);
    if (!p)
        return 0U;
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
}

void PspBus::write8(uint32_t addr, uint8_t val) noexcept {
    uint8_t* p = ptr(addr);
    if (p)
        *p = val;
}

void PspBus::write16(uint32_t addr, uint16_t val) noexcept {
    const uint32_t aligned = addr & ~1U;
    uint8_t* p = ptr(aligned);
    if (!p)
        return;
    p[0] = static_cast<uint8_t>(val & 0xFFU);
    p[1] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
}

void PspBus::write32(uint32_t addr, uint32_t val) noexcept {
    const uint32_t aligned = addr & ~3U;
    uint8_t* p = ptr(aligned);
    if (!p)
        return;
    p[0] = static_cast<uint8_t>(val & 0xFFU);
    p[1] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
    p[2] = static_cast<uint8_t>((val >> 16U) & 0xFFU);
    p[3] = static_cast<uint8_t>((val >> 24U) & 0xFFU);
}

std::string PspBus::read_cstring(uint32_t addr, size_t max_len) const {
    std::string out;
    out.reserve(32);
    for (size_t i = 0; i < max_len; ++i) {
        const uint8_t ch = read8(addr + static_cast<uint32_t>(i));
        if (ch == 0)
            break;
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

void PspBus::write_bytes(uint32_t addr, const void* src, size_t len) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < len; ++i) {
        write8(addr + static_cast<uint32_t>(i), bytes[i]);
    }
}

void PspBus::read_bytes(uint32_t addr, void* dst, size_t len) const noexcept {
    auto* bytes = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < len; ++i) {
        bytes[i] = read8(addr + static_cast<uint32_t>(i));
    }
}

void PspBus::set_heap_base(uint32_t base_addr) noexcept {
    uint32_t aligned = (std::max(base_addr, 0x08900000U) + 0xFFFU) & ~0xFFFU;
    if (aligned >= heap_limit_)
        aligned = 0x09000000U;
    heap_cursor_ = aligned;
}

uint32_t PspBus::alloc_user_block(uint32_t size, uint32_t align) noexcept {
    if (align < 16U)
        align = 16U;
    const uint32_t mask = align - 1U;
    const uint32_t addr = (heap_cursor_ + mask) & ~mask;
    if (size > heap_limit_ || addr > heap_limit_ - size) {
        return 0U;
    }
    heap_cursor_ = addr + size;
    return addr;
}

uint32_t PspBus::free_user_mem_bytes() const noexcept {
    return (heap_cursor_ < heap_limit_) ? (heap_limit_ - heap_cursor_) : 0U;
}

}  // namespace dragon::psp
