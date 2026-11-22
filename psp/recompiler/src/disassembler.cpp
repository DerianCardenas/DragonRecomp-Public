#include "psprecomp/disassembler.h"

#include <algorithm>
#include <queue>

namespace dragon::psprecomp {

namespace {

bool read_word(const dragon::psp::LoadedPspImage& img, uint32_t addr, uint32_t& out) noexcept {
    if ((addr & 3U) != 0U)
        return false;
    for (const auto& seg : img.segments) {
        if (addr >= seg.vaddr && addr + 4U <= seg.vaddr + seg.data.size()) {
            const size_t off = static_cast<size_t>(addr - seg.vaddr);
            const uint8_t* p = seg.data.data() + off;
            out = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
                  (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
            return true;
        }
    }
    return false;
}

bool is_valid_code_addr(const dragon::psp::LoadedPspImage& img, uint32_t addr) noexcept {
    uint32_t dummy = 0;
    return read_word(img, addr, dummy);
}

}  // namespace

PspCfg disassemble_psp(const dragon::psp::LoadedPspImage& img, const dragon::GameConfig& config) {
    PspCfg cfg{};

    for (const auto& imp : img.imports) {
        if (imp.stub_addr != 0U)
            cfg.stub_addresses.insert(imp.stub_addr);
    }

    std::queue<uint32_t> func_queue;
    std::set<uint32_t> discovered_funcs;

    auto enqueue_func = [&](uint32_t addr) {
        if (addr == 0U || (addr & 3U) != 0U)
            return;
        if (cfg.stub_addresses.count(addr) != 0U)
            return;
        if (!is_valid_code_addr(img, addr))
            return;
        if (discovered_funcs.insert(addr).second) {
            func_queue.push(addr);
        }
    };

    enqueue_func(img.entry_point);
    for (const auto& ep : config.entry_points) {
        enqueue_func(ep.addr);
    }

    // Heuristic prologue scan (`addiu $sp, $sp, -N` followed shortly by `sw $ra, ...($sp)`)
    for (const auto& seg : img.segments) {
        if (seg.data.size() < 16U)
            continue;
        // Limit scan to first 2 MB of code segment to keep compile times snappy
        const size_t scan_limit = std::min<size_t>(seg.data.size(), 2U * 1024U * 1024U);
        for (size_t off = 0; off + 12U <= scan_limit; off += 4U) {
            const uint32_t addr = seg.vaddr + static_cast<uint32_t>(off);
            uint32_t w0 = 0;
            if (!read_word(img, addr, w0))
                continue;
            // addiu $sp, $sp, -imm: opcode=0x09, rs=29, rt=29, simm < 0
            if ((w0 & 0xFFFF8000U) == 0x27BD8000U) {
                // Check next 4 instructions for `sw $ra, off($sp)` (0xAFBFxxxx)
                for (uint32_t k = 1; k <= 4; ++k) {
                    uint32_t wk = 0;
                    if (read_word(img, addr + k * 4U, wk) && (wk & 0xFFFF0000U) == 0xAFBF0000U) {
                        enqueue_func(addr);
                        break;
                    }
                }
            }
        }
    }

    constexpr size_t kMaxFuncs = 8192U;
    constexpr size_t kMaxInsnsPerFunc = 4096U;

    while (!func_queue.empty() && cfg.functions.size() < kMaxFuncs) {
        const uint32_t entry = func_queue.front();
        func_queue.pop();

        PspFunction fn{};
        fn.entry_addr = entry;
        fn.end_addr = entry;

        std::queue<uint32_t> bb_queue;
        std::set<uint32_t> visited_insns;
        bb_queue.push(entry);

        while (!bb_queue.empty() && fn.instructions.size() < kMaxInsnsPerFunc) {
            uint32_t pc = bb_queue.front();
            bb_queue.pop();

            while (fn.instructions.size() < kMaxInsnsPerFunc) {
                if (!visited_insns.insert(pc).second)
                    break;

                uint32_t raw = 0;
                if (!read_word(img, pc, raw))
                    break;

                const auto d =
                    dragon::mips::decode_mips(raw, pc, dragon::mips::MipsProfile::Allegrex);
                fn.instructions[pc] = d;
                if (pc + 4U > fn.end_addr)
                    fn.end_addr = pc + 4U;

                if (d.op == dragon::mips::MipsOp::Halt) {
                    break;
                }

                if (d.has_delay_slot) {
                    // Always decode and attach the delay-slot instruction
                    uint32_t ds_raw = 0;
                    if (read_word(img, pc + 4U, ds_raw)) {
                        const auto ds = dragon::mips::decode_mips(
                            ds_raw, pc + 4U, dragon::mips::MipsProfile::Allegrex);
                        fn.instructions[pc + 4U] = ds;
                        visited_insns.insert(pc + 4U);
                        if (pc + 8U > fn.end_addr)
                            fn.end_addr = pc + 8U;
                    }

                    if (d.op == dragon::mips::MipsOp::Jal) {
                        if (cfg.stub_addresses.count(d.target) == 0U) {
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
                        // Treat nearby intra-function jump as local label; otherwise tail call
                        const uint32_t dist =
                            (d.target >= entry) ? (d.target - entry) : (entry - d.target);
                        if (d.target >= entry && dist < 16384U &&
                            is_valid_code_addr(img, d.target)) {
                            fn.local_labels.insert(d.target);
                            bb_queue.push(d.target);
                        } else if (cfg.stub_addresses.count(d.target) == 0U) {
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

}  // namespace dragon::psprecomp
