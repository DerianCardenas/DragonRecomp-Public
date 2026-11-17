#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include "dragon/dynlib.h"

#include "dragon/config.h"
#include "dragon/rom_locator.h"
#include "dragon/sha1.h"
#include "nes/cartridge.h"
#include "nes/runtime.h"
#include "nesrecomp/disassembler.h"
#include "nesrecomp/emitter.h"

namespace {

// Golden hashes of the SHA-1 digest of the raw RGBA32 framebuffer at the two
// checkpoints below, captured from a known-good run against the author's ROM
// (spec 003, ADR-008, Level 2). A framebuffer hash mismatch means the
// recompiled game painted something *different* from the reference, not just
// "something with few colors" — the pre-existing `distinct_colors`/
// `non_backdrop_pixels` checks below still run too, as a human-readable
// fallback diagnostic when the hash fails.
constexpr const char* kTitleFramebufferSha1 = "9825e19da0d2b396f3053339b16672738e7b0991";
constexpr const char* kGameplayFramebufferSha1 = "8a92df24624e255b08745d2ba0f9016143972457";

std::string hash_framebuffer(const std::array<uint32_t, dragon::nes::Ppu::kScreenWidth *
                                                            dragon::nes::Ppu::kScreenHeight>& fb) {
    return dragon::sha1_hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(fb.data()),
                                                     fb.size() * sizeof(uint32_t)));
}

struct DkBootTestState {
    dragon::nes::FrameStats title_stats{};
    dragon::nes::FrameStats gameplay_stats{};
    std::string title_hash;
    std::string gameplay_hash;
    size_t title_fallbacks = 0;
    size_t gameplay_fallbacks = 0;
    bool checked_title = false;
    bool checked_gameplay = false;
};

void on_dk_frame(CpuState* cpu, dragon::nes::Bus* bus, void* user_data) {
    auto* state = static_cast<DkBootTestState*>(user_data);
    const uint64_t frames = bus->completed_frames();

    // Press Start at frames 180..188 to start 1-Player game.
    if (frames >= 180ULL && frames <= 188ULL) {
        bus->set_controller(0, dragon::nes::kButtonStart);
    } else {
        bus->set_controller(0, 0U);
    }

    if (frames == 180ULL) {
        state->title_stats = dragon::nes::analyze_framebuffer(bus->ppu().framebuffer());
        state->title_hash = hash_framebuffer(bus->ppu().framebuffer());
        state->title_fallbacks = bus->fallback_count();
        state->checked_title = true;
    }

    if (frames == 600ULL) {
        state->gameplay_stats = dragon::nes::analyze_framebuffer(bus->ppu().framebuffer());
        state->gameplay_hash = hash_framebuffer(bus->ppu().framebuffer());
        state->gameplay_fallbacks = bus->fallback_count();
        state->checked_gameplay = true;
        cpu->halted = 1U;
    }
}

}  // namespace

int main() {
    const std::filesystem::path toml_path = "nes/games/dk/dk.toml";
    const dragon::GameConfig config = dragon::load_game_config(toml_path);

    const std::filesystem::path rom_path = dragon::resolve_rom_path(config.rom_path, config.sha1);
    if (rom_path.empty()) {
        if (dragon::roms_required()) {
            std::fprintf(stderr,
                         "dk_boot_test: ROM sha1=%s not found (DRAGON_REQUIRE_ROMS=1); set "
                         "DRAGON_ROM_DIR or place it at %s\n",
                         config.sha1.c_str(), config.rom_path.string().c_str());
            return 1;
        }
        std::printf("dk_boot_test: ROM sha1=%s not found; skipping.\n", config.sha1.c_str());
        return 77;
    }

    const dragon::nes::Cartridge cart = dragon::nes::load_ines_rom(rom_path, config.sha1);
    const dragon::nesrecomp::ProgramCFG cfg =
        dragon::nesrecomp::analyze_prg_rom(cart.prg_rom, config);

    assert(cfg.functions.size() >= 150);
    assert(cfg.unofficial_warnings.empty());

    const std::filesystem::path out_dir =
        std::filesystem::path(DRAGON_BINARY_DIR) / "dk_boot_test_gen";
    std::filesystem::remove_all(out_dir);

    dragon::nesrecomp::EmitOptions opts;
    opts.enable_flag_opt = true;
    opts.split_every = 100000;
    const auto written = dragon::nesrecomp::write_generated_output(cfg, config, out_dir, opts);
    assert(!written.empty());

    const std::filesystem::path so_path = out_dir / "libdk_recomp.so";
    const std::string cmd = std::string(DRAGON_C_COMPILER) +
                            " -std=c99 -Wall -Wextra -Wpedantic -Werror -O2 -fPIC -shared"
                            " -I\"" +
                            std::string(DRAGON_RUNTIME_INCLUDE_DIR) + "\" -I\"" + out_dir.string() +
                            "\" \"" + (out_dir / "recomp_funcs_0.c").string() + "\" \"" +
                            (out_dir / "recomp_dispatch.c").string() + "\" -o \"" +
                            so_path.string() + "\"";
    const int rc = std::system(cmd.c_str());
    assert(rc == 0);

    void* handle = dlopen(so_path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
    }
    assert(handle != nullptr);

    const auto* table = static_cast<const RecompFuncEntry*>(dlsym(handle, "kRecompDispatchTable"));
    const auto* table_count =
        static_cast<const size_t*>(dlsym(handle, "kRecompDispatchTableCount"));
    assert(table != nullptr && table_count != nullptr);

    dragon::nes::Bus bus;
    bus.load_cartridge(cart);
    bus.set_dispatch_table(table, *table_count);

    CpuState cpu{};
    dragon::nes::cpu_init(cpu, &bus);
    dragon::nes::cpu_reset(cpu);

    DkBootTestState state{};
    bus.set_frame_callback(&on_dk_frame, &state);

    RecompFunc reset_fn = bus.lookup_function(cpu.pc, 0);
    assert(reset_fn != nullptr);
    reset_fn(&cpu);

    assert(state.checked_title);
    assert(state.checked_gameplay);

    std::printf(
        "dk_boot_test:\n"
        "  Title screen (frame 180): distinct_colors=%zu non_backdrop=%zu fallbacks=%zu sha1=%s\n"
        "  Stage 1 25m  (frame 600): distinct_colors=%zu non_backdrop=%zu fallbacks=%zu sha1=%s\n",
        state.title_stats.distinct_colors, state.title_stats.non_backdrop_pixels,
        state.title_fallbacks, state.title_hash.c_str(), state.gameplay_stats.distinct_colors,
        state.gameplay_stats.non_backdrop_pixels, state.gameplay_fallbacks,
        state.gameplay_hash.c_str());

    // Weak diagnostics kept as a human-readable fallback if the golden-hash
    // assertions below fail ("painted something" vs "painted the reference").
    assert(state.title_stats.distinct_colors >= 5U);
    assert(state.title_stats.non_backdrop_pixels > 4000U);
    assert(state.title_fallbacks == 0U);

    assert(state.gameplay_stats.distinct_colors >= 8U);
    assert(state.gameplay_stats.non_backdrop_pixels > 5000U);
    assert(state.gameplay_fallbacks == 0U);

    // Golden-hash assertions (spec 003, ADR-008, Level 2): the recompiled
    // game must paint *exactly* the reference framebuffer, not merely
    // something colorful.
    if (state.title_hash != kTitleFramebufferSha1) {
        std::fprintf(stderr, "dk_boot_test: title framebuffer sha1 mismatch: got %s expected %s\n",
                     state.title_hash.c_str(), kTitleFramebufferSha1);
        return 1;
    }
    if (state.gameplay_hash != kGameplayFramebufferSha1) {
        std::fprintf(stderr,
                     "dk_boot_test: gameplay framebuffer sha1 mismatch: got %s expected %s\n",
                     state.gameplay_hash.c_str(), kGameplayFramebufferSha1);
        return 1;
    }

    dlclose(handle);
    return 0;
}
