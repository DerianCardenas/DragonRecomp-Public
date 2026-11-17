#pragma once

#include <filesystem>
#include <string>

#include "studio/rom_detector.h"

namespace dragon::studio {

enum class TargetPlatform {
    Linux,
    Windows,
    All,
};

struct PackageOptions {
    std::filesystem::path source_binary;
    RomInfo rom_info;
    std::filesystem::path output_directory;
    TargetPlatform target_platform = TargetPlatform::Linux;
    bool include_launcher_script = true;
    bool include_desktop_entry = true;
    bool bundle_sdl_library = true;
    bool bundle_standalone_cmake = true;
    std::filesystem::path generated_dir;
};

struct PackageResult {
    bool success = false;
    std::filesystem::path package_path;
    std::filesystem::path executable_path;
    std::string error_message;
    size_t total_bytes = 0;
};

class Packager {
public:
    static PackageResult create_portable_package(const PackageOptions& options);
    static PackageResult create_linux_package(const PackageOptions& options,
                                              const std::string& safe_name);
    static PackageResult create_windows_package(const PackageOptions& options,
                                                const std::string& safe_name);

    static bool find_system_sdl3_library(std::filesystem::path& out_path);
    static bool find_system_sdl3_dll(std::filesystem::path& out_path);
    static bool create_run_script(const std::filesystem::path& path, const std::string& bin_name);
    static bool create_run_bat(const std::filesystem::path& path, const std::string& bin_name);
    static bool create_build_windows_bat(const std::filesystem::path& path,
                                         const std::string& bin_name);
    static bool create_desktop_entry(const std::filesystem::path& path, const std::string& title,
                                     const std::string& bin_name);
    static bool create_standalone_cmake(const std::filesystem::path& path,
                                        const PackageOptions& options,
                                        const std::string& safe_name);
    static bool create_readme(const std::filesystem::path& path, const RomInfo& rom);
    static bool create_readme_windows(const std::filesystem::path& path, const RomInfo& rom,
                                      const std::string& bin_name);
    // Creates `<package_dir>/roms/` with a LEEME.txt explaining that the end
    // user must drop their own legal copy of the ROM there (matched by
    // SHA-1, see dragon::resolve_rom_path); the ROM itself is never copied
    // into the package (spec 003, copyright: see AGENTS.md rule 2).
    static bool create_rom_placeholder(const std::filesystem::path& package_dir,
                                       const RomInfo& rom);
    static bool bundle_runtime_sources(const std::filesystem::path& dest_dir,
                                       const PackageOptions& options);
};

}  // namespace dragon::studio
