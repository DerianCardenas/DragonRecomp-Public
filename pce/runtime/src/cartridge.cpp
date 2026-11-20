#include "pce/cartridge.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

namespace dragon::pce {

namespace {

std::string to_lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool has_bad_dump_tag(const std::string& stem_lower) {
    for (size_t i = 0; i < stem_lower.size(); ++i) {
        if (stem_lower[i] == '[' && i + 2 < stem_lower.size() && stem_lower[i + 1] == 'b') {
            if (stem_lower[i + 2] == ']' ||
                (std::isdigit(static_cast<unsigned char>(stem_lower[i + 2])) &&
                 i + 3 < stem_lower.size() && stem_lower[i + 3] == ']')) {
                return true;
            }
            if (stem_lower.compare(i, 5, "[bad]") == 0) {
                return true;
            }
        }
    }
    return false;
}

std::string trim_spaces(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    return s;
}

// Strip [b1], [b2], [b], [bad], [o1] tags from a GoodTools filename stem.
std::string strip_bad_and_overdump_tags(const std::string& stem) {
    std::string out;
    out.reserve(stem.size());
    for (size_t i = 0; i < stem.size();) {
        if (stem[i] == '[') {
            const size_t close = stem.find(']', i + 1);
            if (close != std::string::npos) {
                const std::string tag = to_lower_ascii(stem.substr(i + 1, close - i - 1));
                const bool is_b_or_o =
                    (tag == "b" || tag == "bad" || tag == "o" ||
                     ((tag[0] == 'b' || tag[0] == 'o') && tag.size() == 2 &&
                      std::isdigit(static_cast<unsigned char>(tag[1]))));
                if (is_b_or_o) {
                    while (!out.empty() && out.back() == ' ') {
                        out.pop_back();
                    }
                    i = close + 1;
                    continue;
                }
            }
        }
        out.push_back(stem[i]);
        ++i;
    }
    return trim_spaces(out);
}

// If the user selected a GoodTools Bad Dump (e.g. "Adventure Island (J) [b1].pce"),
// automatically resolve to the clean sibling dump ("Adventure Island (J).pce") in the same folder.
std::filesystem::path resolve_clean_sibling_rom(const std::filesystem::path& path) {
    std::error_code ec;
    const std::string stem = path.stem().string();
    const std::string stem_lower = to_lower_ascii(stem);
    if (!has_bad_dump_tag(stem_lower)) {
        return path;
    }

    const std::filesystem::path parent = path.parent_path();
    const std::string ext = path.extension().string();
    const std::string clean_stem = strip_bad_and_overdump_tags(stem);
    if (!clean_stem.empty() && clean_stem != stem) {
        const std::filesystem::path direct = parent / (clean_stem + ext);
        if (std::filesystem::exists(direct, ec) && !ec) {
            std::printf("[PCE] Bad Dump detectado ('%s'); usando dump limpio '%s'\n",
                        path.filename().string().c_str(), direct.filename().string().c_str());
            return direct;
        }
    }

    // Fallback: scan same directory for a clean sibling with the same title prefix before '(' or '['
    const size_t cut = stem.find_first_of("([");
    const std::string prefix = to_lower_ascii(trim_spaces(cut == std::string::npos ? stem : stem.substr(0, cut)));
    if (prefix.empty() || !std::filesystem::exists(parent, ec)) {
        return path;
    }

    std::filesystem::path best;
    int best_score = -1000;
    for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::filesystem::path cand = entry.path();
        if (to_lower_ascii(cand.extension().string()) != to_lower_ascii(ext)) {
            continue;
        }
        const std::string cstem = cand.stem().string();
        const std::string clower = to_lower_ascii(cstem);
        if (has_bad_dump_tag(clower) || clower.find("sound") != std::string::npos ||
            clower.find("demo") != std::string::npos) {
            continue;
        }
        const size_t ccut = cstem.find_first_of("([");
        const std::string cprefix =
            to_lower_ascii(trim_spaces(ccut == std::string::npos ? cstem : cstem.substr(0, ccut)));
        if (cprefix != prefix) {
            continue;
        }
        int score = 10;
        if (cstem.find('[') == std::string::npos) {
            score += 50;  // Prefer pure clean dump without hack/trainer/overdump brackets
        }
        if (score > best_score) {
            best_score = score;
            best = cand;
        }
    }
    if (!best.empty()) {
        std::printf("[PCE] Bad Dump detectado ('%s'); usando dump limpio '%s'\n",
                    path.filename().string().c_str(), best.filename().string().c_str());
        return best;
    }
    return path;
}

uint8_t reverse_bits8(uint8_t b) noexcept {
    b = static_cast<uint8_t>(((b & 0xF0U) >> 4U) | ((b & 0x0FU) << 4U));
    b = static_cast<uint8_t>(((b & 0xCCU) >> 2U) | ((b & 0x33U) << 2U));
    b = static_cast<uint8_t>(((b & 0xAAU) >> 1U) | ((b & 0x55U) << 1U));
    return b;
}

// Score how plausible a PC Engine bank 0 reset vector & boot sequence look.
int score_pce_boot(const std::vector<uint8_t>& rom, bool reversed) noexcept {
    if (rom.size() < Cartridge::kBankSize) {
        return -100;
    }
    auto rd = [&](size_t off) -> uint8_t {
        const uint8_t v = rom[off];
        return reversed ? reverse_bits8(v) : v;
    };

    const uint16_t reset_vec =
        static_cast<uint16_t>(rd(0x1FFEU) | (static_cast<uint16_t>(rd(0x1FFFU)) << 8U));
    int score = 0;
    if (reset_vec >= 0xE000U && reset_vec < 0xFFF6U) {
        score += 10;
        const size_t entry_off = static_cast<size_t>(reset_vec & 0x1FFFU);
        // Classic HuC6280 boot instructions: SEI ($78), CSH ($D4), CLD ($D8), LDA ($A9), LDX ($A2), JMP ($4C)
        const uint8_t op0 = rd(entry_off);
        if (op0 == 0x78U || op0 == 0xD4U || op0 == 0xD8U || op0 == 0xA9U || op0 == 0xA2U ||
            op0 == 0x4CU) {
            score += 15;
        }
        // Check first 16 bytes of reset handler for SEI ($78) or CSH ($D4) or TAM ($53)
        for (size_t i = 0; i < 16U && entry_off + i < 0x1FF6U; ++i) {
            const uint8_t b = rd(entry_off + i);
            if (b == 0x78U || b == 0xD4U || b == 0x53U) {
                score += 4;
            }
        }
    }
    // IRQ1 ($1FF8) and TIQ ($1FFA) usually point into $E000-$FFFF as well
    const uint16_t irq1 =
        static_cast<uint16_t>(rd(0x1FF8U) | (static_cast<uint16_t>(rd(0x1FF9U)) << 8U));
    if (irq1 >= 0xE000U && irq1 < 0xFFF6U) {
        score += 5;
    }
    return score;
}

}  // namespace

bool Cartridge::load_from_file(const std::filesystem::path& raw_path, std::string* err) {
    const std::filesystem::path path = resolve_clean_sibling_rom(raw_path);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        if (err) {
            *err = "No se pudo abrir el archivo ROM: " + path.string();
        }
        return false;
    }
    const auto sz = in.tellg();
    if (sz <= 0) {
        if (err) {
            *err = "El archivo ROM esta vacio";
        }
        return false;
    }
    std::vector<uint8_t> data(static_cast<size_t>(sz));
    in.seekg(0, std::ios::beg);
    if (!in.read(reinterpret_cast<char*>(data.data()), sz)) {
        if (err) {
            *err = "Error al leer el archivo ROM";
        }
        return false;
    }
    return load_from_bytes(std::move(data), err);
}

bool Cartridge::load_from_bytes(std::vector<uint8_t> data, std::string* err) {
    copier_header_ = false;
    bit_reversed_ = false;

    // Strip 512-byte copier header (Magic Griffin / Super Magic Drive / Pro Fighter)
    if (data.size() >= kBankSize + 512U && (data.size() % kBankSize) == 512U) {
        data.erase(data.begin(), data.begin() + 512);
        copier_header_ = true;
    }

    if (data.size() < kBankSize) {
        if (err) {
            *err = "ROM de PC Engine demasiado pequena (minimo 8 KiB)";
        }
        return false;
    }

    // Pad up to an 8 KiB bank boundary if truncated
    const size_t rem = data.size() % kBankSize;
    if (rem != 0U) {
        data.resize(data.size() + (kBankSize - rem), 0xFFU);
    }

    // Auto-detect bit-reversed North American TurboGrafx-16 dumps
    const int normal_score = score_pce_boot(data, false);
    const int rev_score = score_pce_boot(data, true);
    if (rev_score > normal_score + 5) {
        for (uint8_t& b : data) {
            b = reverse_bits8(b);
        }
        bit_reversed_ = true;
    }

    rom_ = std::move(data);
    init_after_load();
    return true;
}

void Cartridge::init_after_load() noexcept {
    bank_count_ = rom_.size() / kBankSize;
    bram_.fill(0x00U);
    bram_dirty_ = false;

    // Initialize default BRAM header ("HUBM") so games recognize formatted backup RAM
    bram_[0] = 'H';
    bram_[1] = 'U';
    bram_[2] = 'B';
    bram_[3] = 'M';
    bram_[4] = 0x00U;
    bram_[5] = 0x88U;  // End of BRAM ($8800)
    bram_[6] = 0x10U;  // First free byte ($8010)
    bram_[7] = 0x80U;

    // Auto-detect US TurboGrafx-16 region (either bit-reversed dump or standard NEC USA region-lock
    // check `LDA $1000; AND #$40; BEQ` in boot bank 0) so joypad port $1000 bit 6 reports US=0 / JP=1.
    us_region_ = bit_reversed_;
    if (!us_region_ && rom_.size() >= kBankSize) {
        static constexpr uint8_t kUsCheck[6] = {0xADU, 0x00U, 0x10U, 0x29U, 0x40U, 0xF0U};
        for (size_t i = 0; i + sizeof(kUsCheck) <= kBankSize; ++i) {
            if (std::memcmp(rom_.data() + i, kUsCheck, sizeof(kUsCheck)) == 0) {
                us_region_ = true;
                break;
            }
        }
    }

    if (rom_.size() > 1024U * 1024U) {
        mapper_ = MapperType::StreetFighter2;
    } else if (rom_.size() == 384U * 1024U) {
        mapper_ = MapperType::Mirror384K;
    } else {
        mapper_ = MapperType::Standard;
    }
    reset();
}

void Cartridge::reset() noexcept {
    state_ = {};
}

uint8_t Cartridge::read_bank(uint8_t phys_bank, uint16_t offset) const noexcept {
    if (rom_.empty() || phys_bank >= 0x80U) {
        return 0xFFU;
    }

    const size_t off = static_cast<size_t>(offset & 0x1FFFU);
    size_t rom_bank = 0;

    switch (mapper_) {
        case MapperType::StreetFighter2:
            // Banks $00-$3F: fixed first 512 KiB (banks 0..63)
            // Banks $40-$7F: switchable 512 KiB window (1..4) selected by $1FF0-$1FF3
            if (phys_bank < 0x40U) {
                rom_bank = phys_bank;
            } else {
                rom_bank = 0x40U + static_cast<size_t>(state_.sf2_bank & 0x03U) * 0x40U +
                           static_cast<size_t>(phys_bank - 0x40U);
            }
            rom_bank %= bank_count_;
            break;

        case MapperType::Mirror384K:
            // Lower 256 KiB (32 banks) mirrored in $00-$3F; upper 128 KiB (16 banks) in $40-$7F
            if (phys_bank < 0x40U) {
                rom_bank = phys_bank & 0x1FU;
            } else {
                rom_bank = 32U + (phys_bank & 0x0FU);
            }
            break;

        case MapperType::Standard:
        default:
            if (bank_count_ == 96U) {
                // 768 KiB HuCard: $00-$3F -> banks 0..63, $40-$7F -> banks 64..95 mirrored
                rom_bank = (phys_bank < 0x40U) ? phys_bank : (64U + (phys_bank & 0x1FU));
            } else {
                rom_bank = phys_bank % bank_count_;
            }
            break;
    }

    return rom_[rom_bank * kBankSize + off];
}

void Cartridge::write_bank(uint8_t phys_bank, uint16_t offset, uint8_t value) noexcept {
    (void)value;
    // Street Fighter II' Champion Edition mapper: writes to $1FF0-$1FF3 in ROM space
    if (mapper_ == MapperType::StreetFighter2 && phys_bank < 0x80U &&
        (offset & 0x1FFCU) == 0x1FF0U) {
        state_.sf2_bank = static_cast<uint8_t>(offset & 0x03U);
    }
}

uint8_t Cartridge::read_bram(uint16_t offset) const noexcept {
    if (offset < kBramSize) {
        return bram_[offset];
    }
    return 0xFFU;
}

void Cartridge::write_bram(uint16_t offset, uint8_t value) noexcept {
    if (offset < kBramSize) {
        bram_[offset] = value;
        bram_dirty_ = true;
    }
}

bool Cartridge::load_bram(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    in.read(reinterpret_cast<char*>(bram_.data()), static_cast<std::streamsize>(bram_.size()));
    bram_dirty_ = false;
    return in.gcount() > 0;
}

bool Cartridge::save_bram(const std::filesystem::path& path) const {
    if (!bram_dirty_) {
        return true;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bram_.data()),
              static_cast<std::streamsize>(bram_.size()));
    return out.good();
}

}  // namespace dragon::pce
