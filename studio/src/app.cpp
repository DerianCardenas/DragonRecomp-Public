#include "studio/app.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "studio/theme.h"

namespace dragon::studio {

namespace {

void SDLCALL on_rom_file_picked(void* userdata, const char* const* filelist, int /*filter*/) {
    if (userdata && filelist && *filelist) {
        auto* app = static_cast<StudioApp*>(userdata);
        app->queue_load_rom(*filelist);
    }
}

void SDLCALL on_export_folder_picked(void* userdata, const char* const* filelist, int /*filter*/) {
    if (userdata && filelist && *filelist) {
        auto* app = static_cast<StudioApp*>(userdata);
        app->set_export_directory(*filelist);
    }
}

void SDLCALL on_pref_export_folder_picked(void* userdata, const char* const* filelist,
                                          int /*filter*/) {
    if (userdata && filelist && *filelist) {
        auto* app = static_cast<StudioApp*>(userdata);
        app->queue_pref_export_dir(*filelist);
    }
}

void SDLCALL on_import_folder_picked(void* userdata, const char* const* filelist, int /*filter*/) {
    if (userdata && filelist && *filelist) {
        auto* app = static_cast<StudioApp*>(userdata);
        app->queue_import_portable(*filelist);
    }
}

void SDLCALL on_manual_import_rom_picked(void* userdata, const char* const* filelist,
                                         int /*filter*/) {
    if (userdata && filelist && *filelist) {
        auto* app = static_cast<StudioApp*>(userdata);
        app->queue_manual_import_rom(*filelist);
    }
}

struct CoverDialogPayload {
    StudioApp* app;
    std::string sha1;
};

void SDLCALL on_cover_file_picked(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* payload = static_cast<CoverDialogPayload*>(userdata);
    if (payload) {
        if (payload->app && filelist && *filelist) {
            payload->app->queue_assign_cover(payload->sha1, *filelist);
        }
        delete payload;
    }
}

void open_folder_in_file_manager(const std::filesystem::path& folder) {
    if (std::filesystem::exists(folder)) {
#if defined(_WIN32)
        std::string cmd = "explorer \"" + folder.string() + "\"";
        int ret = std::system(cmd.c_str());
        (void)ret;
#else
        std::string cmd = "xdg-open \"" + folder.string() + "\" >/dev/null 2>&1 &";
        int ret = std::system(cmd.c_str());
        (void)ret;
#endif
    }
}

}  // namespace

StudioApp::StudioApp() = default;

StudioApp::~StudioApp() {
    shutdown();
}

bool StudioApp::init(int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "error: SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    window_ = SDL_CreateWindow(
        "DragonRecomp Studio — Recompilador Estatico Autonomo", width, height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_FULLSCREEN);
    if (!window_) {
        std::fprintf(stderr, "error: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetWindowFullscreen(window_, true);

    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        std::fprintf(stderr, "error: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }

    // Enable VSync
    SDL_SetRenderVSync(renderer_, 1);

    // Setup ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    display_scale_ = SDL_GetWindowDisplayScale(window_);
    if (display_scale_ < 1.0f) {
        display_scale_ = 1.0f;
    }

    fonts_ = load_studio_fonts(io, display_scale_);

    settings_ = SettingsStore::defaults();
    SettingsStore::load(settings_);
    draft_settings_ = settings_;
    apply_settings(false);

    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);

    running_ = true;
    library_.load();

    // Remove legacy entries that were added before portable-only enforcement (no package_dir and no
    // executables).
    std::vector<std::string> unexported_shas;
    for (const auto& e : library_.entries()) {
        if (!e.is_portable()) {
            unexported_shas.push_back(e.sha1);
        }
    }
    if (!unexported_shas.empty()) {
        for (const auto& s : unexported_shas) {
            library_.remove(s);
        }
        library_.save();
    }

    return true;
}

void StudioApp::set_export_directory(const std::string& path) {
    std::strncpy(export_dir_buf_, path.c_str(), sizeof(export_dir_buf_) - 1);
    export_dir_buf_[sizeof(export_dir_buf_) - 1] = '\0';
}

void StudioApp::open_pref_export_dir_dialog() {
    const std::string cur = draft_settings_.export_directory.string();
    const char* def = cur.empty() ? nullptr : cur.c_str();
    SDL_ShowOpenFolderDialog(&on_pref_export_folder_picked, this, window_, def, false);
}

void StudioApp::queue_pref_export_dir(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(pending_pref_dir_mutex_);
    pending_pref_export_dir_ = path;
}

void StudioApp::apply_settings(bool persist) {
    if (settings_.export_directory.empty()) {
        settings_.export_directory = SettingsStore::defaults().export_directory;
    }
    apply_studio_theme(settings_.dark_mode, settings_.accent_color, display_scale_);
    pipeline_.set_max_compile_threads(settings_.max_compile_threads);
    pipeline_.set_compile_opt_level(settings_.opt_level);
    set_export_directory(settings_.export_directory.string());
    crt_filter_option_ = settings_.default_crt_filter;
    if (persist) {
        SettingsStore::save(settings_);
    }
}

void StudioApp::quick_save_to_library() {
    if (!active_rom_.is_valid() || pipeline_.stage() != PipelineStage::Ready) {
        return;
    }
    PackageOptions opts;
    auto src_bin = pipeline_.output_binary();
#if defined(_WIN32)
    if (!pipeline_.output_binary_windows().empty() &&
        std::filesystem::exists(pipeline_.output_binary_windows())) {
        src_bin = pipeline_.output_binary_windows();
    }
    opts.target_platform = TargetPlatform::Windows;
#else
    opts.target_platform = TargetPlatform::Linux;
#endif
    opts.source_binary = src_bin;
    opts.rom_info = active_rom_;
    opts.output_directory = settings_.export_directory.empty()
                                ? SettingsStore::defaults().export_directory
                                : settings_.export_directory;
    opts.include_launcher_script = true;
    opts.include_desktop_entry = true;
    opts.bundle_sdl_library = true;
    opts.bundle_standalone_cmake = false;

    last_package_result_ = Packager::create_portable_package(opts);
    if (last_package_result_.success) {
        const bool interpreter_only = (pipeline_.stats().functions == 0);
        const GameMode mode = interpreter_only ? GameMode::Interpreter : GameMode::Native;
        GameEntry* entry =
            library_.add_portable(active_rom_, last_package_result_.package_path,
                                  last_package_result_.linux_executable_path,
                                  last_package_result_.windows_executable_path, mode);
        if (entry) {
            entry->settings.fullscreen = fullscreen_option_;
            entry->settings.crt_filter = crt_filter_option_;
            library_.save();
            library_console_tab_ = console_tab_for_platform(entry->platform);
            selected_library_sha1_ = entry->sha1;
            if (entry->cover_path.empty()) {
                cover_mgr_.download_cover_async(
                    *entry, [this, sha1 = entry->sha1](bool ok, const std::filesystem::path& path) {
                        if (ok) {
                            queue_assign_cover(sha1, path);
                        }
                    });
            }
        }
        export_status_message_ =
            "Guardado en biblioteca: " + last_package_result_.package_path.string();
    } else {
        export_status_message_ = "Error al guardar: " + last_package_result_.error_message;
    }
}

void StudioApp::shutdown() {
    if (running_) {
        running_ = false;
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();

        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
        if (window_) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
        SDL_Quit();
    }
}

void StudioApp::load_rom(const std::filesystem::path& rom_path) {
    RomInfo info = detect_rom(rom_path);
    if (info.is_valid()) {
        active_rom_ = info;
        current_view_ = StudioView::Recompiler;
        show_package_modal_ = false;
        export_status_message_.clear();
        last_package_result_ = PackageResult{};
        diagnostic_status_message_.clear();
        pipeline_.clear_last_game_error();
        pipeline_.start_pipeline(info);
    }
}

int StudioApp::console_tab_for_platform(const std::string& platform_key) const {
    if (platform_key == "snes")
        return 1;
    if (platform_key == "gb" || platform_key == "gbc")
        return 2;
    if (platform_key == "gba")
        return 3;
    if (platform_key == "sms" || platform_key == "gg")
        return 4;
    return 0;  // nes
}

void StudioApp::open_import_portable_dialog() {
    const char* default_dir = export_dir_buf_[0] != '\0' ? export_dir_buf_ : nullptr;
    SDL_ShowOpenFolderDialog(&on_import_folder_picked, this, window_, default_dir, false);
}

void StudioApp::queue_import_portable(const std::filesystem::path& folder_path) {
    std::lock_guard<std::mutex> lock(pending_import_mutex_);
    pending_import_folder_ = folder_path;
}

void StudioApp::open_manual_import_rom_dialog() {
    static const SDL_DialogFileFilter kRomFilters[] = {
        {"ROMs soportadas (*.nes, *.sfc, *.smc, *.gb, *.gbc, *.gba, *.sms, *.gg)",
         "nes;sfc;smc;gb;gbc;gba;sms;gg"},
        {"Todos los archivos", "*"}};
    SDL_ShowOpenFileDialog(&on_manual_import_rom_picked, this, window_, kRomFilters, 2, nullptr,
                           false);
}

void StudioApp::queue_manual_import_rom(const std::filesystem::path& rom_path) {
    std::lock_guard<std::mutex> lock(pending_import_mutex_);
    pending_manual_rom_path_ = rom_path;
}

void StudioApp::import_portable_folder(const std::filesystem::path& folder_path) {
    current_view_ = StudioView::Library;
    PortableInspection insp = GameLibrary::inspect_portable_folder(folder_path);

    if (insp.has_valid_rom()) {
        GameEntry* entry =
            library_.add_portable(insp.rom_info, insp.package_dir, insp.executable_linux,
                                  insp.executable_windows, GameMode::Native);
        if (entry) {
            library_.save();
            library_console_tab_ = console_tab_for_platform(entry->platform);
            library_notice_ = "✔ Juego recompilado importado: " + entry->title;
            if (entry->cover_path.empty()) {
                cover_mgr_.download_cover_async(
                    *entry, [this, sha1 = entry->sha1](bool ok, const std::filesystem::path& path) {
                        if (ok) {
                            queue_assign_cover(sha1, path);
                        }
                    });
            }
        }
        return;
    }

    // No valid ROM was found in `roms/` -> open manual import modal with the required alert
    pending_inspection_ = insp;
    std::strncpy(manual_import_title_buf_, insp.hinted_title.c_str(),
                 sizeof(manual_import_title_buf_) - 1);
    manual_import_title_buf_[sizeof(manual_import_title_buf_) - 1] = '\0';

    switch (insp.hinted_platform) {
        case RomPlatform::NES:
            manual_import_platform_idx_ = 0;
            break;
        case RomPlatform::SNES:
            manual_import_platform_idx_ = 1;
            break;
        case RomPlatform::GameBoy:
            manual_import_platform_idx_ = 2;
            break;
        case RomPlatform::GameBoyColor:
            manual_import_platform_idx_ = 3;
            break;
        case RomPlatform::GameBoyAdvance:
            manual_import_platform_idx_ = 4;
            break;
        case RomPlatform::MasterSystem:
            manual_import_platform_idx_ = 5;
            break;
        case RomPlatform::GameGear:
            manual_import_platform_idx_ = 6;
            break;
        default:
            if (library_console_tab_ == 1)
                manual_import_platform_idx_ = 1;
            else if (library_console_tab_ == 2)
                manual_import_platform_idx_ = 2;
            else if (library_console_tab_ == 3)
                manual_import_platform_idx_ = 4;
            else if (library_console_tab_ == 4)
                manual_import_platform_idx_ = 5;
            else
                manual_import_platform_idx_ = 0;
            break;
    }

    std::strncpy(manual_import_exec_linux_buf_, insp.executable_linux.string().c_str(),
                 sizeof(manual_import_exec_linux_buf_) - 1);
    manual_import_exec_linux_buf_[sizeof(manual_import_exec_linux_buf_) - 1] = '\0';

    std::strncpy(manual_import_exec_win_buf_, insp.executable_windows.string().c_str(),
                 sizeof(manual_import_exec_win_buf_) - 1);
    manual_import_exec_win_buf_[sizeof(manual_import_exec_win_buf_) - 1] = '\0';

    manual_import_rom_buf_[0] = '\0';
    manual_import_copy_rom_ = true;
    manual_import_error_.clear();
    show_manual_import_modal_ = true;
}

void StudioApp::launch_portable_game(const GameEntry& entry) {
    if (pipeline_.is_game_running() || portable_game_pid_ > 0) {
        library_notice_ = "Ya hay un juego en ejecucion.";
        return;
    }

    const std::filesystem::path bin = GameLibrary::resolve_host_executable(entry);
    std::error_code ec;
    if (bin.empty() || !std::filesystem::exists(bin, ec)) {
#if defined(_WIN32)
        if (!entry.executable_linux.empty() && entry.executable_windows.empty()) {
            library_notice_ = "El paquete de \"" + entry.title +
                              "\" fue exportado solo para Linux. Exportalo para Windows o Ambos.";
            return;
        }
#else
        if (!entry.executable_windows.empty() && entry.executable_linux.empty()) {
            library_notice_ = "El paquete de \"" + entry.title +
                              "\" fue exportado solo para Windows (.exe). Exportalo para Linux o "
                              "Ambos.";
            return;
        }
#endif
        library_notice_ = "No se encontro el ejecutable portable de \"" + entry.title + "\" en " +
                          entry.package_dir.string();
        return;
    }

    const std::filesystem::path abs_bin = std::filesystem::absolute(bin, ec);
    const std::filesystem::path work_dir = abs_bin.parent_path();

    // Find a valid ROM path if available (either entry.rom_path or inside work_dir/roms)
    std::filesystem::path rom_to_pass;
    if (!entry.rom_path.empty() && std::filesystem::exists(entry.rom_path, ec)) {
        rom_to_pass = std::filesystem::absolute(entry.rom_path, ec);
    } else {
        const auto insp = GameLibrary::inspect_portable_folder(work_dir);
        if (!insp.found_rom_path.empty() && std::filesystem::exists(insp.found_rom_path, ec)) {
            rom_to_pass = std::filesystem::absolute(insp.found_rom_path, ec);
        }
    }

    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

#if !defined(_WIN32)
    chmod(abs_bin.c_str(), 0755);

    std::vector<std::string> arg_strings;
    arg_strings.push_back(abs_bin.string());
    if (!rom_to_pass.empty()) {
        arg_strings.push_back("--rom");
        arg_strings.push_back(rom_to_pass.string());
    }
    if (entry.settings.fullscreen) {
        arg_strings.push_back("--fullscreen");
    }
    if (entry.settings.crt_filter || settings_.default_crt_filter) {
        arg_strings.push_back("--crt");
    }
    if (settings_.auto_save_state) {
        arg_strings.push_back("--save-state");
        arg_strings.push_back((work_dir / "autosave.state").string());
    }

    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(arg_strings.size() + 1);
    for (auto& s : arg_strings) {
        argv_ptrs.push_back(s.data());
    }
    argv_ptrs.push_back(nullptr);

    const std::string lib_dir = (work_dir / "lib").string();
    std::string ld_path = lib_dir;
    if (const char* cur_ld = std::getenv("LD_LIBRARY_PATH"); cur_ld && cur_ld[0] != '\0') {
        ld_path += ":" + std::string(cur_ld);
    }

    pid_t pid = fork();
    if (pid < 0) {
        library_notice_ = "Error al iniciar el proceso del juego portable.";
        return;
    }
    if (pid == 0) {
        if (chdir(work_dir.c_str()) != 0) {
            _exit(126);
        }
        setenv("LD_LIBRARY_PATH", ld_path.c_str(), 1);
        execv(abs_bin.c_str(), argv_ptrs.data());
        _exit(127);
    }

    portable_game_pid_ = static_cast<int>(pid);
    portable_game_sha1_ = entry.sha1;
    portable_game_start_unix_ = now;
    library_.touch_played(entry.sha1, 0, now);
    library_.save();
    library_notice_ = "▶ Ejecutando portable: " + entry.title;
#else
    std::string cmd = "\"" + abs_bin.string() + "\"";
    if (!rom_to_pass.empty()) {
        cmd += " --rom \"" + rom_to_pass.string() + "\"";
    }
    if (entry.settings.fullscreen) {
        cmd += " --fullscreen";
    }
    if (entry.settings.crt_filter || settings_.default_crt_filter) {
        cmd += " --crt";
    }
    if (settings_.auto_save_state) {
        cmd += " --save-state \"" + (work_dir / "autosave.state").string() + "\"";
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    if (!CreateProcessA(NULL, cmd_buf.data(), NULL, NULL, FALSE, 0, NULL, work_dir.string().c_str(),
                        &si, &pi)) {
        library_notice_ = "No se pudo ejecutar el binario portable de Windows: " + abs_bin.string();
        return;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    portable_game_pid_ = static_cast<int>(pi.dwProcessId);
    portable_game_sha1_ = entry.sha1;
    portable_game_start_unix_ = now;
    library_.touch_played(entry.sha1, 0, now);
    library_.save();
    library_notice_ = "▶ Ejecutando portable: " + entry.title;
#endif
}

void StudioApp::queue_load_rom(const std::filesystem::path& rom_path) {
    std::lock_guard<std::mutex> lock(pending_rom_mutex_);
    pending_rom_to_load_ = rom_path;
}

void StudioApp::open_rom_file_dialog() {
    static const SDL_DialogFileFilter kRomFilters[] = {
        {"Todas las ROMs soportadas (*.nes, *.sfc, *.smc, *.gb, *.gbc, *.gba, *.sms, *.gg)",
         "nes;sfc;smc;gb;gbc;gba;sms;gg"},
        {"Sega Master System / Game Gear (*.sms, *.gg)", "sms;gg"},
        {"ROMs de Game Boy Advance (*.gba)", "gba"},
        {"ROMs de Game Boy / Color (*.gb, *.gbc)", "gb;gbc"},
        {"ROMs de NES (*.nes)", "nes"},
        {"ROMs de SNES (*.sfc, *.smc)", "sfc;smc"},
        {"Todos los archivos", "*"}};
    SDL_ShowOpenFileDialog(&on_rom_file_picked, this, window_, kRomFilters, 7, nullptr, false);
}

void StudioApp::open_cover_file_dialog(const std::string& sha1) {
    static const SDL_DialogFileFilter kCoverFilters[] = {
        {"Imagenes de caratula (*.png, *.jpg, *.jpeg, *.bmp, *.webp)", "png;jpg;jpeg;bmp;webp"},
        {"Todos los archivos", "*"}};
    auto* payload = new CoverDialogPayload{this, sha1};
    SDL_ShowOpenFileDialog(&on_cover_file_picked, payload, window_, kCoverFilters, 2, nullptr,
                           false);
}

void StudioApp::queue_assign_cover(const std::string& sha1,
                                   const std::filesystem::path& cover_path) {
    std::lock_guard<std::mutex> lock(pending_cover_mutex_);
    pending_covers_to_assign_.emplace_back(sha1, cover_path);
}

void StudioApp::process_events() {
    std::filesystem::path pending;
    {
        std::lock_guard<std::mutex> lock(pending_rom_mutex_);
        if (!pending_rom_to_load_.empty()) {
            pending = pending_rom_to_load_;
            pending_rom_to_load_.clear();
        }
    }
    if (!pending.empty()) {
        load_rom(pending);
    }

    std::filesystem::path pending_import;
    std::filesystem::path pending_manual_rom;
    {
        std::lock_guard<std::mutex> lock(pending_import_mutex_);
        if (!pending_import_folder_.empty()) {
            pending_import = pending_import_folder_;
            pending_import_folder_.clear();
        }
        if (!pending_manual_rom_path_.empty()) {
            pending_manual_rom = pending_manual_rom_path_;
            pending_manual_rom_path_.clear();
        }
    }
    if (!pending_import.empty()) {
        import_portable_folder(pending_import);
    }
    if (!pending_manual_rom.empty()) {
        std::strncpy(manual_import_rom_buf_, pending_manual_rom.string().c_str(),
                     sizeof(manual_import_rom_buf_) - 1);
        manual_import_rom_buf_[sizeof(manual_import_rom_buf_) - 1] = '\0';
        RomInfo info = detect_rom(pending_manual_rom);
        if (info.is_valid()) {
            if (manual_import_title_buf_[0] == '\0' && !info.clean_name.empty()) {
                std::strncpy(manual_import_title_buf_, info.clean_name.c_str(),
                             sizeof(manual_import_title_buf_) - 1);
                manual_import_title_buf_[sizeof(manual_import_title_buf_) - 1] = '\0';
            }
            switch (info.platform) {
                case RomPlatform::NES:
                    manual_import_platform_idx_ = 0;
                    break;
                case RomPlatform::SNES:
                    manual_import_platform_idx_ = 1;
                    break;
                case RomPlatform::GameBoy:
                    manual_import_platform_idx_ = 2;
                    break;
                case RomPlatform::GameBoyColor:
                    manual_import_platform_idx_ = 3;
                    break;
                case RomPlatform::GameBoyAdvance:
                    manual_import_platform_idx_ = 4;
                    break;
                case RomPlatform::MasterSystem:
                    manual_import_platform_idx_ = 5;
                    break;
                case RomPlatform::GameGear:
                    manual_import_platform_idx_ = 6;
                    break;
                default:
                    break;
            }
        }
    }

    std::vector<std::pair<std::string, std::filesystem::path>> pending_covers;
    {
        std::lock_guard<std::mutex> lock(pending_cover_mutex_);
        if (!pending_covers_to_assign_.empty()) {
            pending_covers.swap(pending_covers_to_assign_);
        }
    }
    for (const auto& [sha1, path] : pending_covers) {
        std::filesystem::path assigned = path;
        if (path.parent_path() != CoverManager::covers_dir()) {
            assigned = cover_mgr_.assign_manual_cover(sha1, path);
        } else {
            cover_mgr_.invalidate(sha1);
        }
        if (!assigned.empty()) {
            library_.set_cover(sha1, assigned);
            library_.save();
        }
    }

    std::filesystem::path pending_pref_dir;
    {
        std::lock_guard<std::mutex> lock(pending_pref_dir_mutex_);
        if (!pending_pref_export_dir_.empty()) {
            pending_pref_dir = pending_pref_export_dir_;
            pending_pref_export_dir_.clear();
        }
    }
    if (!pending_pref_dir.empty()) {
        draft_settings_.export_directory = pending_pref_dir;
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);

        if (event.type == SDL_EVENT_QUIT) {
            running_ = false;
        } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                   event.window.windowID == SDL_GetWindowID(window_)) {
            running_ = false;
        } else if (event.type == SDL_EVENT_KEY_DOWN &&
                   (event.key.key == SDLK_F11 ||
                    (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)))) {
            const bool is_fs = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
            SDL_SetWindowFullscreen(window_, !is_fs);
        } else if (event.type == SDL_EVENT_DROP_FILE) {
            if (event.drop.data) {
                const std::filesystem::path dropped = event.drop.data;
                std::error_code ec;
                if (std::filesystem::is_directory(dropped, ec)) {
                    import_portable_folder(dropped);
                } else if (CoverManager::is_image_file(dropped)) {
                    std::string target_sha;
                    if (active_rom_.is_valid()) {
                        target_sha = active_rom_.sha1;
                    } else if (const GameEntry* last = library_.last_played()) {
                        target_sha = last->sha1;
                    }
                    if (!target_sha.empty()) {
                        const auto assigned = cover_mgr_.assign_manual_cover(target_sha, dropped);
                        if (!assigned.empty()) {
                            library_.set_cover(target_sha, assigned);
                            library_.save();
                        }
                    }
                } else {
                    load_rom(dropped);
                }
            }
        }
    }
}

void StudioApp::run() {
    while (running_) {
        process_events();

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        render_ui();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer_, static_cast<Uint8>(ThemeColors::BgApp.x * 255.0f),
                               static_cast<Uint8>(ThemeColors::BgApp.y * 255.0f),
                               static_cast<Uint8>(ThemeColors::BgApp.z * 255.0f), 255);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_RenderPresent(renderer_);
    }
}

void StudioApp::render_package_modal() {
    if (!show_package_modal_) {
        return;
    }

    ImGui::OpenPopup("ExportarPaqueteModal");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(840.0f, 620.0f));

    if (ImGui::BeginPopupModal(
            "ExportarPaqueteModal", &show_package_modal_,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        // Custom elegant Header without default OS title bar
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + ImGui::GetContentRegionAvail().x, p0.y + 42.0f);
        draw_list->AddRectFilled(p0, p1, ImGui::GetColorU32(ThemeColors::SurfaceAlt), 6.0f);
        draw_list->AddRect(p0, p1, ImGui::GetColorU32(ThemeColors::Border), 6.0f);

        ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.0f, p0.y + 8.0f));
        ImGui::PushFont(fonts_.h2);
        ImGui::TextColored(ThemeColors::AccentCyan, "EXPORTAR JUEGO PORTABLE INDEPENDIENTE");
        ImGui::PopFont();

        // Top right close button
        ImGui::SetCursorScreenPos(ImVec2(p1.x - 32.0f, p0.y + 6.0f));
        if (ImGui::Button("✕", ImVec2(24.0f, 24.0f))) {
            show_package_modal_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + 12.0f));

        // 1. Destination directory selection
        ImGui::TextColored(ThemeColors::TextPrimary, "Directorio de destino de exportacion:");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 140.0f);
        ImGui::InputText("##ExportDir", export_dir_buf_, sizeof(export_dir_buf_));
        ImGui::SameLine(0, 10.0f);
        if (ImGui::Button("Examinar...", ImVec2(130.0f, 0))) {
            SDL_ShowOpenFolderDialog(&on_export_folder_picked, this, window_, export_dir_buf_,
                                     false);
        }

        // Quick presets
        ImGui::Spacing();
        ImGui::TextColored(ThemeColors::TextMuted, "Accesos rapidos:");
        ImGui::SameLine();
        if (ImGui::SmallButton("ExportedGames")) {
            set_export_directory((std::filesystem::current_path() / "ExportedGames").string());
        }
        const char* home =
            std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : std::getenv("HOME");
        if (home) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Escritorio")) {
                set_export_directory((std::filesystem::path(home) / "Desktop").string());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Carpeta Personal")) {
                set_export_directory(home);
            }
        }

        // Platform Selection
        ImGui::Spacing();
        ImGui::TextColored(ThemeColors::TextPrimary, "Plataforma de destino:");
        ImGui::RadioButton("Linux Nativo (ELF)", &export_platform_idx_, 0);
        ImGui::SameLine(0, 15.0f);
        ImGui::RadioButton("Windows (.exe / Batch)", &export_platform_idx_, 1);
        ImGui::SameLine(0, 15.0f);
        ImGui::RadioButton("Ambos (Multiplataforma)", &export_platform_idx_, 2);

        // Options
        ImGui::Spacing();
        if (export_platform_idx_ == 0 || export_platform_idx_ == 2) {
            ImGui::Checkbox("Lanzador 'run.sh'", &include_launcher_script_);
            ImGui::SameLine(0, 15.0f);
            ImGui::Checkbox("Acceso '.desktop'", &include_desktop_entry_);
        }
        if (export_platform_idx_ == 1 || export_platform_idx_ == 2) {
            if (export_platform_idx_ == 2) {
                ImGui::Spacing();
            }
            ImGui::Checkbox("Lanzador 'run.bat'", &include_windows_batch_);
            ImGui::SameLine(0, 15.0f);
            ImGui::Checkbox("Proyecto CMake para Windows", &bundle_standalone_cmake_);
        }

        ImGui::Spacing();
        ImGui::Dummy(ImVec2(0, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.50f, 0.80f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.60f, 0.92f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.70f, 1.0f, 1.0f));
        if (ImGui::Button("⚡ Exportar Ahora", ImVec2(240.0f, 44.0f))) {
            PackageOptions opts;
            auto src_bin = pipeline_.output_binary();
#if defined(_WIN32)
            if (!pipeline_.output_binary_windows().empty() &&
                std::filesystem::exists(pipeline_.output_binary_windows())) {
                src_bin = pipeline_.output_binary_windows();
            }
#else
            if (export_platform_idx_ == 1 && !pipeline_.output_binary_windows().empty() &&
                std::filesystem::exists(pipeline_.output_binary_windows())) {
                src_bin = pipeline_.output_binary_windows();
            }
#endif
            opts.source_binary = src_bin;
            opts.rom_info = active_rom_;
            opts.output_directory = export_dir_buf_;
            opts.target_platform = (export_platform_idx_ == 0)   ? TargetPlatform::Linux
                                   : (export_platform_idx_ == 1) ? TargetPlatform::Windows
                                                                 : TargetPlatform::All;
            opts.include_launcher_script = include_launcher_script_;
            opts.include_desktop_entry = include_desktop_entry_;
            opts.bundle_sdl_library = true;
            opts.bundle_standalone_cmake = bundle_standalone_cmake_;

            last_package_result_ = Packager::create_portable_package(opts);
            if (last_package_result_.success) {
                export_status_message_ = "Juego exportado satisfactoriamente";

                const bool interpreter_only = (pipeline_.stats().functions == 0);
                const GameMode mode = interpreter_only ? GameMode::Interpreter : GameMode::Native;

                GameEntry* entry =
                    library_.add_portable(active_rom_, last_package_result_.package_path,
                                          last_package_result_.linux_executable_path,
                                          last_package_result_.windows_executable_path, mode);
                if (entry) {
                    entry->settings.fullscreen = fullscreen_option_;
                    entry->settings.crt_filter = crt_filter_option_;
                    library_.save();
                    library_console_tab_ = console_tab_for_platform(entry->platform);
                    if (entry->cover_path.empty()) {
                        cover_mgr_.download_cover_async(
                            *entry,
                            [this, sha1 = entry->sha1](bool ok, const std::filesystem::path& path) {
                                if (ok) {
                                    queue_assign_cover(sha1, path);
                                }
                            });
                    }
                }
            } else {
                export_status_message_ = "Error al exportar: " + last_package_result_.error_message;
            }
        }
        ImGui::PopStyleColor(3);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 2. Package status & full path display
        if (last_package_result_.success) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 alert_p0 = ImGui::GetCursorScreenPos();
            const float alert_w = ImGui::GetContentRegionAvail().x;
            const float alert_h = 38.0f;
            ImVec2 alert_p1 = ImVec2(alert_p0.x + alert_w, alert_p0.y + alert_h);
            dl->AddRectFilled(alert_p0, alert_p1, IM_COL32(18, 48, 34, 255), 6.0f);
            dl->AddRect(alert_p0, alert_p1, IM_COL32(36, 160, 96, 255), 6.0f);

            const char* success_txt = "✔  ¡JUEGO EXPORTADO EXITOSAMENTE!";
            ImVec2 txt_sz = ImGui::CalcTextSize(success_txt);
            float text_x = alert_p0.x + (alert_w - txt_sz.x) * 0.5f;
            float text_y = alert_p0.y + (alert_h - txt_sz.y) * 0.5f;

            ImGui::SetCursorScreenPos(ImVec2(text_x, text_y));
            ImGui::TextColored(ThemeColors::SuccessGreen, "%s", success_txt);

            ImGui::SetCursorScreenPos(ImVec2(alert_p0.x, alert_p1.y + 12.0f));

            ImGui::TextColored(ThemeColors::TextMuted,
                               "Ubicacion completa del paquete independiente:");

            // Framed wrap box for the full path
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.10f, 0.14f, 1.0f));
            ImGui::BeginChild("PathDisplayBox", ImVec2(0.0f, 0.0f),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                              ImGuiWindowFlags_NoScrollbar);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ThemeColors::AccentCyan, "%s",
                               last_package_result_.package_path.string().c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::Spacing();
            if (ImGui::Button("Copiar Ruta", ImVec2(160.0f, 38.0f))) {
                SDL_SetClipboardText(last_package_result_.package_path.string().c_str());
            }
            ImGui::SameLine(0, 14.0f);
            if (ImGui::Button("Abrir Carpeta", ImVec2(160.0f, 38.0f))) {
                open_folder_in_file_manager(last_package_result_.package_path);
            }
            ImGui::SameLine(0, 14.0f);
            if (ImGui::Button("Ver en Biblioteca", ImVec2(180.0f, 38.0f))) {
                show_package_modal_ = false;
                current_view_ = StudioView::Library;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(0, 16.0f);
            ImGui::TextColored(
                ThemeColors::TextMuted, "Tamano: %.2f MiB",
                static_cast<double>(last_package_result_.total_bytes) / (1024.0 * 1024.0));
        } else if (!last_package_result_.error_message.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.30f, 1.0f), "FALLO AL EXPORTAR");
            ImGui::Spacing();
            ImGui::TextWrapped("%s", last_package_result_.error_message.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Cerrar", ImVec2(120.0f, 38.0f))) {
            show_package_modal_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void StudioApp::render_manual_import_modal() {
    if (!show_manual_import_modal_) {
        return;
    }

    ImGui::OpenPopup("ImportarRecompiladoManualModal");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(840.0f, 600.0f));

    if (ImGui::BeginPopupModal(
            "ImportarRecompiladoManualModal", &show_manual_import_modal_,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + ImGui::GetContentRegionAvail().x, p0.y + 42.0f);
        draw_list->AddRectFilled(p0, p1, ImGui::GetColorU32(ThemeColors::SurfaceAlt), 6.0f);
        draw_list->AddRect(p0, p1, ImGui::GetColorU32(ThemeColors::Border), 6.0f);

        ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.0f, p0.y + 8.0f));
        ImGui::PushFont(fonts_.h2);
        ImGui::TextColored(ThemeColors::AccentCyan, "IMPORTAR JUEGO RECOMPILADO — DATOS MANUALES");
        ImGui::PopFont();

        ImGui::SetCursorScreenPos(ImVec2(p1.x - 32.0f, p0.y + 6.0f));
        if (ImGui::Button("✕##CloseManualImport", ImVec2(24.0f, 24.0f))) {
            show_manual_import_modal_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + 12.0f));

        // Required Alert Banner
        ImVec2 alert_p0 = ImGui::GetCursorScreenPos();
        const float alert_w = ImGui::GetContentRegionAvail().x;
        const float alert_h = 46.0f;
        ImVec2 alert_p1 = ImVec2(alert_p0.x + alert_w, alert_p0.y + alert_h);
        draw_list->AddRectFilled(alert_p0, alert_p1, IM_COL32(58, 38, 12, 255), 6.0f);
        draw_list->AddRect(alert_p0, alert_p1, IM_COL32(220, 150, 35, 255), 6.0f);

        const char* alert_msg =
            "⚠  No se encontro ningún rom en la carpeta correspondiente, llena los datos del juego "
            "manualmente";
        ImVec2 alert_sz = ImGui::CalcTextSize(alert_msg);
        ImGui::SetCursorScreenPos(
            ImVec2(alert_p0.x + std::max(12.0f, (alert_w - alert_sz.x) * 0.5f),
                   alert_p0.y + (alert_h - alert_sz.y) * 0.5f));
        ImGui::TextColored(ThemeColors::WarningAmber, "%s", alert_msg);

        ImGui::SetCursorScreenPos(ImVec2(alert_p0.x, alert_p1.y + 14.0f));

        ImGui::TextColored(ThemeColors::TextMuted, "Carpeta del ejecutable seleccionada:");
        ImGui::SameLine();
        ImGui::TextColored(ThemeColors::AccentCyan, "%s",
                           pending_inspection_.package_dir.string().c_str());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 1. Title
        ImGui::TextColored(ThemeColors::TextPrimary, "Titulo del juego:");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##ManualTitle", manual_import_title_buf_,
                         sizeof(manual_import_title_buf_));

        ImGui::Spacing();

        // 2. Platform
        const char* kPlatforms[] = {
            "Nintendo NES (nes)",       "Super Nintendo (snes)",    "Game Boy (gb)",
            "Game Boy Color (gbc)",     "Game Boy Advance (gba)",   "Sega Master System (sms)",
            "Sega Game Gear (gg)",
        };
        const char* kPlatformKeys[] = {"nes", "snes", "gb", "gbc", "gba", "sms", "gg"};
        manual_import_platform_idx_ = std::clamp(manual_import_platform_idx_, 0, 6);

        ImGui::TextColored(ThemeColors::TextPrimary, "Consola / Plataforma:");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("##ManualPlatform", &manual_import_platform_idx_, kPlatforms,
                     IM_ARRAYSIZE(kPlatforms));

        ImGui::Spacing();

        // 3. Detected executables (Linux / Windows)
        const float half_w = (ImGui::GetContentRegionAvail().x - 16.0f) * 0.5f;
        ImGui::BeginGroup();
        ImGui::TextColored(ThemeColors::TextPrimary, "Ejecutable Linux:");
        ImGui::SetNextItemWidth(half_w);
        ImGui::InputText("##ManualExecLinux", manual_import_exec_linux_buf_,
                         sizeof(manual_import_exec_linux_buf_));
        ImGui::EndGroup();

        ImGui::SameLine(0, 16.0f);

        ImGui::BeginGroup();
        ImGui::TextColored(ThemeColors::TextPrimary, "Ejecutable Windows (.exe):");
        ImGui::SetNextItemWidth(half_w);
        ImGui::InputText("##ManualExecWin", manual_import_exec_win_buf_,
                         sizeof(manual_import_exec_win_buf_));
        ImGui::EndGroup();

        ImGui::Spacing();

        // 4. Optional ROM file linking / copying
        ImGui::TextColored(ThemeColors::TextPrimary,
                           "Archivo ROM original (Opcional — para vincular o copiar a 'roms/'):");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 160.0f);
        ImGui::InputText("##ManualRomPath", manual_import_rom_buf_, sizeof(manual_import_rom_buf_));
        ImGui::SameLine(0, 10.0f);
        if (ImGui::Button("Examinar ROM...", ImVec2(150.0f, 0))) {
            open_manual_import_rom_dialog();
        }
        ImGui::Checkbox("Si se selecciona un ROM, copiarlo a la carpeta 'roms/' del paquete",
                        &manual_import_copy_rom_);

        if (!manual_import_error_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ThemeColors::DangerRed, "✖ %s", manual_import_error_.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.65f, 0.45f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.08f, 0.78f, 0.55f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.88f, 0.62f, 1.0f));
        if (ImGui::Button("✔  Guardar e Importar", ImVec2(240.0f, 42.0f))) {
            if (manual_import_title_buf_[0] == '\0') {
                manual_import_error_ = "Por favor ingresa el titulo del juego.";
            } else {
                std::filesystem::path final_rom_path;
                std::error_code ec;
                if (manual_import_rom_buf_[0] != '\0') {
                    std::filesystem::path sel_rom(manual_import_rom_buf_);
                    if (std::filesystem::exists(sel_rom, ec) &&
                        std::filesystem::is_regular_file(sel_rom, ec)) {
                        final_rom_path = sel_rom;
                        if (manual_import_copy_rom_ && !pending_inspection_.package_dir.empty()) {
                            const auto dest_roms = pending_inspection_.package_dir / "roms";
                            std::filesystem::create_directories(dest_roms, ec);
                            const auto dest_file = dest_roms / sel_rom.filename();
                            std::filesystem::copy_file(
                                sel_rom, dest_file,
                                std::filesystem::copy_options::overwrite_existing, ec);
                            if (!ec) {
                                final_rom_path = dest_file;
                            }
                        }
                    }
                }

                const std::string plat_key = kPlatformKeys[manual_import_platform_idx_];
                GameEntry* entry = library_.add_manual_portable(
                    manual_import_title_buf_, plat_key, pending_inspection_.package_dir,
                    manual_import_exec_linux_buf_, manual_import_exec_win_buf_, final_rom_path,
                    pending_inspection_.hinted_sha1);
                if (entry) {
                    library_.save();
                    library_console_tab_ = console_tab_for_platform(entry->platform);
                    library_notice_ = "✔ Juego importado en la biblioteca: " + entry->title;
                    if (entry->cover_path.empty()) {
                        cover_mgr_.download_cover_async(
                            *entry,
                            [this, sha1 = entry->sha1](bool ok, const std::filesystem::path& path) {
                                if (ok) {
                                    queue_assign_cover(sha1, path);
                                }
                            });
                    }
                    show_manual_import_modal_ = false;
                    ImGui::CloseCurrentPopup();
                } else {
                    manual_import_error_ = "No se pudo registrar el juego. Verifica el titulo.";
                }
            }
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, 16.0f);
        if (ImGui::Button("Cancelar", ImVec2(140.0f, 42.0f))) {
            show_manual_import_modal_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void StudioApp::render_ui() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Begin("DragonRecompStudioMain", nullptr, host_flags);
    ImGui::PopStyleVar(4);

    track_library_state();

    // Left 56px navigation rail
    render_sidebar();

    ImGui::SameLine(0.0f, 0.0f);

    // Right main column (Topbar + Active View)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("MainColumn", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    render_topbar();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
    ImGui::BeginChild("ViewContent", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    switch (current_view_) {
        case StudioView::Library:
            render_library();
            break;
        case StudioView::Recompiler:
            render_recompiler();
            break;
        case StudioView::Preferences:
            render_preferences();
            break;
    }

    ImGui::EndChild();
    ImGui::EndChild();

    render_package_modal();
    render_manual_import_modal();

    ImGui::End();
}

}  // namespace dragon::studio
