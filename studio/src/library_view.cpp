// Library view (Spec 005 — Obsidian Dark/Light redesign with right-hand Inspector).
//
// Boxart covers are loaded dynamically via CoverManager (libretro-thumbnails or user local files).
// When unavailable, cards fall back to a stylized per-console gradient with platform badge and
// game abbreviation matching the Studio UI mockup.
#include <algorithm>
#include <cctype>
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

ImU32 scaled_col(const ImVec4& c, float k, float a = 1.0f) {
    return ImGui::GetColorU32(ImVec4(std::clamp(c.x * k, 0.f, 1.f), std::clamp(c.y * k, 0.f, 1.f),
                                     std::clamp(c.z * k, 0.f, 1.f), a));
}

ImVec4 platform_color(const std::string& key) {
    if (key == "nes")
        return ImVec4(0.75f, 0.22f, 0.17f, 1.f);
    if (key == "snes")
        return ImVec4(0.49f, 0.24f, 0.62f, 1.f);
    if (key == "gb")
        return ImVec4(0.45f, 0.56f, 0.17f, 1.f);
    if (key == "gbc")
        return ImVec4(0.10f, 0.62f, 0.55f, 1.f);
    if (key == "gba")
        return ImVec4(0.17f, 0.37f, 0.70f, 1.f);
    if (key == "sms")
        return ImVec4(0.14f, 0.45f, 0.85f, 1.f);
    if (key == "gg")
        return ImVec4(0.16f, 0.56f, 0.76f, 1.f);
    if (key == "pce")
        return ImVec4(0.85f, 0.36f, 0.14f, 1.f);
    return ImVec4(0.35f, 0.38f, 0.45f, 1.f);
}

const char* platform_short(const std::string& key) {
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
    if (key == "sms")
        return "SMS";
    if (key == "gg")
        return "GG";
    if (key == "pce")
        return "PCE";
    return "?";
}

const char* platform_full_name(const std::string& key) {
    if (key == "nes")
        return "Nintendo NES";
    if (key == "snes")
        return "Super Nintendo";
    if (key == "gb")
        return "Game Boy";
    if (key == "gbc")
        return "Game Boy Color";
    if (key == "gba")
        return "Game Boy Advance";
    if (key == "sms")
        return "Sega Master System";
    if (key == "gg")
        return "Sega Game Gear";
    if (key == "pce")
        return "PC Engine / TG-16";
    return "Desconocida";
}

std::string make_abbreviation(const std::string& title) {
    std::string abbr;
    bool new_word = true;
    for (unsigned char ch : title) {
        if (std::isalnum(ch)) {
            if (new_word) {
                abbr.push_back(static_cast<char>(std::toupper(ch)));
                new_word = false;
                if (abbr.size() >= 4) {
                    break;
                }
            }
        } else {
            new_word = true;
        }
    }
    if (abbr.size() < 2) {
        abbr.clear();
        for (unsigned char ch : title) {
            if (std::isalnum(ch)) {
                abbr.push_back(static_cast<char>(std::toupper(ch)));
                if (abbr.size() >= 3) {
                    break;
                }
            }
        }
    }
    return abbr.empty() ? "ROM" : abbr;
}

float sha_tint(const std::string& sha1) {
    unsigned v = 0;
    if (sha1.size() >= 2) {
        std::sscanf(sha1.substr(0, 2).c_str(), "%x", &v);
    }
    return 0.80f + 0.36f * (static_cast<float>(v) / 255.0f);
}

std::string format_playtime(int64_t s) {
    if (s <= 0)
        return "0m";
    const int64_t h = s / 3600;
    const int64_t m = (s % 3600) / 60;
    char buf[48];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%lldh %02lldm", static_cast<long long>(h),
                      static_cast<long long>(m));
    } else {
        std::snprintf(buf, sizeof(buf), "%lldm", static_cast<long long>(std::max<int64_t>(m, 1)));
    }
    return buf;
}

void draw_pill(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg) {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const ImVec2 p1(pos.x + sz.x + 12.f, pos.y + sz.y + 4.f);
    dl->AddRectFilled(pos, p1, bg, 4.f);
    dl->AddText(ImVec2(pos.x + 6.f, pos.y + 2.f), fg, text);
}

void open_dir_in_os(const std::filesystem::path& folder) {
    if (folder.empty() || !std::filesystem::exists(folder)) {
        return;
    }
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

struct CardAction {
    bool selected = false;
    bool play = false;
    bool open_folder = false;
    bool remove = false;
    bool download_cover = false;
    bool manual_cover = false;
    bool reset_cover = false;
};

CardAction draw_game_card(SDL_Renderer* renderer, CoverManager& covers, const StudioFonts& fonts,
                          const GameEntry& g, ImVec2 img_size, float footer_h, bool is_selected) {
    CardAction act;
    const ImVec2 total_size(img_size.x, img_size.y + footer_h);

    ImGui::PushID(g.sha1.c_str());
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##card", total_size);
    const bool hovered = ImGui::IsItemHovered();
    const bool dbl_clicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

    if (clicked) {
        act.selected = true;
    }
    if (dbl_clicked) {
        act.play = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lift = hovered ? -2.f : 0.f;
    const ImVec2 c0(p0.x, p0.y + lift);
    const ImVec2 c1(p0.x + total_size.x, p0.y + total_size.y + lift);
    const ImVec2 img_q0 = c0;
    const ImVec2 img_q1(c0.x + img_size.x, c0.y + img_size.y);

    // Card surface background
    dl->AddRectFilled(c0, c1, ImGui::GetColorU32(ThemeColors::Surface), 10.f);

    SDL_Texture* tex = covers.get_texture(renderer, g.sha1, g.cover_path);
    if (tex) {
        dl->AddImageRounded((ImTextureID)(intptr_t)tex, img_q0, img_q1, ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, 10.f, ImDrawFlags_RoundCornersTop);
    } else {
        const ImVec4 base = platform_color(g.platform);
        const float tint = sha_tint(g.sha1);
        const ImU32 top_col = scaled_col(base, tint * 0.85f);
        const ImU32 bot_col = scaled_col(base, tint * 0.35f);
        dl->AddRectFilledMultiColor(img_q0, img_q1, top_col, top_col, bot_col, bot_col);

        // Platform chip top-left
        if (fonts.small)
            ImGui::PushFont(fonts.small);
        draw_pill(dl, ImVec2(img_q0.x + 10.f, img_q0.y + 10.f), platform_short(g.platform),
                  IM_COL32(0, 0, 0, 115), IM_COL32(255, 255, 255, 225));
        if (fonts.small)
            ImGui::PopFont();

        // Bold game abbreviation centered
        const std::string abbr = make_abbreviation(g.title);
        ImFont* abbr_font = fonts.h1 ? fonts.h1 : ImGui::GetFont();
        const float abbr_sz_px = 26.f;
        const ImVec2 abbr_sz = abbr_font->CalcTextSizeA(abbr_sz_px, FLT_MAX, 0.f, abbr.c_str());
        dl->AddText(abbr_font, abbr_sz_px,
                    ImVec2(img_q0.x + (img_size.x - abbr_sz.x) * 0.5f,
                           img_q0.y + (img_size.y - abbr_sz.y) * 0.5f),
                    IM_COL32(255, 255, 255, 235), abbr.c_str());

        // Playtime bottom-left
        if (fonts.small)
            ImGui::PushFont(fonts.small);
        const std::string pt = format_playtime(g.playtime_s);
        dl->AddText(ImVec2(img_q0.x + 10.f, img_q1.y - 22.f), IM_COL32(255, 255, 255, 190),
                    pt.c_str());
        if (fonts.small)
            ImGui::PopFont();
    }

    // Green ready-dot in top-right corner when host executable exists
    const auto host_bin = GameLibrary::resolve_host_executable(g);
    std::error_code ec;
    const bool ready = !host_bin.empty() && std::filesystem::exists(host_bin, ec);
    dl->AddCircleFilled(ImVec2(img_q1.x - 14.f, img_q0.y + 14.f), 4.5f,
                        ready ? ImGui::GetColorU32(ThemeColors::SuccessGreen)
                              : ImGui::GetColorU32(ThemeColors::WarningAmber));

    if (covers.is_downloading(g.sha1)) {
        draw_pill(dl, ImVec2(img_q0.x + 8.f, img_q1.y - 26.f), "Descargando...",
                  IM_COL32(10, 30, 50, 230), ImGui::GetColorU32(ThemeColors::AccentCyan));
    }

    // Footer with game title
    const ImVec2 title_p0(c0.x + 10.f, img_q1.y + 8.f);
    const ImVec2 title_p1(c1.x - 10.f, c1.y - 6.f);
    dl->PushClipRect(title_p0, title_p1, true);
    ImFont* title_font = fonts.body_bold ? fonts.body_bold : ImGui::GetFont();
    dl->AddText(title_font, 14.f, title_p0,
                is_selected ? ImGui::GetColorU32(ThemeColors::Primary)
                            : ImGui::GetColorU32(ThemeColors::TextPrimary),
                g.title.c_str(), nullptr, img_size.x - 20.f);
    dl->PopClipRect();

    // Card border (Primary when selected, BorderStrong on hover)
    const ImU32 border_col = is_selected ? ImGui::GetColorU32(ThemeColors::Primary)
                             : hovered   ? ImGui::GetColorU32(ThemeColors::BorderStrong)
                                         : ImGui::GetColorU32(ThemeColors::Border);
    dl->AddRect(c0, c1, border_col, 10.f, 0, is_selected ? 2.0f : 1.0f);

    // Context menu (NO Recompilar option; Eliminar only removes from library)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 9.0f));
    if (ImGui::BeginPopupContextItem("##ctx")) {
        if (ImGui::MenuItem("▶  Jugar ahora")) {
            act.play = true;
        }
        if (!g.package_dir.empty()) {
            if (ImGui::MenuItem("Abrir carpeta")) {
                act.open_folder = true;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Descargar caratula (libretro)")) {
            act.download_cover = true;
        }
        if (ImGui::MenuItem("Asignar caratula manual...")) {
            act.manual_cover = true;
        }
        if (!g.cover_path.empty()) {
            if (ImGui::MenuItem("Restablecer caratula")) {
                act.reset_cover = true;
            }
        }
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::DangerRed);
        if (ImGui::MenuItem("Eliminar de la biblioteca")) {
            act.remove = true;
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);

    ImGui::PopID();
    return act;
}

void draw_import_card(ImDrawList* dl, ImVec2 p0, ImVec2 img_size, float footer_h,
                      const StudioFonts& fonts, bool hovered) {
    const float lift = hovered ? -2.f : 0.f;
    const ImVec2 c0(p0.x, p0.y + lift);
    const ImVec2 c1(p0.x + img_size.x, p0.y + img_size.y + footer_h + lift);

    dl->AddRectFilled(c0, c1,
                      hovered ? ImGui::GetColorU32(ThemeColors::SurfaceAlt)
                              : ImGui::GetColorU32(ThemeColors::Surface),
                      10.f);
    dl->AddRect(c0, c1,
                hovered ? ImGui::GetColorU32(ThemeColors::Primary)
                        : ImGui::GetColorU32(ThemeColors::BorderStrong),
                10.f, 0, hovered ? 1.8f : 1.0f);

    const ImVec2 center(c0.x + img_size.x * 0.5f, c0.y + img_size.y * 0.5f);
    const ImU32 plus_col = hovered ? ImGui::GetColorU32(ThemeColors::Primary)
                                   : ImGui::GetColorU32(ThemeColors::TextMuted);
    dl->AddLine(ImVec2(center.x - 14.f, center.y), ImVec2(center.x + 14.f, center.y), plus_col,
                2.5f);
    dl->AddLine(ImVec2(center.x, center.y - 14.f), ImVec2(center.x, center.y + 14.f), plus_col,
                2.5f);

    const char* lbl = "Importar recompilado";
    ImFont* f = fonts.small ? fonts.small : ImGui::GetFont();
    const ImVec2 lsz = f->CalcTextSizeA(13.f, FLT_MAX, 0.f, lbl);
    dl->AddText(f, 13.f, ImVec2(c0.x + (img_size.x - lsz.x) * 0.5f, c0.y + img_size.y + 10.f),
                hovered ? ImGui::GetColorU32(ThemeColors::Primary)
                        : ImGui::GetColorU32(ThemeColors::TextMuted),
                lbl);
}

void draw_inspector_row(const char* label, const char* value, const ImVec4& value_col,
                        const StudioFonts& fonts) {
    if (fonts.small)
        ImGui::PushFont(fonts.small);
    ImGui::TextColored(ThemeColors::TextMuted, "%s", label);
    const float val_w = ImGui::CalcTextSize(value).x;
    const float avail_x = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - val_w));
    ImGui::TextColored(value_col, "%s", value);
    (void)avail_x;
    if (fonts.small)
        ImGui::PopFont();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 line_y = ImGui::GetCursorScreenPos();
    const float x0 = ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x;
    const float x1 =
        ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    dl->AddLine(ImVec2(x0, line_y.y + 2.f), ImVec2(x1, line_y.y + 2.f),
                ImGui::GetColorU32(ThemeColors::Border), 1.0f);
    ImGui::Dummy(ImVec2(0.f, 7.f));
}

}  // namespace

void StudioApp::track_library_state() {
    const PipelineStage stage = pipeline_.stage();
    if (active_rom_.is_valid() && stage == PipelineStage::Ready && prev_stage_ != stage) {
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
    std::string play_sha;
    std::string open_folder_sha;
    std::string remove_sha;
    std::string download_sha;
    std::string manual_sha;
    std::string reset_sha;

    struct ConsoleTabInfo {
        const char* label;
        const char* platform_key;
        std::vector<std::string> keys;
    };
    const ConsoleTabInfo console_tabs[] = {
        {"Nintendo NES", "nes", {"nes"}},
        {"Super Nintendo", "snes", {"snes"}},
        {"Game Boy / Color", "gb", {"gb", "gbc"}},
        {"Game Boy Advance", "gba", {"gba"}},
        {"Sega MS / GG", "sms", {"sms", "gg"}},
        {"PC Engine", "pce", {"pce"}},
    };
    constexpr size_t kConsoleTabCount = sizeof(console_tabs) / sizeof(console_tabs[0]);
    library_console_tab_ = std::clamp(library_console_tab_, 0, static_cast<int>(kConsoleTabCount) - 1);

    const float total_w = ImGui::GetContentRegionAvail().x;
    const float inspector_w = std::clamp(total_w * 0.24f, 260.0f, 360.0f);
    const float gap = 18.0f;
    const float left_w = std::max(320.0f, total_w - inspector_w - gap);

    // =========================================================================
    // LEFT COLUMN: Console Tabs + 3:4 Games Grid
    // =========================================================================
    ImGui::BeginChild("LibraryLeft", ImVec2(left_w, 0.0f), false);

    if (!library_notice_.empty()) {
        const float text_h = ImGui::GetTextLineHeight();
        const float notice_h = std::max(40.0f, text_h + 18.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::SurfaceAlt);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 0.0f));
        ImGui::BeginChild("LibNotice", ImVec2(0, notice_h), true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetCursorPosY((notice_h - text_h) * 0.5f);
        ImGui::TextColored(ThemeColors::AccentCyan, "%s", library_notice_.c_str());
        const float close_sz = std::max(24.0f, text_h + 4.0f);
        ImGui::SetCursorPos(
            ImVec2(ImGui::GetWindowWidth() - close_sz - 10.0f, (notice_h - close_sz) * 0.5f));
        if (ImGui::Button("✕##DismissNotice", ImVec2(close_sz, close_sz))) {
            library_notice_.clear();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // Console Tabs Bar (auto-wraps gracefully on narrower windows)
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 7.0f));
    if (fonts_.body_bold)
        ImGui::PushFont(fonts_.body_bold);

    const float tabs_max_x = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    for (size_t t = 0; t < kConsoleTabCount; ++t) {
        const bool active = (library_console_tab_ == static_cast<int>(t));
        size_t count = 0;
        for (const auto& k : console_tabs[t].keys) {
            count += library_.shelf(k).size();
        }
        char tab_buf[96];
        std::snprintf(tab_buf, sizeof(tab_buf), "%s  %zu", console_tabs[t].label, count);

        if (t > 0) {
            const float next_w = ImGui::CalcTextSize(tab_buf).x + 28.0f;
            const float last_right = ImGui::GetItemRectMax().x;
            if (last_right + 8.0f + next_w <= tabs_max_x) {
                ImGui::SameLine(0, 8.0f);
            }
        }

        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::Primary);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColors::Primary);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColors::Primary);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::SurfaceAlt);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColors::SurfaceHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColors::SurfaceHover);
            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextMuted);
        }

        if (ImGui::Button(tab_buf)) {
            library_console_tab_ = static_cast<int>(t);
        }
        ImGui::PopStyleColor(4);
    }

    if (fonts_.body_bold)
        ImGui::PopFont();
    ImGui::PopStyleVar(2);

    ImGui::Dummy(ImVec2(0.f, 10.f));

    // Collect games in active console tab
    const auto& ctab = console_tabs[library_console_tab_];
    std::vector<const GameEntry*> items;
    for (const auto& k : ctab.keys) {
        const auto part = library_.shelf(k);
        items.insert(items.end(), part.begin(), part.end());
    }
    std::sort(items.begin(), items.end(),
              [](const GameEntry* a, const GameEntry* b) { return a->title < b->title; });

    // Ensure selected_library_sha1_ points to a valid entry in this tab (or clear if empty)
    const GameEntry* selected_entry = nullptr;
    for (const GameEntry* g : items) {
        if (g->sha1 == selected_library_sha1_) {
            selected_entry = g;
            break;
        }
    }
    if (!selected_entry && !items.empty()) {
        selected_entry = items.front();
        selected_library_sha1_ = selected_entry->sha1;
    }

    // 3:4 Games Grid
    const float grid_avail_w = ImGui::GetContentRegionAvail().x;
    const float spacing_x = 14.0f;
    const float spacing_y = 14.0f;
    const float min_card_w = 154.0f;
    const int cols =
        std::max(1, static_cast<int>((grid_avail_w + spacing_x) / (min_card_w + spacing_x)));
    const float card_w =
        std::min(210.0f, (grid_avail_w - spacing_x * static_cast<float>(cols - 1)) /
                             static_cast<float>(cols));
    const ImVec2 img_size(card_w, card_w * (4.0f / 3.0f));
    const float footer_h = std::max(38.0f, ImGui::GetTextLineHeight() * 2.15f + 8.0f);

    ImGui::PushID(ctab.label);
    for (size_t i = 0; i < items.size(); ++i) {
        if ((i % static_cast<size_t>(cols)) != 0) {
            ImGui::SameLine(0, spacing_x);
        } else if (i > 0) {
            ImGui::Dummy(ImVec2(0, spacing_y - 4.0f));
        }

        const bool is_sel = (selected_entry && items[i]->sha1 == selected_entry->sha1);
        CardAction act =
            draw_game_card(renderer_, cover_mgr_, fonts_, *items[i], img_size, footer_h, is_sel);
        if (act.selected) {
            selected_library_sha1_ = items[i]->sha1;
            selected_entry = items[i];
        }
        if (act.play)
            play_sha = items[i]->sha1;
        if (act.open_folder)
            open_folder_sha = items[i]->sha1;
        if (act.remove)
            remove_sha = items[i]->sha1;
        if (act.download_cover)
            download_sha = items[i]->sha1;
        if (act.manual_cover)
            manual_sha = items[i]->sha1;
        if (act.reset_cover)
            reset_sha = items[i]->sha1;
    }

    // "+ Importar recompilado" slot card at end of grid
    if ((items.size() % static_cast<size_t>(cols)) != 0) {
        ImGui::SameLine(0, spacing_x);
    } else if (!items.empty()) {
        ImGui::Dummy(ImVec2(0, spacing_y - 4.0f));
    }
    ImGui::PushID("import_slot_card");
    if (ImGui::InvisibleButton("##import_card", ImVec2(img_size.x, img_size.y + footer_h))) {
        open_import_portable_dialog();
    }
    const bool imp_hov = ImGui::IsItemHovered();
    draw_import_card(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), img_size, footer_h,
                     fonts_, imp_hov);
    ImGui::PopID();
    ImGui::PopID();

    ImGui::EndChild();

    ImGui::SameLine(0, gap);

    // =========================================================================
    // RIGHT COLUMN: Inspector Panel (280px, NO "Recompilar" button)
    // =========================================================================
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::Surface);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 18.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::BeginChild("LibraryInspector", ImVec2(inspector_w, 0.0f), true);

    if (fonts_.small)
        ImGui::PushFont(fonts_.small);
    ImGui::TextColored(ThemeColors::AccentCyan, "INSPECTOR");
    if (fonts_.small)
        ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.f, 4.f));

    if (selected_entry) {
        if (fonts_.h2)
            ImGui::PushFont(fonts_.h2);
        ImGui::TextWrapped("%s", selected_entry->title.c_str());
        if (fonts_.h2)
            ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.f, 12.f));

        // Metadata rows
        draw_inspector_row("Plataforma", platform_full_name(selected_entry->platform),
                           ThemeColors::TextPrimary, fonts_);
        const std::string playtime_str = format_playtime(selected_entry->playtime_s);
        draw_inspector_row("Tiempo jugado", playtime_str.c_str(), ThemeColors::TextPrimary, fonts_);

        const char* mode_str = (selected_entry->mode == GameMode::Native)        ? "Nativo C99"
                               : (selected_entry->mode == GameMode::Interpreter) ? "Interprete"
                                                                                 : "Portable";
        draw_inspector_row("Modo", mode_str, ThemeColors::TextPrimary, fonts_);

        const bool has_lin = !selected_entry->executable_linux.empty();
        const bool has_win = !selected_entry->executable_windows.empty();
        const char* targets_str = (has_lin && has_win) ? "Linux + Windows"
                                  : has_lin            ? "Linux"
                                  : has_win            ? "Windows"
                                                       : "Sin binario";
        draw_inspector_row("Paquete", targets_str, ThemeColors::TextPrimary, fonts_);

        const auto host_bin = GameLibrary::resolve_host_executable(*selected_entry);
        std::error_code ec;
        const bool ready = !host_bin.empty() && std::filesystem::exists(host_bin, ec);
        draw_inspector_row("Estado", ready ? "Listo" : "Sin binario host",
                           ready ? ThemeColors::SuccessGreen : ThemeColors::WarningAmber, fonts_);

        ImGui::Dummy(ImVec2(0.f, 14.f));

        // 1. Primary Action: "▶ Jugar ahora"
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        if (fonts_.body_bold)
            ImGui::PushFont(fonts_.body_bold);

        ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::Primary);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(std::min(ThemeColors::Primary.x * 1.1f, 1.f),
                                     std::min(ThemeColors::Primary.y * 1.1f, 1.f),
                                     std::min(ThemeColors::Primary.z * 1.1f, 1.f), 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColors::Primary);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
        if (ImGui::Button("▶  Jugar ahora", ImVec2(-1.0f, 38.0f))) {
            play_sha = selected_entry->sha1;
        }
        ImGui::PopStyleColor(4);

        ImGui::Dummy(ImVec2(0.f, 4.f));

        // 2. Secondary Action: "Abrir carpeta"
        ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::SurfaceAlt);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColors::SurfaceHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColors::SurfaceHover);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
        if (ImGui::Button("Abrir carpeta", ImVec2(-1.0f, 34.0f))) {
            open_folder_sha = selected_entry->sha1;
        }
        ImGui::PopStyleColor(4);

        ImGui::Dummy(ImVec2(0.f, 4.f));

        // 3. Danger Action: "Eliminar" (removes from library.toml ONLY; never deletes disk files)
        ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::SurfaceAlt);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColors::SurfaceHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColors::SurfaceHover);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::DangerRed);
        if (ImGui::Button("Eliminar", ImVec2(-1.0f, 34.0f))) {
            remove_sha = selected_entry->sha1;
        }
        ImGui::PopStyleColor(4);

        if (fonts_.body_bold)
            ImGui::PopFont();
        ImGui::PopStyleVar();
    } else {
        if (fonts_.body)
            ImGui::PushFont(fonts_.body);
        ImGui::TextColored(ThemeColors::TextMuted, "Sin juegos en %s.", ctab.label);
        ImGui::Dummy(ImVec2(0.f, 8.f));
        ImGui::TextWrapped(
            "Exporta un juego recompilado como portable o importa una carpeta existente para "
            "verlo aqui.");
        if (fonts_.body)
            ImGui::PopFont();

        ImGui::Dummy(ImVec2(0.f, 14.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::Primary);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
        if (ImGui::Button("+ Importar Recompilado", ImVec2(-1.0f, 36.0f))) {
            open_import_portable_dialog();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // =========================================================================
    // Deferred Mutations (safe after iteration)
    // =========================================================================
    if (!remove_sha.empty()) {
        // Removes ONLY from library.toml; files on disk are intentionally untouched.
        if (selected_library_sha1_ == remove_sha) {
            selected_library_sha1_.clear();
        }
        library_.remove(remove_sha);
        cover_mgr_.invalidate(remove_sha);
        library_.save();
        library_notice_ = "Juego eliminado de la biblioteca (archivos conservados en disco).";
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
        library_notice_ = "Caratula restablecida.";
    }
    if (!open_folder_sha.empty()) {
        if (const GameEntry* g = library_.find(open_folder_sha)) {
            open_dir_in_os(g->package_dir);
        }
    }
    if (!play_sha.empty()) {
        if (const GameEntry* g = library_.find(play_sha)) {
            launch_portable_game(*g);
        }
    }
}

}  // namespace dragon::studio
