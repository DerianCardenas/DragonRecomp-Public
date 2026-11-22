#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dragon::psp {

struct LoadedPspImage;

class PspBus {
public:
    static constexpr uint32_t kScratchpadBase = 0x00010000U;
    static constexpr uint32_t kScratchpadSize = 16U * 1024U;  // 16 KB
    static constexpr uint32_t kVramBase = 0x04000000U;
    static constexpr uint32_t kVramSize = 2U * 1024U * 1024U;  // 2 MB eDRAM
    static constexpr uint32_t kMainRamBase = 0x08000000U;
    static constexpr uint32_t kMainRamSize = 64U * 1024U * 1024U;  // 64 MB (PSP-2000/3000)

    PspBus();
    void reset();
    void load_image(const LoadedPspImage& img);

    // Normalize PSP KUSEG/KSEG0/KSEG1/uncached mirror addresses into physical space.
    static inline uint32_t normalize_addr(uint32_t addr) noexcept { return addr & 0x3FFFFFFFU; }

    uint8_t* ptr(uint32_t addr) noexcept;
    const uint8_t* ptr(uint32_t addr) const noexcept;
    bool is_valid_addr(uint32_t addr) const noexcept { return ptr(addr) != nullptr; }

    uint8_t read8(uint32_t addr) const noexcept;
    uint16_t read16(uint32_t addr) const noexcept;
    uint32_t read32(uint32_t addr) const noexcept;

    void write8(uint32_t addr, uint8_t val) noexcept;
    void write16(uint32_t addr, uint16_t val) noexcept;
    void write32(uint32_t addr, uint32_t val) noexcept;

    std::string read_cstring(uint32_t addr, size_t max_len = 256U) const;
    void write_bytes(uint32_t addr, const void* src, size_t len) noexcept;
    void read_bytes(uint32_t addr, void* dst, size_t len) const noexcept;

    // Raw storage accessors (for GE VRAM reads, save states, and tests)
    uint8_t* scratchpad() noexcept { return scratchpad_.data(); }
    const uint8_t* scratchpad() const noexcept { return scratchpad_.data(); }
    uint8_t* vram() noexcept { return vram_.data(); }
    const uint8_t* vram() const noexcept { return vram_.data(); }
    uint8_t* main_ram() noexcept { return main_ram_.data(); }
    const uint8_t* main_ram() const noexcept { return main_ram_.data(); }

    // Simple bump/block user-memory allocator for SysMemUserForUser (grows from image_end up)
    void set_heap_base(uint32_t base_addr) noexcept;
    uint32_t alloc_user_block(uint32_t size, uint32_t align = 64U) noexcept;
    uint32_t free_user_mem_bytes() const noexcept;

private:
    std::array<uint8_t, kScratchpadSize> scratchpad_{};
    std::vector<uint8_t> vram_;
    std::vector<uint8_t> main_ram_;
    uint32_t heap_cursor_ = 0x09000000U;
    uint32_t heap_limit_ = 0x0BF00000U;
};

}  // namespace dragon::psp
