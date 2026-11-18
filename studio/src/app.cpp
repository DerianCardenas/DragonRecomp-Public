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

    float display_scale = SDL_GetWindowDisplayScale(window_);
    if (display_scale < 1.0f) {
        display_scale = 1.0f;
    }

    fonts_ = load_studio_fonts(io, display_scale);
    apply_modern_dark_theme(display_scale);

    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);

    const auto default_export = std::filesystem::current_path() / "ExportedGames";
    std::strncpy(export_dir_buf_, default_export.string().c_str(), sizeof(export_dir_buf_) - 1);
    export_dir_buf_[sizeof(export_dir_buf_) - 1] = '\0';

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
        {"ROMs de NES, SNES, GB/GBC y GBA (*.nes, *.sfc, *.smc, *.gb, *.gbc, *.gba)",
         "nes;sfc;smc;gb;gbc;gba"},
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
        default:
            if (library_console_tab_ == 1)
                manual_import_platform_idx_ = 1;
            else if (library_console_tab_ == 2)
                manual_import_platform_idx_ = 2;
            else if (library_console_tab_ == 3)
                manual_import_platform_idx_ = 4;
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
    if (entry.settings.crt_filter) {
        arg_strings.push_back("--crt");
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
    if (entry.settings.crt_filter) {
        cmd += " --crt";
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
        {"ROMs de NES, SNES, GB/GBC y GBA (*.nes, *.sfc, *.smc, *.gb, *.gbc, *.gba)",
         "nes;sfc;smc;gb;gbc;gba"},
        {"ROMs de Game Boy Advance (*.gba)", "gba"},
        {"ROMs de Game Boy / Color (*.gb, *.gbc)", "gb;gbc"},
        {"ROMs de NES (*.nes)", "nes"},
        {"ROMs de SNES (*.sfc, *.smc)", "sfc;smc"},
        {"Todos los archivos", "*"}};
    SDL_ShowOpenFileDialog(&on_rom_file_picked, this, window_, kRomFilters, 6, nullptr, false);
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
        SDL_SetRenderDrawColor(renderer_, 14, 17, 22, 255);  // ThemeColors::BgApp
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_RenderPresent(renderer_);
    }
}

void StudioApp::render_header() {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float header_h = 82.0f;
    ImVec2 p1 = ImVec2(p0.x + ImGui::GetContentRegionAvail().x, p0.y + header_h);

    // Subtle vertical gradient surface with a thin accent underline.
    const ImU32 top = ImGui::GetColorU32(ImVec4(0.110f, 0.137f, 0.173f, 1.0f));
    const ImU32 bottom = ImGui::GetColorU32(ThemeColors::Surface);
    draw_list->AddRectFilledMultiColor(p0, p1, top, top, bottom, bottom);
    draw_list->AddRect(p0, p1, ImGui::GetColorU32(ThemeColors::Border), 10.0f, 0, 1.0f);
    draw_list->AddRectFilled(ImVec2(p0.x, p1.y - 2.0f), p1,
                             ImGui::GetColorU32(ThemeColors::AccentCyan), 0.0f);

    // Logo & title, crisp H1.
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 24.0f, p0.y + (header_h - 32.0f) * 0.5f));
    ImGui::PushFont(fonts_.h1);
    ImGui::TextColored(ThemeColors::AccentCyan, "DRAGONRECOMP");
    ImGui::SameLine(0, 10.0f);
    ImGui::TextColored(ThemeColors::TextPrimary, "STUDIO");
    const float logo_end_x = ImGui::GetItemRectMax().x;
    ImGui::PopFont();

    // Navigation Tabs in Header: bigger size, plenty of space after title
    const float tabs_x = logo_end_x + 50.0f;
    const float tab_h = 42.0f;
    const float tab_w_lib = 180.0f;
    const float tab_w_rec = 220.0f;

    ImGui::SetCursorScreenPos(ImVec2(tabs_x, p0.y + (header_h - tab_h) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushFont(fonts_.body_bold);

    const bool is_lib = (current_view_ == StudioView::Library);
    if (is_lib) {
        ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::AccentCyan);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06f, 0.08f, 0.10f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::SurfaceAlt);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextMuted);
    }
    if (ImGui::Button("BIBLIOTECA", ImVec2(tab_w_lib, tab_h))) {
        current_view_ = StudioView::Library;
    }
    ImGui::PopStyleColor(2);

    ImGui::SameLine(0, 12.0f);

    const bool is_rec = (current_view_ == StudioView::Recompiler);
    if (is_rec) {
        ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::AccentCyan);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06f, 0.08f, 0.10f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::SurfaceAlt);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextMuted);
    }
    std::string rec_tab = "RECOMPILADOR";
    if (active_rom_.is_valid()) {
        rec_tab = "RECOMP: " + active_rom_.clean_name;
        if (rec_tab.size() > 22) {
            rec_tab = rec_tab.substr(0, 19) + "...";
        }
    }
    if (ImGui::Button(rec_tab.c_str(), ImVec2(tab_w_rec, tab_h))) {
        current_view_ = StudioView::Recompiler;
    }
    ImGui::PopStyleColor(2);
    ImGui::PopFont();
    ImGui::PopStyleVar();

    // System-status pill, right-aligned.
    ImGui::PushFont(fonts_.small);
    const char* nes_lbl = "NES";
    const char* snes_lbl = "SNES";
    const char* gb_lbl = "GB/GBC";
    const char* gba_lbl = "GBA";
    const float dot_w = ImGui::CalcTextSize("●").x;
    const float gap = 6.0f;
    const float seg = 14.0f;
    const float pill_w = 16.0f + dot_w + gap + ImGui::CalcTextSize(nes_lbl).x + seg + dot_w + gap +
                         ImGui::CalcTextSize(snes_lbl).x + seg + dot_w + gap +
                         ImGui::CalcTextSize(gb_lbl).x + seg + dot_w + gap +
                         ImGui::CalcTextSize(gba_lbl).x + 16.0f;
    const float pill_h = 32.0f;
    ImVec2 pill0 = ImVec2(p1.x - pill_w - 16.0f, p0.y + (header_h - pill_h) * 0.5f);
    ImVec2 pill1 = ImVec2(pill0.x + pill_w, pill0.y + pill_h);
    draw_list->AddRectFilled(pill0, pill1, ImGui::GetColorU32(ThemeColors::SurfaceAlt),
                             pill_h * 0.5f);
    draw_list->AddRect(pill0, pill1, ImGui::GetColorU32(ThemeColors::Border), pill_h * 0.5f);

    const float btn_hdr_w = 150.0f;
    ImGui::SetCursorScreenPos(ImVec2(pill0.x - btn_hdr_w - 14.0f, pill0.y));
    if (ImGui::Button("Abrir ROM...", ImVec2(btn_hdr_w, pill_h))) {
        open_rom_file_dialog();
    }

    ImGui::SetCursorScreenPos(ImVec2(pill0.x + 16.0f, pill0.y + 6.0f));
    ImGui::TextColored(ThemeColors::SuccessGreen, "●");
    ImGui::SameLine(0, gap);
    ImGui::TextColored(ThemeColors::TextPrimary, "%s", nes_lbl);
    ImGui::SameLine(0, seg);
    ImGui::TextColored(ThemeColors::SuccessGreen, "●");
    ImGui::SameLine(0, gap);
    ImGui::TextColored(ThemeColors::TextPrimary, "%s", snes_lbl);
    ImGui::SameLine(0, seg);
    ImGui::TextColored(ThemeColors::SuccessGreen, "●");
    ImGui::SameLine(0, gap);
    ImGui::TextColored(ThemeColors::TextPrimary, "%s", gb_lbl);
    ImGui::SameLine(0, seg);
    ImGui::TextColored(ThemeColors::SuccessGreen, "●");
    ImGui::SameLine(0, gap);
    ImGui::TextColored(ThemeColors::TextPrimary, "%s", gba_lbl);
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + 16.0f));
}

void StudioApp::render_drop_zone() {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float zone_height = avail.y - 120.0f;

    ImGui::BeginChild("DropZone", ImVec2(0, zone_height), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 min_p = ImGui::GetWindowPos();
    const ImVec2 max_p =
        ImVec2(min_p.x + ImGui::GetWindowWidth(), min_p.y + ImGui::GetWindowHeight());

    // Stylized dashed drag & drop area
    draw_list->AddRect(ImVec2(min_p.x + 20, min_p.y + 20), ImVec2(max_p.x - 20, max_p.y - 20),
                       IM_COL32(0, 210, 211, 140), 12.0f, 0, 2.0f);

    const float center_y = min_p.y + (zone_height * 0.30f);
    ImGui::SetCursorScreenPos(ImVec2(min_p.x + 40, center_y));

    ImGui::PushFont(fonts_.display);
    const char* drop_text = "ARRASTRA AQUI TU ROM (.nes, .sfc, .gb, .gbc, .gba)";
    float text_width = ImGui::CalcTextSize(drop_text).x;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - text_width) * 0.5f);
    ImGui::TextColored(ThemeColors::AccentCyan, "%s", drop_text);
    ImGui::PopFont();

    ImGui::Spacing();
    const char* sub_text =
        "Deteccion instantanea de plataforma, desensamblado recursivo y compilacion nativa C99";
    float sub_width = ImGui::CalcTextSize(sub_text).x;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - sub_width) * 0.5f);
    ImGui::TextColored(ThemeColors::TextMuted, "%s", sub_text);

    ImGui::Spacing();
    ImGui::Spacing();

    // Prominent file selector button
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.46f, 0.76f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.56f, 0.88f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.22f, 0.66f, 0.98f, 1.0f));

    const char* open_btn_text = "SELECCIONAR ARCHIVO ROM (.nes, .sfc, .gb, .gbc, .gba)...";
    const float open_btn_w = std::min(580.0f, ImGui::GetWindowWidth() - 60.0f);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - open_btn_w) * 0.5f);
    if (ImGui::Button(open_btn_text, ImVec2(open_btn_w, 52.0f))) {
        open_rom_file_dialog();
    }
    ImGui::PopStyleColor(3);

    ImGui::Spacing();
    const char* or_text = "— o arrastra y suelta el archivo directamente en este recuadro —";
    float or_width = ImGui::CalcTextSize(or_text).x;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - or_width) * 0.5f);
    ImGui::TextColored(ThemeColors::TextMuted, "%s", or_text);

    ImGui::Spacing();
    ImGui::Spacing();

    // Fast-sample load buttons for local ROMs
    const char* sample_hdr = "O carga un juego de prueba de la biblioteca local:";
    float sample_width = ImGui::CalcTextSize(sample_hdr).x;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - sample_width) * 0.5f);
    ImGui::TextColored(ThemeColors::TextPrimary, "%s", sample_hdr);

    ImGui::Spacing();

    const float btn_w = 210.0f;
    const float total_btn_w = (btn_w * 4.0f) + 45.0f;
    ImGui::SetCursorPosX(std::max(10.0f, (ImGui::GetWindowWidth() - total_btn_w) * 0.5f));

    if (ImGui::Button("▶ Super Mario (NES)", ImVec2(btn_w, 46.0f))) {
        load_rom("roms/nes/smb.nes");
    }
    ImGui::SameLine(0, 15.0f);
    if (ImGui::Button("▶ Super Mario (SNES)", ImVec2(btn_w, 46.0f))) {
        load_rom("roms/snes/smb_snes.sfc");
    }
    ImGui::SameLine(0, 15.0f);
    if (ImGui::Button("▶ Tetris (GB)", ImVec2(btn_w, 46.0f))) {
        load_rom("roms/gb/tetris.gb");
    }
    ImGui::SameLine(0, 15.0f);
    if (ImGui::Button("▶ FireRed (GBA)", ImVec2(btn_w, 46.0f))) {
        load_rom("roms/gba/firered.gba");
    }

    ImGui::EndChild();
}

void StudioApp::render_rom_card() {
    const float card_h = 158.0f;
    ImGui::BeginChild("RomCard", ImVec2(0, card_h), true);

    const float total_w = ImGui::GetContentRegionAvail().x;
    ImGui::Columns(4, "RomCols", false);
    ImGui::SetColumnWidth(0, std::max(280.0f, total_w * 0.35f));
    ImGui::SetColumnWidth(1, std::max(180.0f, total_w * 0.22f));
    ImGui::SetColumnWidth(2, std::max(180.0f, total_w * 0.20f));

    // Col 1: Title and Platform
    const GameEntry* entry = library_.find(active_rom_.sha1);
    const std::filesystem::path cover_path = entry ? entry->cover_path : std::filesystem::path{};
    SDL_Texture* cover_tex = cover_mgr_.get_texture(renderer_, active_rom_.sha1, cover_path);
    if (cover_tex) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddImageRounded((ImTextureID)(intptr_t)cover_tex, p, ImVec2(p.x + 64.f, p.y + 86.f),
                            ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 4.f);
        ImGui::Dummy(ImVec2(68.f, 86.f));
        ImGui::SameLine();
        ImGui::BeginGroup();
    }

    ImGui::PushFont(fonts_.small);
    ImGui::TextColored(ThemeColors::TextMuted, "TITULO DE LA ROM");
    ImGui::PopFont();
    ImGui::PushFont(fonts_.h2);
    ImGui::TextColored(ThemeColors::TextPrimary, "%s", active_rom_.clean_name.c_str());
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::TextColored(ThemeColors::TextMuted, "PLATAFORMA:");
    ImGui::SameLine();
    ImGui::TextColored(ThemeColors::AccentCyan, "%s", active_rom_.platform_name().c_str());

    if (cover_tex) {
        ImGui::EndGroup();
    }

    ImGui::NextColumn();

    // Col 2: Mapper / Memory Map
    ImGui::PushFont(fonts_.small);
    ImGui::TextColored(ThemeColors::TextMuted, "ARQUITECTURA");
    ImGui::PopFont();
    if (active_rom_.platform == RomPlatform::NES) {
        ImGui::Text("Mapper %d (iNES)", active_rom_.mapper);
        ImGui::TextColored(ThemeColors::TextMuted, "PRG: %zu KB | CHR: %zu KB",
                           active_rom_.prg_size_kb, active_rom_.chr_size_kb);
    } else if (active_rom_.platform == RomPlatform::SNES) {
        ImGui::Text("SNES %s", active_rom_.map_mode_str.c_str());
        ImGui::TextColored(ThemeColors::TextMuted, "Checksum: $%04X %s", active_rom_.checksum,
                           active_rom_.fast_rom ? "(FastROM)" : "(SlowROM)");
    } else if (active_rom_.platform == RomPlatform::GameBoyAdvance) {
        ImGui::Text("GBA (%s)",
                    active_rom_.game_code.empty() ? "AGB" : active_rom_.game_code.c_str());
        ImGui::TextColored(ThemeColors::TextMuted, "ROM: %zu KB | Save: %s",
                           active_rom_.rom_size_kb, active_rom_.save_type_str.c_str());
    } else {
        ImGui::Text("%s", active_rom_.mbc_type_str.c_str());
        ImGui::TextColored(ThemeColors::TextMuted, "ROM: %zu KB | RAM: %zu KB%s",
                           active_rom_.rom_size_kb, active_rom_.ram_size_kb,
                           active_rom_.battery ? " (Bat)" : "");
    }

    ImGui::NextColumn();

    // Col 3: SHA-1 & Size
    ImGui::PushFont(fonts_.small);
    ImGui::TextColored(ThemeColors::TextMuted, "HASH SHA-1");
    ImGui::PopFont();
    ImGui::Text("%.12s...", active_rom_.sha1.c_str());
    ImGui::TextColored(ThemeColors::TextMuted, "Tamano: %zu KiB", active_rom_.file_size / 1024);

    ImGui::NextColumn();

    // Col 4: Action buttons
    ImGui::Dummy(ImVec2(0, 4.0f));
    if (ImGui::Button("📚 Ver Biblioteca", ImVec2(-1, 38.0f))) {
        current_view_ = StudioView::Library;
    }
    ImGui::Spacing();
    if (ImGui::Button("Abrir otra ROM", ImVec2(-1, 38.0f))) {
        open_rom_file_dialog();
    }
    ImGui::Spacing();
    if (ImGui::Button("↻ Limpiar / Volver", ImVec2(-1, 38.0f))) {
        pipeline_.cancel();
        active_rom_ = RomInfo{};
        export_status_message_.clear();
        last_package_result_ = PackageResult{};
    }

    ImGui::Columns(1);
    ImGui::EndChild();
}

void StudioApp::render_pipeline_progress() {
    ImGui::BeginChild("ProgressView", ImVec2(0, 200.0f), true);

    ImGui::Spacing();
    ImGui::PushFont(fonts_.h2);
    ImGui::TextColored(ThemeColors::AccentCyan, "PROCESO DE RECOMPILACION AUTONOMA EN CURSO");
    ImGui::PopFont();
    ImGui::Spacing();

    const float p = pipeline_.progress();
    ImGui::ProgressBar(p, ImVec2(-1, 26.0f));

    ImGui::Spacing();
    ImGui::TextColored(ThemeColors::TextPrimary, "%s",
                       pipeline_.current_step_description().c_str());

    ImGui::Spacing();
    ImGui::Spacing();
    if (ImGui::Button("✖ Cancelar Recompilacion", ImVec2(240.0f, 38.0f))) {
        pipeline_.cancel();
        active_rom_ = RomInfo{};
    }

    ImGui::EndChild();
}

void StudioApp::render_ready_hub() {
    const auto& stats = pipeline_.stats();

    // Stat tiles: four raised surfaces with a caption label and a crisp big value.
    char fn_val[32];
    char ins_val[32];
    char time_val[32];
    std::snprintf(fn_val, sizeof(fn_val), "%zu", stats.functions);
    std::snprintf(ins_val, sizeof(ins_val), "%zu", stats.instructions);
    std::snprintf(time_val, sizeof(time_val), "%.2f s", stats.compilation_time_sec);
    // interpreter_only: the game runs on the cycle-accurate interpreter (needed
    // for titles whose interrupt timing the recompiler cannot match).
    const bool interpreter_only = (stats.functions == 0);

    const char* labels[4] = {"FUNCIONES C99", "INSTRUCCIONES NATIVAS", "TIEMPO DE COMPILACION",
                             "ESTADO"};
    const char* values[4] = {fn_val, ins_val, time_val,
                             interpreter_only ? "Modo Interprete" : "100% Nativo"};
    const ImVec4 value_colors[4] = {
        ThemeColors::AccentCyan, ThemeColors::AccentCyan, ThemeColors::TextPrimary,
        interpreter_only ? ThemeColors::AccentCyan : ThemeColors::SuccessGreen};

    const float tile_gap = 12.0f;
    const float tile_w = (ImGui::GetContentRegionAvail().x - tile_gap * 3.0f) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            ImGui::SameLine(0, tile_gap);
        }
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::SurfaceAlt);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
        ImGui::BeginChild(labels[i], ImVec2(tile_w, 102.0f), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::PushFont(fonts_.small);
        ImGui::TextColored(ThemeColors::TextMuted, "%s", labels[i]);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 6.0f));
        ImFont* val_font = (std::strlen(values[i]) > 10) ? fonts_.h2 : fonts_.h1;
        ImGui::PushFont(val_font);
        ImGui::TextColored(value_colors[i], "%s", values[i]);
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    const bool is_running = pipeline_.is_game_running();
    const float gap = 16.0f;
    const float col_w = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;

    // =========================================================================
    // LEFT CARD: [ BOTONES DE JUGAR, FILTRO, CONTROLES, ETC ]
    // =========================================================================
    ImGui::BeginChild("ActionCard", ImVec2(col_w, 0.0f), true);

    ImGui::PushFont(fonts_.h2);
    ImGui::TextColored(ThemeColors::AccentCyan, "EJECUCION Y CONTROLES");
    ImGui::PopFont();
    ImGui::Spacing();

    const float btn_width = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;

    // Green Play Button
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.65f, 0.45f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.08f, 0.78f, 0.55f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.88f, 0.62f, 1.0f));

    if (is_running) {
        ImGui::Button("● JUEGO EN EJECUCION...", ImVec2(btn_width, 46.0f));
    } else {
        if (ImGui::Button("▶  JUGAR AHORA (60 FPS)", ImVec2(btn_width, 46.0f))) {
            pipeline_.launch_game(fullscreen_option_, crt_filter_option_);
        }
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine(0, 12.0f);

    // Blue Package Export Button
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.45f, 0.75f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.55f, 0.88f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.65f, 0.98f, 1.0f));

    if (ImGui::Button("EXPORTAR PORTABLE", ImVec2(btn_width, 46.0f))) {
        if (export_dir_buf_[0] == '\0') {
            std::string def_dir = (std::filesystem::current_path() / "ExportedGames").string();
            std::strncpy(export_dir_buf_, def_dir.c_str(), sizeof(export_dir_buf_) - 1);
        }
        show_package_modal_ = true;
    }
    ImGui::PopStyleColor(3);

    // Quick toggles below buttons
    ImGui::Spacing();
    ImGui::Dummy(ImVec2(0, 2.0f));
    const float check_space = ImGui::GetContentRegionAvail().x;
    if (check_space < 460.0f) {
        ImGui::Checkbox("Pantalla Completa (--fullscreen)", &fullscreen_option_);
        ImGui::Checkbox("Filtro CRT Retro Scanlines (--crt)", &crt_filter_option_);
    } else {
        ImGui::Checkbox("Pantalla Completa (--fullscreen)", &fullscreen_option_);
        ImGui::SameLine(0, 16.0f);
        ImGui::Checkbox("Filtro CRT Retro Scanlines (--crt)", &crt_filter_option_);
    }

    // Export status banner on Left Card
    if (!export_status_message_.empty()) {
        ImGui::Spacing();
        if (last_package_result_.success) {
            ImGui::TextColored(ThemeColors::SuccessGreen, "✔ %s", export_status_message_.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Abrir Carpeta")) {
                open_folder_in_file_manager(last_package_result_.package_path);
            }
        } else {
            ImGui::TextColored(ThemeColors::DangerRed, "✖ %s", export_status_message_.c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("✕##DismissExport")) {
            export_status_message_.clear();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Controles del juego y atajos de teclado
    ImGui::PushFont(fonts_.body_bold);
    ImGui::TextColored(ThemeColors::AccentCyan, "CONTROLES DEL JUEGO (TECLADO)");
    ImGui::PopFont();
    ImGui::SameLine(0, 10.0f);
    ImGui::PushFont(fonts_.small);
    ImGui::TextColored(ThemeColors::TextMuted, "[ %s ]", active_rom_.platform_name().c_str());
    ImGui::PopFont();

    ImGui::Spacing();

    const float inner_gap = 10.0f;
    const float inner_w = (ImGui::GetContentRegionAvail().x - inner_gap) * 0.5f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::SurfaceAlt);
    ImGui::BeginChild("GameControlsBox", ImVec2(inner_w, 185.0f), true,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(fonts_.small);
    ImGui::TextColored(ThemeColors::AccentBlue, "MANDO / BOTONES");
    ImGui::PopFont();
    ImGui::Spacing();

    if (ImGui::BeginTable("##GameControlsTbl", 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 145.0f);
        ImGui::TableSetupColumn("Val", ImGuiTableColumnFlags_WidthStretch);

        auto render_row = [](const char* key, const char* val,
                             const ImVec4& val_col = ThemeColors::TextPrimary) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ThemeColors::TextMuted, "%s", key);
            ImGui::TableNextColumn();
            ImGui::TextColored(val_col, "%s", val);
        };

        if (active_rom_.platform == RomPlatform::NES) {
            render_row("D-Pad (Mover):", "Flechas / WASD");
            render_row("Boton A (Salto):", "X  /  J");
            render_row("Boton B (Accion):", "Z  /  K");
            render_row("Select / Start:", "RShift o Tab / Enter");
        } else if (active_rom_.platform == RomPlatform::SNES) {
            render_row("D-Pad (Mover):", "Flechas / WASD");
            render_row("B (Salto) / Y (Run):", "X o Espacio / Z o J");
            render_row("A (Giro) / X (Top):", "C o L / V o I");
            render_row("L / R / Sel / Start:", "Q / E / RShift / Enter");
        } else if (active_rom_.platform == RomPlatform::GameBoyAdvance) {
            render_row("D-Pad (Mover):", "Flechas / WASD");
            render_row("Boton A / B:", "Z (A) / X (B)");
            render_row("Gatillos L / R:", "Q (L) / E (R)");
            render_row("Select / Start:", "Bksp o RShift / Enter");
        } else {
            // Game Boy / Game Boy Color
            render_row("D-Pad (Mover):", "Flechas / WASD");
            render_row("Boton A:", "Z  /  J");
            render_row("Boton B:", "X  /  K");
            render_row("Select / Start:", "Bksp o RShift / Enter");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine(0, inner_gap);

    ImGui::BeginChild("HotkeysBox", ImVec2(inner_w, 185.0f), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(fonts_.small);
    ImGui::TextColored(ThemeColors::AccentBlue, "ATAJOS EN VIVO (HOTKEYS)");
    ImGui::PopFont();
    ImGui::Spacing();

    if (ImGui::BeginTable("##HotkeysTbl", 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 135.0f);
        ImGui::TableSetupColumn("Val", ImGuiTableColumnFlags_WidthStretch);

        auto render_hk = [](const char* key, const char* val,
                            const ImVec4& val_col = ThemeColors::TextPrimary) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ThemeColors::TextMuted, "%s", key);
            ImGui::TableNextColumn();
            ImGui::TextColored(val_col, "%s", val);
        };

        render_hk("F5 (Quick Save):", "Guardar Slot", ThemeColors::SuccessGreen);
        render_hk("F7 (Quick Load):", "Cargar Slot", ThemeColors::AccentCyan);
        render_hk("Teclas 0 - 9:", "Cambiar Slot (0-9)");
        render_hk("F8 / F9 / F11:", "Aspecto / CRT / Full");
        render_hk("Esc:", "Salir del juego");

        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::EndChild();

    ImGui::SameLine(0, gap);

    // =========================================================================
    // RIGHT CARD: [ DETALLES DEL COMPILADOR (codigo optimizado, recompilar, etc) ]
    // =========================================================================
    ImGui::BeginChild("LearningCard", ImVec2(col_w, 0.0f), true);

    ImGui::PushFont(fonts_.h2);
    ImGui::TextColored(ThemeColors::AccentCyan, "⚡ DETALLES DEL COMPILADOR");
    ImGui::PopFont();
    ImGui::Spacing();
    const size_t pending = pipeline_.pending_learn_count();

    const auto game_err = pipeline_.last_game_error();
    if (!game_err.empty() && !is_running) {
        ImGui::TextColored(ThemeColors::WarningAmber, "! Atencion durante la ejecucion del juego:");
        ImGui::TextWrapped("%s", game_err.c_str());
        ImGui::Spacing();
    }

    if (is_running) {
        ImGui::TextColored(ThemeColors::AccentCyan, "● Monitorizando ejecucion en tiempo real...");
        ImGui::TextColored(
            ThemeColors::TextMuted,
            "Cualquier rutina no compilada que sea invocada durante el juego sera aprendida.");
    } else if (pending > 0) {
        ImGui::TextColored(
            ThemeColors::WarningAmber,
            "★ Se han aprendido %zu nuevas rutinas durante tu ultima sesion de juego.", pending);
        ImGui::Spacing();
        if (ImGui::Button("⚡ Optimizar y Recompilar Todo", ImVec2(-1, 34.0f))) {
            pipeline_.optimize_and_recompile();
        }
        ImGui::TextColored(ThemeColors::TextMuted,
                           "Al recompilar, el 100%% de los nuevos saltos se integraran como "
                           "funciones nativas C99.");
    } else {
        ImGui::TextColored(ThemeColors::SuccessGreen, "✔ Codigo 100%% Optimizado");
        ImGui::TextColored(ThemeColors::TextMuted,
                           "No hay fallbacks pendientes. El juego se ejecuta con maxima velocidad "
                           "nativa.");
    }

    // Diagnostic & Self-Repair Section
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Spec 003 §3.6 (closes ADR-001): this panel does not diagnose hardware —
    // it maps a selected symptom to one of a few known, concrete actions
    // (purge learned routines, switch to interpreter_only, cycle palette,
    // clean recompile) and reports exactly what it did. Several symptoms have
    // no safe automatic action at all; the result text says so plainly
    // instead of implying a repair that did not happen. Keep the labels below
    // matching that: no "diagnostico"/"reparacion automatica" framing.
    ImGui::PushFont(fonts_.h2);
    ImGui::TextColored(ThemeColors::AccentCyan, "⚙ ACCIONES DE RECUPERACION");
    ImGui::PopFont();
    ImGui::TextColored(
        ThemeColors::TextMuted,
        "Selecciona lo que ves: se intentara una accion conocida (purgar rutinas, modo "
        "interprete, cambiar paleta, recompilar limpio). No todos los sintomas tienen una "
        "accion automatica: si no la hay, se te dira.");
    ImGui::Spacing();

    const char* symptoms[] = {"Pantalla gris",
                              "Pantalla azul",
                              "Juego congelado al cargar (Pantalla de inicio)",
                              "Juego congelado al jugar (Durante la partida)",
                              "Juego no abre / Se cierra de golpe",
                              "Sin sonido",
                              "Sonido diferente / Distorsionado",
                              "Colores diferentes / Paleta extrana",
                              "Controles no responden",
                              "Graficos corruptos / Sprites parpadeando o invisibles",
                              "Velocidad anormal (Muy rapido / Muy lento)"};

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float diag_gap = 12.0f;
    const float combo_w = (avail_w - diag_gap) * 0.60f;
    const float btn_w = (avail_w - diag_gap) * 0.40f;

    ImGui::SetNextItemWidth(combo_w);
    ImGui::Combo("##SymptomCombo", &selected_symptom_idx_, symptoms, IM_ARRAYSIZE(symptoms));

    ImGui::SameLine(0, diag_gap);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.55f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.00f, 0.65f, 0.20f, 1.0f));
    if (ImGui::Button("↻  Intentar Accion", ImVec2(btn_w, 32.0f))) {
        DiagnosticResult res =
            pipeline_.diagnose_and_repair(static_cast<DiagnosticSymptom>(selected_symptom_idx_));
        diagnostic_status_success_ = res.success;
        if (res.success) {
            diagnostic_status_message_ = "✔ " + res.summary;
        } else {
            diagnostic_status_message_ = "ℹ " + res.summary;
        }
        if (!res.details.empty()) {
            diagnostic_status_message_ += "\n" + res.details;
        }
    }
    ImGui::PopStyleColor(3);

    if (!diagnostic_status_message_.empty()) {
        ImGui::Spacing();
        const ImVec4 msg_color =
            diagnostic_status_success_ ? ThemeColors::SuccessGreen : ThemeColors::WarningAmber;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(msg_color, "%s", diagnostic_status_message_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine(0, 10.0f);
        if (ImGui::SmallButton("✕##DismissDiag")) {
            diagnostic_status_message_.clear();
        }
    }

    ImGui::EndChild();
}

void StudioApp::render_error_view() {
    ImGui::BeginChild("ErrorCard", ImVec2(0, 240.0f), true);

    ImGui::PushFont(fonts_.h2);
    ImGui::TextColored(ThemeColors::DangerRed, "ERROR EN EL PIPELINE");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", pipeline_.error_message().c_str());

    ImGui::Spacing();
    ImGui::Spacing();
    if (ImGui::Button("↻ Reintentar Recompilacion", ImVec2(280.0f, 42.0f))) {
        pipeline_.start_pipeline(active_rom_);
    }
    ImGui::SameLine(0, 15.0f);
    if (ImGui::Button("Cancelar", ImVec2(130.0f, 42.0f))) {
        active_rom_ = RomInfo{};
    }

    ImGui::EndChild();
}

void StudioApp::render_package_modal() {
    if (!show_package_modal_) {
        return;
    }

    ImGui::OpenPopup("ExportarPaqueteModal");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(840.0f, 560.0f));

    if (ImGui::BeginPopupModal(
            "ExportarPaqueteModal", &show_package_modal_,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        // Custom elegant Header without default OS title bar
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + ImGui::GetContentRegionAvail().x, p0.y + 38.0f);
        draw_list->AddRectFilled(p0, p1, IM_COL32(24, 29, 38, 255), 6.0f);
        draw_list->AddRect(p0, p1, IM_COL32(45, 55, 75, 255), 6.0f);

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
            ImGui::BeginChild("PathDisplayBox", ImVec2(0, 48.0f), true,
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
            if (ImGui::Button("📚 Ver en Biblioteca", ImVec2(180.0f, 38.0f))) {
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
    ImGui::SetNextWindowSize(ImVec2(840.0f, 560.0f));

    if (ImGui::BeginPopupModal(
            "ImportarRecompiladoManualModal", &show_manual_import_modal_,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + ImGui::GetContentRegionAvail().x, p0.y + 38.0f);
        draw_list->AddRectFilled(p0, p1, IM_COL32(24, 29, 38, 255), 6.0f);
        draw_list->AddRect(p0, p1, IM_COL32(45, 55, 75, 255), 6.0f);

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
            "Nintendo NES (nes)",   "Super Nintendo (snes)",  "Game Boy (gb)",
            "Game Boy Color (gbc)", "Game Boy Advance (gba)",
        };
        const char* kPlatformKeys[] = {"nes", "snes", "gb", "gbc", "gba"};
        manual_import_platform_idx_ = std::clamp(manual_import_platform_idx_, 0, 4);

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
    // Fullscreen main window
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("DragonRecompStudioMain", nullptr, host_flags);
    ImGui::PopStyleVar(2);

    render_header();
    track_library_state();

    if (current_view_ == StudioView::Library) {
        render_library();
    } else {
        if (!active_rom_.is_valid()) {
            render_drop_zone();
        } else {
            render_rom_card();
            ImGui::Spacing();

            switch (pipeline_.stage()) {
                case PipelineStage::Idle:
                    // Auto starts pipeline on load
                    break;
                case PipelineStage::Analyzing:
                case PipelineStage::Disassembling:
                case PipelineStage::Emitting:
                case PipelineStage::CompilingNative:
                    render_pipeline_progress();
                    break;
                case PipelineStage::Ready:
                    render_ready_hub();
                    break;
                case PipelineStage::Failed:
                    render_error_view();
                    break;
            }
        }
    }

    render_package_modal();
    render_manual_import_modal();

    ImGui::End();
}

}  // namespace dragon::studio
