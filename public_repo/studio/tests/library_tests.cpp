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
    const std::string sha = dragon::sha1_hex(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(content.data()), content.size()));

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
    assert(CoverManager::get_system_name("snes") == "Nintendo - Super Nintendo Entertainment System");
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

}  // namespace

int main() {
    test_add_find_remove();
    test_roundtrip();
    test_missing_and_corrupt();
    test_shelves_and_last_played();
    test_relocate_by_sha1();
    test_cover_manager();
    std::cout << "All library tests passed.\n";
    return 0;
}
