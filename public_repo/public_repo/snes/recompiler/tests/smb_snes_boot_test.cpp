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
#include "snes/bus.h"
#include "snes/cartridge.h"
#include "snes/cpu_state.h"
#include "snes/frontend.h"
#include "snes/interpreter.h"
#include "snesrecomp/disassembler.h"
#include "snesrecomp/emitter.h"

namespace {

struct BootTestContext {
    dragon::snes::Bus* bus = nullptr;
    CpuState* cpu = nullptr;
    dragon::snes::FrameStats title_stats{};
    dragon::snes::FrameStats gameplay_stats{};
    size_t title_fallbacks = 0;
    size_t gameplay_fallbacks = 0;
    bool checked_title = false;
    bool checked_gameplay = false;
};

void on_smb_snes_frame(void* user_data, uint64_t frame_index) {
    auto* ctx = static_cast<BootTestContext*>(user_data);

    // Press Start at frames 115..125 to advance from title screen to level
    if (frame_index >= 115ULL && frame_index <= 125ULL) {
        ctx->bus->set_controller_state(dragon::snes::kButtonStart, 0U);
    } else {
        ctx->bus->set_controller_state(0U, 0U);
    }

    if (frame_index == 110ULL) {
        ctx->title_stats = dragon::snes::analyze_framebuffer(ctx->bus->ppu().framebuffer());
        ctx->title_fallbacks = ctx->bus->fallback_count();
        ctx->checked_title = true;
    }

    if (frame_index == 220ULL) {
        ctx->gameplay_stats = dragon::snes::analyze_framebuffer(ctx->bus->ppu().framebuffer());
        ctx->gameplay_fallbacks = ctx->bus->fallback_count();
        ctx->checked_gameplay = true;
        ctx->cpu->halted = 1U;
    }
}

}  // namespace

int main() {
    const std::filesystem::path toml_path = "snes/games/smb_snes/smb_snes.toml";
    const dragon::GameConfig config = dragon::load_game_config(toml_path);

    const std::filesystem::path rom_path = dragon::resolve_rom_path(config.rom_path, config.sha1);
    if (rom_path.empty()) {
        if (dragon::roms_required()) {
            std::fprintf(stderr,
                         "smb_snes_boot_test: ROM sha1=%s not found (DRAGON_REQUIRE_ROMS=1); set "
                         "DRAGON_ROM_DIR or place it at %s\n",
                         config.sha1.c_str(), config.rom_path.string().c_str());
            return 1;
        }
        std::printf("smb_snes_boot_test: ROM sha1=%s not found; skipping.\n", config.sha1.c_str());
        return 77;
    }

    const dragon::snes::Cartridge cart =
        dragon::snes::load_snes_rom(rom_path, config.sha1, config.map);
    const dragon::snesrecomp::ProgramCFG cfg = dragon::snesrecomp::analyze_snes_rom(cart, config);

    const std::filesystem::path out_dir =
        std::filesystem::path(DRAGON_BINARY_DIR) / "smb_snes_boot_test_gen";
    std::filesystem::remove_all(out_dir);

    dragon::snesrecomp::EmitOptions opts;
    opts.enable_flag_opt = true;
    opts.split_every = 100000;  // Single C99 source file for fast shared-lib build
    const auto written = dragon::snesrecomp::write_generated_output(cfg, config, out_dir, opts);
    assert(!written.empty());

    const std::filesystem::path so_path = out_dir / "libsmb_snes_recomp_boot.so";
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

    const auto* stack_tricks =
        static_cast<const uint32_t*>(dlsym(handle, "kRecompStackTrickTargets"));
    const auto* stack_trick_count =
        static_cast<const size_t*>(dlsym(handle, "kRecompStackTrickTargetCount"));
    assert(stack_tricks != nullptr && stack_trick_count != nullptr);

    dragon::snes::Bus bus;
    bus.load_cartridge(cart);
    bus.set_dispatch_table(table, *table_count);
    bus.set_stack_trick_targets(stack_tricks, *stack_trick_count);

    CpuState cpu{};
    dragon::snes::cpu_init(cpu, &bus);
    dragon::snes::cpu_reset(cpu);

    BootTestContext ctx{};
    ctx.bus = &bus;
    ctx.cpu = &cpu;
    bus.set_frame_callback(&on_smb_snes_frame, &ctx);

    RecompFunc reset_fn = bus.lookup_function(cpu.pb, cpu.pc, cpu_mode_flags(&cpu));
    assert(reset_fn != nullptr);
    reset_fn(&cpu);

    assert(ctx.checked_title);
    assert(ctx.checked_gameplay);

    std::printf(
        "smb_snes_boot_test:\n"
        "  Title screen (frame 110): distinct_colors=%zu non_backdrop=%zu fallbacks=%zu\n"
        "  Gameplay     (frame 220): distinct_colors=%zu non_backdrop=%zu fallbacks=%zu\n",
        ctx.title_stats.distinct_colors, ctx.title_stats.non_backdrop_pixels, ctx.title_fallbacks,
        ctx.gameplay_stats.distinct_colors, ctx.gameplay_stats.non_backdrop_pixels,
        ctx.gameplay_fallbacks);

    // Assert multi-color framebuffer and zero fallbacks
    assert(ctx.title_stats.distinct_colors >= 4U);
    assert(ctx.title_stats.non_backdrop_pixels > 5000U);
    assert(ctx.title_fallbacks == 0U);

    assert(ctx.gameplay_stats.distinct_colors >= 4U);
    assert(ctx.gameplay_stats.non_backdrop_pixels > 5000U);
    assert(ctx.gameplay_fallbacks == 0U);

    dlclose(handle);
    return 0;
}
