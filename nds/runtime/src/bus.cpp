#include "nds/bus.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "dragon/save_state.h"
#include "nds/interpreter.h"

namespace dragon::nds {

namespace {

constexpr uint32_t kNdsStateVersion = 1U;

inline uint16_t rd16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8U));
}

inline uint32_t rd32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
}

inline void wr16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>(v >> 8U);
}

inline void wr32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
    p[3] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
}

uint16_t crc16_nds(const uint8_t* data, size_t len) noexcept {
    static constexpr uint16_t kVal[8] = {0xC0C1, 0xC181, 0xC301, 0xC601,
                                         0xCC01, 0xD801, 0xF001, 0xA001};
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 1U) {
                crc = static_cast<uint16_t>((crc >> 1U) ^ (kVal[b] << (7 - b)));
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc;
}

}  // namespace

Bus::Bus() {
    main_ram_.assign(4U * 1024U * 1024U, 0U);
    init_firmware_settings();
}

void Bus::init_firmware_settings() noexcept {
    fw_flash_.assign(256U * 1024U, 0xFFU);
    // Firmware header (0x000..0x1FF)
    std::memset(fw_flash_.data(), 0U, 0x200U);
    // User settings offset = 0x7FC0 (* 8 = 0x3FE00)
    wr16(&fw_flash_[0x20U], 0x7FC0U);
    // WiFi settings: length = 0x0138, MAC address = 00:09:BF:12:34:56, channels = 0x3FFE
    wr16(&fw_flash_[0x2CU], 0x0138U);
    fw_flash_[0x36U] = 0x00U;
    fw_flash_[0x37U] = 0x09U;
    fw_flash_[0x38U] = 0xBFU;
    fw_flash_[0x39U] = 0x12U;
    fw_flash_[0x3AU] = 0x34U;
    fw_flash_[0x3BU] = 0x56U;
    wr16(&fw_flash_[0x3CU], 0x3FFEU);
    const uint16_t wifi_crc = crc16_nds(&fw_flash_[0x2CU], 0x138U);
    wr16(&fw_flash_[0x2AU], wifi_crc);

    // User Settings block at 0x3FE00 (and backup copy at 0x3FF00)
    uint8_t* us = &fw_flash_[0x3FE00U];
    std::memset(us, 0U, 0x100U);
    us[0x00] = 5U;  // Version 5
    us[0x02] = 2U;  // Favorite color = blue
    us[0x03] = 1U;  // Birthday month = Jan
    us[0x04] = 1U;  // Birthday day = 1
    const char* nick = "DRAGON";
    for (int i = 0; i < 6; ++i) {
        us[0x06 + i * 2] = static_cast<uint8_t>(nick[i]);
    }
    us[0x1A] = 6U;
    // Touchscreen calibration (1:1 linear mapping with non-zero point 1 so NitroSDK accepts it)
    wr16(&us[0x58], 0x0010U);
    wr16(&us[0x5A], 0x0010U);
    us[0x5C] = 1U;
    us[0x5D] = 1U;
    wr16(&us[0x5E], 0x0FF0U);
    wr16(&us[0x60], 0x0BF0U);
    us[0x62] = 255U;
    us[0x63] = 191U;
    wr16(&us[0x64], 0x0001U);  // English
    wr16(&us[0x70], 0x0001U);  // Update counter
    const uint16_t crc = crc16_nds(us, 0x70U);
    wr16(&us[0x72], crc);
    std::memcpy(&fw_flash_[0x3FF00U], us, 0x100U);
}

bool Bus::load_rom(const std::filesystem::path& path, std::string* err) {
    return cart_.load_from_file(path, err);
}

bool Bus::load_rom_bytes(const std::vector<uint8_t>& raw, std::string* err) {
    return cart_.load_from_bytes(raw, err);
}

void Bus::reset(NdsCpuState& arm9, NdsCpuState& arm7) noexcept {
    arm7_ptr_ = &arm7;
    std::fill(main_ram_.begin(), main_ram_.end(), 0U);
    itcm_.fill(0U);
    dtcm_.fill(0U);
    shared_wram_.fill(0U);
    arm7_wram_.fill(0U);
    palette_ram_.fill(0U);
    oam_.fill(0U);
    vram_a_.fill(0U);
    vram_b_.fill(0U);
    vram_c_.fill(0U);
    vram_d_.fill(0U);
    vram_e_.fill(0U);
    vram_f_.fill(0U);
    vram_g_.fill(0U);
    vram_h_.fill(0U);
    vram_i_.fill(0U);
    vramcnt_.fill(0U);
    wramcnt_ = 0x03U;

    combined_fb_.fill(0xFF000000U);
    engine_a_.reset();
    engine_b_.reset();
    gx3d_.reset();
    spu_.reset();
    cart_.reset();

    dispstat9_ = dispstat7_ = 0U;
    vcount_ = 0U;
    powcnt1_ = 0x820FU;
    exmemcnt_ = 0U;
    postflg9_ = 1U;
    postflg7_ = 1U;
    last_sync_arm9_ = 0ULL;
    scanline_cycle_acc_ = 0ULL;
    frame_count_ = 0ULL;

    arm9_ime_ = arm9_ie_ = arm9_if_ = 0U;
    arm7_ime_ = arm7_ie_ = arm7_if_ = 0U;

    ipcsync9_ = ipcsync7_ = 0U;
    ipcfifocnt9_ = ipcfifocnt7_ = 0U;
    fifo_9to7_r_ = fifo_9to7_w_ = fifo_9to7_cnt_ = 0U;
    fifo_7to9_r_ = fifo_7to9_w_ = fifo_7to9_cnt_ = 0U;
    fifo_9to7_last_ = fifo_7to9_last_ = 0U;

    divcnt_ = 0U;
    div_numer_ = div_denom_ = div_result_ = div_rem_ = 0;
    sqrtcnt_ = 0U;
    sqrt_param_ = 0ULL;
    sqrt_result_ = 0U;

    dma9_.fill(DmaChannel{});
    dma7_.fill(DmaChannel{});
    dma9_fill_.fill(0U);
    tmr9_.fill(TimerChannel{});
    tmr7_.fill(TimerChannel{});

    spicnt_ = 0U;
    spidata_ = 0U;
    tsc_pending_ = 0U;
    tsc_byte_ = 0U;
    fw_cmd_ = 0U;
    fw_pos_ = 0U;
    fw_addr_ = 0U;
    pmic_cmd_ = 0U;
    pmic_pos_ = 0U;
    pmic_regs_ = {0x0DU, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U};

    // Direct Boot: load ARM9 & ARM7 binaries from cartridge into RAM
    uint32_t arm9_entry = 0x02000000U;
    uint32_t arm7_entry = 0x02380000U;

    if (cart_.is_loaded()) {
        const auto& h = cart_.header();
        arm9_entry = h.arm9_entry_addr;
        arm7_entry = h.arm7_entry_addr;

        // Copy ARM9 binary
        for (uint32_t i = 0; i < h.arm9_size; ++i) {
            arm9_write8(h.arm9_ram_addr + i, cart_.read_rom8(h.arm9_rom_offset + i));
        }
        // Copy ARM7 binary
        for (uint32_t i = 0; i < h.arm7_size; ++i) {
            arm7_write8(h.arm7_ram_addr + i, cart_.read_rom8(h.arm7_rom_offset + i));
        }

        // Populate NDS Direct Boot Info block in Main RAM (GBATEK)
        // 0x027FFE00..0x027FFF6F: Copy of ROM header 0x000..0x16F
        for (uint32_t i = 0; i < 0x170U; ++i) {
            main_ram_[0x3FFE00U + i] = cart_.read_rom8(i);
        }
        // Chip ID copies
        wr32(&main_ram_[0x3FF800U], cart_.chip_id());
        wr32(&main_ram_[0x3FF804U], cart_.chip_id());
        wr32(&main_ram_[0x3FFC00U], cart_.chip_id());
        wr32(&main_ram_[0x3FFC04U], cart_.chip_id());
        // Header CRC16 & secure area status
        wr16(&main_ram_[0x3FF808U], rd16(&main_ram_[0x3FFE00U + 0x15EU]));
        wr16(&main_ram_[0x3FF80AU], rd16(&main_ram_[0x3FFE00U + 0x06CU]));
        wr16(&main_ram_[0x3FFC08U], rd16(&main_ram_[0x3FFE00U + 0x15EU]));
        wr16(&main_ram_[0x3FFC0AU], rd16(&main_ram_[0x3FFE00U + 0x06CU]));
        // Boot indicator (1 = normal) & User Settings at 0x027FFC80
        wr16(&main_ram_[0x3FFC40U], 0x0001U);
        std::memcpy(&main_ram_[0x3FFC80U], &fw_flash_[0x3FE00U], 0x74U);
    }

    cpu_reset_arm9(arm9, *this, arm9_entry);
    cpu_reset_arm7(arm7, *this, arm7_entry);
    ram_page_dirty_.fill(0U);
    trace_ipc_ = (std::getenv("NDS_TRACE_IPC") != nullptr);
}

// ============================================================================
// VRAM Bank Mapping Helpers (Banks A..I)
// ============================================================================

uint8_t Bus::read_vram_lcdc(uint32_t offset) const noexcept {
    if (offset < 0x20000U)
        return vram_a_[offset];
    if (offset < 0x40000U)
        return vram_b_[offset - 0x20000U];
    if (offset < 0x60000U)
        return vram_c_[offset - 0x40000U];
    if (offset < 0x80000U)
        return vram_d_[offset - 0x60000U];
    if (offset < 0x90000U)
        return vram_e_[offset - 0x80000U];
    if (offset < 0x94000U)
        return vram_f_[offset - 0x90000U];
    if (offset < 0x98000U)
        return vram_g_[offset - 0x94000U];
    if (offset < 0xA0000U)
        return vram_h_[offset - 0x98000U];
    if (offset < 0xA4000U)
        return vram_i_[offset - 0xA0000U];
    return 0U;
}

void Bus::write_vram_lcdc(uint32_t offset, uint8_t val) noexcept {
    if (offset < 0x20000U)
        vram_a_[offset] = val;
    else if (offset < 0x40000U)
        vram_b_[offset - 0x20000U] = val;
    else if (offset < 0x60000U)
        vram_c_[offset - 0x40000U] = val;
    else if (offset < 0x80000U)
        vram_d_[offset - 0x60000U] = val;
    else if (offset < 0x90000U)
        vram_e_[offset - 0x80000U] = val;
    else if (offset < 0x94000U)
        vram_f_[offset - 0x90000U] = val;
    else if (offset < 0x98000U)
        vram_g_[offset - 0x94000U] = val;
    else if (offset < 0xA0000U)
        vram_h_[offset - 0x98000U] = val;
    else if (offset < 0xA4000U)
        vram_i_[offset - 0xA0000U] = val;
}

uint8_t Bus::read_vram_bg_a(uint32_t offset) const noexcept {
    const uint32_t off = offset & 0x7FFFFU;
    uint8_t res = 0U;
    for (int i = 0; i < 4; ++i) {
        if ((vramcnt_[i] & 0x87U) == 0x81U) {  // Enabled, MST=1 (BG A)
            const uint32_t base = ((vramcnt_[i] >> 3U) & 3U) * 0x20000U;
            if (off >= base && off < base + 0x20000U) {
                const auto& bank =
                    (i == 0) ? vram_a_ : (i == 1 ? vram_b_ : (i == 2 ? vram_c_ : vram_d_));
                res |= bank[off - base];
            }
        }
    }
    if ((vramcnt_[4] & 0x87U) == 0x81U && off < 0x10000U) {
        res |= vram_e_[off];
    }
    for (int i = 5; i <= 6; ++i) {
        if ((vramcnt_[i] & 0x87U) == 0x81U) {
            const uint32_t ofs = (vramcnt_[i] >> 3U) & 3U;
            const uint32_t base = ((ofs & 1U) * 0x4000U) + ((ofs & 2U) * 0x8000U);
            if (off >= base && off < base + 0x4000U) {
                res |= (i == 5 ? vram_f_ : vram_g_)[off - base];
            }
        }
    }
    return res;
}

uint8_t Bus::read_vram_obj_a(uint32_t offset) const noexcept {
    const uint32_t off = offset & 0x3FFFFU;
    uint8_t res = 0U;
    for (int i = 0; i < 2; ++i) {
        if ((vramcnt_[i] & 0x87U) == 0x82U) {
            const uint32_t base = ((vramcnt_[i] >> 3U) & 1U) * 0x20000U;
            if (off >= base && off < base + 0x20000U) {
                res |= (i == 0 ? vram_a_ : vram_b_)[off - base];
            }
        }
    }
    if ((vramcnt_[4] & 0x87U) == 0x82U && off < 0x10000U) {
        res |= vram_e_[off];
    }
    for (int i = 5; i <= 6; ++i) {
        if ((vramcnt_[i] & 0x87U) == 0x82U) {
            const uint32_t ofs = (vramcnt_[i] >> 3U) & 3U;
            const uint32_t base = ((ofs & 1U) * 0x4000U) + ((ofs & 2U) * 0x8000U);
            if (off >= base && off < base + 0x4000U) {
                res |= (i == 5 ? vram_f_ : vram_g_)[off - base];
            }
        }
    }
    return res;
}

uint8_t Bus::read_vram_bg_b(uint32_t offset) const noexcept {
    const uint32_t off = offset & 0x1FFFFU;
    uint8_t res = 0U;
    if ((vramcnt_[2] & 0x87U) == 0x84U) {
        res |= vram_c_[off];
    }
    if ((vramcnt_[7] & 0x87U) == 0x81U && off < 0x8000U) {
        res |= vram_h_[off];
    }
    if ((vramcnt_[8] & 0x87U) == 0x81U && off >= 0x8000U && off < 0xC000U) {
        res |= vram_i_[off - 0x8000U];
    }
    return res;
}

uint8_t Bus::read_vram_obj_b(uint32_t offset) const noexcept {
    const uint32_t off = offset & 0x1FFFFU;
    uint8_t res = 0U;
    if ((vramcnt_[3] & 0x87U) == 0x84U) {
        res |= vram_d_[off];
    }
    if ((vramcnt_[8] & 0x87U) == 0x82U && off < 0x4000U) {
        res |= vram_i_[off];
    }
    return res;
}

uint8_t Bus::read_vram_tex(uint32_t offset) const noexcept {
    const uint32_t off = offset & 0x7FFFFU;
    const uint32_t slot = off >> 17U;
    for (int i = 0; i < 4; ++i) {
        if ((vramcnt_[i] & 0x87U) == 0x83U && (((vramcnt_[i] >> 3U) & 3U) == slot)) {
            const auto& bank =
                (i == 0) ? vram_a_ : (i == 1 ? vram_b_ : (i == 2 ? vram_c_ : vram_d_));
            return bank[off & 0x1FFFFU];
        }
    }
    return 0U;
}

uint8_t Bus::read_vram_tex_pal(uint32_t offset) const noexcept {
    if (offset >= 0x18000U) {
        return 0U;
    }
    const uint32_t off = offset;
    if ((vramcnt_[4] & 0x87U) == 0x83U && off < 0x10000U) {
        return vram_e_[off];
    }
    for (int i = 5; i <= 6; ++i) {
        if ((vramcnt_[i] & 0x87U) == 0x83U) {
            const uint32_t ofs = (vramcnt_[i] >> 3U) & 3U;
            const uint32_t base = ((ofs & 1U) * 0x4000U) + ((ofs & 2U) * 0x8000U);
            if (off >= base && off < base + 0x4000U) {
                return (i == 5 ? vram_f_ : vram_g_)[off - base];
            }
        }
    }
    return 0U;
}

uint8_t Bus::read_vram_region(uint32_t addr) const noexcept {
    const uint32_t region = addr & 0x00E00000U;
    if (region == 0x00000000U)
        return read_vram_bg_a(addr & 0x1FFFFFU);
    if (region == 0x00200000U)
        return read_vram_bg_b(addr & 0x1FFFFFU);
    if (region == 0x00400000U)
        return read_vram_obj_a(addr & 0x1FFFFFU);
    if (region == 0x00600000U)
        return read_vram_obj_b(addr & 0x1FFFFFU);
    return read_vram_lcdc(addr & 0xFFFFFU);
}

void Bus::write_vram_region(uint32_t addr, uint8_t val) noexcept {
    const uint32_t region = addr & 0x00E00000U;
    if (region == 0x00800000U) {
        write_vram_lcdc(addr & 0xFFFFFU, val);
        return;
    }
    if (region == 0x00000000U) {
        const uint32_t off = addr & 0x7FFFFU;
        for (int i = 0; i < 4; ++i) {
            if ((vramcnt_[i] & 0x87U) == 0x81U) {
                const uint32_t base = ((vramcnt_[i] >> 3U) & 3U) * 0x20000U;
                if (off >= base && off < base + 0x20000U) {
                    auto& bank =
                        (i == 0) ? vram_a_ : (i == 1 ? vram_b_ : (i == 2 ? vram_c_ : vram_d_));
                    bank[off - base] = val;
                }
            }
        }
        if ((vramcnt_[4] & 0x87U) == 0x81U && off < 0x10000U)
            vram_e_[off] = val;
        for (int i = 5; i <= 6; ++i) {
            if ((vramcnt_[i] & 0x87U) == 0x81U) {
                const uint32_t ofs = (vramcnt_[i] >> 3U) & 3U;
                const uint32_t base = ((ofs & 1U) * 0x4000U) + ((ofs & 2U) * 0x8000U);
                if (off >= base && off < base + 0x4000U)
                    (i == 5 ? vram_f_ : vram_g_)[off - base] = val;
            }
        }
    } else if (region == 0x00200000U) {
        const uint32_t off = addr & 0x1FFFFU;
        if ((vramcnt_[2] & 0x87U) == 0x84U)
            vram_c_[off] = val;
        if ((vramcnt_[7] & 0x87U) == 0x81U && off < 0x8000U)
            vram_h_[off] = val;
        if ((vramcnt_[8] & 0x87U) == 0x81U && off >= 0x8000U && off < 0xC000U)
            vram_i_[off - 0x8000U] = val;
    } else if (region == 0x00400000U) {
        const uint32_t off = addr & 0x3FFFFU;
        for (int i = 0; i < 2; ++i) {
            if ((vramcnt_[i] & 0x87U) == 0x82U) {
                const uint32_t base = ((vramcnt_[i] >> 3U) & 1U) * 0x20000U;
                if (off >= base && off < base + 0x20000U)
                    (i == 0 ? vram_a_ : vram_b_)[off - base] = val;
            }
        }
        if ((vramcnt_[4] & 0x87U) == 0x82U && off < 0x10000U)
            vram_e_[off] = val;
        for (int i = 5; i <= 6; ++i) {
            if ((vramcnt_[i] & 0x87U) == 0x82U) {
                const uint32_t ofs = (vramcnt_[i] >> 3U) & 3U;
                const uint32_t base = ((ofs & 1U) * 0x4000U) + ((ofs & 2U) * 0x8000U);
                if (off >= base && off < base + 0x4000U)
                    (i == 5 ? vram_f_ : vram_g_)[off - base] = val;
            }
        }
    } else if (region == 0x00600000U) {
        const uint32_t off = addr & 0x1FFFFU;
        if ((vramcnt_[3] & 0x87U) == 0x84U)
            vram_d_[off] = val;
        if ((vramcnt_[8] & 0x87U) == 0x82U && off < 0x4000U)
            vram_i_[off] = val;
    }
}

uint16_t Bus::read_pal16(bool engine_a, uint32_t offset) const noexcept {
    const uint32_t base = engine_a ? 0U : 0x400U;
    return rd16(&palette_ram_[base + (offset & 0x3FEU)]);
}

uint16_t Bus::read_bg_ext_pal16(bool engine_a, int slot, uint32_t pal_idx,
                                uint32_t color_idx) const noexcept {
    const uint32_t off = ((pal_idx & 0x0FU) * 256U + (color_idx & 0xFFU)) * 2U;
    const uint32_t s = static_cast<uint32_t>(slot & 3);
    if (engine_a) {
        if ((vramcnt_[4] & 0x87U) == 0x84U) {
            return rd16(&vram_e_[s * 0x2000U + off]);
        }
        for (int i = 5; i <= 6; ++i) {
            if ((vramcnt_[i] & 0x87U) == 0x84U) {
                const uint32_t base_slot = ((vramcnt_[i] >> 3U) & 1U) * 2U;
                if (s >= base_slot && s < base_slot + 2U) {
                    const auto& bank = (i == 5) ? vram_f_ : vram_g_;
                    return rd16(&bank[(s - base_slot) * 0x2000U + off]);
                }
            }
        }
    } else {
        if ((vramcnt_[7] & 0x87U) == 0x82U) {
            return rd16(&vram_h_[s * 0x2000U + off]);
        }
    }
    return read_pal16(engine_a, (color_idx & 0xFFU) * 2U);
}

uint16_t Bus::read_obj_ext_pal16(bool engine_a, uint32_t pal_idx,
                                 uint32_t color_idx) const noexcept {
    // GBATEK DS Video Memory Mapping:
    // Engine A OBJ Ext Palette (8 KB, 16x256x2B): VRAM-F or VRAM-G (MST=5)
    // Engine B OBJ Ext Palette (8 KB, 16x256x2B): VRAM-I (MST=3)
    const uint32_t off = ((pal_idx & 0x0FU) * 256U + (color_idx & 0xFFU)) * 2U;
    if (engine_a) {
        if ((vramcnt_[5] & 0x87U) == 0x85U) {
            return rd16(&vram_f_[off]);
        }
        if ((vramcnt_[6] & 0x87U) == 0x85U) {
            return rd16(&vram_g_[off]);
        }
    } else {
        if ((vramcnt_[8] & 0x87U) == 0x83U) {
            return rd16(&vram_i_[off]);
        }
    }
    return read_pal16(engine_a, 0x200U + (color_idx & 0xFFU) * 2U);
}

uint16_t Bus::read_oam16(bool engine_a, uint32_t offset) const noexcept {
    const uint32_t base = engine_a ? 0U : 0x400U;
    return rd16(&oam_[base + (offset & 0x3FEU)]);
}

uint8_t Bus::spu_read8(uint32_t addr) const noexcept {
    const uint32_t top = addr >> 24U;
    if (top == 0x02U) {
        return main_ram_[addr & 0x3FFFFFU];
    }
    if (top == 0x03U) {
        if (addr < 0x03800000U) {
            if (wramcnt_ == 3U)
                return shared_wram_[addr & 0x7FFFU];
            if (wramcnt_ == 2U)
                return shared_wram_[0x4000U + (addr & 0x3FFFU)];
            if (wramcnt_ == 1U)
                return shared_wram_[addr & 0x3FFFU];
        }
        return arm7_wram_[addr & 0xFFFFU];
    }
    return 0U;
}

// ============================================================================
// Hardware Math (ARM9 Div & Sqrt)
// ============================================================================

void Bus::update_div() noexcept {
    const uint32_t mode = divcnt_ & 3U;
    divcnt_ &= ~(1U << 14);
    int64_t num = div_numer_;
    int64_t den = div_denom_;
    if (mode == 0U) {
        num = static_cast<int32_t>(div_numer_);
        den = static_cast<int32_t>(div_denom_);
    } else if (mode == 1U || mode == 3U) {
        den = static_cast<int32_t>(div_denom_);
    }
    if (den == 0) {
        divcnt_ |= (1U << 14);
        div_result_ = (num < 0) ? 1 : -1;
        div_rem_ = num;
    } else if (num == (-0x7FFFFFFFFFFFFFFFLL - 1LL) && den == -1) {
        div_result_ = num;
        div_rem_ = 0;
    } else {
        div_result_ = num / den;
        div_rem_ = num % den;
    }
}

void Bus::update_sqrt() noexcept {
    const uint64_t val = (sqrtcnt_ & 1U) ? sqrt_param_ : static_cast<uint32_t>(sqrt_param_);
    uint64_t res = 0ULL;
    uint64_t bit = 1ULL << 62U;
    uint64_t n = val;
    while (bit > n)
        bit >>= 2U;
    while (bit != 0ULL) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1U) + bit;
        } else {
            res >>= 1U;
        }
        bit >>= 2U;
    }
    sqrt_result_ = static_cast<uint32_t>(res);
}

// ============================================================================
// DMA & Timers
// ============================================================================

void Bus::run_dma9(int ch, int mode_filter, NdsCpuState* cpu) noexcept {
    auto& d = dma9_[ch];
    if ((d.cnt & (1U << 31)) == 0U)
        return;
    const int mode = static_cast<int>((d.cnt >> 27U) & 7U);
    if (mode != mode_filter)
        return;

    const bool is_32 = (d.cnt & (1U << 26)) != 0U;
    uint32_t count = d.cnt & 0x1FFFFFU;
    if (count == 0U)
        count = 0x200000U;
    const uint32_t step = is_32 ? 4U : 2U;
    const int dst_ctl = static_cast<int>((d.cnt >> 21U) & 3U);
    const int src_ctl = static_cast<int>((d.cnt >> 23U) & 3U);

    for (uint32_t i = 0; i < count; ++i) {
        if (is_32) {
            const uint32_t v = arm9_read32(d.int_sad, cpu);
            arm9_write32(d.int_dad, v, cpu);
        } else {
            const uint16_t v = arm9_read16(d.int_sad, cpu);
            arm9_write16(d.int_dad, v, cpu);
        }
        if (src_ctl == 0)
            d.int_sad += step;
        else if (src_ctl == 1)
            d.int_sad -= step;
        if (dst_ctl == 0 || dst_ctl == 3)
            d.int_dad += step;
        else if (dst_ctl == 1)
            d.int_dad -= step;
    }

    if ((d.cnt & (1U << 25)) == 0U || mode == 0) {
        d.cnt &= ~(1U << 31);
    } else if (dst_ctl == 3) {
        d.int_dad = d.dad;
    }
    if (d.cnt & (1U << 30)) {
        raise_arm9_irq(kIrqDma0 << ch);
    }
}

void Bus::run_dma7(int ch, int mode_filter) noexcept {
    auto& d = dma7_[ch];
    if ((d.cnt & (1U << 31)) == 0U)
        return;
    const int mode = static_cast<int>((d.cnt >> 28U) & 3U);
    if (mode != mode_filter)
        return;

    const bool is_32 = (d.cnt & (1U << 26)) != 0U;
    uint32_t count = d.cnt & 0xFFFFU;
    if (count == 0U)
        count = 0x10000U;
    const uint32_t step = is_32 ? 4U : 2U;
    const int dst_ctl = static_cast<int>((d.cnt >> 21U) & 3U);
    const int src_ctl = static_cast<int>((d.cnt >> 23U) & 3U);

    for (uint32_t i = 0; i < count; ++i) {
        if (is_32) {
            arm7_write32(d.int_dad, arm7_read32(d.int_sad));
        } else {
            arm7_write16(d.int_dad, arm7_read16(d.int_sad));
        }
        if (src_ctl == 0)
            d.int_sad += step;
        else if (src_ctl == 1)
            d.int_sad -= step;
        if (dst_ctl == 0 || dst_ctl == 3)
            d.int_dad += step;
        else if (dst_ctl == 1)
            d.int_dad -= step;
    }

    if ((d.cnt & (1U << 25)) == 0U || mode == 0) {
        d.cnt &= ~(1U << 31);
    } else if (dst_ctl == 3) {
        d.int_dad = d.dad;
    }
    if (d.cnt & (1U << 30)) {
        raise_arm7_irq(kIrqDma0 << ch);
    }
}

void Bus::step_timers(std::array<TimerChannel, 4>& tmrs, uint32_t cycles, bool is_arm9) noexcept {
    static constexpr uint32_t kPrescale[4] = {1U, 64U, 256U, 1024U};
    // ARM9 timers tick at 33.51 MHz (half ARM9 clock)
    const uint32_t base_ticks = is_arm9 ? (cycles >> 1U) : cycles;
    uint32_t prev_overflows = 0U;

    for (int i = 0; i < 4; ++i) {
        auto& t = tmrs[i];
        if ((t.cnt & (1U << 7)) == 0U) {
            prev_overflows = 0U;
            continue;
        }
        const bool cascade = (i > 0) && ((t.cnt & (1U << 2)) != 0U);
        uint32_t inc = 0U;
        if (cascade) {
            inc = prev_overflows;
        } else {
            const uint32_t p = kPrescale[t.cnt & 3U];
            t.sub_cycles += base_ticks;
            inc = t.sub_cycles / p;
            t.sub_cycles %= p;
        }

        uint32_t ov = 0U;
        if (inc > 0U) {
            uint32_t cur = t.counter + inc;
            if (cur >= 0x10000U) {
                const uint32_t span = std::max<uint32_t>(1U, 0x10000U - t.reload);
                const uint32_t extra = cur - 0x10000U;
                ov = 1U + (extra / span);
                t.counter = static_cast<uint16_t>(t.reload + (extra % span));
                if (t.cnt & (1U << 6)) {
                    if (is_arm9)
                        raise_arm9_irq(kIrqTimer0 << i);
                    else
                        raise_arm7_irq(kIrqTimer0 << i);
                }
            } else {
                t.counter = static_cast<uint16_t>(cur);
            }
        }
        prev_overflows = ov;
    }
}

// ============================================================================
// ARM7 SPI (Powerman, Firmware Flash, TSC2046 Touchscreen)
// ============================================================================

void Bus::spi_transfer(uint8_t val) noexcept {
    const uint32_t dev = (spicnt_ >> 8U) & 3U;
    const bool hold = (spicnt_ & (1U << 11)) != 0U;

    if (dev == 2U) {
        // TSC2046 Touchscreen Controller
        if (val & 0x80U) {
            // Pipelined TSC2046: output remaining low byte of previous conversion if any
            spidata_ = (tsc_byte_ == 1U) ? static_cast<uint8_t>(tsc_pending_ & 0xFFU) : 0U;
            const uint32_t ch = (val >> 4U) & 7U;
            uint16_t adc = 0U;
            if (ch == 1U) {  // Touch Y
                adc = touch_down_ ? static_cast<uint16_t>(touch_y_ << 4U) : 0x0FFFU;
            } else if (ch == 5U) {  // Touch X
                adc = touch_down_ ? static_cast<uint16_t>(touch_x_ << 4U) : 0x0000U;
            } else if (ch == 6U) {  // AUX / Mic
                adc = 0x0800U;
            }
            tsc_pending_ = static_cast<uint16_t>(adc << 3U);
            tsc_byte_ = 0U;
        } else {
            if (tsc_byte_ == 0U) {
                spidata_ = static_cast<uint8_t>(tsc_pending_ >> 8U);
                tsc_byte_ = 1U;
            } else {
                spidata_ = static_cast<uint8_t>(tsc_pending_ & 0xFFU);
                tsc_byte_ = 0U;
            }
        }
    } else if (dev == 1U) {
        // Firmware Flash (256 KB NVRAM: ST M45PE20)
        if (fw_pos_ == 0U) {
            fw_cmd_ = val;
            fw_addr_ = 0U;
            spidata_ = 0U;
            if (fw_cmd_ == 0x06U) {
                fw_status_ |= 0x02U;  // WREN
            } else if (fw_cmd_ == 0x04U) {
                fw_status_ &= ~0x02U;  // WRDI
            }
            fw_pos_ = 1U;
        } else {
            switch (fw_cmd_) {
                case 0x05U:  // RDSR
                    spidata_ = fw_status_;
                    break;
                case 0x9FU:  // RDID (JEDEC ID: 256 KB ST M45PE20)
                    spidata_ = (fw_pos_ == 1U) ? 0x20U : (fw_pos_ == 2U ? 0x40U : 0x12U);
                    break;
                case 0x03U:  // READ (24-bit address)
                    if (fw_pos_ <= 3U) {
                        fw_addr_ = (fw_addr_ << 8U) | val;
                        spidata_ = 0U;
                    } else {
                        spidata_ = fw_flash_[fw_addr_ & 0x3FFFFU];
                        ++fw_addr_;
                    }
                    break;
                case 0x02U:  // PP (Page Program)
                case 0x0AU:  // PW (Page Write)
                    if (fw_pos_ <= 3U) {
                        fw_addr_ = (fw_addr_ << 8U) | val;
                    } else if (fw_status_ & 0x02U) {
                        fw_flash_[fw_addr_ & 0x3FFFFU] = val;
                        ++fw_addr_;
                    }
                    spidata_ = 0U;
                    break;
                default:
                    spidata_ = 0xFFU;
                    break;
            }
            ++fw_pos_;
        }
        if (!hold) {
            if (fw_cmd_ == 0x02U || fw_cmd_ == 0x0AU) {
                fw_status_ &= ~0x02U;
            }
            fw_pos_ = 0U;
        }
    } else {
        // PMIC (Power Management IC, dev == 0): 5 registers (0x00..0x04)
        if (pmic_pos_ == 0U) {
            pmic_cmd_ = val;
            pmic_pos_ = 1U;
            spidata_ = 0U;
        } else {
            const uint8_t reg = pmic_cmd_ & 0x07U;
            if (pmic_cmd_ & 0x80U) {
                spidata_ = pmic_regs_[reg];
            } else {
                if (reg != 1U) {  // Reg 1 is read-only battery status
                    pmic_regs_[reg] = val;
                }
                spidata_ = 0U;
            }
            pmic_pos_ = 0U;
        }
        if (!hold) {
            pmic_pos_ = 0U;
        }
    }

    if (spicnt_ & (1U << 14)) {
        raise_arm7_irq(1U << 23);  // SPI IRQ
    }
}

// ============================================================================
// ARM9 Bus Read / Write
// ============================================================================

uint8_t Bus::arm9_read8(uint32_t addr, const NdsCpuState* cpu) noexcept {
    // 1) DTCM check (if enabled via CP15)
    if (cpu && (cpu->cp15_ctrl & (1U << 16)) && addr >= cpu->dtcm_base &&
        addr < cpu->dtcm_base + cpu->dtcm_size) {
        return dtcm_[(addr - cpu->dtcm_base) & 0x3FFFU];
    }
    // 2) ITCM check
    if (cpu && (cpu->cp15_ctrl & (1U << 18)) && addr < cpu->itcm_size) {
        return itcm_[addr & 0x7FFFU];
    }
    const uint32_t top = addr >> 24U;
    switch (top) {
        case 0x00U:
            return itcm_[addr & 0x7FFFU];
        case 0x02U:
            return main_ram_[addr & 0x3FFFFFU];
        case 0x03U:
            if (wramcnt_ == 0U)
                return shared_wram_[addr & 0x7FFFU];
            if (wramcnt_ == 1U)
                return shared_wram_[0x4000U + (addr & 0x3FFFU)];
            if (wramcnt_ == 2U)
                return shared_wram_[addr & 0x3FFFU];
            return 0U;
        case 0x04U: {
            const uint32_t w = arm9_read32(addr & ~3U, cpu);
            return static_cast<uint8_t>((w >> ((addr & 3U) * 8U)) & 0xFFU);
        }
        case 0x05U:
            return palette_ram_[addr & 0x7FFU];
        case 0x06U:
            return read_vram_region(addr);
        case 0x07U:
            return oam_[addr & 0x7FFU];
        default:
            return 0U;
    }
}

uint16_t Bus::arm9_read16(uint32_t addr, const NdsCpuState* cpu) noexcept {
    addr &= ~1U;
    if (cpu && (cpu->cp15_ctrl & (1U << 16)) && addr >= cpu->dtcm_base &&
        addr < cpu->dtcm_base + cpu->dtcm_size) {
        return rd16(&dtcm_[(addr - cpu->dtcm_base) & 0x3FFEU]);
    }
    const uint32_t top = addr >> 24U;
    if (top == 0x02U) {
        return rd16(&main_ram_[addr & 0x3FFFFEU]);
    }
    if (top == 0x04U) {
        const uint32_t w = arm9_read32(addr & ~3U, cpu);
        return static_cast<uint16_t>((w >> ((addr & 2U) * 8U)) & 0xFFFFU);
    }
    return static_cast<uint16_t>(arm9_read8(addr, cpu) |
                                 (static_cast<uint16_t>(arm9_read8(addr + 1U, cpu)) << 8U));
}

uint32_t Bus::arm9_read32(uint32_t addr, const NdsCpuState* cpu) noexcept {
    addr &= ~3U;
    if (cpu && (cpu->cp15_ctrl & (1U << 16)) && addr >= cpu->dtcm_base &&
        addr < cpu->dtcm_base + cpu->dtcm_size) {
        return rd32(&dtcm_[(addr - cpu->dtcm_base) & 0x3FFCU]);
    }
    if (cpu && (cpu->cp15_ctrl & (1U << 18)) && addr < cpu->itcm_size) {
        return rd32(&itcm_[addr & 0x7FFCU]);
    }
    const uint32_t top = addr >> 24U;
    if (top == 0x02U) {
        return rd32(&main_ram_[addr & 0x3FFFFCU]);
    }
    if (top == 0x04U) {
        const uint32_t off = addr & 0xFFFFFFU;
        if (off < 0x60U) {
            if (off == 0x04U) {
                return static_cast<uint32_t>(dispstat9_) | (static_cast<uint32_t>(vcount_) << 16U);
            }
            return engine_a_.read32(off);
        }
        if (off == 0x60U)
            return gx3d_.read_disp3dcnt();
        if (off == 0x6CU)
            return engine_a_.read_master_bright();
        if (off >= 0xB0U && off < 0xE0U) {
            const int ch = static_cast<int>((off - 0xB0U) / 12U);
            const uint32_t r = (off - 0xB0U) % 12U;
            return (r == 0U) ? dma9_[ch].sad : (r == 4U ? dma9_[ch].dad : dma9_[ch].cnt);
        }
        if (off >= 0xE0U && off < 0xF0U)
            return dma9_fill_[(off - 0xE0U) >> 2U];
        if (off >= 0x100U && off < 0x110U) {
            const int ch = static_cast<int>((off - 0x100U) >> 2U);
            return static_cast<uint32_t>(tmr9_[ch].counter) |
                   (static_cast<uint32_t>(tmr9_[ch].cnt) << 16U);
        }
        if (off == 0x130U) {
            return static_cast<uint32_t>((~buttons_) & 0x03FFU);
        }
        if (off == 0x180U) {
            return static_cast<uint32_t>(ipcsync9_);
        }
        if (off == 0x184U) {
            uint16_t c = ipcfifocnt9_ & 0x8404U;
            if (fifo_9to7_cnt_ == 0U)
                c |= 0x0001U;
            if (fifo_9to7_cnt_ >= 16U)
                c |= 0x0002U;
            if (fifo_7to9_cnt_ == 0U)
                c |= 0x0100U;
            if (fifo_7to9_cnt_ >= 16U)
                c |= 0x0200U;
            return c;
        }
        if (off == 0x1A0U) {
            return static_cast<uint32_t>(cart_.read_auxspicnt()) |
                   (static_cast<uint32_t>(cart_.read_auxspidata()) << 16U);
        }
        if (off == 0x1A4U)
            return cart_.read_romctrl();
        if (off == 0x204U)
            return exmemcnt_;
        if (off == 0x208U)
            return arm9_ime_;
        if (off == 0x210U)
            return arm9_ie_;
        if (off == 0x214U)
            return arm9_if_;
        if (off == 0x240U) {
            return static_cast<uint32_t>(vramcnt_[0]) | (static_cast<uint32_t>(vramcnt_[1]) << 8U) |
                   (static_cast<uint32_t>(vramcnt_[2]) << 16U) |
                   (static_cast<uint32_t>(vramcnt_[3]) << 24U);
        }
        if (off == 0x244U) {
            return static_cast<uint32_t>(vramcnt_[4]) | (static_cast<uint32_t>(vramcnt_[5]) << 8U) |
                   (static_cast<uint32_t>(vramcnt_[6]) << 16U) |
                   (static_cast<uint32_t>(wramcnt_) << 24U);
        }
        if (off == 0x248U) {
            return static_cast<uint32_t>(vramcnt_[7]) | (static_cast<uint32_t>(vramcnt_[8]) << 8U);
        }
        if (off == 0x280U)
            return divcnt_;
        if (off == 0x290U)
            return static_cast<uint32_t>(div_numer_);
        if (off == 0x294U)
            return static_cast<uint32_t>(static_cast<uint64_t>(div_numer_) >> 32U);
        if (off == 0x298U)
            return static_cast<uint32_t>(div_denom_);
        if (off == 0x29CU)
            return static_cast<uint32_t>(static_cast<uint64_t>(div_denom_) >> 32U);
        if (off == 0x2A0U)
            return static_cast<uint32_t>(div_result_);
        if (off == 0x2A4U)
            return static_cast<uint32_t>(static_cast<uint64_t>(div_result_) >> 32U);
        if (off == 0x2A8U)
            return static_cast<uint32_t>(div_rem_);
        if (off == 0x2ACU)
            return static_cast<uint32_t>(static_cast<uint64_t>(div_rem_) >> 32U);
        if (off == 0x2B0U)
            return sqrtcnt_;
        if (off == 0x2B4U)
            return sqrt_result_;
        if (off == 0x2B8U)
            return static_cast<uint32_t>(sqrt_param_);
        if (off == 0x2BCU)
            return static_cast<uint32_t>(sqrt_param_ >> 32U);
        if (off == 0x300U)
            return postflg9_;
        if (off == 0x304U)
            return powcnt1_;
        if (off >= 0x320U && off < 0x700U)
            return gx3d_.read32(off);
        if (off >= 0x1000U && off < 0x1060U)
            return engine_b_.read32(off - 0x1000U);
        if (off == 0x106CU)
            return engine_b_.read_master_bright();
        if (off == 0x100000U) {
            // IPCFIFORECEIVE (ARM7 -> ARM9)
            if ((ipcfifocnt9_ & (1U << 15)) && fifo_7to9_cnt_ > 0U) {
                fifo_7to9_last_ = fifo_7to9_[fifo_7to9_r_];
                fifo_7to9_r_ = (fifo_7to9_r_ + 1U) & 15U;
                --fifo_7to9_cnt_;
                if (fifo_7to9_cnt_ == 0U && (ipcfifocnt7_ & (1U << 2))) {
                    raise_arm7_irq(kIrqIpcSendEmpty);
                }
            }
            return fifo_7to9_last_;
        }
        if (off == 0x100010U) {
            const uint32_t w = cart_.read_carddata();
            if (cart_.consume_transfer_complete_irq()) {
                raise_arm9_irq(kIrqCardComplete);
            }
            return w;
        }
        return 0U;
    }
    return static_cast<uint32_t>(arm9_read16(addr, cpu)) |
           (static_cast<uint32_t>(arm9_read16(addr + 2U, cpu)) << 16U);
}

void Bus::arm9_write8(uint32_t addr, uint8_t val, NdsCpuState* cpu) noexcept {
    if (cpu && (cpu->cp15_ctrl & (1U << 16)) && addr >= cpu->dtcm_base &&
        addr < cpu->dtcm_base + cpu->dtcm_size) {
        dtcm_[(addr - cpu->dtcm_base) & 0x3FFFU] = val;
        return;
    }
    if (cpu && (cpu->cp15_ctrl & (1U << 18)) && addr < cpu->itcm_size) {
        itcm_[addr & 0x7FFFU] = val;
        return;
    }
    const uint32_t top = addr >> 24U;
    switch (top) {
        case 0x00U:
            itcm_[addr & 0x7FFFU] = val;
            return;
        case 0x02U:
            main_ram_[addr & 0x3FFFFFU] = val;
            ram_page_dirty_[(addr & 0x3FFFFFU) >> 12U] = 1U;
            return;
        case 0x03U:
            if (wramcnt_ == 0U)
                shared_wram_[addr & 0x7FFFU] = val;
            else if (wramcnt_ == 1U)
                shared_wram_[0x4000U + (addr & 0x3FFFU)] = val;
            else if (wramcnt_ == 2U)
                shared_wram_[addr & 0x3FFFU] = val;
            return;
        case 0x04U: {
            const uint32_t off = addr & 0xFFFFFFU;
            if (off >= 0x240U && off <= 0x249U) {
                if (off == 0x247U)
                    wramcnt_ = val & 3U;
                else if (off < 0x247U)
                    vramcnt_[off - 0x240U] = val;
                else
                    vramcnt_[off - 0x241U] = val;
                return;
            }
            if (off >= 0x1A8U && off <= 0x1AFU) {
                cart_.write_cardcmd(static_cast<int>(off - 0x1A8U), val);
                return;
            }
            if (off == 0x1A0U) {
                cart_.write_auxspicnt((cart_.read_auxspicnt() & 0xFF00U) | val);
                return;
            }
            if (off == 0x1A1U) {
                cart_.write_auxspicnt((cart_.read_auxspicnt() & 0x00FFU) |
                                      (static_cast<uint16_t>(val) << 8U));
                return;
            }
            if (off == 0x1A2U) {
                cart_.write_auxspidata(val);
                return;
            }
            if (off == 0x181U) {
                ipcsync9_ = (ipcsync9_ & 0x000FU) | ((static_cast<uint16_t>(val) << 8U) & 0x4F00U);
                ipcsync7_ = (ipcsync7_ & 0xFFF0U) | (val & 0x0FU);
                if ((val & (1U << 5)) && (ipcsync7_ & (1U << 14))) {
                    raise_arm7_irq(kIrqIpcSync);
                }
                return;
            }
            if (off == 0x208U) {
                arm9_ime_ = val & 1U;
                return;
            }
            if (off == 0x06CU) {
                engine_a_.write_master_bright((engine_a_.read_master_bright() & 0xFF00U) | val);
                return;
            }
            if (off == 0x06DU) {
                engine_a_.write_master_bright((engine_a_.read_master_bright() & 0x00FFU) |
                                              (static_cast<uint16_t>(val) << 8U));
                return;
            }
            if (off == 0x106CU) {
                engine_b_.write_master_bright((engine_b_.read_master_bright() & 0xFF00U) | val);
                return;
            }
            if (off == 0x106DU) {
                engine_b_.write_master_bright((engine_b_.read_master_bright() & 0x00FFU) |
                                              (static_cast<uint16_t>(val) << 8U));
                return;
            }
            if (off == 0x300U) {
                postflg9_ = val & 3U;
                return;
            }
            if (off < 0x60U) {
                const uint32_t shift = (addr & 3U) * 8U;
                engine_a_.write32(off & ~3U, static_cast<uint32_t>(val) << shift, 0xFFU << shift);
                return;
            }
            if (off >= 0x1000U && off < 0x1060U) {
                const uint32_t shift = (addr & 3U) * 8U;
                engine_b_.write32((off - 0x1000U) & ~3U, static_cast<uint32_t>(val) << shift,
                                  0xFFU << shift);
                return;
            }
            return;
        }
        case 0x05U:
            palette_ram_[addr & 0x7FFU] = val;
            return;
        case 0x06U:
            write_vram_region(addr, val);
            return;
        case 0x07U:
            oam_[addr & 0x7FFU] = val;
            return;
        default:
            return;
    }
}

void Bus::arm9_write16(uint32_t addr, uint16_t val, NdsCpuState* cpu) noexcept {
    addr &= ~1U;
    if (cpu && (cpu->cp15_ctrl & (1U << 16)) && addr >= cpu->dtcm_base &&
        addr < cpu->dtcm_base + cpu->dtcm_size) {
        wr16(&dtcm_[(addr - cpu->dtcm_base) & 0x3FFEU], val);
        return;
    }
    const uint32_t top = addr >> 24U;
    if (top == 0x02U) {
        wr16(&main_ram_[addr & 0x3FFFFEU], val);
        ram_page_dirty_[(addr & 0x3FFFFFU) >> 12U] = 1U;
        return;
    }
    if (top == 0x04U) {
        const uint32_t off = addr & 0xFFFFFFU;
        if (off == 0x04U) {
            dispstat9_ = (dispstat9_ & 0x0007U) | (val & 0xFFB8U);
            return;
        }
        if (off == 0x60U) {
            gx3d_.write_disp3dcnt(val);
            return;
        }
        if (off == 0x6CU) {
            engine_a_.write_master_bright(val);
            return;
        }
        if (off == 0x106CU) {
            engine_b_.write_master_bright(val);
            return;
        }
        if (off == 0x1A0U) {
            cart_.write_auxspicnt(val);
            return;
        }
        if (off == 0x1A2U) {
            cart_.write_auxspidata(static_cast<uint8_t>(val & 0xFFU));
            return;
        }
        if (off == 0x180U) {
            ipcsync9_ = (ipcsync9_ & 0x000FU) | (val & 0x4F00U);
            ipcsync7_ = (ipcsync7_ & 0xFFF0U) | ((val >> 8U) & 0x0FU);
            if ((val & (1U << 13)) && (ipcsync7_ & (1U << 14))) {
                raise_arm7_irq(kIrqIpcSync);
            }
            return;
        }
        if (off == 0x184U) {
            if (val & (1U << 3)) {
                fifo_9to7_r_ = fifo_9to7_w_ = fifo_9to7_cnt_ = 0U;
            }
            ipcfifocnt9_ = val & 0x8404U;
            if ((val & (1U << 2)) && fifo_9to7_cnt_ == 0U) {
                raise_arm9_irq(kIrqIpcSendEmpty);
            }
            if ((val & (1U << 10)) && fifo_7to9_cnt_ > 0U) {
                raise_arm9_irq(kIrqIpcRecvNotEmpty);
            }
            return;
        }
        if (off == 0x204U) {
            exmemcnt_ = val;
            return;
        }
        if (off == 0x208U) {
            arm9_ime_ = val & 1U;
            return;
        }
        if (off == 0x210U) {
            arm9_ie_ = (arm9_ie_ & 0xFFFF0000U) | val;
            return;
        }
        if (off == 0x212U) {
            arm9_ie_ = (arm9_ie_ & 0x0000FFFFU) | (static_cast<uint32_t>(val) << 16U);
            return;
        }
        if (off == 0x214U) {
            arm9_if_ &= ~static_cast<uint32_t>(val);
            return;
        }
        if (off == 0x216U) {
            arm9_if_ &= ~(static_cast<uint32_t>(val) << 16U);
            return;
        }
        if (off >= 0xB0U && off < 0xE0U) {
            const int ch = static_cast<int>((off - 0xB0U) / 12U);
            const uint32_t r = (off - 0xB0U) % 12U;
            if (r == 8U) {
                dma9_[ch].cnt = (dma9_[ch].cnt & 0xFFFF0000U) | val;
            } else if (r == 10U) {
                const uint32_t next =
                    (dma9_[ch].cnt & 0x0000FFFFU) | (static_cast<uint32_t>(val) << 16U);
                arm9_write32(addr - 2U, next, cpu);
            }
            return;
        }
        if (off == 0x280U) {
            divcnt_ = static_cast<uint16_t>(val & 3U);
            update_div();
            return;
        }
        if (off == 0x2B0U) {
            sqrtcnt_ = static_cast<uint16_t>(val & 1U);
            update_sqrt();
            return;
        }
        if (off == 0x354U) {
            gx3d_.write32(0x354U, val);
            return;
        }
        if (off == 0x304U) {
            powcnt1_ = val;
            return;
        }
        if (off >= 0x100U && off < 0x110U) {
            const int ch = static_cast<int>((off - 0x100U) >> 2U);
            if ((off & 2U) == 0U) {
                tmr9_[ch].reload = val;
            } else {
                const bool was_en = (tmr9_[ch].cnt & (1U << 7)) != 0U;
                tmr9_[ch].cnt = val;
                if (!was_en && (val & (1U << 7))) {
                    tmr9_[ch].counter = tmr9_[ch].reload;
                    tmr9_[ch].sub_cycles = 0U;
                }
            }
            return;
        }
        if (off < 0x60U) {
            const uint32_t shift = (off & 2U) * 8U;
            engine_a_.write32(off & ~3U, static_cast<uint32_t>(val) << shift, 0xFFFFU << shift);
            return;
        }
        if (off >= 0x1000U && off < 0x1060U) {
            const uint32_t shift = (off & 2U) * 8U;
            engine_b_.write32((off - 0x1000U) & ~3U, static_cast<uint32_t>(val) << shift,
                              0xFFFFU << shift);
            return;
        }
    }
    arm9_write8(addr, static_cast<uint8_t>(val & 0xFFU), cpu);
    arm9_write8(addr + 1U, static_cast<uint8_t>(val >> 8U), cpu);
}

void Bus::arm9_write32(uint32_t addr, uint32_t val, NdsCpuState* cpu) noexcept {
    addr &= ~3U;
    if (cpu && (cpu->cp15_ctrl & (1U << 16)) && addr >= cpu->dtcm_base &&
        addr < cpu->dtcm_base + cpu->dtcm_size) {
        wr32(&dtcm_[(addr - cpu->dtcm_base) & 0x3FFCU], val);
        return;
    }
    if (cpu && (cpu->cp15_ctrl & (1U << 18)) && addr < cpu->itcm_size) {
        wr32(&itcm_[addr & 0x7FFCU], val);
        return;
    }
    const uint32_t top = addr >> 24U;
    if (top == 0x02U) {
        wr32(&main_ram_[addr & 0x3FFFFCU], val);
        ram_page_dirty_[(addr & 0x3FFFFFU) >> 12U] = 1U;
        return;
    }
    if (top == 0x04U) {
        const uint32_t off = addr & 0xFFFFFFU;
        if (off < 0x60U) {
            if (off == 0x04U) {
                dispstat9_ = (dispstat9_ & 0x0007U) | (static_cast<uint16_t>(val) & 0xFFB8U);
                return;
            }
            engine_a_.write32(off, val, 0xFFFFFFFFU);
            return;
        }
        if (off >= 0x1000U && off < 0x1060U) {
            engine_b_.write32(off - 0x1000U, val, 0xFFFFFFFFU);
            return;
        }
        if (off >= 0xB0U && off < 0xE0U) {
            const int ch = static_cast<int>((off - 0xB0U) / 12U);
            const uint32_t r = (off - 0xB0U) % 12U;
            if (r == 0U) {
                dma9_[ch].sad = val;
            } else if (r == 4U) {
                dma9_[ch].dad = val;
            } else {
                const bool was_en = (dma9_[ch].cnt & (1U << 31)) != 0U;
                dma9_[ch].cnt = val;
                if (!was_en && (val & (1U << 31))) {
                    const uint32_t mode = (val >> 27U) & 7U;
                    dma9_[ch].int_sad = dma9_[ch].sad;
                    dma9_[ch].int_dad = dma9_[ch].dad;
                    if (mode == 0U) {
                        run_dma9(ch, 0, cpu);  // Immediate DMA
                    } else if (mode == 5U) {
                        uint32_t guard = 0U;
                        while (cart_.has_data_ready() && (dma9_[ch].cnt & (1U << 31)) &&
                               guard++ < 4096U) {
                            run_dma9(ch, 5, cpu);  // DS Cart Slot DMA (word-by-word repeat)
                        }
                        if (cart_.consume_transfer_complete_irq()) {
                            raise_arm9_irq(kIrqCardComplete);
                        }
                    } else if (mode == 7U) {
                        run_dma9(ch, 7, cpu);  // GXFIFO DMA
                    }
                }
            }
            return;
        }
        if (off >= 0xE0U && off < 0xF0U) {
            dma9_fill_[(off - 0xE0U) >> 2U] = val;
            return;
        }
        if (off == 0x188U) {
            // IPCFIFOSEND (ARM9 -> ARM7)
            if (trace_ipc_) {
                std::fprintf(stderr, "[IPC 9->7] f=%llu word=%08X tag=%u data=%07X\n",
                             static_cast<unsigned long long>(frame_count_), val, val & 0x1FU,
                             val >> 6U);
            }
            if ((ipcfifocnt9_ & (1U << 15)) && fifo_9to7_cnt_ < 16U) {
                const bool was_empty = (fifo_9to7_cnt_ == 0U);
                fifo_9to7_[fifo_9to7_w_] = val;
                fifo_9to7_w_ = (fifo_9to7_w_ + 1U) & 15U;
                ++fifo_9to7_cnt_;
                if (was_empty && (ipcfifocnt7_ & (1U << 10))) {
                    raise_arm7_irq(kIrqIpcRecvNotEmpty);
                }
            }
            return;
        }
        if (off == 0x1A4U) {
            cart_.write_romctrl(val);
            // DS Cart Slot DMA fires per ready word until the block completes or DMA disables
            uint32_t guard = 0U;
            while (cart_.has_data_ready() && guard++ < 4096U) {
                bool any = false;
                for (int ch = 0; ch < 4; ++ch) {
                    if ((dma9_[ch].cnt & (1U << 31)) && (((dma9_[ch].cnt >> 27U) & 7U) == 5U)) {
                        run_dma9(ch, 5, cpu);
                        any = true;
                    }
                }
                if (!any) {
                    break;
                }
            }
            if (cart_.consume_transfer_complete_irq()) {
                raise_arm9_irq(kIrqCardComplete);
            }
            return;
        }
        if (off == 0x1A8U || off == 0x1ACU) {
            const int base = (off == 0x1A8U) ? 0 : 4;
            for (int i = 0; i < 4; ++i) {
                cart_.write_cardcmd(base + i, static_cast<uint8_t>((val >> (i * 8)) & 0xFFU));
            }
            return;
        }
        if (off == 0x208U) {
            arm9_ime_ = val & 1U;
            return;
        }
        if (off == 0x210U) {
            arm9_ie_ = val;
            return;
        }
        if (off == 0x214U) {
            arm9_if_ &= ~val;
            return;
        }
        if (off == 0x280U) {
            divcnt_ = static_cast<uint16_t>(val & 3U);
            update_div();
            return;
        }
        if (off == 0x290U) {
            div_numer_ = static_cast<int64_t>(
                (static_cast<uint64_t>(div_numer_) & 0xFFFFFFFF00000000ULL) | val);
            update_div();
            return;
        }
        if (off == 0x294U) {
            div_numer_ = static_cast<int64_t>((static_cast<uint64_t>(div_numer_) & 0xFFFFFFFFULL) |
                                              (static_cast<uint64_t>(val) << 32U));
            update_div();
            return;
        }
        if (off == 0x298U) {
            div_denom_ = static_cast<int64_t>(
                (static_cast<uint64_t>(div_denom_) & 0xFFFFFFFF00000000ULL) | val);
            update_div();
            return;
        }
        if (off == 0x29CU) {
            div_denom_ = static_cast<int64_t>((static_cast<uint64_t>(div_denom_) & 0xFFFFFFFFULL) |
                                              (static_cast<uint64_t>(val) << 32U));
            update_div();
            return;
        }
        if (off == 0x2B0U) {
            sqrtcnt_ = static_cast<uint16_t>(val & 1U);
            update_sqrt();
            return;
        }
        if (off == 0x2B8U) {
            sqrt_param_ = (sqrt_param_ & 0xFFFFFFFF00000000ULL) | val;
            update_sqrt();
            return;
        }
        if (off == 0x2BCU) {
            sqrt_param_ = (sqrt_param_ & 0xFFFFFFFFULL) | (static_cast<uint64_t>(val) << 32U);
            update_sqrt();
            return;
        }
        if (off == 0x300U) {
            postflg9_ = static_cast<uint8_t>(val & 3U);
            return;
        }
        if (off >= 0x320U && off < 0x700U) {
            gx3d_.write32(off, val);
            if (gx3d_.gxfifo_irq_wanted()) {
                raise_arm9_irq(kIrqGxFifo);
            }
            return;
        }
    }
    arm9_write16(addr, static_cast<uint16_t>(val & 0xFFFFU), cpu);
    arm9_write16(addr + 2U, static_cast<uint16_t>(val >> 16U), cpu);
}

// ============================================================================
// ARM7 Bus Read / Write
// ============================================================================

uint8_t Bus::arm7_read8(uint32_t addr) noexcept {
    const uint32_t top = addr >> 24U;
    switch (top) {
        case 0x02U:
            return main_ram_[addr & 0x3FFFFFU];
        case 0x03U:
            if (addr < 0x03800000U) {
                if (wramcnt_ == 3U)
                    return shared_wram_[addr & 0x7FFFU];
                if (wramcnt_ == 2U)
                    return shared_wram_[0x4000U + (addr & 0x3FFFU)];
                if (wramcnt_ == 1U)
                    return shared_wram_[addr & 0x3FFFU];
            }
            return arm7_wram_[addr & 0xFFFFU];
        case 0x04U: {
            const uint32_t w = arm7_read32(addr & ~3U);
            return static_cast<uint8_t>((w >> ((addr & 3U) * 8U)) & 0xFFU);
        }
        case 0x06U: {
            const uint32_t off = addr & 0x3FFFFU;
            if ((vramcnt_[2] & 0x87U) == 0x82U) {
                const uint32_t base = ((vramcnt_[2] >> 3U) & 1U) * 0x20000U;
                if (off >= base && off < base + 0x20000U)
                    return vram_c_[off - base];
            }
            if ((vramcnt_[3] & 0x87U) == 0x82U) {
                const uint32_t base = ((vramcnt_[3] >> 3U) & 1U) * 0x20000U;
                if (off >= base && off < base + 0x20000U)
                    return vram_d_[off - base];
            }
            return 0U;
        }
        default:
            return 0U;
    }
}

uint16_t Bus::arm7_read16(uint32_t addr) noexcept {
    addr &= ~1U;
    const uint32_t top = addr >> 24U;
    if (top == 0x02U)
        return rd16(&main_ram_[addr & 0x3FFFFEU]);
    if (top == 0x04U) {
        const uint32_t w = arm7_read32(addr & ~3U);
        return static_cast<uint16_t>((w >> ((addr & 2U) * 8U)) & 0xFFFFU);
    }
    return static_cast<uint16_t>(arm7_read8(addr) |
                                 (static_cast<uint16_t>(arm7_read8(addr + 1U)) << 8U));
}

uint32_t Bus::arm7_read32(uint32_t addr) noexcept {
    addr &= ~3U;
    const uint32_t top = addr >> 24U;
    if (top == 0x02U)
        return rd32(&main_ram_[addr & 0x3FFFFCU]);
    if (top == 0x04U) {
        const uint32_t off = addr & 0xFFFFFFU;
        if (off == 0x04U) {
            return static_cast<uint32_t>(dispstat7_) | (static_cast<uint32_t>(vcount_) << 16U);
        }
        if (off >= 0xB0U && off < 0xE0U) {
            const int ch = static_cast<int>((off - 0xB0U) / 12U);
            const uint32_t r = (off - 0xB0U) % 12U;
            return (r == 0U) ? dma7_[ch].sad : (r == 4U ? dma7_[ch].dad : dma7_[ch].cnt);
        }
        if (off >= 0x100U && off < 0x110U) {
            const int ch = static_cast<int>((off - 0x100U) >> 2U);
            return static_cast<uint32_t>(tmr7_[ch].counter) |
                   (static_cast<uint32_t>(tmr7_[ch].cnt) << 16U);
        }
        if (off == 0x130U) {
            return static_cast<uint32_t>((~buttons_) & 0x03FFU);
        }
        if (off == 0x134U) {
            // 0x136 = EXTKEYIN (bit 0=X, bit 1=Y, bit 6=Pen Down active-low, bit 7=Hinge open=0)
            uint16_t ext = 0x007FU;
            if (buttons_ & kBtnX)
                ext &= ~0x0001U;
            if (buttons_ & kBtnY)
                ext &= ~0x0002U;
            if (touch_down_)
                ext &= ~0x0040U;
            return static_cast<uint32_t>(ext) << 16U;
        }
        if (off == 0x138U)
            return rtc_reg_;
        if (off == 0x180U)
            return ipcsync7_;
        if (off == 0x184U) {
            uint16_t c = ipcfifocnt7_ & 0x8404U;
            if (fifo_7to9_cnt_ == 0U)
                c |= 0x0001U;
            if (fifo_7to9_cnt_ >= 16U)
                c |= 0x0002U;
            if (fifo_9to7_cnt_ == 0U)
                c |= 0x0100U;
            if (fifo_9to7_cnt_ >= 16U)
                c |= 0x0200U;
            return c;
        }
        if (off == 0x1A0U) {
            return static_cast<uint32_t>(cart_.read_auxspicnt()) |
                   (static_cast<uint32_t>(cart_.read_auxspidata()) << 16U);
        }
        if (off == 0x1A4U)
            return cart_.read_romctrl();
        if (off == 0x1C0U) {
            return static_cast<uint32_t>(spicnt_) | (static_cast<uint32_t>(spidata_) << 16U);
        }
        if (off == 0x208U)
            return arm7_ime_;
        if (off == 0x210U)
            return arm7_ie_;
        if (off == 0x214U)
            return arm7_if_;
        if (off == 0x240U)
            return wramcnt_;
        if (off == 0x300U)
            return postflg7_;
        if (off >= 0x400U && off < 0x500U)
            return spu_.read32(off - 0x400U);
        if (off == 0x500U) {
            return static_cast<uint32_t>(spu_.read_soundcnt());
        }
        if (off == 0x504U) {
            return static_cast<uint32_t>(spu_.read_soundbias());
        }
        if (off == 0x100000U) {
            // IPCFIFORECEIVE (ARM9 -> ARM7)
            if ((ipcfifocnt7_ & (1U << 15)) && fifo_9to7_cnt_ > 0U) {
                fifo_9to7_last_ = fifo_9to7_[fifo_9to7_r_];
                fifo_9to7_r_ = (fifo_9to7_r_ + 1U) & 15U;
                --fifo_9to7_cnt_;
                if (fifo_9to7_cnt_ == 0U && (ipcfifocnt9_ & (1U << 2))) {
                    raise_arm9_irq(kIrqIpcSendEmpty);
                }
            }
            return fifo_9to7_last_;
        }
        if (off == 0x100010U) {
            const uint32_t w = cart_.read_carddata();
            if (cart_.consume_transfer_complete_irq()) {
                raise_arm7_irq(kIrqCardComplete);
            }
            return w;
        }
        return 0U;
    }
    return static_cast<uint32_t>(arm7_read16(addr)) |
           (static_cast<uint32_t>(arm7_read16(addr + 2U)) << 16U);
}

void Bus::arm7_write8(uint32_t addr, uint8_t val) noexcept {
    const uint32_t top = addr >> 24U;
    switch (top) {
        case 0x02U:
            main_ram_[addr & 0x3FFFFFU] = val;
            ram_page_dirty_[(addr & 0x3FFFFFU) >> 12U] = 1U;
            return;
        case 0x03U:
            if (addr < 0x03800000U) {
                if (wramcnt_ == 3U) {
                    shared_wram_[addr & 0x7FFFU] = val;
                    return;
                }
                if (wramcnt_ == 2U) {
                    shared_wram_[0x4000U + (addr & 0x3FFFU)] = val;
                    return;
                }
                if (wramcnt_ == 1U) {
                    shared_wram_[addr & 0x3FFFU] = val;
                    return;
                }
            }
            arm7_wram_[addr & 0xFFFFU] = val;
            return;
        case 0x04U: {
            const uint32_t off = addr & 0xFFFFFFU;
            if (off == 0x138U) {
                rtc_reg_ = val;
                return;
            }
            if (off == 0x181U) {
                ipcsync7_ = (ipcsync7_ & 0x000FU) | ((static_cast<uint16_t>(val) << 8U) & 0x4F00U);
                ipcsync9_ = (ipcsync9_ & 0xFFF0U) | (val & 0x0FU);
                if ((val & (1U << 5)) && (ipcsync9_ & (1U << 14))) {
                    raise_arm9_irq(kIrqIpcSync);
                }
                return;
            }
            if (off == 0x208U) {
                arm7_ime_ = val & 1U;
                return;
            }
            if (off == 0x1A0U) {
                cart_.write_auxspicnt((cart_.read_auxspicnt() & 0xFF00U) | val);
                return;
            }
            if (off == 0x1A1U) {
                cart_.write_auxspicnt((cart_.read_auxspicnt() & 0x00FFU) |
                                      (static_cast<uint16_t>(val) << 8U));
                return;
            }
            if (off == 0x1A2U) {
                cart_.write_auxspidata(val);
                return;
            }
            if (off >= 0x1A8U && off <= 0x1AFU) {
                cart_.write_cardcmd(static_cast<int>(off - 0x1A8U), val);
                return;
            }
            if (off == 0x1C0U) {
                spicnt_ = ((spicnt_ & 0xFF00U) | val) & 0xCF03U;
                return;
            }
            if (off == 0x1C1U) {
                spicnt_ = ((spicnt_ & 0x00FFU) | (static_cast<uint16_t>(val) << 8U)) & 0xCF03U;
                return;
            }
            if (off == 0x1C2U) {
                spi_transfer(val);
                return;
            }
            if (off == 0x300U) {
                postflg7_ = val & 1U;
                return;
            }
            if (off == 0x301U) {
                // HALTCNT (bit 7..6 == 2 -> Halt ARM7)
                if ((val & 0xC0U) == 0x80U && arm7_ptr_) {
                    arm7_ptr_->halted = 1U;
                }
                return;
            }
            const uint32_t shift = (addr & 3U) * 8U;
            const uint32_t mask = 0xFFU << shift;
            if (off >= 0x400U && off < 0x500U) {
                spu_.write32((off - 0x400U) & ~3U, static_cast<uint32_t>(val) << shift, mask,
                             *this);
                return;
            }
            if (off == 0x500U) {
                spu_.write_soundcnt((spu_.read_soundcnt() & 0xFF00U) | val);
                return;
            }
            if (off == 0x501U) {
                spu_.write_soundcnt((spu_.read_soundcnt() & 0x00FFU) |
                                    (static_cast<uint16_t>(val) << 8U));
                return;
            }
            return;
        }
        case 0x06U: {
            const uint32_t off = addr & 0x3FFFFU;
            if ((vramcnt_[2] & 0x87U) == 0x82U) {
                const uint32_t base = ((vramcnt_[2] >> 3U) & 1U) * 0x20000U;
                if (off >= base && off < base + 0x20000U)
                    vram_c_[off - base] = val;
            }
            if ((vramcnt_[3] & 0x87U) == 0x82U) {
                const uint32_t base = ((vramcnt_[3] >> 3U) & 1U) * 0x20000U;
                if (off >= base && off < base + 0x20000U)
                    vram_d_[off - base] = val;
            }
            return;
        }
        default:
            return;
    }
}

void Bus::arm7_write16(uint32_t addr, uint16_t val) noexcept {
    addr &= ~1U;
    const uint32_t top = addr >> 24U;
    if (top == 0x02U) {
        wr16(&main_ram_[addr & 0x3FFFFEU], val);
        ram_page_dirty_[(addr & 0x3FFFFFU) >> 12U] = 1U;
        return;
    }
    if (top == 0x04U) {
        const uint32_t off = addr & 0xFFFFFFU;
        if (off == 0x04U) {
            dispstat7_ = (dispstat7_ & 0x0007U) | (val & 0xFFB8U);
            return;
        }
        if (off == 0x180U) {
            ipcsync7_ = (ipcsync7_ & 0x000FU) | (val & 0x4F00U);
            ipcsync9_ = (ipcsync9_ & 0xFFF0U) | ((val >> 8U) & 0x0FU);
            if ((val & (1U << 13)) && (ipcsync9_ & (1U << 14))) {
                raise_arm9_irq(kIrqIpcSync);
            }
            return;
        }
        if (off == 0x184U) {
            if (val & (1U << 3)) {
                fifo_7to9_r_ = fifo_7to9_w_ = fifo_7to9_cnt_ = 0U;
            }
            ipcfifocnt7_ = val & 0x8404U;
            if ((val & (1U << 2)) && fifo_7to9_cnt_ == 0U) {
                raise_arm7_irq(kIrqIpcSendEmpty);
            }
            if ((val & (1U << 10)) && fifo_9to7_cnt_ > 0U) {
                raise_arm7_irq(kIrqIpcRecvNotEmpty);
            }
            return;
        }
        if (off == 0x1A0U) {
            cart_.write_auxspicnt(val);
            return;
        }
        if (off == 0x1A2U) {
            cart_.write_auxspidata(static_cast<uint8_t>(val & 0xFFU));
            return;
        }
        if (off == 0x1C0U) {
            spicnt_ = val & 0xCF03U;
            return;
        }
        if (off == 0x1C2U) {
            spi_transfer(static_cast<uint8_t>(val & 0xFFU));
            return;
        }
        if (off == 0x208U) {
            arm7_ime_ = val & 1U;
            return;
        }
        if (off == 0x500U) {
            spu_.write_soundcnt(val);
            return;
        }
        if (off == 0x504U) {
            spu_.write_soundbias(val);
            return;
        }
        if (off >= 0x100U && off < 0x110U) {
            const int ch = static_cast<int>((off - 0x100U) >> 2U);
            if ((off & 2U) == 0U) {
                tmr7_[ch].reload = val;
            } else {
                const bool was_en = (tmr7_[ch].cnt & (1U << 7)) != 0U;
                tmr7_[ch].cnt = val;
                if (!was_en && (val & (1U << 7))) {
                    tmr7_[ch].counter = tmr7_[ch].reload;
                    tmr7_[ch].sub_cycles = 0U;
                }
            }
            return;
        }
        if (off >= 0x400U && off < 0x500U) {
            const uint32_t shift = (off & 2U) * 8U;
            spu_.write32((off - 0x400U) & ~3U, static_cast<uint32_t>(val) << shift,
                         0xFFFFU << shift, *this);
            return;
        }
    }
    arm7_write8(addr, static_cast<uint8_t>(val & 0xFFU));
    arm7_write8(addr + 1U, static_cast<uint8_t>(val >> 8U));
}

void Bus::arm7_write32(uint32_t addr, uint32_t val) noexcept {
    addr &= ~3U;
    const uint32_t top = addr >> 24U;
    if (top == 0x02U) {
        wr32(&main_ram_[addr & 0x3FFFFCU], val);
        ram_page_dirty_[(addr & 0x3FFFFFU) >> 12U] = 1U;
        return;
    }
    if (top == 0x04U) {
        const uint32_t off = addr & 0xFFFFFFU;
        if (off >= 0xB0U && off < 0xE0U) {
            const int ch = static_cast<int>((off - 0xB0U) / 12U);
            const uint32_t r = (off - 0xB0U) % 12U;
            if (r == 0U) {
                dma7_[ch].sad = val;
            } else if (r == 4U) {
                dma7_[ch].dad = val;
            } else {
                const bool was_en = (dma7_[ch].cnt & (1U << 31)) != 0U;
                dma7_[ch].cnt = val;
                if (val & (1U << 31)) {
                    const uint32_t mode = (val >> 28U) & 3U;
                    if (!was_en || mode == 0U) {
                        dma7_[ch].int_sad = dma7_[ch].sad;
                        dma7_[ch].int_dad = dma7_[ch].dad;
                    }
                    if (mode == 0U) {
                        run_dma7(ch, 0);
                    }
                }
            }
            return;
        }
        if (off == 0x188U) {
            // IPCFIFOSEND (ARM7 -> ARM9)
            if (trace_ipc_) {
                std::fprintf(stderr, "[IPC 7->9] f=%llu word=%08X tag=%u data=%07X\n",
                             static_cast<unsigned long long>(frame_count_), val, val & 0x1FU,
                             val >> 6U);
            }
            if ((ipcfifocnt7_ & (1U << 15)) && fifo_7to9_cnt_ < 16U) {
                const bool was_empty = (fifo_7to9_cnt_ == 0U);
                fifo_7to9_[fifo_7to9_w_] = val;
                fifo_7to9_w_ = (fifo_7to9_w_ + 1U) & 15U;
                ++fifo_7to9_cnt_;
                if (was_empty && (ipcfifocnt9_ & (1U << 10))) {
                    raise_arm9_irq(kIrqIpcRecvNotEmpty);
                }
            }
            return;
        }
        if (off == 0x1A4U) {
            cart_.write_romctrl(val);
            uint32_t guard = 0U;
            while (cart_.has_data_ready() && guard++ < 4096U) {
                bool any = false;
                for (int ch = 0; ch < 4; ++ch) {
                    if ((dma7_[ch].cnt & (1U << 31)) && (((dma7_[ch].cnt >> 28U) & 3U) == 2U)) {
                        run_dma7(ch, 2);
                        any = true;
                    }
                }
                if (!any) {
                    break;
                }
            }
            if (cart_.consume_transfer_complete_irq()) {
                raise_arm7_irq(kIrqCardComplete);
            }
            return;
        }
        if (off == 0x1A8U || off == 0x1ACU) {
            const int base = (off == 0x1A8U) ? 0 : 4;
            for (int i = 0; i < 4; ++i) {
                cart_.write_cardcmd(base + i, static_cast<uint8_t>((val >> (i * 8)) & 0xFFU));
            }
            return;
        }
        if (off == 0x208U) {
            arm7_ime_ = val & 1U;
            return;
        }
        if (off == 0x210U) {
            arm7_ie_ = val;
            return;
        }
        if (off == 0x214U) {
            arm7_if_ &= ~val;
            return;
        }
        if (off >= 0x400U && off < 0x500U) {
            spu_.write32(off - 0x400U, val, 0xFFFFFFFFU, *this);
            return;
        }
    }
    arm7_write16(addr, static_cast<uint16_t>(val & 0xFFFFU));
    arm7_write16(addr + 2U, static_cast<uint16_t>(val >> 16U));
}

// ============================================================================
// Scanline Stepping, Screen Composition & Recompiler Table
// ============================================================================

void Bus::compose_screens() noexcept {
    const auto& fb_a = engine_a_.framebuffer();
    const auto& fb_b = engine_b_.framebuffer();
    const auto& top = top_is_engine_a() ? fb_a : fb_b;
    const auto& bot = top_is_engine_a() ? fb_b : fb_a;
    std::memcpy(combined_fb_.data(), top.data(), top.size() * sizeof(uint32_t));
    std::memcpy(combined_fb_.data() + top.size(), bot.data(), bot.size() * sizeof(uint32_t));
}

void Bus::step_scanline(NdsCpuState& arm9, NdsCpuState& arm7) noexcept {
    if (vcount_ < kSingleHeight) {
        engine_a_.render_scanline(vcount_, *this, gx3d_.framebuffer());
        engine_b_.render_scanline(vcount_, *this, nullptr);
        // HBlank DMA & IRQ
        for (int ch = 0; ch < 4; ++ch)
            run_dma9(ch, 2, &arm9);
        if (dispstat9_ & (1U << 4))
            raise_arm9_irq(kIrqHBlank);
        if (dispstat7_ & (1U << 4))
            raise_arm7_irq(kIrqHBlank);
    }

    step_timers(tmr9_, kArm9CyclesPerLine, true);
    step_timers(tmr7_, kArm7CyclesPerLine, false);
    spu_.step(kArm7CyclesPerLine, *this);

    ++vcount_;
    if (vcount_ >= kTotalScanlines) {
        vcount_ = 0U;
    }

    auto update_dispstat = [&](uint16_t& ds, bool is_arm9) {
        ds &= ~0x0007U;
        if (vcount_ >= kSingleHeight && vcount_ < 262U)
            ds |= 0x0001U;  // VBlank flag
        const uint16_t lyc = static_cast<uint16_t>((ds >> 8U) | ((ds & 0x0080U) << 1U));
        if (vcount_ == lyc) {
            ds |= 0x0004U;
            if (ds & (1U << 5)) {
                if (is_arm9)
                    raise_arm9_irq(kIrqVCount);
                else
                    raise_arm7_irq(kIrqVCount);
            }
        }
    };
    update_dispstat(dispstat9_, true);
    update_dispstat(dispstat7_, false);

    if (vcount_ == kSingleHeight) {
        // Enter VBlank!
        gx3d_.on_vblank(*this);
        compose_screens();
        ++frame_count_;

        for (int ch = 0; ch < 4; ++ch) {
            run_dma9(ch, 1, &arm9);
            run_dma7(ch, 1);
        }
        if (dispstat9_ & (1U << 3))
            raise_arm9_irq(kIrqVBlank);
        if (dispstat7_ & (1U << 3))
            raise_arm7_irq(kIrqVBlank);

        if (frame_cb_) {
            frame_cb_(&arm9, this, frame_cb_user_);
        }
    }

    if (gx3d_.gxfifo_irq_wanted()) {
        raise_arm9_irq(kIrqGxFifo);
    }

    // Wake halted CPUs on enabled IRQ
    if (arm9.halted == 1U && ((arm9_ie_ & arm9_if_) != 0U)) {
        arm9.halted = 0U;
    }
    if (arm7.halted == 1U && ((arm7_ie_ & arm7_if_) != 0U)) {
        arm7.halted = 0U;
    }
}

void Bus::sync_arm9_cycles(uint64_t arm9_cycles, NdsCpuState& arm9) noexcept {
    if (arm9_cycles <= last_sync_arm9_ || !arm7_ptr_) {
        return;
    }
    const uint64_t delta9 = arm9_cycles - last_sync_arm9_;
    last_sync_arm9_ = arm9_cycles;

    // Step ARM7 in 2:1 lockstep with ARM9
    const uint64_t target7 = arm9_cycles >> 1U;
    while (arm7_ptr_->cycles < target7 && arm7_ptr_->halted != 2U) {
        if (arm7_ptr_->halted == 1U) {
            if ((arm7_ie_ & arm7_if_) != 0U) {
                arm7_ptr_->halted = 0U;
            } else {
                arm7_ptr_->cycles = target7;
                break;
            }
        }
        if (arm7_irq_pending() && !arm7_ptr_->i_disable) {
            cpu_dispatch_irq(*arm7_ptr_, *this);
        }
        cpu_step(*arm7_ptr_, *this);
    }

    scanline_cycle_acc_ += delta9;
    while (scanline_cycle_acc_ >= kArm9CyclesPerLine) {
        scanline_cycle_acc_ -= kArm9CyclesPerLine;
        step_scanline(arm9, *arm7_ptr_);
    }
}

void Bus::register_recomp_functions(const NdsRecompFuncEntry* entries, size_t count) {
    recomp_table_.clear();
    recomp_table_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const uint64_t key =
            (static_cast<uint64_t>(entries[i].addr) << 1U) | (entries[i].thumb ? 1ULL : 0ULL);
        recomp_table_[key] = entries[i].func;
    }
}

NdsRecompFunc Bus::lookup_recomp_function(uint32_t addr, bool thumb) const noexcept {
    if ((addr >> 24U) == 0x02U && ram_page_dirty_[(addr & 0x3FFFFFU) >> 12U] != 0U) {
        return nullptr;
    }
    const uint64_t key = (static_cast<uint64_t>(addr) << 1U) | (thumb ? 1ULL : 0ULL);
    const auto it = recomp_table_.find(key);
    return (it != recomp_table_.end()) ? it->second : nullptr;
}

bool Bus::save_state(const NdsCpuState& arm9, const NdsCpuState& arm7,
                     const std::filesystem::path& path) const {
    std::vector<uint8_t> blob;
    blob.reserve(sizeof(NdsCpuState) * 2U + main_ram_.size() + itcm_.size() + dtcm_.size() +
                 shared_wram_.size() + arm7_wram_.size() + palette_ram_.size() + oam_.size());
    auto push_bytes = [&](const void* ptr, size_t n) {
        const auto* p = static_cast<const uint8_t*>(ptr);
        blob.insert(blob.end(), p, p + n);
    };
    push_bytes(&arm9, sizeof(arm9));
    push_bytes(&arm7, sizeof(arm7));
    push_bytes(main_ram_.data(), main_ram_.size());
    push_bytes(itcm_.data(), itcm_.size());
    push_bytes(dtcm_.data(), dtcm_.size());
    push_bytes(shared_wram_.data(), shared_wram_.size());
    push_bytes(arm7_wram_.data(), arm7_wram_.size());
    push_bytes(palette_ram_.data(), palette_ram_.size());
    push_bytes(oam_.data(), oam_.size());
    push_bytes(vramcnt_.data(), vramcnt_.size());
    push_bytes(&wramcnt_, sizeof(wramcnt_));

    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    dragon::SaveStateHeader hdr{};
    std::memcpy(hdr.system, "NDS ", 4);
    hdr.version = kNdsStateVersion;
    hdr.payload_size = static_cast<uint32_t>(blob.size());
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(blob.data()),
              static_cast<std::streamsize>(blob.size()));
    return out.good();
}

bool Bus::load_state(NdsCpuState& arm9, NdsCpuState& arm7, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    dragon::SaveStateHeader hdr{};
    in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!in || !hdr.is_valid("NDS ", kNdsStateVersion)) {
        return false;
    }
    const size_t expected = sizeof(NdsCpuState) * 2U + main_ram_.size() + itcm_.size() +
                            dtcm_.size() + shared_wram_.size() + arm7_wram_.size() +
                            palette_ram_.size() + oam_.size() + vramcnt_.size() + sizeof(wramcnt_);
    if (hdr.payload_size != expected) {
        return false;
    }
    std::vector<uint8_t> blob(expected);
    in.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(expected));
    if (!in) {
        return false;
    }
    size_t off = 0;
    auto pop_bytes = [&](void* dst, size_t n) {
        std::memcpy(dst, blob.data() + off, n);
        off += n;
    };
    void* saved_bus9 = arm9.bus;
    void* saved_bus7 = arm7.bus;
    pop_bytes(&arm9, sizeof(arm9));
    pop_bytes(&arm7, sizeof(arm7));
    arm9.bus = saved_bus9;
    arm7.bus = saved_bus7;
    pop_bytes(main_ram_.data(), main_ram_.size());
    pop_bytes(itcm_.data(), itcm_.size());
    pop_bytes(dtcm_.data(), dtcm_.size());
    pop_bytes(shared_wram_.data(), shared_wram_.size());
    pop_bytes(arm7_wram_.data(), arm7_wram_.size());
    pop_bytes(palette_ram_.data(), palette_ram_.size());
    pop_bytes(oam_.data(), oam_.size());
    pop_bytes(vramcnt_.data(), vramcnt_.size());
    pop_bytes(&wramcnt_, sizeof(wramcnt_));
    last_sync_arm9_ = arm9.cycles;
    return true;
}

}  // namespace dragon::nds
