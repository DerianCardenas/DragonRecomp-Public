#include "psxrecomp/disassembler.h"

#include <algorithm>
#include <queue>

namespace dragon::psxrecomp {

namespace {

inline bool is_bios_vector(uint32_t addr) noexcept {
    const uint32_t p = addr & 0x1FFFFFFFU;
    return p == 0x000000A0U || p == 0x000000B0U || p == 0x000000C0U;
}

bool read_word(const dragon::ps1::LoadedPs1Image& img, uint32_t addr, uint32_t& out) noexcept {
    if ((addr & 3U) != 0U)
        return false;
    const uint32_t phys = addr & 0x1FFFFFFFU;
    const uint32_t base = img.load_addr & 0x1FFFFFFFU;
    if (phys < base)
        return false;
    const uint32_t off = phys - base;
    if (off + 4U > img.exe_body.size())
        return false;
    const uint8_t* p = img.exe_body.data() + off;
    out = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
          (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
    return true;
}

bool is_valid_code_addr(const dragon::ps1::LoadedPs1Image& img, uint32_t addr) noexcept {
    uint32_t dummy = 0;
    return read_word(img, addr, dummy);
}

}  // namespace

Ps1Cfg disassemble_ps1(const dragon::ps1::LoadedPs1Image& img, const dragon::GameConfig& config) {
    Ps1Cfg cfg{};
    std::queue<uint32_t> func_queue;
    std::set<uint32_t> discovered_funcs;

    auto enqueue_func = [&](uint32_t addr) {
        if (addr == 0U || (addr & 3U) != 0U || is_bios_vector(addr))
            return;
        if (!is_valid_code_addr(img, addr))
            return;
        if (discovered_funcs.insert(addr).second) {
            func_queue.push(addr);
        }
    };

    enqueue_func(img.initial_pc);
    for (const auto& ep : config.entry_points) {
        enqueue_func(ep.addr);
    }

    // Prologue scan: `addiu $sp, $sp, -N` followed by `sw $ra, off($sp)`
    const size_t scan_limit = std::min<size_t>(img.exe_body.size(), 1024U * 1024U);
    for (size_t off = 0; off + 16U <= scan_limit; off += 4U) {
        const uint32_t addr = img.load_addr + static_cast<uint32_t>(off);
        uint32_t w0 = 0;
        if (!read_word(img, addr, w0))
            continue;
        if ((w0 & 0xFFFF8000U) == 0x27BD8000U) {
            for (uint32_t k = 1; k <= 4; ++k) {
                uint32_t wk = 0;
                if (read_word(img, addr + k * 4U, wk) && (wk & 0xFFFF0000U) == 0xAFBF0000U) {
                    enqueue_func(addr);
                    break;
                }
            }
        }
    }

    constexpr size_t kMaxFuncs = 8192U;
    constexpr size_t kMaxInsnsPerFunc = 4096U;

    while (!func_queue.empty() && cfg.functions.size() < kMaxFuncs) {
        const uint32_t entry = func_queue.front();
        func_queue.pop();

        Ps1Function fn{};
        fn.entry_addr = entry;
        fn.end_addr = entry;

        std::queue<uint32_t> bb_queue;
        std::set<uint32_t> visited;
        bb_queue.push(entry);

        while (!bb_queue.empty() && fn.instructions.size() < kMaxInsnsPerFunc) {
            uint32_t pc = bb_queue.front();
            bb_queue.pop();

            while (fn.instructions.size() < kMaxInsnsPerFunc) {
                if (!visited.insert(pc).second)
                    break;
                uint32_t raw = 0;
                if (!read_word(img, pc, raw))
                    break;

                const auto d =
                    dragon::mips::decode_mips(raw, pc, dragon::mips::MipsProfile::R3000A);
                fn.instructions[pc] = d;
                if (pc + 4U > fn.end_addr)
                    fn.end_addr = pc + 4U;

                if (d.has_delay_slot) {
                    uint32_t ds_raw = 0;
                    if (read_word(img, pc + 4U, ds_raw)) {
                        const auto ds = dragon::mips::decode_mips(
                            ds_raw, pc + 4U, dragon::mips::MipsProfile::R3000A);
                        fn.instructions[pc + 4U] = ds;
                        visited.insert(pc + 4U);
                        if (pc + 8U > fn.end_addr)
                            fn.end_addr = pc + 8U;
                    }

                    if (d.op == dragon::mips::MipsOp::Jal) {
                        if (!is_bios_vector(d.target)) {
                            fn.direct_calls.insert(d.target);
                            enqueue_func(d.target);
                        }
                        pc += 8U;
                        continue;
                    }
                    if (d.op == dragon::mips::MipsOp::Jalr) {
                        pc += 8U;
                        continue;
                    }
                    if (d.op == dragon::mips::MipsOp::J) {
                        const uint32_t dist =
                            (d.target >= entry) ? (d.target - entry) : (entry - d.target);
                        if (d.target >= entry && dist < 16384U &&
                            is_valid_code_addr(img, d.target)) {
                            fn.local_labels.insert(d.target);
                            bb_queue.push(d.target);
                        } else if (!is_bios_vector(d.target)) {
                            fn.direct_calls.insert(d.target);
                            enqueue_func(d.target);
                        }
                        break;
                    }
                    if (d.op == dragon::mips::MipsOp::Jr) {
                        break;
                    }
                    if (d.is_branch) {
                        if (is_valid_code_addr(img, d.target)) {
                            fn.local_labels.insert(d.target);
                            bb_queue.push(d.target);
                        }
                        pc += 8U;
                        continue;
                    }
                }
                pc += 4U;
            }
        }

        if (!fn.instructions.empty()) {
            cfg.total_instructions += fn.instructions.size();
            cfg.functions[entry] = std::move(fn);
        }
    }

    return cfg;
}

}  // namespace dragon::psxrecomp
