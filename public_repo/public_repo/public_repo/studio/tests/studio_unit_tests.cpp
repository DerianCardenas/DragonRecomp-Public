#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "dragon/config.h"
#include "studio/packager.h"
#include "studio/pipeline.h"
#include "studio/rom_detector.h"

using namespace dragon::studio;

namespace {

void test_detect_rom_nes() {
    std::cout << "[Test] detect_rom NES...\n";
    const std::filesystem::path nes_path = "roms/nes/smb.nes";
    if (!std::filesystem::exists(nes_path)) {
        std::cout << "  (skipped: roms/nes/smb.nes not present)\n";
        return;
    }

    RomInfo info = detect_rom(nes_path);
    assert(info.is_valid());
    assert(info.platform == RomPlatform::NES);
    assert(info.mapper == 0);
    assert(info.prg_size_kb == 32);
    assert(info.chr_size_kb == 8);
    assert(!info.sha1.empty());
    std::cout << "  NES detection passed: " << info.clean_name << " (SHA-1: " << info.sha1 << ")\n";
}

void test_detect_rom_snes() {
    std::cout << "[Test] detect_rom SNES...\n";
    const std::filesystem::path snes_path = "roms/snes/smb_snes.sfc";
    if (!std::filesystem::exists(snes_path)) {
        std::cout << "  (skipped: roms/snes/smb_snes.sfc not present)\n";
        return;
    }

    RomInfo info = detect_rom(snes_path);
    assert(info.is_valid());
    assert(info.platform == RomPlatform::SNES);
    assert(info.map_mode_str == "hirom");
    assert(info.internal_title == "SUPER MARIO BROS.SNES");
    assert(!info.sha1.empty());
    std::cout << "  SNES detection passed: " << info.clean_name << " (SHA-1: " << info.sha1
              << ")\n";
}

void test_detect_rom_gb_and_gbc() {
    std::cout << "[Test] detect_rom GB and GBC...\n";
    const auto temp_dir = std::filesystem::temp_directory_path() / "dragon_test_gb_detect";
    std::filesystem::create_directories(temp_dir);

    // 1. Synthetic DMG Game Boy ROM
    const auto dmg_path = temp_dir / "tetris_test.gb";
    {
        std::vector<uint8_t> data(32768, 0);
        static constexpr uint8_t kLogo[48] = {
            0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
            0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
            0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
            0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E};
        std::memcpy(data.data() + 0x0104, kLogo, sizeof(kLogo));
        const char title[] = "TETRIS";
        std::memcpy(data.data() + 0x0134, title, sizeof(title) - 1);
        data[0x0143] = 0x00;  // DMG only
        data[0x0147] = 0x01;  // MBC1
        data[0x0148] = 0x01;  // 64 KiB
        data[0x0149] = 0x02;  // 8 KiB RAM

        // Checksum
        uint8_t csum = 0;
        for (size_t i = 0x0134; i <= 0x014C; ++i) {
            csum = static_cast<uint8_t>(csum - data[i] - 1U);
        }
        data[0x014D] = csum;

        std::ofstream out(dmg_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }

    RomInfo dmg_info = detect_rom(dmg_path);
    assert(dmg_info.is_valid());
    assert(dmg_info.platform == RomPlatform::GameBoy);
    assert(dmg_info.clean_name == "TETRIS");
    assert(dmg_info.mbc_type_str == "MBC1");
    assert(dmg_info.rom_size_kb == 64);
    assert(dmg_info.ram_size_kb == 8);

    // 2. Synthetic Game Boy Color ROM
    const auto gbc_path = temp_dir / "pkmn_gold.gbc";
    {
        std::vector<uint8_t> data(65536, 0);
        static constexpr uint8_t kLogo[48] = {
            0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
            0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
            0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
            0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E};
        std::memcpy(data.data() + 0x0104, kLogo, sizeof(kLogo));
        const char title[] = "POKEMON GOLD";
        std::memcpy(data.data() + 0x0134, title, sizeof(title) - 1);
        data[0x0143] = 0x80;  // CGB supported
        data[0x0147] = 0x10;  // MBC3+TIMER+RAM+BATTERY
        data[0x0148] = 0x02;  // 128 KiB
        data[0x0149] = 0x03;  // 32 KiB RAM

        uint8_t csum = 0;
        for (size_t i = 0x0134; i <= 0x014C; ++i) {
            csum = static_cast<uint8_t>(csum - data[i] - 1U);
        }
        data[0x014D] = csum;

        std::ofstream out(gbc_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }

    RomInfo gbc_info = detect_rom(gbc_path);
    assert(gbc_info.is_valid());
    assert(gbc_info.platform == RomPlatform::GameBoyColor);
    assert(gbc_info.cgb_flag == true);
    assert(gbc_info.battery == true);
    assert(gbc_info.mbc_type_str == "MBC3+TIMER+RAM+BATTERY");

    std::filesystem::remove_all(temp_dir);
    std::cout << "  GB and GBC detection tests passed!\n";
}

void test_detect_rom_gba() {
    std::cout << "[Test] detect_rom GBA...\n";
    const auto temp_dir = std::filesystem::temp_directory_path() / "dragon_test_gba_detect";
    std::filesystem::create_directories(temp_dir);

    const auto gba_path = temp_dir / "emerald_test.gba";
    {
        std::vector<uint8_t> data(0x2000, 0);
        // ARM branch at 0x00: B 0x080000C0
        data[0x00] = 0x2E;
        data[0x01] = 0x00;
        data[0x02] = 0x00;
        data[0x03] = 0xEA;
        const char title[] = "POKEMON EMER";
        std::memcpy(data.data() + 0xA0, title, 12);
        const char code[] = "BPEE";
        std::memcpy(data.data() + 0xAC, code, 4);
        data[0xB2] = 0x96;
        uint8_t chk = 0;
        for (size_t i = 0xA0; i <= 0xBC; ++i) {
            chk = static_cast<uint8_t>(chk - data[i]);
        }
        data[0xBD] = static_cast<uint8_t>(chk - 0x19U);
        const char sig[] = "FLASH1M_V103";
        std::memcpy(data.data() + 0x1000, sig, sizeof(sig) - 1);

        std::ofstream out(gba_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }

    RomInfo gba_info = detect_rom(gba_path);
    assert(gba_info.is_valid());
    assert(gba_info.platform == RomPlatform::GameBoyAdvance);
    assert(gba_info.clean_name == "POKEMON EMER");
    assert(gba_info.game_code == "BPEE");
    assert(gba_info.save_type_str == "FLASH 128K");
    assert(gba_info.battery == true);

    std::filesystem::remove_all(temp_dir);
    std::cout << "  GBA detection tests passed!\n";
}

void test_detect_rom_invalid() {
    std::cout << "[Test] detect_rom invalid...\n";
    RomInfo info = detect_rom("non_existent_file.xyz");
    assert(!info.is_valid());
    assert(info.platform == RomPlatform::Unknown);
    std::cout << "  Invalid ROM detection passed.\n";
}

void test_packager() {
    std::cout << "[Test] packager create_portable_package...\n";
    const auto temp_dir = std::filesystem::temp_directory_path() / "dragon_test_packager";
    std::filesystem::create_directories(temp_dir);

    const auto dummy_bin = temp_dir / "dummy_game";
    {
        std::ofstream out(dummy_bin);
        out << "#!/bin/sh\necho hello\n";
    }

    RomInfo dummy_rom;
    dummy_rom.clean_name = "Super Test Bros";
    dummy_rom.platform = RomPlatform::NES;
    dummy_rom.sha1 = "1234567890abcdef";
    dummy_rom.file_size = 32768;

    PackageOptions opts;
    opts.source_binary = dummy_bin;
    opts.rom_info = dummy_rom;
    opts.output_directory = temp_dir / "exported";
    opts.include_launcher_script = true;
    opts.include_desktop_entry = true;
    opts.bundle_sdl_library = false;

    PackageResult res = Packager::create_portable_package(opts);
    assert(res.success);
    assert(std::filesystem::exists(res.package_path));
    assert(std::filesystem::exists(res.executable_path));
    assert(std::filesystem::exists(res.package_path / "run.sh"));
    assert(std::filesystem::exists(res.package_path / "Super_Test_Bros.desktop"));
    assert(std::filesystem::exists(res.package_path / "README.txt"));
    assert(res.total_bytes > 0);

    // Verify run.sh contents (cd "$SCRIPT_DIR" and clean LD_LIBRARY_PATH)
    {
        std::ifstream in(res.package_path / "run.sh");
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(content.find("cd \"$SCRIPT_DIR\"") != std::string::npos);
        assert(content.find("export LD_LIBRARY_PATH=\"$SCRIPT_DIR/lib\"") != std::string::npos);
    }

    // Verify .desktop contents (absolute Exec and Path)
    {
        std::ifstream in(res.package_path / "Super_Test_Bros.desktop");
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(content.find("Path=" + std::filesystem::absolute(res.package_path).string()) !=
               std::string::npos);
        assert(content.find("Exec=\"" +
                            std::filesystem::absolute(res.package_path / "run.sh").string()) !=
               std::string::npos);
    }

    std::filesystem::remove_all(temp_dir);
    std::cout << "  Packager tests passed: created valid standalone Linux package structure.\n";
}

void test_packager_windows() {
    std::cout << "[Test] packager create_portable_package (Windows)...\n";
    const auto temp_dir = std::filesystem::temp_directory_path() / "dragon_test_packager_win";
    std::filesystem::create_directories(temp_dir);

    const auto dummy_bin = temp_dir / "dummy_game.exe";
    {
        std::ofstream out(dummy_bin);
        out << "MZ dummy pe header\n";
    }

    RomInfo dummy_rom;
    dummy_rom.clean_name = "Super Test Bros";
    dummy_rom.platform = RomPlatform::NES;
    dummy_rom.sha1 = "1234567890abcdef";
    dummy_rom.file_size = 32768;

    PackageOptions opts;
    opts.source_binary = dummy_bin;
    opts.rom_info = dummy_rom;
    opts.output_directory = temp_dir / "exported";
    opts.target_platform = TargetPlatform::Windows;
    opts.include_launcher_script = true;
    opts.bundle_standalone_cmake = true;
    opts.bundle_sdl_library = true;

    PackageResult res = Packager::create_portable_package(opts);
    assert(res.success);
    assert(std::filesystem::exists(res.package_path));
    assert(std::filesystem::exists(res.package_path / "Super_Test_Bros.exe"));
    assert(std::filesystem::exists(res.package_path / "run.bat"));
    assert(std::filesystem::exists(res.package_path / "build_windows.bat"));
    assert(std::filesystem::exists(res.package_path / "CMakeLists.txt"));
    assert(std::filesystem::exists(res.package_path / "README.txt"));
    assert(std::filesystem::exists(res.package_path / "src" / "game_main.cpp"));
    assert(std::filesystem::exists(res.package_path / "include" / "nes" / "cpu_state.h"));
    assert(std::filesystem::exists(res.package_path / "include" / "dragon" / "dynlib.h"));
    assert(res.total_bytes > 0);

    // Also test packaging when source_binary has NO .exe extension (e.g. game_binary)
    {
        const auto dummy_bin_no_ext = temp_dir / "game_binary";
        {
            std::ofstream out(dummy_bin_no_ext);
            out << "MZ dummy pe header without ext\n";
        }
        PackageOptions opts_no_ext = opts;
        opts_no_ext.source_binary = dummy_bin_no_ext;
        opts_no_ext.output_directory = temp_dir / "exported_no_ext";
        opts_no_ext.target_platform = TargetPlatform::Windows;
        PackageResult res_no_ext = Packager::create_portable_package(opts_no_ext);
        assert(res_no_ext.success);
        assert(std::filesystem::exists(res_no_ext.package_path / "Super_Test_Bros.exe"));
        assert(std::filesystem::exists(res_no_ext.package_path / "run.bat"));
    }

    // Also test TargetPlatform::All
    opts.target_platform = TargetPlatform::All;
    PackageResult res_all = Packager::create_portable_package(opts);
    assert(res_all.success);
    assert(std::filesystem::exists(temp_dir / "exported" / "Super_Test_Bros_Linux"));
    assert(std::filesystem::exists(temp_dir / "exported" / "Super_Test_Bros_Windows"));

    std::filesystem::remove_all(temp_dir);
    std::cout << "  Packager Windows & Multiplatform tests passed: created valid standalone "
                 "Windows project structure.\n";
}

void test_pipeline_toml_generation() {
    std::cout << "[Test] pipeline generate_initial_toml...\n";
    const auto temp_dir = std::filesystem::temp_directory_path() / "dragon_test_pipeline_toml";
    std::filesystem::create_directories(temp_dir);

    // Test NES SMB ROM
    const std::filesystem::path nes_path = "roms/nes/smb.nes";
    if (std::filesystem::exists(nes_path)) {
        RomInfo info = detect_rom(nes_path);
        assert(info.is_valid());

        Pipeline pipeline;
        const auto toml_path = temp_dir / "game_nes.toml";
        bool ok = pipeline.generate_initial_toml(info, toml_path);
        assert(ok);
        assert(std::filesystem::exists(toml_path));

        // Must load cleanly with strict TOML validator (no unknown keys in [output])
        dragon::GameConfig cfg = dragon::load_game_config(toml_path);
        assert(cfg.platform == "nes");
        assert(cfg.sha1 == info.sha1);
        std::cout << "  NES TOML generation and validation passed!\n";
    }

    // Test SNES SMB ROM
    const std::filesystem::path snes_path = "roms/snes/smb_snes.sfc";
    if (std::filesystem::exists(snes_path)) {
        RomInfo info = detect_rom(snes_path);
        assert(info.is_valid());

        Pipeline pipeline;
        const auto toml_path = temp_dir / "game_snes.toml";
        bool ok = pipeline.generate_initial_toml(info, toml_path);
        assert(ok);
        assert(std::filesystem::exists(toml_path));

        dragon::GameConfig cfg = dragon::load_game_config(toml_path);
        assert(cfg.platform == "snes");
        assert(cfg.sha1 == info.sha1);
        std::cout << "  SNES TOML generation and validation passed!\n";
    }

    // Test GB TOML Generation
    {
        RomInfo gb_info;
        gb_info.platform = RomPlatform::GameBoy;
        gb_info.clean_name = "Tetris";
        gb_info.file_path = "roms/gb/tetris.gb";
        gb_info.sha1 = "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678";
        gb_info.mbc_type_str = "ROM ONLY";

        Pipeline pipeline;
        const auto toml_path = temp_dir / "game_gb.toml";
        bool ok = pipeline.generate_initial_toml(gb_info, toml_path);
        assert(ok);
        assert(std::filesystem::exists(toml_path));

        dragon::GameConfig cfg = dragon::load_game_config(toml_path);
        assert(cfg.platform == "gb");
        assert(cfg.sha1 == gb_info.sha1);
        assert(cfg.entry_points.size() >= 5);
        std::cout << "  GB TOML generation and validation passed!\n";
    }

    // Test GBA TOML Generation
    {
        RomInfo gba_info;
        gba_info.platform = RomPlatform::GameBoyAdvance;
        gba_info.clean_name = "Some Other GBA Game";
        gba_info.file_path = "roms/gba/game.gba";
        gba_info.sha1 = "f1e2d3c4b5a69788796a5b4c3d2e1f0987654321";
        gba_info.save_type_str = "FLASH 128K";

        Pipeline pipeline;
        const auto toml_path = temp_dir / "game_gba.toml";
        bool ok = pipeline.generate_initial_toml(gba_info, toml_path);
        assert(ok);
        assert(std::filesystem::exists(toml_path));

        dragon::GameConfig cfg = dragon::load_game_config(toml_path);
        assert(cfg.platform == "gba");
        assert(cfg.sha1 == gba_info.sha1);
        assert(cfg.entry_points.size() >= 1);
        assert(cfg.entry_points[0].addr == 0x08000000U);
        assert(!cfg.interpreter_only);
        std::cout << "  GBA TOML generation and validation passed!\n";
    }

    // Test GBA TOML Generation: Pokemon Emerald auto-falls-back to interpreter_only.
    // Its VBlank handler calls into the m4a sound-engine mixer, whose recompiled code
    // never returns (see docs/GBA_SPEC.md §6.1), freezing the title screen. Matched by
    // SHA-1 (robust) or by clean_name (covers a re-dumped/retitled copy of the ROM).
    {
        RomInfo emerald_info;
        emerald_info.platform = RomPlatform::GameBoyAdvance;
        emerald_info.clean_name = "Pokemon - Edicion Esmeralda (Spain)";
        emerald_info.file_path = "roms/gba/emerald.gba";
        emerald_info.sha1 = "fe1558a3dcb0360ab558969e09b690888b846dd9";
        emerald_info.save_type_str = "FLASH 128K";

        Pipeline pipeline;
        const auto toml_path = temp_dir / "game_gba_emerald.toml";
        bool ok = pipeline.generate_initial_toml(emerald_info, toml_path);
        assert(ok);

        dragon::GameConfig cfg = dragon::load_game_config(toml_path);
        assert(cfg.platform == "gba");
        assert(cfg.interpreter_only);
        std::cout << "  GBA Pokemon Emerald interpreter_only auto-fallback passed!\n";
    }

    std::filesystem::remove_all(temp_dir);
}

void test_pipeline_compile_nes() {
    std::cout << "[Test] pipeline compile_c_to_binary NES...\n";
    const std::filesystem::path nes_path = "roms/nes/smb.nes";
    if (!std::filesystem::exists(nes_path)) {
        std::cout << "  (skipped: roms/nes/smb.nes not present)\n";
        return;
    }

    const auto temp_dir = std::filesystem::temp_directory_path() / "dragon_test_pipeline_compile";
    std::filesystem::create_directories(temp_dir);

    RomInfo info = detect_rom(nes_path);
    Pipeline pipeline;
    const auto toml_path = temp_dir / "game.toml";
    assert(pipeline.generate_initial_toml(info, toml_path));

    const auto gen_dir = temp_dir / "generated";
    std::filesystem::create_directories(gen_dir);

    // Run nesrecomp
    std::string recomp_cmd = "./build/nes/recompiler/nesrecomp " + toml_path.string() + " --rom " +
                             nes_path.string() + " --out " + gen_dir.string() +
                             " --single-file > /dev/null 2>&1";
    int ret = std::system(recomp_cmd.c_str());
    assert(ret == 0);

    const auto out_bin = temp_dir / "game_bin";
    bool compiled = pipeline.compile_c_to_binary(info, gen_dir, out_bin);
    if (!compiled) {
        std::cerr << "Compilation failed: " << pipeline.error_message() << "\n";
    }
    assert(compiled);
    assert(std::filesystem::exists(out_bin));
    std::cout << "  NES native compilation passed: generated " << out_bin << "\n";

    std::filesystem::remove_all(temp_dir);
}

void test_pipeline_compile_snes() {
    std::cout << "[Test] pipeline compile_c_to_binary SNES...\n";
    const std::filesystem::path snes_path = "roms/snes/smb_snes.sfc";
    if (!std::filesystem::exists(snes_path)) {
        std::cout << "  (skipped: roms/snes/smb_snes.sfc not present)\n";
        return;
    }

    const auto temp_dir =
        std::filesystem::temp_directory_path() / "dragon_test_pipeline_compile_snes";
    std::filesystem::create_directories(temp_dir);

    RomInfo info = detect_rom(snes_path);
    Pipeline pipeline;
    const auto toml_path = temp_dir / "game.toml";
    assert(pipeline.generate_initial_toml(info, toml_path));

    const auto gen_dir = temp_dir / "generated";
    std::filesystem::create_directories(gen_dir);

    // Run snesrecomp
    std::string recomp_cmd = "./build/snes/recompiler/snesrecomp " + toml_path.string() +
                             " --rom " + snes_path.string() + " --out " + gen_dir.string() +
                             " --single-file > /dev/null 2>&1";
    int ret = std::system(recomp_cmd.c_str());
    assert(ret == 0);

    const auto out_bin = temp_dir / "game_bin";
    bool compiled = pipeline.compile_c_to_binary(info, gen_dir, out_bin);
    if (!compiled) {
        std::cerr << "Compilation failed: " << pipeline.error_message() << "\n";
    }
    assert(compiled);
    assert(std::filesystem::exists(out_bin));
    std::cout << "  SNES native compilation passed: generated " << out_bin << "\n";

    std::filesystem::remove_all(temp_dir);
}

void test_pipeline_compile_gb() {
    std::cout << "[Test] pipeline compile_c_to_binary GB...\n";
    const auto temp_dir =
        std::filesystem::temp_directory_path() / "dragon_test_pipeline_compile_gb";
    std::filesystem::create_directories(temp_dir);

    RomInfo gb_info;
    gb_info.platform = RomPlatform::GameBoy;
    gb_info.clean_name = "Tetris Recomp";
    gb_info.file_path = "roms/gb/tetris.gb";
    gb_info.sha1 = "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678";
    gb_info.mbc_type_str = "ROM ONLY";

    const auto gen_dir = temp_dir / "generated";
    std::filesystem::create_directories(gen_dir);

    // Mock generated C file and header matching gb_recomp_game_main expectations
    {
        std::ofstream h(gen_dir / "recomp_functions.h");
        h << "#pragma once\n"
          << "#include <stddef.h>\n"
          << "#include \"gb/cpu_state.h\"\n"
          << "#ifdef __cplusplus\n"
          << "extern \"C\" {\n"
          << "#endif\n"
          << "void func_00_0100(CpuState* cpu);\n"
          << "extern const RecompFuncEntry kRecompDispatchTable[];\n"
          << "extern const size_t kRecompDispatchTableCount;\n"
          << "#ifdef __cplusplus\n"
          << "}\n"
          << "#endif\n";

        std::ofstream c(gen_dir / "game.c");
        c << "#include \"recomp_functions.h\"\n"
          << "void func_00_0100(CpuState* cpu) {\n"
          << "    if (cpu) cpu->halted = 1;\n"
          << "}\n"
          << "const RecompFuncEntry kRecompDispatchTable[] = {\n"
          << "    { 0x0100U, 0U, func_00_0100 }\n"
          << "};\n"
          << "const size_t kRecompDispatchTableCount = 1;\n";
    }

    Pipeline pipeline;
    const auto out_bin = temp_dir / "game_bin";
    bool compiled = pipeline.compile_c_to_binary(gb_info, gen_dir, out_bin);
    if (!compiled) {
        std::cerr << "GB Compilation failed: " << pipeline.error_message() << "\n";
    }
    assert(compiled);
    assert(std::filesystem::exists(out_bin));
    std::cout << "  GB native compilation passed: generated " << out_bin << "\n";

    // Test packager with GB binary
    PackageOptions opts;
    opts.source_binary = out_bin;
    opts.generated_dir = gen_dir;
    opts.rom_info = gb_info;
    opts.output_directory = temp_dir / "exported";
    opts.include_launcher_script = true;
    opts.include_desktop_entry = true;
    opts.bundle_standalone_cmake = true;

    PackageResult res = Packager::create_portable_package(opts);
    assert(res.success);
    assert(std::filesystem::exists(res.package_path / "run.sh"));
    assert(std::filesystem::exists(res.package_path / "CMakeLists.txt"));
    assert(std::filesystem::exists(res.package_path / "include" / "gb" / "bus.h"));
    std::cout << "  GB standalone package bundle passed!\n";

    std::filesystem::remove_all(temp_dir);
}

void test_pipeline_compile_gba() {
    std::cout << "[Test] pipeline compile_c_to_binary GBA...\n";
    const auto temp_dir =
        std::filesystem::temp_directory_path() / "dragon_test_pipeline_compile_gba";
    std::filesystem::create_directories(temp_dir);

    RomInfo gba_info;
    gba_info.platform = RomPlatform::GameBoyAdvance;
    gba_info.clean_name = "Emerald Recomp";
    gba_info.file_path = "roms/gba/emerald.gba";
    gba_info.sha1 = "f1e2d3c4b5a69788796a5b4c3d2e1f0987654321";
    gba_info.save_type_str = "FLASH 128K";

    const auto gen_dir = temp_dir / "generated";
    std::filesystem::create_directories(gen_dir);

    {
        std::ofstream h(gen_dir / "recomp_functions.h");
        h << "#pragma once\n"
          << "#include <stddef.h>\n"
          << "#include \"gba/cpu_state.h\"\n"
          << "#ifdef __cplusplus\n"
          << "extern \"C\" {\n"
          << "#endif\n"
          << "void func_08000000_arm(CpuState* cpu);\n"
          << "extern const RecompFuncEntry kRecompDispatchTable[];\n"
          << "extern const size_t kRecompDispatchTableCount;\n"
          << "#ifdef __cplusplus\n"
          << "}\n"
          << "#endif\n";

        std::ofstream c(gen_dir / "recomp_000.c");
        c << "#include \"recomp_functions.h\"\n"
          << "void func_08000000_arm(CpuState* cpu) {\n"
          << "    if (cpu) cpu->halted = 1;\n"
          << "}\n"
          << "const RecompFuncEntry kRecompDispatchTable[] = {\n"
          << "    { 0x08000000U, 0U, &func_08000000_arm },\n"
          << "    { 0U, 0U, (RecompFunc)0 }\n"
          << "};\n"
          << "const size_t kRecompDispatchTableCount = 1U;\n";
    }

    Pipeline pipeline;
    const auto out_bin = temp_dir / "game_bin";
    bool compiled = pipeline.compile_c_to_binary(gba_info, gen_dir, out_bin);
    if (!compiled) {
        std::cerr << "GBA Compilation failed: " << pipeline.error_message() << "\n";
    }
    assert(compiled);
    assert(std::filesystem::exists(out_bin));
    std::cout << "  GBA native compilation passed: generated " << out_bin << "\n";

    PackageOptions opts;
    opts.source_binary = out_bin;
    opts.generated_dir = gen_dir;
    opts.rom_info = gba_info;
    opts.output_directory = temp_dir / "exported";
    opts.include_launcher_script = true;
    opts.include_desktop_entry = true;
    opts.bundle_standalone_cmake = true;

    PackageResult res = Packager::create_portable_package(opts);
    assert(res.success);
    assert(std::filesystem::exists(res.package_path / "run.sh"));
    assert(std::filesystem::exists(res.package_path / "CMakeLists.txt"));
    assert(std::filesystem::exists(res.package_path / "include" / "gba" / "bus.h"));
    std::cout << "  GBA standalone package bundle passed!\n";

    std::filesystem::remove_all(temp_dir);
}

void test_diagnose_and_repair() {
    std::cout << "[Test] pipeline diagnose_and_repair (honest contract)...\n";
    Pipeline pipeline;

    // No active ROM: must fail honestly.
    auto res_empty = pipeline.diagnose_and_repair(DiagnosticSymptom::GrayScreen);
    assert(!res_empty.success);
    assert(!res_empty.summary.empty());

    const std::filesystem::path nes_path = "roms/nes/smb.nes";
    if (std::filesystem::exists(nes_path)) {
        RomInfo info = detect_rom(nes_path);
        assert(info.is_valid());
        // start_pipeline sets the active ROM (and kicks off a background
        // recompile that the Pipeline destructor will join).
        pipeline.start_pipeline(info);

        // Honest contract: symptoms with no safe automatic repair report
        // success == false with an explanatory, non-empty summary — they must
        // NOT claim a fictional hardware fix.
        const DiagnosticSymptom no_autofix[] = {
            DiagnosticSymptom::GrayScreen,  // no pending learned fallbacks => nothing to do
            DiagnosticSymptom::BlueScreen,
            DiagnosticSymptom::CorruptGraphics,
            DiagnosticSymptom::ControlsUnresponsive,
            DiagnosticSymptom::AbnormalSpeed,
            DiagnosticSymptom::NoSound,
        };
        for (DiagnosticSymptom s : no_autofix) {
            auto res = pipeline.diagnose_and_repair(s);
            assert(!res.success);
            assert(!res.summary.empty());
        }

        pipeline.cancel();
    }

    assert(std::filesystem::exists("knowledge/nes.toml"));
    assert(std::filesystem::exists("knowledge/snes.toml"));
    std::cout << "  Diagnostic honest-contract test passed!\n";
}

}  // namespace

int main() {
    test_detect_rom_nes();
    test_detect_rom_snes();
    test_detect_rom_gb_and_gbc();
    test_detect_rom_gba();
    test_detect_rom_invalid();
    test_packager();
    test_packager_windows();
    test_pipeline_toml_generation();
    test_pipeline_compile_nes();
    test_pipeline_compile_snes();
    test_pipeline_compile_gb();
    test_pipeline_compile_gba();
    test_diagnose_and_repair();
    std::cout << "All studio unit tests passed!\n";
    return 0;
}
