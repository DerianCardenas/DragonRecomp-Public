#include "psp/loader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <span>
#include <unordered_map>

#include "dragon/file_io.h"
#include "dragon/sha1.h"
#include "psp/hle.h"

#ifdef DRAGON_HAVE_ZLIB
#include <zlib.h>
#endif

namespace dragon::psp {

namespace {

inline uint16_t rd16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8U);
}

inline uint32_t rd32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
}

inline uint64_t rd64(const uint8_t* p) noexcept {
    return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32U);
}

inline void wr32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
    p[3] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
}

std::string to_upper_path(std::string s) {
    for (char& c : s) {
        if (c == '\\')
            c = '/';
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    while (!s.empty() && s.front() == '/')
        s.erase(s.begin());
    const auto semi = s.find(';');
    if (semi != std::string::npos)
        s.resize(semi);
    return s;
}

uint8_t* find_in_segments(std::vector<LoadedSegment>& segs, uint32_t vaddr, size_t need) {
    for (auto& s : segs) {
        if (vaddr >= s.vaddr && vaddr < s.vaddr + s.data.size()) {
            const size_t off = static_cast<size_t>(vaddr - s.vaddr);
            if (off + need <= s.data.size()) {
                return s.data.data() + off;
            }
        }
    }
    return nullptr;
}

const uint8_t* find_in_segments_const(const std::vector<LoadedSegment>& segs, uint32_t vaddr,
                                      size_t need) {
    for (const auto& s : segs) {
        if (vaddr >= s.vaddr && vaddr < s.vaddr + s.data.size()) {
            const size_t off = static_cast<size_t>(vaddr - s.vaddr);
            if (off + need <= s.data.size()) {
                return s.data.data() + off;
            }
        }
    }
    return nullptr;
}

std::string read_cstr_from_segs(const std::vector<LoadedSegment>& segs, uint32_t vaddr) {
    std::string out;
    for (uint32_t i = 0; i < 64U; ++i) {
        const uint8_t* p = find_in_segments_const(segs, vaddr + i, 1);
        if (!p || *p == 0)
            break;
        out.push_back(static_cast<char>(*p));
    }
    return out;
}

void apply_prx_relocations(const uint8_t* elf, size_t elf_size,
                           const std::vector<uint32_t>& seg_bases, std::vector<LoadedSegment>& segs,
                           uint32_t reloc_off, uint32_t reloc_size, uint32_t sh_type) {
    if (sh_type == 0x700000A0U) {
        // Standard PRX Elf32_Rel entries: r_offset (4B), r_info (4B)
        // r_info: bits 0..7 = type, bits 8..15 = ofs_base_seg, bits 16..23 = addr_base_seg
        uint32_t hi16_addr = 0;
        bool has_hi16 = false;
        for (uint32_t off = 0; off + 8U <= reloc_size; off += 8U) {
            if (reloc_off + off + 8U > elf_size)
                break;
            const uint32_t r_offset = rd32(elf + reloc_off + off);
            const uint32_t r_info = rd32(elf + reloc_off + off + 4U);
            const uint8_t type = static_cast<uint8_t>(r_info & 0xFFU);
            const uint8_t ofs_seg = static_cast<uint8_t>((r_info >> 8U) & 0xFFU);
            const uint8_t addr_seg = static_cast<uint8_t>((r_info >> 16U) & 0xFFU);
            if (ofs_seg >= seg_bases.size() || addr_seg >= seg_bases.size())
                continue;
            const uint32_t patch_va = seg_bases[ofs_seg] + r_offset;
            const uint32_t base = seg_bases[addr_seg];
            uint8_t* dst = find_in_segments(segs, patch_va, 4);
            if (!dst)
                continue;
            uint32_t op = rd32(dst);
            switch (type) {
                case 0:
                    break;  // R_MIPS_NONE
                case 2:     // R_MIPS_32
                    op += base;
                    wr32(dst, op);
                    break;
                case 4: {  // R_MIPS_26
                    uint32_t target = ((op & 0x03FFFFFFU) << 2U) + base;
                    op = (op & 0xFC000000U) | ((target >> 2U) & 0x03FFFFFFU);
                    wr32(dst, op);
                    break;
                }
                case 5:  // R_MIPS_HI16 (defer until LO16)
                    hi16_addr = patch_va;
                    has_hi16 = true;
                    break;
                case 6: {  // R_MIPS_LO16
                    const int16_t lo = static_cast<int16_t>(op & 0xFFFFU);
                    if (has_hi16) {
                        uint8_t* hi_dst = find_in_segments(segs, hi16_addr, 4);
                        if (hi_dst) {
                            const uint32_t hi_op = rd32(hi_dst);
                            const uint32_t full = ((hi_op & 0xFFFFU) << 16U) +
                                                  static_cast<uint32_t>(static_cast<int32_t>(lo)) +
                                                  base;
                            const uint32_t new_lo = full & 0xFFFFU;
                            const uint32_t new_hi = ((full + 0x8000U) >> 16U) & 0xFFFFU;
                            wr32(hi_dst, (hi_op & 0xFFFF0000U) | new_hi);
                            wr32(dst, (op & 0xFFFF0000U) | new_lo);
                        }
                        has_hi16 = false;
                    } else {
                        const uint32_t full =
                            static_cast<uint32_t>(static_cast<int32_t>(lo)) + base;
                        wr32(dst, (op & 0xFFFF0000U) | (full & 0xFFFFU));
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}

void scan_module_imports(LoadedPspImage& out, uint32_t modinfo_va) {
    // SceModuleInfo layout (52 bytes):
    // +0x00: uint16_t modattribute, uint8_t modversion[2]
    // +0x04: char modname[28]
    // +0x20: uint32_t gp_value
    // +0x24: uint32_t ent_top, +0x28: ent_end
    // +0x2C: uint32_t stub_top, +0x30: stub_end
    const uint8_t* mi = find_in_segments_const(out.segments, modinfo_va, 52);
    if (!mi)
        return;

    const uint32_t gp = rd32(mi + 0x20U);
    if (gp != 0U)
        out.gp_value = gp;

    uint32_t stub_top = rd32(mi + 0x2CU);
    uint32_t stub_end = rd32(mi + 0x30U);
    if (out.is_prx && stub_top < out.base_addr && stub_end > stub_top) {
        stub_top += out.base_addr;
        stub_end += out.base_addr;
    }

    uint32_t next_syscall = PspHle::kSyscallBase;
    uint32_t cur = stub_top;
    while (cur + 20U <= stub_end) {
        const uint8_t* hdr = find_in_segments_const(out.segments, cur, 20);
        if (!hdr)
            break;
        const uint32_t name_va = rd32(hdr + 0x00U);
        const uint8_t len_words = hdr[0x09U];  // struct size in 32-bit words (usually 5 or 6)
        const uint16_t func_count = rd16(hdr + 0x0AU);
        const uint32_t nid_table_va = rd32(hdr + 0x0CU);
        const uint32_t stub_table_va = rd32(hdr + 0x10U);
        const std::string mod_name = read_cstr_from_segs(out.segments, name_va);

        for (uint16_t i = 0; i < func_count; ++i) {
            const uint8_t* nid_ptr = find_in_segments_const(out.segments, nid_table_va + i * 4U, 4);
            if (!nid_ptr)
                break;
            NidStubImport imp{};
            imp.module_name = mod_name;
            imp.nid = rd32(nid_ptr);
            imp.stub_addr = stub_table_va + static_cast<uint32_t>(i) * 8U;
            imp.syscall_code = next_syscall++;
            out.imports.push_back(std::move(imp));
        }

        const uint32_t step = (len_words >= 5U) ? static_cast<uint32_t>(len_words) * 4U : 20U;
        cur += step;
    }
}

void walk_iso_directory(const LoadedPspImage& img, uint32_t dir_lba, uint32_t dir_size,
                        const std::string& prefix, std::vector<IsoEntry>& out_entries,
                        int depth = 0) {
    if (depth > 8 || dir_size == 0 || dir_size > 1024U * 1024U)
        return;
    std::vector<uint8_t> buf(dir_size);
    if (!img.read_disc_bytes(static_cast<uint64_t>(dir_lba) * 2048ULL, buf.data(), buf.size())) {
        return;
    }

    size_t pos = 0;
    while (pos < buf.size()) {
        const uint8_t rec_len = buf[pos];
        if (rec_len == 0) {
            // Advance to next 2048-byte sector boundary
            pos = ((pos / 2048U) + 1U) * 2048U;
            continue;
        }
        if (pos + rec_len > buf.size() || rec_len < 34U)
            break;
        const uint32_t extent_lba = rd32(buf.data() + pos + 2U);
        const uint32_t data_len = rd32(buf.data() + pos + 10U);
        const uint8_t flags = buf[pos + 25U];
        const uint8_t name_len = buf[pos + 32U];
        if (pos + 33U + name_len <= buf.size() && name_len > 0) {
            // Skip "." (0x00) and ".." (0x01)
            if (!(name_len == 1 && (buf[pos + 33U] == 0x00 || buf[pos + 33U] == 0x01))) {
                std::string name(reinterpret_cast<const char*>(buf.data() + pos + 33U), name_len);
                const auto semi = name.find(';');
                if (semi != std::string::npos)
                    name.resize(semi);
                if (!name.empty() && name.back() == '.')
                    name.pop_back();
                const std::string full =
                    to_upper_path(prefix.empty() ? name : (prefix + "/" + name));
                const bool is_dir = (flags & 0x02U) != 0U;
                IsoEntry entry{full, extent_lba, data_len, is_dir};
                out_entries.push_back(entry);
                if (is_dir) {
                    walk_iso_directory(img, extent_lba, data_len, full, out_entries, depth + 1);
                }
            }
        }
        pos += rec_len;
    }
}

bool parse_iso_filesystem(LoadedPspImage& img, std::string& error) {
    // Primary Volume Descriptor at sector 16 (offset 0x8000)
    std::array<uint8_t, 2048> pvd{};
    if (!img.read_disc_bytes(16ULL * 2048ULL, pvd.data(), pvd.size())) {
        error = "failed to read ISO9660 Primary Volume Descriptor";
        return false;
    }
    if (pvd[0] != 0x01 || std::memcmp(pvd.data() + 1, "CD001", 5) != 0) {
        error = "invalid ISO9660 signature (missing CD001)";
        return false;
    }
    const uint32_t root_lba = rd32(pvd.data() + 156U + 2U);
    const uint32_t root_size = rd32(pvd.data() + 156U + 10U);
    img.iso_entries.clear();
    walk_iso_directory(img, root_lba, root_size, "", img.iso_entries);

    // Try PARAM.SFO for title / disc ID
    if (const IsoEntry* sfo = img.find_iso_entry("PSP_GAME/PARAM.SFO")) {
        std::vector<uint8_t> sfo_buf(sfo->size);
        if (img.read_disc_bytes(static_cast<uint64_t>(sfo->lba) * 2048ULL, sfo_buf.data(),
                                sfo_buf.size())) {
            parse_param_sfo(sfo_buf.data(), sfo_buf.size(), img.title, img.disc_id);
        }
    }

    // Prefer decrypted BOOT.BIN over encrypted ~PSP EBOOT.BIN when available
    const IsoEntry* boot = img.find_iso_entry("PSP_GAME/SYSDIR/BOOT.BIN");
    const IsoEntry* eboot = img.find_iso_entry("PSP_GAME/SYSDIR/EBOOT.BIN");
    const IsoEntry* chosen = nullptr;
    std::vector<uint8_t> exe_buf;

    auto try_entry = [&](const IsoEntry* entry) -> bool {
        if (!entry || entry->size < 64U)
            return false;
        std::vector<uint8_t> tmp(entry->size);
        if (!img.read_disc_bytes(static_cast<uint64_t>(entry->lba) * 2048ULL, tmp.data(),
                                 tmp.size())) {
            return false;
        }
        if (tmp.size() >= 4 && std::memcmp(tmp.data(),
                                           "\x7F"
                                           "ELF",
                                           4) == 0) {
            exe_buf = std::move(tmp);
            chosen = entry;
            return true;
        }
        return false;
    };

    if (!try_entry(boot) && !try_entry(eboot)) {
        // Check if EBOOT.BIN is encrypted ~PSP
        if (eboot && eboot->size >= 4) {
            std::array<uint8_t, 4> magic{};
            if (img.read_disc_bytes(static_cast<uint64_t>(eboot->lba) * 2048ULL, magic.data(), 4) &&
                std::memcmp(magic.data(), "~PSP", 4) == 0) {
                img.encrypted_eboot = true;
                error =
                    "encrypted commercial ~PSP EBOOT.BIN detected (decrypt BOOT.BIN/EBOOT.BIN "
                    "first)";
                return false;
            }
        }
        error = "no ELF executable found in ISO (checked PSP_GAME/SYSDIR/BOOT.BIN and EBOOT.BIN)";
        return false;
    }

    (void)chosen;
    return load_psp_elf_buffer(exe_buf.data(), exe_buf.size(), img, error);
}

}  // namespace

void parse_param_sfo(const uint8_t* data, size_t size, std::string& title, std::string& disc_id) {
    if (!data || size < 20U)
        return;
    if (data[0] != 0x00 || data[1] != 'P' || data[2] != 'S' || data[3] != 'F')
        return;
    const uint32_t key_table_off = rd32(data + 8U);
    const uint32_t data_table_off = rd32(data + 12U);
    const uint32_t count = rd32(data + 16U);

    for (uint32_t i = 0; i < count; ++i) {
        const size_t ent_off = 20U + static_cast<size_t>(i) * 16U;
        if (ent_off + 16U > size)
            break;
        const uint16_t key_off = rd16(data + ent_off);
        const uint16_t data_fmt = rd16(data + ent_off + 2U);
        const uint32_t data_len = rd32(data + ent_off + 4U);
        const uint32_t val_off = rd32(data + ent_off + 12U);
        if (key_table_off + key_off >= size)
            continue;
        std::string key;
        for (size_t k = key_table_off + key_off; k < size && data[k] != 0; ++k) {
            key.push_back(static_cast<char>(data[k]));
        }
        if ((data_fmt == 0x0204U || data_fmt == 0x0004U) && data_table_off + val_off < size &&
            data_len > 0) {
            const size_t max_n = std::min<size_t>(data_len, size - (data_table_off + val_off));
            std::string val(reinterpret_cast<const char*>(data + data_table_off + val_off), max_n);
            while (!val.empty() && val.back() == '\0')
                val.pop_back();
            if (key == "TITLE")
                title = val;
            else if (key == "DISC_ID")
                disc_id = val;
        }
    }
}

bool LoadedPspImage::read_disc_bytes(uint64_t offset, uint8_t* dst, size_t len) const {
    if (source_path.empty() || !dst)
        return false;
    if (len == 0)
        return true;

    std::ifstream f(source_path, std::ios::binary);
    if (!f)
        return false;

    if (!is_cso) {
        f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        f.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(len));
        return f.gcount() == static_cast<std::streamsize>(len);
    }

#ifdef DRAGON_HAVE_ZLIB
    if (cso_block_size == 0 || cso_index.size() < 2)
        return false;
    size_t done = 0;
    std::vector<uint8_t> comp_buf;
    std::vector<uint8_t> block_buf(cso_block_size);
    while (done < len) {
        const uint64_t cur_off = offset + done;
        const size_t blk = static_cast<size_t>(cur_off / cso_block_size);
        const size_t in_blk = static_cast<size_t>(cur_off % cso_block_size);
        if (blk + 1U >= cso_index.size())
            return false;

        const uint32_t idx0 = cso_index[blk];
        const uint32_t idx1 = cso_index[blk + 1U];
        const bool uncompressed = (idx0 & 0x80000000U) != 0U;
        const uint64_t read_off = static_cast<uint64_t>(idx0 & 0x7FFFFFFFU) << cso_index_shift;
        const uint64_t next_off = static_cast<uint64_t>(idx1 & 0x7FFFFFFFU) << cso_index_shift;
        const size_t chunk_len = (next_off > read_off) ? static_cast<size_t>(next_off - read_off)
                                                       : static_cast<size_t>(cso_block_size);

        f.seekg(static_cast<std::streamoff>(read_off), std::ios::beg);
        if (uncompressed) {
            f.read(reinterpret_cast<char*>(block_buf.data()),
                   static_cast<std::streamsize>(cso_block_size));
            if (f.gcount() != static_cast<std::streamsize>(cso_block_size))
                return false;
        } else {
            comp_buf.resize(chunk_len);
            f.read(reinterpret_cast<char*>(comp_buf.data()),
                   static_cast<std::streamsize>(chunk_len));
            z_stream zs{};
            if (inflateInit2(&zs, -15) != Z_OK)
                return false;
            zs.next_in = comp_buf.data();
            zs.avail_in = static_cast<uInt>(comp_buf.size());
            zs.next_out = block_buf.data();
            zs.avail_out = static_cast<uInt>(block_buf.size());
            const int ret = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            if (ret != Z_STREAM_END && ret != Z_OK)
                return false;
        }

        const size_t take = std::min(len - done, static_cast<size_t>(cso_block_size) - in_blk);
        std::memcpy(dst + done, block_buf.data() + in_blk, take);
        done += take;
    }
    return true;
#else
    return false;
#endif
}

const IsoEntry* LoadedPspImage::find_iso_entry(const std::string& path) const {
    const std::string norm = to_upper_path(path);
    for (const auto& e : iso_entries) {
        if (e.path == norm)
            return &e;
    }
    return nullptr;
}

bool load_psp_elf_buffer(const uint8_t* data, size_t size, LoadedPspImage& out,
                         std::string& error) {
    if (!data || size < 52U) {
        error = "PSP ELF buffer too small";
        return false;
    }
    if (std::memcmp(data,
                    "\x7F"
                    "ELF",
                    4) != 0) {
        error = "missing ELF magic header";
        return false;
    }
    // ELF32 little-endian MIPS (e_ident[4]=1, e_ident[5]=1, e_machine=8)
    if (data[4] != 1 || data[5] != 1) {
        error = "unsupported ELF class/endianness (expected ELF32 little-endian)";
        return false;
    }
    const uint16_t e_type = rd16(data + 16U);
    const uint16_t e_machine = rd16(data + 18U);
    if (e_machine != 8U) {
        error = "ELF e_machine is not MIPS (expected 8)";
        return false;
    }

    const bool is_prx = (e_type == 0xFFA0U);
    out.is_prx = is_prx;
    if (out.format == ImageFormat::Unknown) {
        out.format = is_prx ? ImageFormat::Prx : ImageFormat::Elf;
    }

    const uint32_t e_entry = rd32(data + 24U);
    const uint32_t e_phoff = rd32(data + 28U);
    const uint32_t e_shoff = rd32(data + 32U);
    const uint16_t e_phentsize = rd16(data + 42U);
    const uint16_t e_phnum = rd16(data + 44U);
    const uint16_t e_shentsize = rd16(data + 46U);
    const uint16_t e_shnum = rd16(data + 48U);
    const uint16_t e_shstrndx = rd16(data + 50U);

    out.base_addr = is_prx ? 0x08804000U : 0U;
    out.entry_point = is_prx ? (out.base_addr + e_entry) : e_entry;
    out.segments.clear();
    out.imports.clear();

    std::vector<uint32_t> seg_bases;
    uint32_t max_end = 0x08804000U;
    uint32_t modinfo_paddr = 0;

    for (uint16_t i = 0; i < e_phnum; ++i) {
        const size_t poff = e_phoff + static_cast<size_t>(i) * e_phentsize;
        if (poff + 32U > size)
            break;
        const uint32_t p_type = rd32(data + poff + 0U);
        const uint32_t p_offset = rd32(data + poff + 4U);
        const uint32_t p_vaddr = rd32(data + poff + 8U);
        const uint32_t p_paddr = rd32(data + poff + 12U);
        const uint32_t p_filesz = rd32(data + poff + 16U);
        const uint32_t p_memsz = rd32(data + poff + 20U);

        if (p_type == 1U) {  // PT_LOAD
            const uint32_t load_va = is_prx ? (out.base_addr + p_vaddr) : p_vaddr;
            seg_bases.push_back(load_va);
            LoadedSegment seg{};
            seg.vaddr = load_va;
            seg.memsz = std::max(p_memsz, p_filesz);
            seg.data.assign(seg.memsz, 0U);
            if (p_filesz > 0 && p_offset < size) {
                const size_t copy_n = std::min<size_t>(p_filesz, size - p_offset);
                std::memcpy(seg.data.data(), data + p_offset, copy_n);
            }
            if (load_va + seg.memsz > max_end) {
                max_end = load_va + seg.memsz;
            }
            if (modinfo_paddr == 0 && p_paddr != 0 && p_paddr != p_vaddr) {
                modinfo_paddr = p_paddr;
            }
            out.segments.push_back(std::move(seg));
        } else if (p_type == 0x700000A0U && is_prx) {
            apply_prx_relocations(data, size, seg_bases, out.segments, p_offset, p_filesz, p_type);
        }
    }

    out.image_end = max_end;
    if (out.segments.empty()) {
        error = "ELF contains no PT_LOAD segments";
        return false;
    }

    // Scan section headers for PRX relocations and .rodata.sceModuleInfo
    uint32_t modinfo_va = 0;
    uint32_t shstr_off = 0;
    uint32_t shstr_size = 0;
    if (e_shoff != 0 && e_shstrndx < e_shnum &&
        e_shoff + static_cast<size_t>(e_shstrndx + 1U) * e_shentsize <= size) {
        const size_t so = e_shoff + static_cast<size_t>(e_shstrndx) * e_shentsize;
        shstr_off = rd32(data + so + 16U);
        shstr_size = rd32(data + so + 20U);
    }

    for (uint16_t i = 0; i < e_shnum; ++i) {
        const size_t soff = e_shoff + static_cast<size_t>(i) * e_shentsize;
        if (soff + 40U > size)
            break;
        const uint32_t sh_name = rd32(data + soff + 0U);
        const uint32_t sh_type = rd32(data + soff + 4U);
        const uint32_t sh_addr = rd32(data + soff + 12U);
        const uint32_t sh_offset = rd32(data + soff + 16U);
        const uint32_t sh_size = rd32(data + soff + 20U);

        if (is_prx && sh_type == 0x700000A0U) {
            apply_prx_relocations(data, size, seg_bases, out.segments, sh_offset, sh_size, sh_type);
        }

        if (shstr_off != 0 && sh_name < shstr_size && shstr_off + sh_name < size) {
            const char* sname = reinterpret_cast<const char*>(data + shstr_off + sh_name);
            if (std::strcmp(sname, ".rodata.sceModuleInfo") == 0) {
                modinfo_va = is_prx ? (out.base_addr + sh_addr) : sh_addr;
            }
        }
    }

    if (modinfo_va == 0 && modinfo_paddr != 0) {
        // p_paddr in PSP PRX encodes file offset of SceModuleInfo
        const uint32_t p_off = modinfo_paddr & 0x7FFFFFFFU;
        for (uint16_t i = 0; i < e_phnum; ++i) {
            const size_t poff = e_phoff + static_cast<size_t>(i) * e_phentsize;
            if (poff + 32U > size)
                break;
            if (rd32(data + poff) != 1U)
                continue;
            const uint32_t p_offset = rd32(data + poff + 4U);
            const uint32_t p_vaddr = rd32(data + poff + 8U);
            const uint32_t p_filesz = rd32(data + poff + 16U);
            if (p_off >= p_offset && p_off < p_offset + p_filesz) {
                modinfo_va = (is_prx ? out.base_addr : 0U) + p_vaddr + (p_off - p_offset);
                break;
            }
        }
    }

    if (modinfo_va != 0) {
        scan_module_imports(out, modinfo_va);
    }

    if (out.sha1_hex.empty()) {
        out.sha1_hex = dragon::sha1_hex(std::span<const uint8_t>(data, size));
    }
    return true;
}

bool load_psp_image(const std::filesystem::path& path, LoadedPspImage& out, std::string& error) {
    out = LoadedPspImage{};
    out.source_path = path;

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        error = "failed to open PSP file: " + path.string();
        return false;
    }
    const auto file_size = static_cast<uint64_t>(f.tellg());
    if (file_size < 40U) {
        error = "PSP file is too small";
        return false;
    }
    f.seekg(0, std::ios::beg);
    std::array<uint8_t, 40> hdr{};
    f.read(reinterpret_cast<char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));

    // 1. Direct ELF / PRX
    if (std::memcmp(hdr.data(),
                    "\x7F"
                    "ELF",
                    4) == 0) {
        std::vector<uint8_t> buf = dragon::try_read_binary_file(path);
        if (buf.empty()) {
            error = "failed to read PSP ELF file: " + path.string();
            return false;
        }
        out.title = path.stem().string();
        out.sha1_hex = dragon::sha1_hex(buf);
        return load_psp_elf_buffer(buf.data(), buf.size(), out, error);
    }

    // 2. PBP container ("\0PBP")
    if (hdr[0] == 0x00 && hdr[1] == 'P' && hdr[2] == 'B' && hdr[3] == 'P') {
        out.format = ImageFormat::Pbp;
        std::vector<uint8_t> buf = dragon::try_read_binary_file(path);
        if (buf.empty()) {
            error = "failed to read PSP PBP file: " + path.string();
            return false;
        }
        out.sha1_hex = dragon::sha1_hex(buf);
        const uint32_t sfo_off = rd32(buf.data() + 8U);
        const uint32_t icon0_off = rd32(buf.data() + 12U);
        const uint32_t data_psp_off = rd32(buf.data() + 32U);
        const uint32_t data_psar_off = rd32(buf.data() + 36U);
        if (sfo_off < buf.size() && icon0_off > sfo_off) {
            const size_t sfo_len = std::min<size_t>(icon0_off - sfo_off, buf.size() - sfo_off);
            parse_param_sfo(buf.data() + sfo_off, sfo_len, out.title, out.disc_id);
        }
        if (out.title.empty())
            out.title = path.stem().string();
        if (data_psp_off >= buf.size()) {
            error = "PBP DATA.PSP offset out of bounds";
            return false;
        }
        const uint32_t psp_end = (data_psar_off > data_psp_off && data_psar_off <= buf.size())
                                     ? data_psar_off
                                     : static_cast<uint32_t>(buf.size());
        const size_t psp_len = static_cast<size_t>(psp_end - data_psp_off);
        if (psp_len >= 4 && std::memcmp(buf.data() + data_psp_off, "~PSP", 4) == 0) {
            out.encrypted_eboot = true;
            error = "encrypted ~PSP DATA.PSP inside PBP";
            return false;
        }
        return load_psp_elf_buffer(buf.data() + data_psp_off, psp_len, out, error);
    }

    // 3. CSO compressed ISO ("CISO")
    if (std::memcmp(hdr.data(), "CISO", 4) == 0) {
        out.format = ImageFormat::Cso;
        out.is_cso = true;
        out.disc_total_bytes = rd64(hdr.data() + 8U);
        out.cso_block_size = rd32(hdr.data() + 16U);
        out.cso_index_shift = hdr[21U];
        if (out.cso_block_size == 0 || out.disc_total_bytes == 0) {
            error = "invalid CSO header";
            return false;
        }
        const size_t num_blocks = static_cast<size_t>(
            (out.disc_total_bytes + out.cso_block_size - 1U) / out.cso_block_size);
        out.cso_index.resize(num_blocks + 1U);
        f.seekg(24, std::ios::beg);
        f.read(reinterpret_cast<char*>(out.cso_index.data()),
               static_cast<std::streamsize>(out.cso_index.size() * sizeof(uint32_t)));
        out.title = path.stem().string();
        // Fast deterministic hash over header + index
        out.sha1_hex = dragon::sha1_hex(hdr);
        return parse_iso_filesystem(out, error);
    }

    // 4. Raw ISO9660
    if (file_size >= 17ULL * 2048ULL) {
        out.format = ImageFormat::Iso;
        out.is_cso = false;
        out.disc_total_bytes = file_size;
        out.title = path.stem().string();
        std::array<uint8_t, 2048> pvd{};
        if (out.read_disc_bytes(16ULL * 2048ULL, pvd.data(), pvd.size())) {
            out.sha1_hex = dragon::sha1_hex(pvd);
        }
        return parse_iso_filesystem(out, error);
    }

    error = "unrecognized PSP image format";
    return false;
}

}  // namespace dragon::psp
