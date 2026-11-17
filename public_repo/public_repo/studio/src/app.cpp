#include "studio/app.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

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

    window_ = SDL_CreateWindow("DragonRecomp Studio — Recompilador Estatico Autonomo", width,
                               height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window_) {
        std::fprintf(stderr, "error: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

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
        show_package_modal_ = false;
        export_status_message_.clear();
        last_package_result_ = PackageResult{};
        diagnostic_status_message_.clear();
        pipeline_.clear_last_game_error();
        pipeline_.start_pipeline(info);
    }
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

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);

        if (event.type == SDL_EVENT_QUIT) {
            running_ = false;
        } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                   event.window.windowID == SDL_GetWindowID(window_)) {
            running_ = false;
        } else if (event.type == SDL_EVENT_DROP_FILE) {
            if (event.drop.data) {
                load_rom(event.drop.data);
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
    const float header_h = 76.0f;
    ImVec2 p1 = ImVec2(p0.x + ImGui::GetContentRegionAvail().x, p0.y + header_h);

    // Subtle vertical gradient surface with a thin accent underline.
    const ImU32 top = ImGui::GetColorU32(ImVec4(0.110f, 0.137f, 0.173f, 1.0f));
    const ImU32 bottom = ImGui::GetColorU32(ThemeColors::Surface);
    draw_list->AddRectFilledMultiColor(p0, p1, top, top, bottom, bottom);
    draw_list->AddRect(p0, p1, ImGui::GetColorU32(ThemeColors::Border), 10.0f, 0, 1.0f);
    draw_list->AddRectFilled(ImVec2(p0.x, p1.y - 2.0f), p1,
                             ImGui::GetColorU32(ThemeColors::AccentCyan), 0.0f);

    // Logo & title, crisp H1.
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 20.0f, p0.y + (header_h - 32.0f) * 0.5f));
    ImGui::PushFont(fonts_.h1);
    ImGui::TextColored(ThemeColors::AccentCyan, "DRAGONRECOMP");
    ImGui::SameLine(0, 10.0f);
    ImGui::TextColored(ThemeColors::TextPrimary, "STUDIO");
    ImGui::PopFont();

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

    render_package_modal();

    ImGui::End();
}

}  // namespace dragon::studio
