#include "ps1/loader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <span>

#include "dragon/file_io.h"
#include "dragon/sha1.h"

namespace dragon::ps1 {

namespace {

inline uint32_t rd32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
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

bool parse_cue_first_bin(const std::filesystem::path& cue_path, std::filesystem::path& out_bin,
                         uint32_t& out_sector_size, uint32_t& out_data_offset) {
    std::ifstream f(cue_path);
    if (!f)
        return false;
    out_sector_size = 2352U;
    out_data_offset = 24U;  // Mode 2/2352 default for PS1
    std::string line;
    bool found_file = false;
    while (std::getline(f, line)) {
        std::string trimmed = line;
        while (!trimmed.empty() &&
               (trimmed.front() == ' ' || trimmed.front() == '\t' || trimmed.front() == '\r')) {
            trimmed.erase(trimmed.begin());
        }
        std::string up = trimmed;
        std::transform(up.begin(), up.end(), up.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (!found_file && up.rfind("FILE ", 0) == 0) {
            const auto q1 = trimmed.find('"');
            const auto q2 =
                (q1 != std::string::npos) ? trimmed.find('"', q1 + 1U) : std::string::npos;
            std::string bin_name;
            if (q1 != std::string::npos && q2 != std::string::npos) {
                bin_name = trimmed.substr(q1 + 1U, q2 - q1 - 1U);
            } else {
                const auto sp = trimmed.find(' ', 5U);
                bin_name = trimmed.substr(5U, sp - 5U);
            }
            out_bin = cue_path.parent_path() / bin_name;
            found_file = true;
        } else if (found_file && up.find("TRACK 01") != std::string::npos) {
            if (up.find("MODE1/2048") != std::string::npos) {
                out_sector_size = 2048U;
                out_data_offset = 0U;
            } else if (up.find("MODE1/2352") != std::string::npos) {
                out_sector_size = 2352U;
                out_data_offset = 16U;
            } else {
                out_sector_size = 2352U;
                out_data_offset = 24U;
            }
            break;
        }
    }
    return found_file && std::filesystem::exists(out_bin);
}

bool detect_bin_geometry(const std::filesystem::path& bin_path, uint32_t& sector_size,
                         uint32_t& data_offset, uint32_t& total_sectors) {
    std::ifstream f(bin_path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    const auto sz = static_cast<uint64_t>(f.tellg());
    if (sz < 17ULL * 2048ULL)
        return false;

    auto check = [&](uint32_t ss, uint32_t off) -> bool {
        if (16ULL * ss + off + 6ULL > sz)
            return false;
        std::array<uint8_t, 6> sig{};
        f.seekg(static_cast<std::streamoff>(16ULL * ss + off), std::ios::beg);
        f.read(reinterpret_cast<char*>(sig.data()), 6);
        return sig[0] == 0x01 && std::memcmp(sig.data() + 1, "CD001", 5) == 0;
    };

    if (check(2352U, 24U)) {
        sector_size = 2352U;
        data_offset = 24U;
        total_sectors = static_cast<uint32_t>(sz / 2352U);
        return true;
    }
    if (check(2352U, 16U)) {
        sector_size = 2352U;
        data_offset = 16U;
        total_sectors = static_cast<uint32_t>(sz / 2352U);
        return true;
    }
    if (check(2048U, 0U)) {
        sector_size = 2048U;
        data_offset = 0U;
        total_sectors = static_cast<uint32_t>(sz / 2048U);
        return true;
    }
    return false;
}

void walk_iso_dir(const LoadedPs1Image& img, uint32_t dir_lba, uint32_t dir_size,
                  const std::string& prefix, std::vector<Ps1IsoEntry>& out, int depth = 0) {
    if (depth > 8 || dir_size == 0U || dir_size > 1024U * 1024U)
        return;
    const uint32_t num_secs = (dir_size + 2047U) / 2048U;
    std::vector<uint8_t> buf(static_cast<size_t>(num_secs) * 2048U, 0U);
    for (uint32_t s = 0; s < num_secs; ++s) {
        if (!img.read_sector_2048(dir_lba + s, buf.data() + static_cast<size_t>(s) * 2048U)) {
            return;
        }
    }

    size_t pos = 0;
    while (pos < dir_size) {
        const uint8_t rec_len = buf[pos];
        if (rec_len == 0U) {
            pos = ((pos / 2048U) + 1U) * 2048U;
            continue;
        }
        if (pos + rec_len > buf.size() || rec_len < 34U)
            break;
        const uint32_t lba = rd32(buf.data() + pos + 2U);
        const uint32_t sz = rd32(buf.data() + pos + 10U);
        const uint8_t flags = buf[pos + 25U];
        const uint8_t name_len = buf[pos + 32U];
        if (pos + 33U + name_len <= buf.size() && name_len > 0U) {
            if (!(name_len == 1U && (buf[pos + 33U] == 0x00 || buf[pos + 33U] == 0x01))) {
                std::string name(reinterpret_cast<const char*>(buf.data() + pos + 33U), name_len);
                const auto semi = name.find(';');
                if (semi != std::string::npos)
                    name.resize(semi);
                if (!name.empty() && name.back() == '.')
                    name.pop_back();
                const std::string full =
                    to_upper_path(prefix.empty() ? name : (prefix + "/" + name));
                const bool is_dir = (flags & 0x02U) != 0U;
                out.push_back(Ps1IsoEntry{full, lba, sz, is_dir});
                if (is_dir) {
                    walk_iso_dir(img, lba, sz, full, out, depth + 1);
                }
            }
        }
        pos += rec_len;
    }
}

std::string parse_system_cnf_boot(const std::string& text) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1U;
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        std::string up = line;
        std::transform(up.begin(), up.end(), up.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (up.find("BOOT") != std::string::npos && up.find("BOOT2") == std::string::npos) {
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string val = line.substr(eq + 1U);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
                val.erase(val.begin());
            std::string up_val = val;
            std::transform(up_val.begin(), up_val.end(), up_val.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            const auto cdpos = up_val.find("CDROM:");
            if (cdpos != std::string::npos) {
                val = val.substr(cdpos + 6U);
            }
            return to_upper_path(val);
        }
    }
    return "";
}

bool parse_ps1_iso(LoadedPs1Image& img, bool load_boot_exe, std::string& error) {
    std::array<uint8_t, 2048> pvd{};
    if (!img.read_sector_2048(16U, pvd.data())) {
        error = "failed to read PS1 ISO9660 Primary Volume Descriptor";
        return false;
    }
    if (pvd[0] != 0x01 || std::memcmp(pvd.data() + 1, "CD001", 5) != 0) {
        error = "invalid ISO9660 PVD signature";
        return false;
    }

    // Check system identifier (offset 8..39) or volume id (40..71)
    std::string sys_id(reinterpret_cast<const char*>(pvd.data() + 8U), 32U);
    while (!sys_id.empty() && (sys_id.back() == ' ' || sys_id.back() == '\0'))
        sys_id.pop_back();
    std::string vol_id(reinterpret_cast<const char*>(pvd.data() + 40U), 32U);
    while (!vol_id.empty() && (vol_id.back() == ' ' || vol_id.back() == '\0'))
        vol_id.pop_back();

    const uint32_t root_lba = rd32(pvd.data() + 156U + 2U);
    const uint32_t root_sz = rd32(pvd.data() + 156U + 10U);
    img.iso_entries.clear();
    walk_iso_dir(img, root_lba, root_sz, "", img.iso_entries);

    // Reject PSP UMD ISOs (which have PSP_GAME/PARAM.SFO)
    if (img.find_iso_entry("PSP_GAME/PARAM.SFO") != nullptr) {
        error = "disc image is a PSP UMD, not PlayStation 1";
        return false;
    }

    const Ps1IsoEntry* cnf = img.find_iso_entry("SYSTEM.CNF");
    const bool is_psx_sys = (sys_id.find("PLAYSTATION") != std::string::npos);
    if (!cnf && !is_psx_sys) {
        error = "not a PlayStation 1 disc (missing SYSTEM.CNF and PLAYSTATION system ID)";
        return false;
    }

    std::string boot_path = "PSX.EXE";
    if (cnf && cnf->size > 0U && cnf->size < 8192U) {
        std::string cnf_text(cnf->size, '\0');
        if (img.read_disc_file(*cnf, 0U, reinterpret_cast<uint8_t*>(cnf_text.data()), cnf->size)) {
            const std::string parsed = parse_system_cnf_boot(cnf_text);
            if (!parsed.empty())
                boot_path = parsed;
        }
    }

    img.boot_id = boot_path;
    if (img.title.empty()) {
        img.title = !vol_id.empty() ? vol_id : img.source_path.stem().string();
    }
    if (img.sha1_hex.empty()) {
        img.sha1_hex = dragon::sha1_hex(pvd);
    }

    if (!load_boot_exe)
        return true;

    const Ps1IsoEntry* exe_ent = img.find_iso_entry(boot_path);
    if (!exe_ent) {
        error = "PS1 boot executable not found on disc: " + boot_path;
        return false;
    }
    std::vector<uint8_t> exe_buf(exe_ent->size);
    if (!img.read_disc_file(*exe_ent, 0U, exe_buf.data(), exe_buf.size())) {
        error = "failed to read PS1 boot executable from disc: " + boot_path;
        return false;
    }
    return load_ps1_exe_buffer(exe_buf.data(), exe_buf.size(), img, error);
}

}  // namespace

bool LoadedPs1Image::read_sector_2048(uint32_t lba, uint8_t* dst2048) const {
    if (disc_bin_path.empty() || !dst2048 || lba >= total_sectors)
        return false;
    std::ifstream f(disc_bin_path, std::ios::binary);
    if (!f)
        return false;
    const uint64_t off = static_cast<uint64_t>(lba) * sector_size + sector_data_offset;
    f.seekg(static_cast<std::streamoff>(off), std::ios::beg);
    f.read(reinterpret_cast<char*>(dst2048), 2048);
    return f.gcount() == 2048;
}

bool LoadedPs1Image::read_disc_file(const Ps1IsoEntry& entry, uint32_t offset, uint8_t* dst,
                                    size_t len) const {
    if (!dst)
        return false;
    if (len == 0U)
        return true;
    if (offset >= entry.size)
        return false;
    const size_t to_read = std::min<size_t>(len, entry.size - offset);
    size_t done = 0;
    std::array<uint8_t, 2048> sec{};
    while (done < to_read) {
        const uint32_t cur = offset + static_cast<uint32_t>(done);
        const uint32_t lba = entry.lba + (cur / 2048U);
        const uint32_t in_sec = cur % 2048U;
        if (!read_sector_2048(lba, sec.data()))
            return false;
        const size_t take = std::min<size_t>(to_read - done, 2048U - in_sec);
        std::memcpy(dst + done, sec.data() + in_sec, take);
        done += take;
    }
    return true;
}

const Ps1IsoEntry* LoadedPs1Image::find_iso_entry(const std::string& path) const {
    const std::string norm = to_upper_path(path);
    for (const auto& e : iso_entries) {
        if (e.path == norm)
            return &e;
    }
    return nullptr;
}

bool load_ps1_exe_buffer(const uint8_t* data, size_t size, LoadedPs1Image& out,
                         std::string& error) {
    if (!data || size < 2048U) {
        error = "PS-X EXE buffer smaller than 2048-byte header";
        return false;
    }
    if (std::memcmp(data, "PS-X EXE", 8) != 0) {
        error = "missing 'PS-X EXE' header magic";
        return false;
    }

    out.initial_pc = rd32(data + 0x10U);
    out.initial_gp = rd32(data + 0x14U);
    out.load_addr = rd32(data + 0x18U);
    out.load_size = rd32(data + 0x1CU);
    out.sp_base = rd32(data + 0x30U);
    out.sp_offset = rd32(data + 0x34U);
    if (out.sp_base == 0U)
        out.sp_base = 0x801FFFF0U;

    const size_t avail = size - 2048U;
    const size_t body_sz = (out.load_size > 0U && out.load_size <= avail) ? out.load_size : avail;
    out.exe_body.assign(data + 2048U, data + 2048U + body_sz);
    out.load_size = static_cast<uint32_t>(body_sz);
    if (out.format == Ps1ImageFormat::Unknown) {
        out.format = Ps1ImageFormat::PsExe;
    }
    if (out.sha1_hex.empty()) {
        out.sha1_hex = dragon::sha1_hex(std::span<const uint8_t>(data, size));
    }
    return true;
}

bool load_ps1_image(const std::filesystem::path& path, LoadedPs1Image& out, std::string& error) {
    out = LoadedPs1Image{};
    out.source_path = path;

    if (!std::filesystem::exists(path)) {
        error = "PS1 file does not exist: " + path.string();
        return false;
    }

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // 1. .cue sheet
    if (ext == ".cue") {
        std::filesystem::path bin_path;
        uint32_t ss = 2352U, off = 24U;
        if (!parse_cue_first_bin(path, bin_path, ss, off)) {
            error = "failed to resolve track 1 binary from CUE sheet: " + path.string();
            return false;
        }
        out.format = Ps1ImageFormat::CueBin;
        out.disc_bin_path = bin_path;
        out.title = path.stem().string();
        if (!detect_bin_geometry(bin_path, out.sector_size, out.sector_data_offset,
                                 out.total_sectors)) {
            out.sector_size = ss;
            out.sector_data_offset = off;
            out.total_sectors =
                static_cast<uint32_t>(std::filesystem::file_size(bin_path) / out.sector_size);
        }
        return parse_ps1_iso(out, true, error);
    }

    // 2. Peek first 16 bytes for PS-X EXE
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "failed to open PS1 file: " + path.string();
        return false;
    }
    std::array<uint8_t, 16> hdr{};
    f.read(reinterpret_cast<char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
    if (std::memcmp(hdr.data(), "PS-X EXE", 8) == 0) {
        const auto buf = dragon::try_read_binary_file(path);
        out.title = path.stem().string();
        out.format = Ps1ImageFormat::PsExe;
        return load_ps1_exe_buffer(buf.data(), buf.size(), out, error);
    }

    // 3. Direct .bin or .iso disc image
    if (detect_bin_geometry(path, out.sector_size, out.sector_data_offset, out.total_sectors)) {
        out.format = (out.sector_size == 2048U) ? Ps1ImageFormat::Iso : Ps1ImageFormat::Bin;
        out.disc_bin_path = path;
        out.title = path.stem().string();
        return parse_ps1_iso(out, true, error);
    }

    error = "unrecognized PlayStation 1 image format";
    return false;
}

bool probe_ps1_image(const std::filesystem::path& path, std::string& out_title,
                     std::string& out_boot_id, std::string& out_sha1) {
    LoadedPs1Image img;
    img.source_path = path;
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".cue") {
        std::filesystem::path bin_path;
        uint32_t ss = 2352U, off = 24U;
        if (!parse_cue_first_bin(path, bin_path, ss, off))
            return false;
        img.disc_bin_path = bin_path;
        if (!detect_bin_geometry(bin_path, img.sector_size, img.sector_data_offset,
                                 img.total_sectors)) {
            return false;
        }
        std::string err;
        if (!parse_ps1_iso(img, false, err))
            return false;
        out_title = img.title;
        out_boot_id = img.boot_id;
        out_sha1 = img.sha1_hex;
        return true;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::array<uint8_t, 16> hdr{};
    f.read(reinterpret_cast<char*>(hdr.data()), 16);
    if (std::memcmp(hdr.data(), "PS-X EXE", 8) == 0) {
        const auto buf = dragon::try_read_binary_file(path);
        out_title = path.stem().string();
        out_boot_id = "PSX-EXE";
        out_sha1 = dragon::sha1_hex(buf);
        return true;
    }

    if (ext == ".bin" || ext == ".iso") {
        if (!detect_bin_geometry(path, img.sector_size, img.sector_data_offset,
                                 img.total_sectors)) {
            return false;
        }
        img.disc_bin_path = path;
        std::string err;
        if (!parse_ps1_iso(img, false, err))
            return false;
        out_title = img.title;
        out_boot_id = img.boot_id;
        out_sha1 = img.sha1_hex;
        return true;
    }

    return false;
}

}  // namespace dragon::ps1
