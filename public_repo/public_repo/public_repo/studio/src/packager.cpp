#include "studio/packager.h"

#include <chrono>
#include <fstream>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "dragon/file_io.h"

namespace dragon::studio {

namespace {

void make_executable(const std::filesystem::path& path) {
#if !defined(_WIN32)
    chmod(path.c_str(), 0755);
#else
    (void)path;
#endif
}

std::filesystem::path find_repo_root() {
    auto cur = std::filesystem::current_path();
    for (int i = 0; i < 5; ++i) {
        if (std::filesystem::exists(cur / "CMakeLists.txt") &&
            std::filesystem::exists(cur / "common")) {
            return cur;
        }
        if (!cur.has_parent_path() || cur == cur.parent_path()) {
            break;
        }
        cur = cur.parent_path();
    }
    return std::filesystem::current_path();
}

std::string sanitize_name(const std::string& name) {
    std::string safe_name;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            safe_name.push_back(c);
        } else if (c == ' ' || c == '_' || c == '-') {
            safe_name.push_back('_');
        }
    }
    if (safe_name.empty()) {
        safe_name = "DragonGame";
    }
    return safe_name;
}

}  // namespace

bool Packager::find_system_sdl3_library(std::filesystem::path& out_path) {
    const std::vector<std::filesystem::path> search_paths = {
        "/usr/lib/libSDL3.so.0",       "/usr/lib64/libSDL3.so.0",
        "/usr/local/lib/libSDL3.so.0", "/usr/lib/x86_64-linux-gnu/libSDL3.so.0",
        "/usr/lib/libSDL3.so",         "/usr/lib64/libSDL3.so",
        "/usr/local/lib/libSDL3.so",   "/usr/lib/x86_64-linux-gnu/libSDL3.so",
    };

    for (const auto& p : search_paths) {
        if (std::filesystem::exists(p)) {
            out_path = p;
            return true;
        }
    }
    return false;
}

bool Packager::find_system_sdl3_dll(std::filesystem::path& out_path) {
    const char* env_path = std::getenv("SDL3_DLL_PATH");
    if (env_path && std::filesystem::exists(env_path)) {
        out_path = env_path;
        return true;
    }

    const auto cur = std::filesystem::current_path();
    const auto repo = find_repo_root();

    std::vector<std::filesystem::path> search_paths = {
        cur / "SDL3.dll",
        cur / "lib" / "SDL3.dll",
        cur / "build" / "SDL3.dll",
        cur / "build" / "studio" / "SDL3.dll",
        repo / "SDL3.dll",
        repo / "build" / "SDL3.dll",
        repo / "build" / "studio" / "SDL3.dll",
        repo / "third_party" / "SDL3.dll",
        "C:\\Windows\\System32\\SDL3.dll",
        "C:\\tools\\SDL3\\bin\\SDL3.dll",
        "C:\\vcpkg\\installed\\x64-windows\\bin\\SDL3.dll",
        "C:\\msys64\\ucrt64\\bin\\SDL3.dll",
        "C:\\msys64\\mingw64\\bin\\SDL3.dll",
        "/usr/x86_64-w64-mingw32/bin/SDL3.dll",
        "/usr/x86_64-w64-mingw32/lib/SDL3.dll",
    };

    if (const char* user_prof = std::getenv("USERPROFILE")) {
        search_paths.push_back(std::filesystem::path(user_prof) / "tools" / "SDL3-3.4.20" /
                               "x86_64-w64-mingw32" / "bin" / "SDL3.dll");
        search_paths.push_back(std::filesystem::path(user_prof) / "tools" / "SDL3" / "bin" /
                               "SDL3.dll");
    }

    for (const auto& p : search_paths) {
        if (std::filesystem::exists(p)) {
            out_path = p;
            return true;
        }
    }

    // Check Wine / Proton search directories if on Linux
    const char* home = std::getenv("HOME");
    if (home) {
        const std::vector<std::filesystem::path> home_candidates = {
            std::filesystem::path(home) /
                ".local/share/anime-games-launcher/packages/persistent/ff748b3efc59b21a-lua_proton/"
                "versions/dwproton-11.0-15-x86_64/files/share/wine/mono/wine-mono-11.3.0/lib/"
                "x86_64/SDL3.dll",
            std::filesystem::path(home) /
                ".local/share/xodus-nexus/runtimes/releases/"
                "xodus-nexus-runtime-11.0-20261020-g8411926d/files/share/xalia/SDL3.dll",
        };
        for (const auto& p : home_candidates) {
            if (std::filesystem::exists(p)) {
                out_path = p;
                return true;
            }
        }
    }

    return false;
}

bool Packager::create_run_script(const std::filesystem::path& path, const std::string& bin_name) {
    std::ofstream out(path);
    if (!out.is_open()) {
        return false;
    }

    out << "#!/usr/bin/env bash\n"
        << "# Auto-generated portable launcher for DragonRecomp game\n"
        << "set -e\n"
        << "SCRIPT_DIR=\"$(cd \"$(dirname \"${BASH_SOURCE[0]}\")\" && pwd)\"\n"
        << "cd \"$SCRIPT_DIR\"\n"
        << "if [ -d \"$SCRIPT_DIR/lib\" ]; then\n"
        << "    if [ -n \"$LD_LIBRARY_PATH\" ]; then\n"
        << "        export LD_LIBRARY_PATH=\"$SCRIPT_DIR/lib:$LD_LIBRARY_PATH\"\n"
        << "    else\n"
        << "        export LD_LIBRARY_PATH=\"$SCRIPT_DIR/lib\"\n"
        << "    fi\n"
        << "fi\n"
        << "exec \"$SCRIPT_DIR/" << bin_name << "\" \"$@\"\n";
    out.close();

    make_executable(path);
    return true;
}

bool Packager::create_run_bat(const std::filesystem::path& path, const std::string& bin_name) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }

    out << "@echo off\r\n"
        << "setlocal\r\n"
        << "set \"SCRIPT_DIR=%~dp0\"\r\n"
        << "cd /d \"%SCRIPT_DIR%\"\r\n"
        << "set \"PATH=%SCRIPT_DIR%;%SCRIPT_DIR%lib;%PATH%\"\r\n\r\n"
        << "if exist \"%SCRIPT_DIR%" << bin_name << ".exe\" (\r\n"
        << "    start \"\" \"%SCRIPT_DIR%" << bin_name << ".exe\" %*\r\n"
        << ") else if exist \"%SCRIPT_DIR%build\\Release\\" << bin_name << ".exe\" (\r\n"
        << "    start \"\" \"%SCRIPT_DIR%build\\Release\\" << bin_name << ".exe\" %*\r\n"
        << ") else (\r\n"
        << "    echo ========================================================\r\n"
        << "    echo   " << bin_name << " (DragonRecomp Native Windows)\r\n"
        << "    echo ========================================================\r\n"
        << "    echo No se encontro el ejecutable '" << bin_name << ".exe'.\r\n"
        << "    echo Si aun no esta compilado, ejecuta 'build_windows.bat'.\r\n"
        << "    echo.\r\n"
        << "    pause\r\n"
        << ")\r\n";
    out.close();
    return true;
}

bool Packager::create_build_windows_bat(const std::filesystem::path& path,
                                        const std::string& bin_name) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }

    out << "@echo off\r\n"
        << "setlocal\r\n"
        << "echo ========================================================\r\n"
        << "echo   Compilando " << bin_name << " para Windows (DragonRecomp)\r\n"
        << "echo ========================================================\r\n"
        << "where cmake >nul 2>nul\r\n"
        << "if %errorlevel% neq 0 (\r\n"
        << "    echo Error: CMake no esta instalado o no se encuentra en el PATH.\r\n"
        << "    echo Descarga e instala CMake desde https://cmake.org/download/\r\n"
        << "    pause\r\n"
        << "    exit /b 1\r\n"
        << ")\r\n\r\n"
        << "if not exist build mkdir build\r\n"
        << "cd build\r\n"
        << "cmake ..\r\n"
        << "if %errorlevel% neq 0 (\r\n"
        << "    echo Error en la configuracion CMake.\r\n"
        << "    cd ..\r\n"
        << "    pause\r\n"
        << "    exit /b 1\r\n"
        << ")\r\n\r\n"
        << "cmake --build . --config Release\r\n"
        << "if %errorlevel% neq 0 (\r\n"
        << "    echo Error en la compilacion.\r\n"
        << "    cd ..\r\n"
        << "    pause\r\n"
        << "    exit /b 1\r\n"
        << ")\r\n\r\n"
        << "echo.\r\n"
        << "echo ========================================================\r\n"
        << "echo   Compilacion exitosa. Copiando ejecutable a la raiz...\r\n"
        << "echo ========================================================\r\n"
        << "if exist Release\\" << bin_name << ".exe copy /y Release\\" << bin_name
        << ".exe ..\\ >nul\r\n"
        << "if exist " << bin_name << ".exe copy /y " << bin_name << ".exe ..\\ >nul\r\n"
        << "cd ..\r\n"
        << "echo Listo. Puedes iniciar el juego haciendo doble clic en 'run.bat' o '" << bin_name
        << ".exe'.\r\n"
        << "pause\r\n";
    out.close();
    return true;
}

bool Packager::create_desktop_entry(const std::filesystem::path& path, const std::string& title,
                                    const std::string& bin_name) {
    std::ofstream out(path);
    if (!out.is_open()) {
        return false;
    }

    const auto parent_dir = std::filesystem::absolute(path.parent_path());
    const auto run_script = parent_dir / "run.sh";

    out << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Version=1.0\n"
        << "Name=" << title << "\n"
        << "GenericName=Video Game (DragonRecomp Native)\n"
        << "Comment=Recompiled native Linux game (" << bin_name
        << ") generated by DragonRecomp Studio\n"
        << "Exec=\"" << run_script.string() << "\"\n"
        << "Path=" << parent_dir.string() << "\n"
        << "Icon=input-gaming\n"
        << "Terminal=false\n"
        << "Categories=Game;Emulator;\n";
    out.close();

    make_executable(path);
    return true;
}

bool Packager::create_standalone_cmake(const std::filesystem::path& path,
                                       const PackageOptions& options,
                                       const std::string& safe_name) {
    std::ofstream out(path);
    if (!out.is_open()) {
        return false;
    }

    out << "cmake_minimum_required(VERSION 3.25)\n"
        << "project(" << safe_name << " LANGUAGES C CXX)\n\n"
        << "set(CMAKE_C_STANDARD 99)\n"
        << "set(CMAKE_C_STANDARD_REQUIRED ON)\n"
        << "set(CMAKE_CXX_STANDARD 20)\n"
        << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n"
        << "set(CMAKE_INSTALL_RPATH \"$ORIGIN/lib:$ORIGIN\")\n"
        << "set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)\n\n"
        << "if(MSVC)\n"
        << "  add_compile_options(/W4 /permissive- /utf-8 /EHsc)\n"
        << "  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)\n"
        << "else()\n"
        << "  add_compile_options(-O2 -Wall -Wextra)\n"
        << "endif()\n\n"
        << "find_package(SDL3 CONFIG QUIET)\n"
        << "if(NOT SDL3_FOUND)\n"
        << "  find_package(SDL3 REQUIRED)\n"
        << "endif()\n\n"
        << "file(GLOB RECOMP_SOURCES \"src/*.c\")\n"
        << "file(GLOB RUNTIME_SOURCES \"src/runtime/*.cpp\" \"src/common/*.cpp\")\n\n"
        << "add_executable(" << safe_name << "\n"
        << "  \"src/game_main.cpp\"\n"
        << "  ${RECOMP_SOURCES}\n"
        << "  ${RUNTIME_SOURCES}\n"
        << ")\n\n"
        << "target_include_directories(" << safe_name << " PRIVATE\n"
        << "  \"include\"\n"
        << "  \"src\"\n"
        << "  \"third_party/tomlplusplus\"\n"
        << ")\n\n"
        << "target_compile_definitions(" << safe_name << " PRIVATE\n"
        << "  DRAGON_GAME_TITLE=\"" << options.rom_info.clean_name << "\"\n"
        << "  DRAGON_DEFAULT_ROM_PATH=\"" << options.rom_info.file_path.generic_string() << "\"\n"
        << "  DRAGON_DEFAULT_ROM_SHA1=\"" << options.rom_info.sha1 << "\"\n"
        << "  DRAGON_HAVE_SDL3=1\n"
        << ")\n\n"
        << "target_link_libraries(" << safe_name << " PRIVATE SDL3::SDL3)\n"
        << "if(WIN32)\n"
        << "  set_target_properties(" << safe_name
        << " PROPERTIES WIN32_EXECUTABLE TRUE SUFFIX \".exe\")\n"
        << "endif()\n";
    out.close();
    return true;
}

bool Packager::create_readme(const std::filesystem::path& path, const RomInfo& rom) {
    std::ofstream out(path);
    if (!out.is_open()) {
        return false;
    }

    out << "======================================================================\n"
        << "  " << rom.clean_name << " — Recompilacion Nativa para Linux\n"
        << "  Generado con DragonRecomp Studio (DragonRecomp Platform)\n"
        << "======================================================================\n\n"
        << "Plataforma original: " << rom.platform_name() << "\n"
        << "Hash SHA-1:         " << rom.sha1 << "\n"
        << "Tamano ROM:         " << (rom.file_size / 1024) << " KiB\n\n"
        << "IMPORTANTE - ROM ORIGINAL REQUERIDO:\n"
        << "  Este paquete NO trae el ROM (ver 'roms/LEEME.txt'). Coloca tu propia\n"
        << "  copia legal en la carpeta 'roms/' antes de jugar.\n\n"
        << "COMO EJECUTAR:\n"
        << "  - Simplemente haz doble clic en 'run.sh' o ejecuta en tu terminal:\n"
        << "      ./run.sh\n"
        << "  - En Steam Deck / SteamOS:\n"
        << "      Anade 'run.sh' como juego no de Steam en el modo escritorio.\n\n"
        << "CONTROLES PREDETERMINADOS:\n"
        << "  - Teclado:\n"
        << "      Flechas       : Cruceta (D-Pad)\n"
        << "      Z             : Boton B (NES/SNES) / Salto o Carrera\n"
        << "      X             : Boton A (NES/SNES) / Accion\n"
        << "      A             : Boton Y (SNES)\n"
        << "      S             : Boton X (SNES)\n"
        << "      Q / W         : Botones L / R (SNES)\n"
        << "      Espacio       : Select\n"
        << "      Enter         : Start\n"
        << "      Escape        : Salir del juego\n"
        << "  - Mando / Gamepad:\n"
        << "      Cruceta / Stick: Movimiento\n"
        << "      A / B / X / Y  : Botones de accion\n"
        << "      Start / Back   : Start / Select\n\n"
        << "======================================================================\n";
    out.close();
    return true;
}

bool Packager::create_readme_windows(const std::filesystem::path& path, const RomInfo& rom,
                                     const std::string& bin_name) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }

    out << "======================================================================\r\n"
        << "  " << rom.clean_name << " — Recompilacion Nativa para Windows\r\n"
        << "  Generado con DragonRecomp Studio (DragonRecomp Platform)\r\n"
        << "======================================================================\r\n\r\n"
        << "Plataforma original: " << rom.platform_name() << "\r\n"
        << "Hash SHA-1:         " << rom.sha1 << "\r\n"
        << "Tamano ROM:         " << (rom.file_size / 1024) << " KiB\r\n"
        << "Ejecutable Windows: " << bin_name << ".exe\r\n\r\n"
        << "IMPORTANTE - ROM ORIGINAL REQUERIDO:\r\n"
        << "  Este paquete NO trae el ROM (ver 'roms\\LEEME.txt'). Coloca tu propia\r\n"
        << "  copia legal en la carpeta 'roms\\' antes de jugar.\r\n\r\n"
        << "COMO EJECUTAR EN WINDOWS:\r\n"
        << "  1. Haz doble clic en '" << bin_name << ".exe' o en 'run.bat'.\r\n"
        << "  2. Asegurate de que 'SDL3.dll' se encuentre en esta carpeta o en la carpeta "
           "'lib\\'.\r\n"
        << "     (Si falta, descargalo desde: https://github.com/libsdl-org/SDL/releases)\r\n\r\n"
        << "COMO RECOMPILAR EN WINDOWS (OPCIONAL):\r\n"
        << "  - Haz doble clic en 'build_windows.bat' (requiere Visual Studio 2022 o MinGW y "
           "CMake).\r\n"
        << "  - O mediante consola de comandos (cmd/PowerShell):\r\n"
        << "      cmake -B build\r\n"
        << "      cmake --build build --config Release\r\n\r\n"
        << "CONTROLES PREDETERMINADOS:\r\n"
        << "  - Teclado:\r\n"
        << "      Flechas       : Cruceta (D-Pad)\r\n"
        << "      Z             : Boton B (NES/SNES) / Salto o Carrera\r\n"
        << "      X             : Boton A (NES/SNES) / Accion\r\n"
        << "      A             : Boton Y (SNES)\r\n"
        << "      S             : Boton X (SNES)\r\n"
        << "      Q / W         : Botones L / R (SNES)\r\n"
        << "      Espacio       : Select\r\n"
        << "      Enter         : Start\r\n"
        << "      Escape        : Salir del juego\r\n"
        << "  - Mando / Gamepad (Xbox, PlayStation, etc.):\r\n"
        << "      Cruceta / Stick: Movimiento\r\n"
        << "      A / B / X / Y  : Botones de accion\r\n"
        << "      Start / Back   : Start / Select\r\n\r\n"
        << "======================================================================\r\n";
    out.close();
    return true;
}

bool Packager::create_rom_placeholder(const std::filesystem::path& package_dir,
                                      const RomInfo& rom) {
    const auto roms_dir = package_dir / "roms";
    std::error_code ec;
    std::filesystem::create_directories(roms_dir, ec);
    if (ec) {
        return false;
    }

    // Deliberately empty except for this note: the recompiled game binary
    // still reads the original ROM for graphics/audio data at startup (the
    // recompiler only translates the CPU logic, not the assets). Like every
    // other static-recompilation project (Zelda64Recomp, N64Recomp, nxnative,
    // ...), this package never bundles a copy of the ROM -- only the person
    // running it may add their own, since redistributing it would be
    // redistributing Nintendo's copyrighted data (see AGENTS.md rule 2).
    const auto note_path = roms_dir / "LEEME.txt";
    std::ofstream out(note_path);
    if (!out.is_open()) {
        return false;
    }
    out << "======================================================================\n"
        << "  ROM ORIGINAL REQUERIDO\n"
        << "======================================================================\n\n"
        << "Este paquete NO incluye el ROM de \"" << rom.clean_name << "\".\n"
        << "El ejecutable solo contiene la logica del juego recompilada a codigo\n"
        << "nativo; los graficos y el audio los sigue leyendo del ROM original, igual\n"
        << "que Zelda64Recomp, N64Recomp o nxnative. Esto es intencional: distribuir\n"
        << "el ROM de Nintendo seria distribuir su contenido con derechos de autor.\n\n"
        << "COMO JUGAR:\n"
        << "  1. Coloca tu PROPIA copia legal del ROM en esta carpeta ('roms/'),\n"
        << "     con cualquier nombre de archivo.\n"
        << "  2. El juego la detecta automaticamente por su huella SHA-1:\n"
        << "       " << rom.sha1 << "\n"
        << "  3. Si prefieres no moverla aqui, indica su ruta al ejecutar:\n"
        << "       ./run.sh --rom /ruta/a/tu/rom      (Linux)\n"
        << "       run.bat --rom C:\\ruta\\a\\tu\\rom      (Windows)\n\n"
        << "Si el ROM no coincide con el SHA-1 esperado (otra revision, otro idioma,\n"
        << "un ROM distinto), el juego no lo encontrara y te lo dira al arrancar.\n";
    out.close();
    return true;
}

bool Packager::bundle_runtime_sources(const std::filesystem::path& dest_dir,
                                      const PackageOptions& options) {
    const auto repo = find_repo_root();
    std::string platform_str = "nes";
    if (options.rom_info.platform == RomPlatform::SNES) {
        platform_str = "snes";
    } else if (options.rom_info.platform == RomPlatform::GameBoy ||
               options.rom_info.platform == RomPlatform::GameBoyColor) {
        platform_str = "gb";
    } else if (options.rom_info.platform == RomPlatform::GameBoyAdvance) {
        platform_str = "gba";
    }

    try {
        std::filesystem::create_directories(dest_dir / "src" / "runtime");
        std::filesystem::create_directories(dest_dir / "src" / "common");
        std::filesystem::create_directories(dest_dir / "include" / platform_str);
        std::filesystem::create_directories(dest_dir / "include" / "dragon");
        std::filesystem::create_directories(dest_dir / "third_party" / "tomlplusplus");

        // Copy common headers & sources
        const auto common_inc = repo / "common" / "include" / "dragon";
        if (std::filesystem::exists(common_inc)) {
            for (const auto& entry : std::filesystem::directory_iterator(common_inc)) {
                if (entry.is_regular_file()) {
                    std::filesystem::copy_file(
                        entry.path(), dest_dir / "include" / "dragon" / entry.path().filename(),
                        std::filesystem::copy_options::overwrite_existing);
                }
            }
        }

        const auto common_src = repo / "common" / "src";
        if (std::filesystem::exists(common_src)) {
            for (const auto& entry : std::filesystem::directory_iterator(common_src)) {
                if (entry.is_regular_file() && entry.path().extension() == ".cpp") {
                    std::filesystem::copy_file(
                        entry.path(), dest_dir / "src" / "common" / entry.path().filename(),
                        std::filesystem::copy_options::overwrite_existing);
                }
            }
        }

        const auto toml_hdr = repo / "third_party" / "tomlplusplus" / "toml.hpp";
        if (std::filesystem::exists(toml_hdr)) {
            std::filesystem::copy_file(toml_hdr,
                                       dest_dir / "third_party" / "tomlplusplus" / "toml.hpp",
                                       std::filesystem::copy_options::overwrite_existing);
        }

        // Copy platform runtime headers & sources
        const auto runtime_inc = repo / platform_str / "runtime" / "include" / platform_str;
        if (std::filesystem::exists(runtime_inc)) {
            for (const auto& entry : std::filesystem::directory_iterator(runtime_inc)) {
                if (entry.is_regular_file()) {
                    std::filesystem::copy_file(
                        entry.path(), dest_dir / "include" / platform_str / entry.path().filename(),
                        std::filesystem::copy_options::overwrite_existing);
                }
            }
        }

        const auto runtime_src = repo / platform_str / "runtime" / "src";
        if (std::filesystem::exists(runtime_src)) {
            for (const auto& entry : std::filesystem::directory_iterator(runtime_src)) {
                if (entry.is_regular_file() && entry.path().extension() == ".cpp") {
                    const auto fname = entry.path().filename().string();
                    if (fname != "nes_runner_main.cpp" && fname != "snes_runner_main.cpp" &&
                        fname != "gb_runner_main.cpp" && fname != "gba_runner_main.cpp" &&
                        fname != "nes_recomp_game_main.cpp" &&
                        fname != "snes_recomp_game_main.cpp" &&
                        fname != "gb_recomp_game_main.cpp" && fname != "gba_recomp_game_main.cpp") {
                        std::filesystem::copy_file(
                            entry.path(), dest_dir / "src" / "runtime" / entry.path().filename(),
                            std::filesystem::copy_options::overwrite_existing);
                    }
                }
            }
        }

        // Copy game_main.cpp
        std::string main_name = "nes_recomp_game_main.cpp";
        if (options.rom_info.platform == RomPlatform::SNES) {
            main_name = "snes_recomp_game_main.cpp";
        } else if (options.rom_info.platform == RomPlatform::GameBoy ||
                   options.rom_info.platform == RomPlatform::GameBoyColor) {
            main_name = "gb_recomp_game_main.cpp";
        } else if (options.rom_info.platform == RomPlatform::GameBoyAdvance) {
            main_name = "gba_recomp_game_main.cpp";
        }
        const auto main_cpp = repo / platform_str / "runtime" / "src" / main_name;
        if (std::filesystem::exists(main_cpp)) {
            std::filesystem::copy_file(main_cpp, dest_dir / "src" / "game_main.cpp",
                                       std::filesystem::copy_options::overwrite_existing);
        }

        // Copy generated C sources & header
        std::filesystem::path gen_src_dir = options.generated_dir;
        if (gen_src_dir.empty() && !options.source_binary.empty()) {
            const auto candidate = options.source_binary.parent_path() / "generated";
            if (std::filesystem::exists(candidate)) {
                gen_src_dir = candidate;
            }
        }
        if (gen_src_dir.empty()) {
            const auto candidate =
                repo / "build" / "studio_cache" / options.rom_info.sha1 / "generated";
            if (std::filesystem::exists(candidate)) {
                gen_src_dir = candidate;
            }
        }

        if (!gen_src_dir.empty() && std::filesystem::exists(gen_src_dir)) {
            for (const auto& entry : std::filesystem::directory_iterator(gen_src_dir)) {
                if (entry.is_regular_file()) {
                    if (entry.path().extension() == ".c") {
                        std::filesystem::copy_file(
                            entry.path(), dest_dir / "src" / entry.path().filename(),
                            std::filesystem::copy_options::overwrite_existing);
                    } else if (entry.path().extension() == ".h") {
                        std::filesystem::copy_file(
                            entry.path(), dest_dir / "include" / entry.path().filename(),
                            std::filesystem::copy_options::overwrite_existing);
                        std::filesystem::copy_file(
                            entry.path(), dest_dir / "src" / entry.path().filename(),
                            std::filesystem::copy_options::overwrite_existing);
                    }
                }
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

PackageResult Packager::create_linux_package(const PackageOptions& options,
                                             const std::string& safe_name) {
    PackageResult result;
    std::filesystem::path package_dir = options.output_directory / (safe_name + "_Linux");

    try {
        std::filesystem::create_directories(package_dir);
    } catch (const std::exception& e) {
        result.error_message = "No se pudo crear la carpeta de destino: " + std::string(e.what());
        return result;
    }

    // Copy binary
    std::filesystem::path dest_bin = package_dir / safe_name;
    std::filesystem::path src_bin;

    std::vector<std::filesystem::path> candidates;
    if (!options.source_binary.empty()) {
        candidates.push_back(options.source_binary);
        const auto parent = options.source_binary.parent_path();
        candidates.push_back(parent / safe_name);
        candidates.push_back(parent / options.rom_info.clean_name);
        candidates.push_back(parent / "game_binary");
    }

    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec) && std::filesystem::is_regular_file(c, ec) &&
            std::filesystem::file_size(c, ec) > 0) {
            src_bin = c;
            break;
        }
    }

    if (!src_bin.empty()) {
        try {
            std::filesystem::copy_file(src_bin, dest_bin,
                                       std::filesystem::copy_options::overwrite_existing);
            make_executable(dest_bin);
        } catch (const std::exception& e) {
            result.error_message = "Fallo al copiar el ejecutable: " + std::string(e.what());
            return result;
        }
    } else if (!options.bundle_standalone_cmake) {
        result.error_message = "No se encontro el binario compilado de Linux para empaquetar.";
        return result;
    }

    // Bundle SDL3 if requested
    if (options.bundle_sdl_library) {
        std::filesystem::path sdl_path;
        if (find_system_sdl3_library(sdl_path)) {
            std::filesystem::path lib_dir = package_dir / "lib";
            std::filesystem::create_directories(lib_dir);
            std::filesystem::copy_file(sdl_path, lib_dir / sdl_path.filename(),
                                       std::filesystem::copy_options::overwrite_existing);
        }
    }

    // Create run.sh
    if (options.include_launcher_script) {
        create_run_script(package_dir / "run.sh", safe_name);
    }

    // Create .desktop
    if (options.include_desktop_entry) {
        create_desktop_entry(package_dir / (safe_name + ".desktop"), options.rom_info.clean_name,
                             safe_name);
    }

    // Create README.txt
    create_readme(package_dir / "README.txt", options.rom_info);

    // roms/ placeholder: the package never bundles the ROM itself (spec 003).
    create_rom_placeholder(package_dir, options.rom_info);

    // Create standalone CMakeLists.txt & bundle sources if requested
    if (options.bundle_standalone_cmake) {
        create_standalone_cmake(package_dir / "CMakeLists.txt", options, safe_name);
        bundle_runtime_sources(package_dir, options);
    }

    // Calculate total size
    size_t bytes = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(package_dir)) {
        if (entry.is_regular_file()) {
            bytes += entry.file_size();
        }
    }

    result.success = true;
    result.package_path = package_dir;
    result.executable_path = dest_bin;
    result.total_bytes = bytes;
    return result;
}

PackageResult Packager::create_windows_package(const PackageOptions& options,
                                               const std::string& safe_name) {
    PackageResult result;
    std::filesystem::path package_dir = options.output_directory / (safe_name + "_Windows");

    try {
        std::filesystem::create_directories(package_dir);
    } catch (const std::exception& e) {
        result.error_message = "No se pudo crear la carpeta de destino: " + std::string(e.what());
        return result;
    }

    // Resolve source binary among candidate paths
    std::filesystem::path dest_bin = package_dir / (safe_name + ".exe");
    std::filesystem::path src_bin;

    std::vector<std::filesystem::path> candidates;
    if (!options.source_binary.empty()) {
        if (options.source_binary.extension() == ".exe") {
            candidates.push_back(options.source_binary);
        }
        candidates.push_back(options.source_binary.string() + ".exe");
        auto with_ext = options.source_binary;
        with_ext.replace_extension(".exe");
        candidates.push_back(with_ext);

#if defined(_WIN32)
        candidates.push_back(options.source_binary);
#endif

        const auto parent = options.source_binary.parent_path();
        candidates.push_back(parent / (safe_name + ".exe"));
        candidates.push_back(parent / (options.rom_info.clean_name + ".exe"));
        candidates.push_back(parent / "game_binary.exe");
        candidates.push_back(parent / "game_binary");
    }

    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec) && std::filesystem::is_regular_file(c, ec) &&
            std::filesystem::file_size(c, ec) > 0) {
            src_bin = c;
            break;
        }
    }

    if (!src_bin.empty()) {
        std::error_code ec;
        std::filesystem::copy_file(src_bin, dest_bin,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            result.error_message = "Fallo al copiar el ejecutable de Windows: " + ec.message();
            return result;
        }
    } else if (!options.bundle_standalone_cmake) {
        result.error_message = "No se encontro el binario compilado de Windows para empaquetar.";
        return result;
    }

    // Always bundle SDL3.dll for a completely portable, standalone Windows package
    {
        std::filesystem::path sdl_dll;
        if (!options.source_binary.empty()) {
            const auto cache_sdl = options.source_binary.parent_path() / "SDL3.dll";
            if (std::filesystem::exists(cache_sdl)) {
                sdl_dll = cache_sdl;
            }
        }
        if (sdl_dll.empty()) {
            find_system_sdl3_dll(sdl_dll);
        }
        if (!sdl_dll.empty()) {
            try {
                std::filesystem::copy_file(sdl_dll, package_dir / "SDL3.dll",
                                           std::filesystem::copy_options::overwrite_existing);
                std::filesystem::path lib_dir = package_dir / "lib";
                std::filesystem::create_directories(lib_dir);
                std::filesystem::copy_file(sdl_dll, lib_dir / "SDL3.dll",
                                           std::filesystem::copy_options::overwrite_existing);
            } catch (...) {
            }
        }
    }

    // Create run.bat
    if (options.include_launcher_script) {
        create_run_bat(package_dir / "run.bat", safe_name);
    }

    // Create build_windows.bat & standalone CMakeLists.txt
    if (options.bundle_standalone_cmake) {
        create_build_windows_bat(package_dir / "build_windows.bat", safe_name);
        create_standalone_cmake(package_dir / "CMakeLists.txt", options, safe_name);
        bundle_runtime_sources(package_dir, options);
    }

    // Create README.txt
    create_readme_windows(package_dir / "README.txt", options.rom_info, safe_name);

    // roms/ placeholder: the package never bundles the ROM itself (spec 003).
    create_rom_placeholder(package_dir, options.rom_info);

    // Calculate total size
    size_t bytes = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(package_dir)) {
        if (entry.is_regular_file()) {
            bytes += entry.file_size();
        }
    }

    result.success = true;
    result.package_path = package_dir;
    result.executable_path = dest_bin;
    result.total_bytes = bytes;
    return result;
}

PackageResult Packager::create_portable_package(const PackageOptions& options) {
    PackageResult result;

    bool source_exists = std::filesystem::exists(options.source_binary);
    if (!source_exists && !options.source_binary.empty()) {
        if (std::filesystem::exists(options.source_binary.string() + ".exe")) {
            source_exists = true;
        } else if (std::filesystem::exists(options.source_binary.parent_path() /
                                           "game_binary.exe")) {
            source_exists = true;
        } else if (std::filesystem::exists(options.source_binary.parent_path() / "game_binary")) {
            source_exists = true;
        }
    }

    if (!source_exists && !options.bundle_standalone_cmake) {
        result.error_message =
            "El binario compilado de origen no existe: " + options.source_binary.string();
        return result;
    }

    const std::string safe_name = sanitize_name(options.rom_info.clean_name);

    if (options.target_platform == TargetPlatform::Linux) {
        return create_linux_package(options, safe_name);
    } else if (options.target_platform == TargetPlatform::Windows) {
        return create_windows_package(options, safe_name);
    } else {
        // TargetPlatform::All: Create both Linux and Windows packages!
        PackageResult res_lin = create_linux_package(options, safe_name);
        PackageResult res_win = create_windows_package(options, safe_name);

        if (!res_lin.success) {
            return res_lin;
        }
        if (!res_win.success) {
            return res_win;
        }

        result.success = true;
        result.package_path = options.output_directory;
        result.executable_path = res_win.executable_path;
        result.total_bytes = res_lin.total_bytes + res_win.total_bytes;
        return result;
    }
}

}  // namespace dragon::studio
