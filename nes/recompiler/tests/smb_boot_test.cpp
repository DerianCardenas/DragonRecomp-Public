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
#include "nes/cartridge.h"
#include "nes/runtime.h"
#include "nesrecomp/disassembler.h"
#include "nesrecomp/emitter.h"

namespace {

struct BootTestState {
    dragon::nes::FrameStats title_stats{};
    dragon::nes::FrameStats gameplay_stats{};
    size_t title_fallbacks = 0;
    size_t gameplay_fallbacks = 0;
    uint8_t smb_oper_mode = 0;
    bool checked_title = false;
    bool checked_gameplay = false;
};

void on_smb_frame(CpuState* cpu, dragon::nes::Bus* bus, void* user_data) {
    auto* state = static_cast<BootTestState*>(user_data);
    const uint64_t frames = bus->completed_frames();

    // Press Start at frames 180..188 to leave the title screen and enter World 1-1.
    if (frames >= 180ULL && frames <= 188ULL) {
        bus->set_controller(0, dragon::nes::kButtonStart);
    } else {
        bus->set_controller(0, 0U);
    }

    if (frames == 180ULL) {
        state->title_stats = dragon::nes::analyze_framebuffer(bus->ppu().framebuffer());
        state->title_fallbacks = bus->fallback_count();
        state->checked_title = true;
    }

    if (frames == 480ULL) {
        state->gameplay_stats = dragon::nes::analyze_framebuffer(bus->ppu().framebuffer());
        state->gameplay_fallbacks = bus->fallback_count();
        state->smb_oper_mode = bus->read(0x0770U);
        state->checked_gameplay = true;
        cpu->halted = 1U;
    }
}

}  // namespace

int main() {
    const std::filesystem::path toml_path = "nes/games/smb/smb.toml";
    const dragon::GameConfig config = dragon::load_game_config(toml_path);

    const std::filesystem::path rom_path = dragon::resolve_rom_path(config.rom_path, config.sha1);
    if (rom_path.empty()) {
        if (dragon::roms_required()) {
            std::fprintf(stderr,
                         "smb_boot_test: ROM sha1=%s not found (DRAGON_REQUIRE_ROMS=1); set "
                         "DRAGON_ROM_DIR or place it at %s\n",
                         config.sha1.c_str(), config.rom_path.string().c_str());
            return 1;
        }
        std::printf("smb_boot_test: ROM sha1=%s not found; skipping.\n", config.sha1.c_str());
        return 77;
    }

    const dragon::nes::Cartridge cart = dragon::nes::load_ines_rom(rom_path, config.sha1);
    const dragon::nesrecomp::ProgramCFG cfg =
        dragon::nesrecomp::analyze_prg_rom(cart.prg_rom, config);

    const std::filesystem::path out_dir =
        std::filesystem::path(DRAGON_BINARY_DIR) / "smb_boot_test_gen";
    std::filesystem::remove_all(out_dir);

    dragon::nesrecomp::EmitOptions opts;
    opts.enable_flag_opt = true;
    opts.split_every = 100000;  // Single C99 source file for fast shared-lib build
    const auto written = dragon::nesrecomp::write_generated_output(cfg, config, out_dir, opts);
    assert(!written.empty());

    const std::filesystem::path so_path = out_dir / "libsmb_recomp.so";
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

    BootTestState state{};
    bus.set_frame_callback(&on_smb_frame, &state);

    RecompFunc reset_fn = bus.lookup_function(cpu.pc, 0);
    assert(reset_fn != nullptr);
    reset_fn(&cpu);

    assert(state.checked_title);
    assert(state.checked_gameplay);

    std::printf(
        "smb_boot_test:\n"
        "  Title screen (frame 180):    distinct_colors=%zu non_backdrop=%zu fallbacks=%zu\n"
        "  World 1-1    (frame 480):    distinct_colors=%zu non_backdrop=%zu fallbacks=%zu "
        "OperMode=%u\n",
        state.title_stats.distinct_colors, state.title_stats.non_backdrop_pixels,
        state.title_fallbacks, state.gameplay_stats.distinct_colors,
        state.gameplay_stats.non_backdrop_pixels, state.gameplay_fallbacks, state.smb_oper_mode);

    // Title screen assertions (non-blank framebuffer, multiple colors, 0 interpreter fallbacks)
    assert(state.title_stats.distinct_colors >= 6U);
    assert(state.title_stats.non_backdrop_pixels > 5000U);
    assert(state.title_fallbacks == 0U);

    // World 1-1 gameplay assertions (~300 frames after pressing Start, 0 interpreter fallbacks)
    assert(state.gameplay_stats.distinct_colors >= 8U);
    assert(state.gameplay_stats.non_backdrop_pixels > 5000U);
    assert(state.gameplay_fallbacks == 0U);
    assert(state.smb_oper_mode == 1U);

    dlclose(handle);
    return 0;
}
