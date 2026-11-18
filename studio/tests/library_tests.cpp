// GameLibrary tests: no ROM required (synthetic RomInfo / files), per AGENTS.md.
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>

#include "dragon/sha1.h"
#include "studio/cover_manager.h"
#include "studio/library.h"
#include "studio/packager.h"

using namespace dragon::studio;

namespace {

RomInfo make_rom(const std::string& sha1, const std::string& name, RomPlatform p,
                 const std::filesystem::path& path = "x.nes") {
    RomInfo r;
    r.platform = p;
    r.sha1 = sha1;
    r.clean_name = name;
    r.file_path = path;
    return r;
}

std::filesystem::path fresh_dir(const char* name) {
    const auto d = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

void test_add_find_remove() {
    std::cout << "[Test] library add/find/remove...\n";
    GameLibrary lib(fresh_dir("dragon_lib_t1") / "library.toml");
    assert(lib.add_rom(make_rom("AAAA", "Tetris", RomPlatform::GameBoy)) != nullptr);
    assert(lib.entries().size() == 1);
    assert(lib.find("aaaa") != nullptr);  // SHA-1 lookup is case-insensitive
    // Re-adding the same SHA-1 must not duplicate it.
    lib.add_rom(make_rom("aaaa", "Other name", RomPlatform::GameBoy));
    assert(lib.entries().size() == 1);
    assert(lib.find("aaaa")->title == "Tetris");
    assert(lib.add_rom(RomInfo{}) == nullptr);  // invalid ROM is rejected
    assert(lib.remove("AAAA"));
    assert(!lib.remove("aaaa"));
    assert(lib.entries().empty());
}

void test_roundtrip() {
    std::cout << "[Test] library save/load round-trip...\n";
    const auto file = fresh_dir("dragon_lib_t2") / "sub" / "library.toml";
    {
        GameLibrary lib(file);
        auto* e = lib.add_rom(make_rom("b1", "Zelda \"ALttP\"", RomPlatform::SNES, "z.sfc"));
        assert(e);
        e->settings.crt_filter = true;
        e->settings.scale = "smooth";
        e->settings.volume = 40;
        lib.set_mode("b1", GameMode::Interpreter);
        lib.set_cover("b1", "covers/b1.png");
        lib.touch_played("b1", 120, 1000);
        lib.add_rom(make_rom("b2", "Metroid", RomPlatform::GameBoyAdvance, "m.gba"));
        assert(lib.save());
    }
    GameLibrary lib(file);
    assert(lib.load());
    assert(lib.entries().size() == 2);
    const auto* z = lib.find("b1");
    assert(z);
    assert(z->title == "Zelda \"ALttP\"");
    assert(z->platform == "snes");
    assert(z->mode == GameMode::Interpreter);
    assert(z->playtime_s == 120 && z->last_played == 1000);
    assert(z->settings.crt_filter && !z->settings.fullscreen);
    assert(z->settings.scale == "smooth" && z->settings.volume == 40);
    assert(z->cover_path == std::filesystem::path("covers/b1.png"));
    assert(!std::filesystem::exists(file.string() + ".tmp"));
}

void test_missing_and_corrupt() {
    std::cout << "[Test] library missing/corrupt file...\n";
    const auto dir = fresh_dir("dragon_lib_t3");
    GameLibrary missing(dir / "nope.toml");
    assert(missing.load() && missing.entries().empty());

    const auto bad = dir / "bad.toml";
    std::ofstream(bad) << "this is [[ not toml";
    GameLibrary corrupt(bad);
    assert(!corrupt.load() && corrupt.entries().empty());
}

void test_shelves_and_last_played() {
    std::cout << "[Test] library shelves/last_played...\n";
    GameLibrary lib(fresh_dir("dragon_lib_t4") / "library.toml");
    lib.add_rom(make_rom("c1", "zebra", RomPlatform::NES));
    lib.add_rom(make_rom("c2", "Apple", RomPlatform::NES));
    lib.add_rom(make_rom("c3", "Tetris", RomPlatform::GameBoy));
    const auto nes = lib.shelf("nes");
    assert(nes.size() == 2 && nes[0]->title == "Apple" && nes[1]->title == "zebra");
    assert(lib.shelf("gb").size() == 1 && lib.shelf("gba").empty());
    assert(lib.last_played() == nullptr);
    lib.touch_played("c1", 10, 500);
    lib.touch_played("c3", 10, 900);
    lib.touch_played("c3", -5, 950);  // negative durations never reduce playtime
    assert(lib.last_played()->sha1 == "c3");
    assert(lib.find("c3")->playtime_s == 10);
}

void test_relocate_by_sha1() {
    std::cout << "[Test] library relocate ROM by SHA-1...\n";
    const auto dir = fresh_dir("dragon_lib_t5");
    const auto moved = dir / "newhome" / "game.gb";
    std::filesystem::create_directories(moved.parent_path());
    const std::string content = "synthetic rom bytes";
    std::ofstream(moved, std::ios::binary) << content;
    const std::string sha = dragon::sha1_hex(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(content.data()), content.size()));

    GameLibrary lib(dir / "library.toml");
    lib.add_rom(make_rom(sha, "Moved", RomPlatform::GameBoy, dir / "oldhome" / "game.gb"));
    assert(lib.relocate_rom(sha, dir));
    assert(lib.find(sha)->rom_path == moved);
    assert(!lib.relocate_rom("ffff", dir));  // unknown game
}

void test_cover_manager() {
    std::cout << "[Test] cover manager helper functions & candidate lookup...\n";
    assert(CoverManager::is_image_file("cover.png"));
    assert(CoverManager::is_image_file("COVER.PNG"));
    assert(CoverManager::is_image_file("art.jpg"));
    assert(CoverManager::is_image_file("photo.jpeg"));
    assert(CoverManager::is_image_file("game.bmp"));
    assert(CoverManager::is_image_file("box.webp"));
    assert(!CoverManager::is_image_file("game.nes"));
    assert(!CoverManager::is_image_file("rom.gba"));
    assert(!CoverManager::is_image_file("doc.txt"));
    assert(!CoverManager::is_image_file(""));

    assert(CoverManager::get_system_name("nes") == "Nintendo - Nintendo Entertainment System");
    assert(CoverManager::get_system_name("snes") ==
           "Nintendo - Super Nintendo Entertainment System");
    assert(CoverManager::get_system_name("gb") == "Nintendo - Game Boy");
    assert(CoverManager::get_system_name("gbc") == "Nintendo - Game Boy Color");
    assert(CoverManager::get_system_name("gba") == "Nintendo - Game Boy Advance");
    assert(CoverManager::get_system_name("unknown").empty());

    GameEntry tetris;
    tetris.platform = "gb";
    tetris.title = "Tetris";
    tetris.rom_path = "roms/gb/tetris.gb";
    auto tetris_cands = CoverManager::get_candidate_names(tetris);
    assert(!tetris_cands.empty());
    bool found_tetris_rev = false;
    for (const auto& c : tetris_cands) {
        if (c.find("Tetris (World) (Rev A)") != std::string::npos) {
            found_tetris_rev = true;
            break;
        }
    }
    assert(found_tetris_rev);

    GameEntry poke;
    poke.platform = "gba";
    poke.title = "Pokemon - FireRed Version";
    auto poke_cands = CoverManager::get_candidate_names(poke);
    assert(!poke_cands.empty());

    // Manual cover assignment validation
    const auto dir = fresh_dir("dragon_cover_t1");
    const auto dummy_img = dir / "test_cover.png";
    std::ofstream(dummy_img) << "dummy png content";

    CoverManager mgr;
    assert(!mgr.is_downloading("1234"));
    auto assigned = mgr.assign_manual_cover("1234", dummy_img);
    assert(!assigned.empty());
    assert(std::filesystem::exists(assigned));
    mgr.invalidate("1234");

    // Invalid non-image file assignment must fail
    const auto dummy_rom = dir / "test.nes";
    std::ofstream(dummy_rom) << "dummy nes content";
    assert(mgr.assign_manual_cover("5678", dummy_rom).empty());
}

void test_portable_and_import() {
    std::cout << "[Test] library portable entries, host executable resolution & import...\n";
    const auto dir = fresh_dir("dragon_lib_portable_t1");

    const auto dummy_lin_bin = dir / "dummy_game";
    {
        std::ofstream out(dummy_lin_bin, std::ios::binary);
        out << "#!/bin/sh\necho ok\n";
    }
    const auto dummy_win_bin = dir / "dummy_game.exe";
    {
        std::ofstream out(dummy_win_bin, std::ios::binary);
        out << "MZ dummy pe\n";
    }

    RomInfo rom;
    rom.clean_name = "Contra Test";
    rom.platform = RomPlatform::NES;
    rom.sha1 = "0123456789abcdef0123456789abcdef01234567";
    rom.file_size = 32768;

    PackageOptions opts;
    opts.source_binary = dummy_lin_bin;
    opts.rom_info = rom;
    opts.output_directory = dir / "exported";
    opts.target_platform = TargetPlatform::All;
    opts.include_launcher_script = true;
    opts.include_desktop_entry = true;
    opts.bundle_sdl_library = false;
    opts.bundle_standalone_cmake = false;

    PackageResult pkg_res = Packager::create_portable_package(opts);
    assert(pkg_res.success);
    assert(!pkg_res.linux_executable_path.empty());
    assert(!pkg_res.windows_executable_path.empty());

    // 1. Add portable and verify round-trip + host executable resolution
    const auto lib_file = dir / "library.toml";
    {
        GameLibrary lib(lib_file);
        auto* e = lib.add_portable(rom, pkg_res.package_path, pkg_res.linux_executable_path,
                                   pkg_res.windows_executable_path, GameMode::Native);
        assert(e != nullptr);
        assert(e->is_portable());
        const auto host_exec = GameLibrary::resolve_host_executable(*e);
        assert(!host_exec.empty());
        assert(std::filesystem::exists(host_exec));
        assert(lib.save());
    }
    {
        GameLibrary lib(lib_file);
        assert(lib.load());
        const auto* e = lib.find(rom.sha1);
        assert(e != nullptr);
        assert(e->is_portable());
        assert(e->package_dir == pkg_res.package_path);
        assert(e->executable_linux == pkg_res.linux_executable_path);
        assert(e->executable_windows == pkg_res.windows_executable_path);
    }

    // 2. Inspect portable folder WITHOUT ROM in roms/ -> has_valid_rom() must be false
    {
#if defined(_WIN32)
        const auto& host_pkg_path = pkg_res.windows_package_path;
#else
        const auto& host_pkg_path = pkg_res.linux_package_path;
#endif
        const auto insp = GameLibrary::inspect_portable_folder(host_pkg_path);
        assert(insp.has_executable());
        assert(!insp.has_valid_rom());
        assert(insp.hinted_sha1 == rom.sha1);
        assert(insp.hinted_title == "Contra Test");

        // Manual import fallback registration
        GameLibrary manual_lib(dir / "manual_lib.toml");
        auto* me = manual_lib.add_manual_portable("Contra Manual", "nes", insp.package_dir,
                                                  insp.executable_linux, insp.executable_windows,
                                                  {}, insp.hinted_sha1);
        assert(me != nullptr);
        assert(me->is_portable());
        assert(me->title == "Contra Manual");
        assert(me->platform == "nes");
        assert(me->sha1 == rom.sha1);
        assert(!GameLibrary::resolve_host_executable(*me).empty());
    }

    // 3. Place a valid synthetic iNES ROM inside `roms/` and verify automatic import inspection
    {
        std::vector<uint8_t> ines(16 + 16384, 0);
        ines[0] = 'N';
        ines[1] = 'E';
        ines[2] = 'S';
        ines[3] = 0x1A;
        ines[4] = 1;  // 16 KB PRG
#if defined(_WIN32)
        const auto& host_pkg_path = pkg_res.windows_package_path;
#else
        const auto& host_pkg_path = pkg_res.linux_package_path;
#endif
        const auto rom_in_pkg = host_pkg_path / "roms" / "contra.nes";
        std::ofstream out(rom_in_pkg, std::ios::binary);
        out.write(reinterpret_cast<const char*>(ines.data()),
                  static_cast<std::streamsize>(ines.size()));
        out.close();

        // Update LEEME.txt with the actual SHA-1 of the synthetic ROM so hinted_sha1 matches
        const std::string actual_sha = dragon::sha1_hex(std::span<const uint8_t>(ines));
        std::ofstream(host_pkg_path / "roms" / "LEEME.txt")
            << "Este paquete NO incluye el ROM de \"Contra Test\".\n"
            << "  " << actual_sha << "\n";
        std::ofstream(host_pkg_path / "README.txt")
            << "  Contra Test \xE2\x80\x94 Recompilacion Nativa\n"
            << "Plataforma original: NES\n"
            << "Hash SHA-1:         " << actual_sha << "\n";

        const auto insp = GameLibrary::inspect_portable_folder(host_pkg_path);
        assert(insp.has_executable());
        assert(insp.has_valid_rom());
        assert(insp.rom_info.sha1 == actual_sha);
        assert(insp.found_rom_path == rom_in_pkg);
    }
}

}  // namespace

int main() {
    test_add_find_remove();
    test_roundtrip();
    test_missing_and_corrupt();
    test_shelves_and_last_played();
    test_relocate_by_sha1();
    test_cover_manager();
    test_portable_and_import();
    std::cout << "All library tests passed.\n";
    return 0;
}
