// Shelves-by-console library view (layout 3 of plan-studio-library-ui).
//
// Boxart covers are loaded dynamically via CoverManager (libretro-thumbnails or user local files).
// When unavailable, cards fall back to a procedural per-console gradient tinted by the SHA-1.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "dragon/config.h"
#include "imgui.h"
#include "studio/app.h"
#include "studio/cover_manager.h"
#include "studio/theme.h"

namespace dragon::studio {

namespace {

struct PlatformStyle {
    const char* key;
    const char* label;
    ImVec4 base;
};

ImU32 scaled(const ImVec4& c, float k, float a = 1.0f) {
    return ImGui::GetColorU32(ImVec4(std::clamp(c.x * k, 0.f, 1.f), std::clamp(c.y * k, 0.f, 1.f),
                                     std::clamp(c.z * k, 0.f, 1.f), a));
}

ImVec4 platform_color(const std::string& key) {
    if (key == "nes")
        return ImVec4(0.75f, 0.22f, 0.17f, 1.f);
    if (key == "snes")
        return ImVec4(0.56f, 0.27f, 0.68f, 1.f);
    if (key == "gb")
        return ImVec4(0.50f, 0.60f, 0.17f, 1.f);
    if (key == "gbc")
        return ImVec4(0.10f, 0.62f, 0.55f, 1.f);
    if (key == "gba")
        return ImVec4(0.17f, 0.37f, 0.70f, 1.f);
    return ImVec4(0.35f, 0.38f, 0.45f, 1.f);
}

const char* platform_label(const std::string& key) {
    if (key == "nes")
        return "NES";
    if (key == "snes")
        return "SNES";
    if (key == "gb")
        return "GB";
    if (key == "gbc")
        return "GBC";
    if (key == "gba")
        return "GBA";
    return "?";
}

float sha_tint(const std::string& sha1) {
    // 0.80 .. 1.20, stable per game, so neighbouring placeholders differ.
    unsigned v = 0;
    if (sha1.size() >= 2) {
        std::sscanf(sha1.substr(0, 2).c_str(), "%x", &v);
    }
    return 0.80f + 0.40f * (static_cast<float>(v) / 255.0f);
}

std::string format_playtime(int64_t s) {
    if (s <= 0)
        return "Sin jugar";
    const int64_t h = s / 3600;
    const int64_t m = (s % 3600) / 60;
    char buf[48];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%lld h %02lld m", static_cast<long long>(h),
                      static_cast<long long>(m));
    } else {
        std::snprintf(buf, sizeof(buf), "%lld min",
                      static_cast<long long>(std::max<int64_t>(m, 1)));
    }
    return buf;
}

void draw_pill(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg) {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const ImVec2 p1(pos.x + sz.x + 14.f, pos.y + sz.y + 6.f);
    dl->AddRectFilled(pos, p1, bg, 99.f);
    dl->AddText(ImVec2(pos.x + 7.f, pos.y + 3.f), fg, text);
}

void draw_add_slot_card(ImDrawList* dl, ImVec2 p0, ImVec2 img_size, const char* badge_label,
                        ImVec4 console_color, bool hovered) {
    const float lift = hovered ? -5.f : 0.f;
    const ImVec2 img_q0(p0.x, p0.y + lift);
    const ImVec2 img_q1(p0.x + img_size.x, p0.y + img_size.y + lift);

    dl->AddRectFilled(img_q0, img_q1,
                      hovered ? IM_COL32(32, 42, 56, 240) : IM_COL32(20, 25, 34, 200), 10.f);
    dl->AddRect(
        img_q0, img_q1,
        hovered ? ImGui::GetColorU32(ThemeColors::AccentCyan) : scaled(console_color, 0.45f), 10.f,
        0, hovered ? 2.5f : 1.2f);

    const char* plus_symbol = "+";
    const ImVec2 plus_sz = ImGui::CalcTextSize(plus_symbol);
    dl->AddText(ImVec2(img_q0.x + (img_size.x - plus_sz.x) * 0.5f, img_q0.y + img_size.y * 0.36f),
                hovered ? ImGui::GetColorU32(ThemeColors::AccentCyan)
                        : ImGui::GetColorU32(ThemeColors::TextMuted),
                plus_symbol);

    const char* hint_text = badge_label;
    const ImVec2 hint_sz = ImGui::CalcTextSize(hint_text);
    dl->AddText(ImVec2(img_q0.x + (img_size.x - hint_sz.x) * 0.5f, img_q0.y + img_size.y * 0.46f),
                scaled(console_color, 0.95f), hint_text);

    // Title area below matching game cards
    const char* label_below = "+ Importar recompilado";
    const ImVec2 lbl_sz = ImGui::CalcTextSize(label_below);
    dl->AddText(ImVec2(p0.x + (img_size.x - lbl_sz.x) * 0.5f, p0.y + img_size.y + 8.0f + lift),
                hovered ? ImGui::GetColorU32(ThemeColors::AccentCyan)
                        : ImGui::GetColorU32(ThemeColors::TextMuted),
                label_below);
}

// Draws one game card. The cover artwork is clean (no badges or overlays). Title sits BELOW the
// image.
bool draw_game_card(SDL_Renderer* renderer, CoverManager& covers, const StudioFonts& fonts,
                    const GameEntry& g, ImVec2 img_size, float title_h, bool* remove,
                    bool* req_download, bool* req_manual, bool* req_reset, bool* req_open_recomp,
                    bool* req_open_folder) {
    const ImVec2 total_size(img_size.x, img_size.y + title_h);

    ImGui::PushID(g.sha1.c_str());
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##card", total_size);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 base = platform_color(g.platform);
    const float tint = sha_tint(g.sha1);
    const float lift = hovered ? -5.f : 0.f;

    // Artwork rectangle
    const ImVec2 img_q0(p0.x, p0.y + lift);
    const ImVec2 img_q1(p0.x + img_size.x, p0.y + img_size.y + lift);

    SDL_Texture* tex = covers.get_texture(renderer, g.sha1, g.cover_path);
    if (tex) {
        // Pure artwork, crisp with 10px rounded corners, NO badges or overlays over it!
        dl->AddImageRounded((ImTextureID)(intptr_t)tex, img_q0, img_q1, ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, 10.f);
    } else {
        // Fallback tinted procedural color
        dl->AddRectFilled(img_q0, img_q1, scaled(base, tint), 10.f);
        const char* plabel = platform_label(g.platform);
        const ImVec2 psz = ImGui::CalcTextSize(plabel);
        dl->AddText(
            ImVec2(img_q0.x + (img_size.x - psz.x) * 0.5f, img_q0.y + (img_size.y - psz.y) * 0.5f),
            IM_COL32(255, 255, 255, 180), plabel);
    }

    // Border around artwork (accent cyan on hover)
    dl->AddRect(img_q0, img_q1,
                hovered ? ImGui::GetColorU32(ThemeColors::AccentCyan)
                        : ImGui::GetColorU32(ThemeColors::Border),
                10.f, 0, hovered ? 2.5f : 1.f);

    // If downloading, show subtle indicator
    if (covers.is_downloading(g.sha1)) {
        draw_pill(dl, ImVec2(img_q0.x + 8.f, img_q0.y + 8.f), "Descargando...",
                  IM_COL32(10, 30, 50, 230), ImGui::GetColorU32(ThemeColors::AccentCyan));
    }

    // Title area BELOW the image (clean text, no badges)
    const ImVec2 title_p0(p0.x + 2.0f, p0.y + img_size.y + 8.0f + lift);
    const ImVec2 title_p1(p0.x + img_size.x - 2.0f, p0.y + total_size.y + lift);

    dl->PushClipRect(title_p0, title_p1, true);
    ImFont* title_font = fonts.body_bold ? fonts.body_bold : ImGui::GetFont();
    dl->AddText(title_font, 18.0f, title_p0,
                hovered ? ImGui::GetColorU32(ThemeColors::AccentCyan)
                        : ImGui::GetColorU32(ThemeColors::TextPrimary),
                g.title.c_str(), nullptr, img_size.x - 4.0f);
    dl->PopClipRect();

    bool play_from_menu = false;
    if (ImGui::BeginPopupContextItem("##ctx")) {
        if (ImGui::MenuItem("▶ Ejecutar juego portable")) {
            play_from_menu = true;
        }
        if (!g.package_dir.empty()) {
            if (ImGui::MenuItem("📁 Abrir carpeta del portable")) {
                *req_open_folder = true;
            }
        }
        if (!g.rom_path.empty()) {
            if (ImGui::MenuItem("⚙ Abrir ROM en Recompilador")) {
                *req_open_recomp = true;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Descargar caratula (libretro)")) {
            *req_download = true;
        }
        if (ImGui::MenuItem("Asignar caratula manual...")) {
            *req_manual = true;
        }
        if (!g.cover_path.empty()) {
            if (ImGui::MenuItem("Restablecer caratula")) {
                *req_reset = true;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quitar de la biblioteca")) {
            *remove = true;
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return clicked || play_from_menu;
}

}  // namespace

void StudioApp::track_library_state() {
    const PipelineStage stage = pipeline_.stage();
    if (active_rom_.is_valid() && stage == PipelineStage::Ready && prev_stage_ != stage) {
        // Record whether Studio had to fall back to the interpreter for this game.
        GameMode mode = GameMode::Native;
        try {
            const auto cfg = dragon::load_game_config(pipeline_.config_file_path());
            if (cfg.interpreter_only) {
                mode = GameMode::Interpreter;
            }
        } catch (...) {
            mode = GameMode::Unknown;
        }
        library_.set_mode(active_rom_.sha1, mode);
        library_.save();
    }
    prev_stage_ = stage;

    const bool running = pipeline_.is_game_running();
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    if (running && !was_game_running_) {
        game_start_unix_ = now;
    } else if (!running && was_game_running_ && active_rom_.is_valid()) {
        library_.touch_played(active_rom_.sha1, now - game_start_unix_, now);
        library_.save();
    }
    was_game_running_ = running;

    // Also track portable games launched directly from the Library
    if (portable_game_pid_ > 0) {
#if !defined(_WIN32)
        int status = 0;
        pid_t r = waitpid(static_cast<pid_t>(portable_game_pid_), &status, WNOHANG);
        if (r != 0) {
            const int64_t elapsed = std::max<int64_t>(0, now - portable_game_start_unix_);
            library_.touch_played(portable_game_sha1_, elapsed, now);
            library_.save();
            if (r > 0 && WIFEXITED(status) && WEXITSTATUS(status) != 0 && elapsed < 3) {
                library_notice_ = "El ejecutable portable termino con error (codigo " +
                                  std::to_string(WEXITSTATUS(status)) +
                                  "). Verifica que la ROM este en la carpeta 'roms/' del paquete.";
            }
            portable_game_pid_ = -1;
            portable_game_sha1_.clear();
        }
#else
        HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                   static_cast<DWORD>(portable_game_pid_));
        bool exited = true;
        if (hProc) {
            if (WaitForSingleObject(hProc, 0) == WAIT_TIMEOUT) {
                exited = false;
            }
            CloseHandle(hProc);
        }
        if (exited) {
            const int64_t elapsed = std::max<int64_t>(0, now - portable_game_start_unix_);
            library_.touch_played(portable_game_sha1_, elapsed, now);
            library_.save();
            portable_game_pid_ = -1;
            portable_game_sha1_.clear();
        }
#endif
    }
}

void StudioApp::render_library() {
    ImGui::BeginChild("LibraryView", ImVec2(0, 0), false);

    std::string play_sha;
    std::string open_recomp_sha;
    std::string open_folder_sha;
    std::string remove_sha;
    std::string download_sha;
    std::string manual_sha;
    std::string reset_sha;

    // --- Continue playing / Welcome Hero ----------------------------------
    if (const GameEntry* last = library_.last_played()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::SurfaceAlt);
        ImGui::BeginChild("Hero", ImVec2(0, 155.0f), true, ImGuiWindowFlags_NoScrollbar);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const ImVec4 base = platform_color(last->platform);
        const ImVec2 hero_sz(100.f, 135.f);

        SDL_Texture* hero_tex = cover_mgr_.get_texture(renderer_, last->sha1, last->cover_path);
        if (hero_tex) {
            dl->AddImageRounded((ImTextureID)(intptr_t)hero_tex, o,
                                ImVec2(o.x + hero_sz.x, o.y + hero_sz.y), ImVec2(0, 0),
                                ImVec2(1, 1), IM_COL32_WHITE, 8.f);
        } else {
            dl->AddRectFilled(o, ImVec2(o.x + hero_sz.x, o.y + hero_sz.y),
                              scaled(base, sha_tint(last->sha1)), 8.f);
            dl->AddText(ImVec2(o.x + 10.f, o.y + 10.f), IM_COL32(255, 255, 255, 255),
                        platform_label(last->platform));
        }
        dl->AddRect(o, ImVec2(o.x + hero_sz.x, o.y + hero_sz.y),
                    ImGui::GetColorU32(ThemeColors::Border), 8.f, 0, 1.f);

        ImGui::SetCursorScreenPos(ImVec2(o.x + hero_sz.x + 20.f, o.y + 4.f));
        ImGui::BeginGroup();
        ImGui::PushFont(fonts_.small);
        ImGui::TextColored(ThemeColors::TextMuted, "CONTINUAR JUGANDO");
        ImGui::PopFont();

        ImGui::PushFont(fonts_.h1);
        ImGui::TextColored(ThemeColors::TextPrimary, "%s", last->title.c_str());
        ImGui::PopFont();

        ImGui::PushFont(fonts_.body);
        ImGui::TextColored(ThemeColors::TextMuted, "Tiempo jugado: %s",
                           format_playtime(last->playtime_s).c_str());
        ImGui::PopFont();

        ImGui::Dummy(ImVec2(0, 6.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        ImGui::PushFont(fonts_.body_bold);
        if (ImGui::Button("▶ Jugar ahora", ImVec2(180.f, 46.f))) {
            play_sha = last->sha1;
        }
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::EndGroup();

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    } else {
        // Welcome banner for new library with generous size and prominent text
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::SurfaceAlt);
        ImGui::BeginChild("WelcomeHero", ImVec2(0, 160.0f), true, ImGuiWindowFlags_NoScrollbar);

        ImGui::PushFont(fonts_.h1);
        ImGui::TextColored(ThemeColors::AccentCyan, "BIBLIOTECA DE JUEGOS PORTABLES");
        ImGui::PopFont();

        ImGui::Dummy(ImVec2(0, 2.0f));
        ImGui::PushFont(fonts_.body_bold);
        ImGui::TextColored(ThemeColors::TextPrimary,
                           "Tus juegos recompilados y exportados como portables apareceran aqui "
                           "listos para jugar.");
        ImGui::PopFont();

        ImGui::Dummy(ImVec2(0, 2.0f));
        ImGui::PushFont(fonts_.body);
        ImGui::TextColored(ThemeColors::TextMuted,
                           "Importa una carpeta de juego recompilado existente o recompila y "
                           "exporta un nuevo ROM.");
        ImGui::PopFont();

        ImGui::Dummy(ImVec2(0, 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        ImGui::PushFont(fonts_.body_bold);
        const float import_btn_w =
            std::max(250.0f, ImGui::CalcTextSize("📥 Importar Recompilado...").x + 36.0f);
        if (ImGui::Button("📥 Importar Recompilado...", ImVec2(import_btn_w, 46.f))) {
            open_import_portable_dialog();
        }
        ImGui::SameLine(0, 14.0f);
        const float welcome_btn_w =
            std::max(240.0f, ImGui::CalcTextSize("+ Recompilar nueva ROM...").x + 36.0f);
        if (ImGui::Button("+ Recompilar nueva ROM...", ImVec2(welcome_btn_w, 46.f))) {
            open_rom_file_dialog();
        }
        ImGui::PopFont();
        ImGui::PopStyleVar();

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    if (!library_notice_.empty()) {
        ImGui::TextColored(ThemeColors::WarningAmber, "%s", library_notice_.c_str());
        ImGui::Spacing();
    }

    // --- Library controls ---
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        ImGui::PushFont(fonts_.body_bold);
        const float btn_h = 46.0f;
        const float btn_imp_w =
            std::max(250.0f, ImGui::CalcTextSize("📥 Importar Recompilado...").x + 36.0f);
        if (ImGui::Button("📥 Importar Recompilado...", ImVec2(btn_imp_w, btn_h))) {
            open_import_portable_dialog();
        }
        ImGui::SameLine(0, 14.f);
        const float btn1_w =
            std::max(240.0f, ImGui::CalcTextSize("+ Recompilar nueva ROM...").x + 36.0f);
        if (ImGui::Button("+ Recompilar nueva ROM...", ImVec2(btn1_w, btn_h))) {
            open_rom_file_dialog();
        }
        if (!library_.entries().empty()) {
            ImGui::SameLine(0, 14.f);
            const float btn2_w =
                std::max(310.0f, ImGui::CalcTextSize("Descargar caratulas faltantes").x + 36.0f);
            if (ImGui::Button("Descargar caratulas faltantes", ImVec2(btn2_w, btn_h))) {
                size_t count = 0;
                for (const auto& entry : library_.entries()) {
                    if (entry.cover_path.empty() && !cover_mgr_.is_downloading(entry.sha1)) {
                        count++;
                        cover_mgr_.download_cover_async(
                            entry,
                            [this, sha1 = entry.sha1](bool ok, const std::filesystem::path& p) {
                                if (ok) {
                                    queue_assign_cover(sha1, p);
                                }
                            });
                    }
                }
                if (count > 0) {
                    library_notice_ = "Buscando caratulas para " + std::to_string(count) +
                                      " juego(s) en libretro-thumbnails...";
                } else {
                    library_notice_ = "Todos los juegos ya tienen caratula asignada.";
                }
            }
        }
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::Spacing();
    }

    // --- Console Tabs Bar -------------------------------------------------
    struct ConsoleTabInfo {
        const char* label;
        const char* badge;
        const char* platform_key;
        std::vector<std::string> keys;
    };
    const ConsoleTabInfo console_tabs[] = {
        {"Nintendo NES", "NES", "nes", {"nes"}},
        {"Super Nintendo", "SNES", "snes", {"snes"}},
        {"Game Boy / Color", "GB/GBC", "gb", {"gb", "gbc"}},
        {"Game Boy Advance", "GBA", "gba", {"gba"}},
    };

    library_console_tab_ = std::clamp(library_console_tab_, 0, 3);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushFont(fonts_.body_bold);
    for (size_t t = 0; t < 4; ++t) {
        if (t > 0) {
            ImGui::SameLine(0, 12.0f);
        }
        const bool active = (library_console_tab_ == static_cast<int>(t));

        // Count games for this tab
        size_t count = 0;
        for (const auto& k : console_tabs[t].keys) {
            count += library_.shelf(k).size();
        }

        std::string tab_title = console_tabs[t].label;
        tab_title += " (" + std::to_string(count) + ")";

        ImVec4 tab_col = platform_color(console_tabs[t].platform_key);

        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, tab_col);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06f, 0.08f, 0.10f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::SurfaceAlt);
            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextMuted);
        }

        if (ImGui::Button(tab_title.c_str(), ImVec2(0, 42.0f))) {
            library_console_tab_ = static_cast<int>(t);
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::Spacing();

    // Boxart image size (3:4 ratio) and title height beneath the image
    const ImVec2 img_size(190.f, 255.f);
    const float title_h = 48.0f;
    const ImVec2 card_size(img_size.x, img_size.y + title_h);

    // --- Filtered Console Grid ---
    const auto& ctab = console_tabs[library_console_tab_];
    std::vector<const GameEntry*> items;
    for (const auto& k : ctab.keys) {
        const auto part = library_.shelf(k);
        items.insert(items.end(), part.begin(), part.end());
    }
    std::sort(items.begin(), items.end(),
              [](const GameEntry* a, const GameEntry* b) { return a->title < b->title; });

    ImGui::PushFont(fonts_.h2);
    ImGui::TextColored(platform_color(ctab.platform_key), "%s", ctab.label);
    ImGui::PopFont();
    ImGui::SameLine(0, 10.f);
    ImGui::PushFont(fonts_.body);
    ImGui::TextColored(ThemeColors::TextMuted, "(%zu juegos registrados)", items.size());
    ImGui::PopFont();
    ImGui::Spacing();

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float spacing_x = 20.0f;
    const float spacing_y = 22.0f;
    int cols = std::max(1, static_cast<int>((avail_w + spacing_x) / (card_size.x + spacing_x)));

    ImGui::PushID(ctab.label);
    for (size_t i = 0; i < items.size(); ++i) {
        if ((i % cols) != 0) {
            ImGui::SameLine(0, spacing_x);
        } else if (i > 0) {
            ImGui::Dummy(ImVec2(0, spacing_y - 4.0f));
        }
        bool remove = false;
        bool req_download = false;
        bool req_manual = false;
        bool req_reset = false;
        bool req_open_recomp = false;
        bool req_open_folder = false;
        if (draw_game_card(renderer_, cover_mgr_, fonts_, *items[i], img_size, title_h, &remove,
                           &req_download, &req_manual, &req_reset, &req_open_recomp,
                           &req_open_folder)) {
            play_sha = items[i]->sha1;
        }
        if (remove)
            remove_sha = items[i]->sha1;
        if (req_download)
            download_sha = items[i]->sha1;
        if (req_manual)
            manual_sha = items[i]->sha1;
        if (req_reset)
            reset_sha = items[i]->sha1;
        if (req_open_recomp)
            open_recomp_sha = items[i]->sha1;
        if (req_open_folder)
            open_folder_sha = items[i]->sha1;
    }

    // Add slot card
    if ((items.size() % cols) != 0) {
        ImGui::SameLine(0, spacing_x);
    } else if (!items.empty()) {
        ImGui::Dummy(ImVec2(0, spacing_y - 4.0f));
    }
    ImGui::PushID("add_slot_grid");
    if (ImGui::InvisibleButton("##add_btn_grid", card_size)) {
        open_import_portable_dialog();
    }
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec4 base = platform_color(ctab.platform_key);
    draw_add_slot_card(dl, p0, img_size, ctab.badge, base, hov);
    ImGui::PopID();
    ImGui::PopID();
    ImGui::Spacing();

    ImGui::EndChild();

    // Mutations happen after drawing so entry pointers stay valid while iterating.
    if (!remove_sha.empty()) {
        library_.remove(remove_sha);
        cover_mgr_.invalidate(remove_sha);
        library_.save();
    }
    if (!download_sha.empty()) {
        if (const GameEntry* g = library_.find(download_sha)) {
            cover_mgr_.download_cover_async(
                *g, [this, sha1 = g->sha1](bool ok, const std::filesystem::path& p) {
                    if (ok) {
                        queue_assign_cover(sha1, p);
                    }
                });
            library_notice_ = "Buscando caratula para \"" + g->title + "\"...";
        }
    }
    if (!manual_sha.empty()) {
        open_cover_file_dialog(manual_sha);
    }
    if (!reset_sha.empty()) {
        library_.set_cover(reset_sha, {});
        cover_mgr_.invalidate(reset_sha);
        library_.save();
        library_notice_ = "Caratula restablecida al gradiente predeterminado.";
    }
    if (!open_folder_sha.empty()) {
        if (const GameEntry* g = library_.find(open_folder_sha)) {
            if (!g->package_dir.empty() && std::filesystem::exists(g->package_dir)) {
#if defined(_WIN32)
                std::string cmd = "explorer \"" + g->package_dir.string() + "\"";
                int ret = std::system(cmd.c_str());
                (void)ret;
#else
                std::string cmd = "xdg-open \"" + g->package_dir.string() + "\" >/dev/null 2>&1 &";
                int ret = std::system(cmd.c_str());
                (void)ret;
#endif
            }
        }
    }
    if (!open_recomp_sha.empty()) {
        if (const GameEntry* g = library_.find(open_recomp_sha)) {
            if (library_.relocate_rom(g->sha1, g->rom_path.parent_path())) {
                load_rom(g->rom_path);
            } else {
                library_notice_ = "No se encontro el archivo ROM original de \"" + g->title + "\".";
            }
        }
    }
    if (!play_sha.empty()) {
        if (const GameEntry* g = library_.find(play_sha)) {
            launch_portable_game(*g);
        }
    }
}
}  // namespace dragon::studio
