// Studio UI Views (Spec 005 — Sidebar, Topbar, Recompiler, Preferences).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "imgui.h"
#include "studio/app.h"
#include "studio/theme.h"

namespace dragon::studio {

namespace {

void open_dir_os(const std::filesystem::path& folder) {
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

// Draws a dashed rounded rectangle border for the ROM drop-zone.
void draw_dashed_rect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, float thickness = 1.5f,
                      float dash = 8.0f, float gap = 6.0f) {
    auto draw_edge = [&](ImVec2 a, ImVec2 b) {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.1f)
            return;
        const float ux = dx / len;
        const float uy = dy / len;
        for (float t = 0.0f; t < len; t += dash + gap) {
            const float t_end = std::min(t + dash, len);
            dl->AddLine(ImVec2(a.x + ux * t, a.y + uy * t),
                        ImVec2(a.x + ux * t_end, a.y + uy * t_end), col, thickness);
        }
    };
    const float r = 6.0f;
    draw_edge(ImVec2(p0.x + r, p0.y), ImVec2(p1.x - r, p0.y));
    draw_edge(ImVec2(p1.x, p0.y + r), ImVec2(p1.x, p1.y - r));
    draw_edge(ImVec2(p1.x - r, p1.y), ImVec2(p0.x + r, p1.y));
    draw_edge(ImVec2(p0.x, p1.y - r), ImVec2(p0.x, p0.y + r));
}

// Custom iOS/HTML-style pill toggle switch matching `.toggle` in mockup.
bool toggle_switch(const char* id, bool* v) {
    const float w = 40.0f;
    const float h = 22.0f;
    const float radius = h * 0.5f;

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (clicked) {
        *v = !*v;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = *v ? ImGui::GetColorU32(ThemeColors::Primary)
                        : ImGui::GetColorU32(ThemeColors::BorderStrong);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, radius);

    const float knob_x = *v ? (p.x + w - radius) : (p.x + radius);
    dl->AddCircleFilled(ImVec2(knob_x, p.y + radius), radius - 3.0f, IM_COL32(255, 255, 255, 255));
    return clicked;
}

// Vector icon drawers for the 56px sidebar.
void draw_grid_icon(ImDrawList* dl, ImVec2 c, ImU32 col) {
    const float s = 6.5f;
    const float g = 2.0f;
    dl->AddRect(ImVec2(c.x - s - g * 0.5f, c.y - s - g * 0.5f),
                ImVec2(c.x - g * 0.5f, c.y - g * 0.5f), col, 1.5f, 0, 1.7f);
    dl->AddRect(ImVec2(c.x + g * 0.5f, c.y - s - g * 0.5f),
                ImVec2(c.x + s + g * 0.5f, c.y - g * 0.5f), col, 1.5f, 0, 1.7f);
    dl->AddRect(ImVec2(c.x - s - g * 0.5f, c.y + g * 0.5f),
                ImVec2(c.x - g * 0.5f, c.y + s + g * 0.5f), col, 1.5f, 0, 1.7f);
    dl->AddRect(ImVec2(c.x + g * 0.5f, c.y + g * 0.5f),
                ImVec2(c.x + s + g * 0.5f, c.y + s + g * 0.5f), col, 1.5f, 0, 1.7f);
}

void draw_bolt_icon(ImDrawList* dl, ImVec2 c, ImU32 col) {
    ImVec2 pts[6] = {
        ImVec2(c.x + 1.0f, c.y - 8.5f), ImVec2(c.x - 6.5f, c.y + 1.0f),
        ImVec2(c.x + 0.0f, c.y + 1.0f), ImVec2(c.x - 1.5f, c.y + 8.5f),
        ImVec2(c.x + 6.5f, c.y - 1.0f), ImVec2(c.x + 0.0f, c.y - 1.0f),
    };
    dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, 1.7f);
}

void draw_gear_icon(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddCircle(c, 3.2f, col, 16, 1.7f);
    dl->AddCircle(c, 6.8f, col, 20, 1.6f);
    for (int i = 0; i < 8; ++i) {
        const float a = static_cast<float>(i) * 3.14159265f * 0.25f;
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        dl->AddLine(ImVec2(c.x + ca * 6.8f, c.y + sa * 6.8f),
                    ImVec2(c.x + ca * 9.2f, c.y + sa * 9.2f), col, 2.0f);
    }
}

void draw_info_icon(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddCircle(c, 7.5f, col, 20, 1.5f);
    dl->AddCircleFilled(ImVec2(c.x, c.y - 3.2f), 1.1f, col);
    dl->AddLine(ImVec2(c.x, c.y - 0.5f), ImVec2(c.x, c.y + 4.0f), col, 1.7f);
}

const char* rom_platform_badge(RomPlatform p) {
    switch (p) {
        case RomPlatform::NES:
            return "NES";
        case RomPlatform::SNES:
            return "SNES";
        case RomPlatform::GameBoy:
            return "GB";
        case RomPlatform::GameBoyColor:
            return "GBC";
        case RomPlatform::GameBoyAdvance:
            return "GBA";
        case RomPlatform::MasterSystem:
            return "SMS";
        case RomPlatform::GameGear:
            return "GG";
        case RomPlatform::PCEngine:
            return "PCE";
        default:
            return "ROM";
    }
}

const char* rom_platform_display_name(RomPlatform p) {
    switch (p) {
        case RomPlatform::NES:
            return "NES";
        case RomPlatform::SNES:
            return "Super Nintendo";
        case RomPlatform::GameBoy:
            return "Game Boy";
        case RomPlatform::GameBoyColor:
            return "Game Boy Color";
        case RomPlatform::GameBoyAdvance:
            return "Game Boy Advance";
        case RomPlatform::MasterSystem:
            return "Sega Master System";
        case RomPlatform::GameGear:
            return "Sega Game Gear";
        case RomPlatform::PCEngine:
            return "PC Engine / TurboGrafx-16";
        default:
            return "Consola";
    }
}

struct ControlRow {
    const char* action;
    const char* keys;
};

std::vector<ControlRow> console_controls_for(RomPlatform p) {
    switch (p) {
        case RomPlatform::PCEngine:
            return {
                {"Cruceta", "Flechas / WASD"},
                {"Boton I", "Z / J"},
                {"Boton II", "X / K"},
                {"Run / Select", "Enter / Shift o Tab"},
            };
        case RomPlatform::MasterSystem:
            return {
                {"Cruceta", "Flechas / WASD"},
                {"Boton 1", "Z / J"},
                {"Boton 2", "X / K"},
                {"Pause (NMI)", "Enter / Espacio"},
            };
        case RomPlatform::GameGear:
            return {
                {"Cruceta", "Flechas / WASD"},
                {"Boton 1", "Z / J"},
                {"Boton 2", "X / K"},
                {"Start", "Enter / Espacio"},
            };
        case RomPlatform::NES:
            return {
                {"Cruceta", "Flechas / WASD"},
                {"Boton A", "X / J"},
                {"Boton B", "Z / K"},
                {"Start / Select", "Enter / Tab o Shift"},
            };
        case RomPlatform::SNES:
            return {
                {"Cruceta", "Flechas / WASD"},
                {"B / A", "Z / X (J / K)"},
                {"Y / X", "C / V (L / I)"},
                {"L / R", "Q / E"},
                {"Start / Select", "Enter / Tab o Shift"},
            };
        case RomPlatform::GameBoy:
        case RomPlatform::GameBoyColor:
            return {
                {"Cruceta", "Flechas / WASD"},
                {"Boton A / B", "Z / X (J / K)"},
                {"Start / Select", "Enter / Backspace o Shift"},
            };
        case RomPlatform::GameBoyAdvance:
            return {
                {"Cruceta", "Flechas / WASD"},
                {"Boton A / B", "Z / X (J / K)"},
                {"L / R", "Q / E (U / I)"},
                {"Start / Select", "Enter / Backspace o Shift"},
            };
        default:
            return {
                {"Cruceta", "Flechas / WASD"},
                {"Botones", "Z / X"},
                {"Start", "Enter"},
            };
    }
}

}  // namespace

void StudioApp::render_sidebar() {
    const float sidebar_w = 56.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::Surface);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("SidebarNav", ImVec2(sidebar_w, 0.0f), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 win_pos = ImGui::GetWindowPos();
    const ImVec2 win_sz = ImGui::GetWindowSize();

    // Right border separator
    dl->AddLine(ImVec2(win_pos.x + win_sz.x - 1.0f, win_pos.y),
                ImVec2(win_pos.x + win_sz.x - 1.0f, win_pos.y + win_sz.y),
                ImGui::GetColorU32(ThemeColors::Border), 1.0f);

    // Top "DR" Logo Badge (32x32 rounded 8)
    const ImVec2 logo_p0(win_pos.x + (sidebar_w - 32.0f) * 0.5f, win_pos.y + 12.0f);
    const ImVec2 logo_p1(logo_p0.x + 32.0f, logo_p0.y + 32.0f);
    dl->AddRectFilled(logo_p0, logo_p1, ImGui::GetColorU32(ThemeColors::Primary), 8.0f);
    ImFont* logo_font = fonts_.body_bold ? fonts_.body_bold : ImGui::GetFont();
    const ImVec2 dr_sz = logo_font->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, "DR");
    dl->AddText(logo_font, 15.0f,
                ImVec2(logo_p0.x + (32.0f - dr_sz.x) * 0.5f, logo_p0.y + (32.0f - dr_sz.y) * 0.5f),
                IM_COL32(255, 255, 255, 255), "DR");

    auto nav_button = [&](const char* id, const char* tooltip, StudioView view, float y,
                          void (*icon_fn)(ImDrawList*, ImVec2, ImU32)) {
        const float btn_sz = 40.0f;
        const ImVec2 bp0(win_pos.x + (sidebar_w - btn_sz) * 0.5f, win_pos.y + y);
        const ImVec2 bp1(bp0.x + btn_sz, bp0.y + btn_sz);
        ImGui::SetCursorScreenPos(bp0);
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(btn_sz, btn_sz));
        const bool hovered = ImGui::IsItemHovered();
        const bool active = (current_view_ == view);

        if (active) {
            dl->AddRectFilled(bp0, bp1, ImGui::GetColorU32(ThemeColors::PrimaryDim), 8.0f);
        } else if (hovered) {
            dl->AddRectFilled(bp0, bp1, ImGui::GetColorU32(ThemeColors::SurfaceAlt), 8.0f);
        }

        const ImU32 icon_col = active    ? ImGui::GetColorU32(ThemeColors::Primary)
                               : hovered ? ImGui::GetColorU32(ThemeColors::TextPrimary)
                                         : ImGui::GetColorU32(ThemeColors::TextMuted);
        icon_fn(dl, ImVec2(bp0.x + btn_sz * 0.5f, bp0.y + btn_sz * 0.5f), icon_col);

        if (hovered) {
            ImGui::SetTooltip("%s", tooltip);
        }
        if (clicked) {
            current_view_ = view;
            if (view == StudioView::Preferences) {
                draft_settings_ = settings_;
                pref_status_message_.clear();
            }
        }
    };

    nav_button("##nav_lib", "Biblioteca", StudioView::Library, 66.0f, &draw_grid_icon);
    nav_button("##nav_rec", "Recompilador", StudioView::Recompiler, 114.0f, &draw_bolt_icon);
    nav_button("##nav_pref", "Preferencias", StudioView::Preferences, 162.0f, &draw_gear_icon);

    // Bottom version info icon
    const ImVec2 info_p0(win_pos.x + (sidebar_w - 36.0f) * 0.5f, win_pos.y + win_sz.y - 48.0f);
    ImGui::SetCursorScreenPos(info_p0);
    ImGui::InvisibleButton("##info_ver", ImVec2(36.0f, 36.0f));
    const bool info_hov = ImGui::IsItemHovered();
    draw_info_icon(dl, ImVec2(info_p0.x + 18.0f, info_p0.y + 18.0f),
                   info_hov ? ImGui::GetColorU32(ThemeColors::TextPrimary)
                            : ImGui::GetColorU32(ThemeColors::TextMuted));
    if (info_hov) {
        ImGui::SetTooltip("DragonRecomp Studio v1.0.0\nNES · SNES · GB/GBC · GBA");
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void StudioApp::render_topbar() {
    const float topbar_h = std::max(52.0f, ImGui::GetFrameHeight() + 20.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::Surface);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("TopBar", ImVec2(0.0f, topbar_h), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 sz = ImGui::GetWindowSize();
    const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);

    // Bottom border
    dl->AddLine(ImVec2(p0.x, p1.y - 1.0f), ImVec2(p1.x, p1.y - 1.0f),
                ImGui::GetColorU32(ThemeColors::Border), 1.0f);

    // Brand Title: "Dragon" + "Recomp" (Primary) + " Studio"
    if (fonts_.h2)
        ImGui::PushFont(fonts_.h2);
    const float title_h = ImGui::GetTextLineHeight();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 20.0f, p0.y + (topbar_h - title_h) * 0.5f));
    ImGui::TextColored(ThemeColors::TextPrimary, "Dragon");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(ThemeColors::Primary, "Recomp");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(ThemeColors::TextPrimary, " Studio");
    if (fonts_.h2)
        ImGui::PopFont();

    // Right controls: Status pill + Primary CTA button
    char pill_text[64];
    std::snprintf(pill_text, sizeof(pill_text), "%u nucleos · %s", settings_.max_compile_threads,
                  opt_level_flag(settings_.opt_level));

    const char* cta_label =
        (current_view_ == StudioView::Library) ? "+ Importar Recompilado" : "+ Importar ROM";

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    if (fonts_.small)
        ImGui::PushFont(fonts_.small);
    const float small_h = ImGui::GetTextLineHeight();
    const float cta_w = ImGui::CalcTextSize(cta_label).x + 28.0f;
    const float pill_txt_w = ImGui::CalcTextSize(pill_text).x;
    if (fonts_.small)
        ImGui::PopFont();

    const float cta_h = std::max(32.0f, small_h + 14.0f);
    const float cta_x = p1.x - cta_w - 20.0f;
    const float cta_y = p0.y + (topbar_h - cta_h) * 0.5f;

    // Status pill to the left of CTA button
    const float pill_h = std::max(28.0f, small_h + 10.0f);
    const float pill_w = pill_txt_w + 34.0f;
    const float pill_x = cta_x - pill_w - 12.0f;
    const float pill_y = p0.y + (topbar_h - pill_h) * 0.5f;

    dl->AddRectFilled(ImVec2(pill_x, pill_y), ImVec2(pill_x + pill_w, pill_y + pill_h),
                      ImGui::GetColorU32(ThemeColors::SurfaceAlt), pill_h * 0.5f);
    dl->AddRect(ImVec2(pill_x, pill_y), ImVec2(pill_x + pill_w, pill_y + pill_h),
                ImGui::GetColorU32(ThemeColors::Border), pill_h * 0.5f);
    dl->AddCircleFilled(ImVec2(pill_x + 13.0f, pill_y + pill_h * 0.5f), 3.5f,
                        ImGui::GetColorU32(ThemeColors::SuccessGreen));
    ImFont* pill_font = fonts_.small ? fonts_.small : ImGui::GetFont();
    const float pill_font_sz = pill_font->LegacySize;
    const ImVec2 pt_sz = pill_font->CalcTextSizeA(pill_font_sz, FLT_MAX, 0.0f, pill_text);
    dl->AddText(pill_font, pill_font_sz,
                ImVec2(pill_x + 22.0f, pill_y + (pill_h - pt_sz.y) * 0.5f),
                ImGui::GetColorU32(ThemeColors::TextMuted), pill_text);

    // Primary CTA Button
    ImGui::SetCursorScreenPos(ImVec2(cta_x, cta_y));
    ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::Primary);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(std::min(ThemeColors::Primary.x * 1.1f, 1.f),
                                 std::min(ThemeColors::Primary.y * 1.1f, 1.f),
                                 std::min(ThemeColors::Primary.z * 1.1f, 1.f), 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColors::Primary);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
    if (fonts_.small)
        ImGui::PushFont(fonts_.small);
    if (ImGui::Button(cta_label, ImVec2(cta_w, cta_h))) {
        if (current_view_ == StudioView::Library) {
            open_import_portable_dialog();
        } else {
            open_rom_file_dialog();
        }
    }
    if (fonts_.small)
        ImGui::PopFont();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void StudioApp::render_recompiler() {
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float max_recomp_w = 1280.0f;
    const float total_w = std::min(avail_w, max_recomp_w);
    const float offset_x = std::max(0.0f, (avail_w - total_w) * 0.5f);
    if (offset_x > 0.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset_x);
    }

    const float side_w = std::clamp(total_w * 0.28f, 260.0f, 360.0f);
    const float gap = 18.0f;
    const float left_w = std::max(380.0f, total_w - side_w - gap);

    // =========================================================================
    // LEFT COLUMN: Drop-zone + Active ROM Recomp-Result Card (NO "ROMs de prueba")
    // =========================================================================
    ImGui::BeginChild("RecompLeft", ImVec2(left_w, 0.0f), false);

    // --- 1. Drop-Zone ---
    const float line_h = ImGui::GetTextLineHeight();
    const float content_h = line_h * 5.0f + 78.0f;
    const float drop_h = std::max(active_rom_.is_valid() ? 224.0f : 310.0f, content_h + 56.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::Surface);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::BeginChild("DropZoneCard", ImVec2(0.0f, drop_h), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 dz0 = ImGui::GetWindowPos();
    const ImVec2 dz1(dz0.x + ImGui::GetWindowWidth(), dz0.y + ImGui::GetWindowHeight());
    const bool dz_hov = ImGui::IsWindowHovered();
    draw_dashed_rect(dl, ImVec2(dz0.x + 2.f, dz0.y + 2.f), ImVec2(dz1.x - 2.f, dz1.y - 2.f),
                     dz_hov ? ImGui::GetColorU32(ThemeColors::Primary)
                            : ImGui::GetColorU32(ThemeColors::BorderStrong),
                     1.6f, 8.0f, 6.0f);

    const float pad_top = std::max(20.0f, (drop_h - content_h) * 0.5f);
    ImGui::SetCursorPosY(pad_top);

    // Heading
    if (fonts_.h2)
        ImGui::PushFont(fonts_.h2);
    const char* dz_title = "Arrastra tu ROM aqui";
    const float dz_tw = ImGui::CalcTextSize(dz_title).x;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - dz_tw) * 0.5f);
    ImGui::TextColored(ThemeColors::TextPrimary, "%s", dz_title);
    if (fonts_.h2)
        ImGui::PopFont();

    ImGui::Dummy(ImVec2(0.f, 6.f));

    // Format chips (.nes, .sfc, .smc, .gb, .gbc, .gba, .sms, .gg, .pce)
    const char* chips[] = {".nes", ".sfc", ".smc", ".gb", ".gbc", ".gba", ".sms", ".gg", ".pce"};
    constexpr int kChipCount = 9;
    if (fonts_.small)
        ImGui::PushFont(fonts_.small);
    float total_chips_w = 0.0f;
    for (int i = 0; i < kChipCount; ++i) {
        total_chips_w += ImGui::CalcTextSize(chips[i]).x + 18.0f;
        if (i > 0)
            total_chips_w += 6.0f;
    }
    float cx = dz0.x + (ImGui::GetWindowWidth() - total_chips_w) * 0.5f;
    const float cy = ImGui::GetCursorScreenPos().y;
    for (int i = 0; i < kChipCount; ++i) {
        const ImVec2 csz = ImGui::CalcTextSize(chips[i]);
        const float cw = csz.x + 18.0f;
        const float ch = csz.y + 8.0f;
        dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + cw, cy + ch),
                          ImGui::GetColorU32(ThemeColors::SurfaceAlt), 5.0f);
        dl->AddRect(ImVec2(cx, cy), ImVec2(cx + cw, cy + ch),
                    ImGui::GetColorU32(ThemeColors::Border), 5.0f);
        dl->AddText(ImVec2(cx + 9.0f, cy + 4.0f), ImGui::GetColorU32(ThemeColors::TextMuted),
                    chips[i]);
        cx += cw + 6.0f;
    }
    if (fonts_.small)
        ImGui::PopFont();

    ImGui::Dummy(ImVec2(0.f, 34.f));

    // Cyan "Seleccionar archivo" button
    const char* pick_lbl = "Seleccionar archivo";
    const float pick_w = 190.0f;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - pick_w) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::AccentCyanDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(ThemeColors::AccentCyan.x, ThemeColors::AccentCyan.y,
                                 ThemeColors::AccentCyan.z, 0.24f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColors::AccentCyanDim);
    ImGui::PushStyleColor(ImGuiCol_Border, ThemeColors::AccentCyan);
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::AccentCyan);
    if (fonts_.body_bold)
        ImGui::PushFont(fonts_.body_bold);
    if (ImGui::Button(pick_lbl, ImVec2(pick_w, 36.0f))) {
        open_rom_file_dialog();
    }
    if (fonts_.body_bold)
        ImGui::PopFont();
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);

    ImGui::Dummy(ImVec2(0.f, 6.f));
    if (fonts_.small)
        ImGui::PushFont(fonts_.small);
    const char* auto_lbl = "Deteccion automatica de plataforma";
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(auto_lbl).x) * 0.5f);
    ImGui::TextColored(ThemeColors::TextMuted, "%s", auto_lbl);
    if (fonts_.small)
        ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // --- 2. Active ROM Recomp-Result Card ---
    if (active_rom_.is_valid()) {
        ImGui::Dummy(ImVec2(0.f, 14.f));

        const PipelineStage stage = pipeline_.stage();
        const auto& stats = pipeline_.stats();
        const bool is_ready = (stage == PipelineStage::Ready);
        const bool is_failed = (stage == PipelineStage::Failed);
        const bool is_compiling =
            (stage == PipelineStage::Analyzing || stage == PipelineStage::Disassembling ||
             stage == PipelineStage::Emitting || stage == PipelineStage::CompilingNative);
        const bool is_running = pipeline_.is_game_running();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::Surface);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
        ImGui::BeginChild("RecompResultCard", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);

        // Card Header: Title + Platform badge + Status badge + SHA-1
        if (fonts_.h2)
            ImGui::PushFont(fonts_.h2);
        ImGui::TextColored(ThemeColors::TextPrimary, "%s", active_rom_.clean_name.c_str());
        if (fonts_.h2)
            ImGui::PopFont();

        ImGui::SameLine(0, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::PrimaryDim);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::Primary);
        if (fonts_.small)
            ImGui::PushFont(fonts_.small);
        ImGui::SmallButton(rom_platform_badge(active_rom_.platform));
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, 8.0f);
        const char* st_lbl = is_ready       ? "Listo"
                             : is_failed    ? "Error"
                             : is_compiling ? "Compilando..."
                                            : "Preparando";
        const ImVec4 st_col = is_ready    ? ThemeColors::SuccessGreen
                              : is_failed ? ThemeColors::DangerRed
                                          : ThemeColors::Primary;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(st_col.x, st_col.y, st_col.z, 0.16f));
        ImGui::PushStyleColor(ImGuiCol_Text, st_col);
        ImGui::SmallButton(st_lbl);
        ImGui::PopStyleColor(2);

        // Right-aligned short SHA-1 + Clear button
        char sha_buf[24];
        std::snprintf(sha_buf, sizeof(sha_buf), "SHA-1 %.8s", active_rom_.sha1.c_str());
        const float sha_w = ImGui::CalcTextSize(sha_buf).x;
        ImGui::SameLine(
            std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - sha_w - 72.0f));
        ImGui::TextColored(ThemeColors::TextMuted, "%s", sha_buf);
        ImGui::SameLine(0, 8.0f);
        if (ImGui::SmallButton("Limpiar")) {
            pipeline_.cancel();
            active_rom_ = RomInfo{};
            export_status_message_.clear();
            last_package_result_ = PackageResult{};
            diagnostic_status_message_.clear();
        }
        if (fonts_.small)
            ImGui::PopFont();
        ImGui::PopStyleVar();

        ImGui::Dummy(ImVec2(0.f, 8.f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.f, 12.f));

        // Body: 2 columns (Metrics/Progress/Options on left, Action Stack on right)
        const float body_w = ImGui::GetContentRegionAvail().x;
        const float act_col_w = 186.0f;
        const float info_col_w = std::max(220.0f, body_w - act_col_w - 18.0f);

        ImGui::BeginGroup();
        ImGui::PushItemWidth(info_col_w);

        // 3-column metrics strip
        const float m_gap = 10.0f;
        const float m_w = (info_col_w - m_gap * 2.0f) / 3.0f;
        char fn_buf[32], ins_buf[32], time_buf[32];
        std::snprintf(fn_buf, sizeof(fn_buf), "%zu", stats.functions);
        std::snprintf(ins_buf, sizeof(ins_buf), "%zu", stats.instructions);
        std::snprintf(time_buf, sizeof(time_buf), "%.1fs", stats.compilation_time_sec);
        const char* m_labels[3] = {"Funciones C99", "Instrucciones", "Compilacion"};
        const char* m_vals[3] = {fn_buf, ins_buf, time_buf};

        for (int i = 0; i < 3; ++i) {
            if (i > 0)
                ImGui::SameLine(0, m_gap);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::SurfaceAlt);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
            ImGui::BeginChild(m_labels[i], ImVec2(m_w, 0.0f), ImGuiChildFlags_AutoResizeY,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            if (fonts_.small)
                ImGui::PushFont(fonts_.small);
            ImGui::TextColored(ThemeColors::TextMuted, "%s", m_labels[i]);
            if (fonts_.small)
                ImGui::PopFont();
            if (fonts_.h2)
                ImGui::PushFont(fonts_.h2);
            ImGui::TextColored(ThemeColors::TextPrimary, "%s", m_vals[i]);
            if (fonts_.h2)
                ImGui::PopFont();
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        }

        ImGui::Dummy(ImVec2(0.f, 12.f));

        // Gradient Progress Bar (Primary -> AccentCyan)
        const float prog = is_ready ? 1.0f : std::clamp(pipeline_.progress(), 0.0f, 1.0f);
        ImDrawList* rdl = ImGui::GetWindowDrawList();
        const ImVec2 pb0 = ImGui::GetCursorScreenPos();
        const ImVec2 pb1(pb0.x + info_col_w, pb0.y + 8.0f);
        rdl->AddRectFilled(pb0, pb1, ImGui::GetColorU32(ThemeColors::SurfaceAlt), 4.0f);
        if (prog > 0.005f) {
            const ImVec2 pf1(pb0.x + info_col_w * prog, pb0.y + 8.0f);
            const ImU32 c_l = ImGui::GetColorU32(ThemeColors::Primary);
            const ImU32 c_r = ImGui::GetColorU32(ThemeColors::AccentCyan);
            rdl->AddRectFilledMultiColor(pb0, pf1, c_l, c_r, c_r, c_l);
        }
        ImGui::Dummy(ImVec2(info_col_w, 12.0f));

        if (fonts_.small)
            ImGui::PushFont(fonts_.small);
        if (is_failed) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + info_col_w);
            ImGui::TextColored(ThemeColors::DangerRed, "%s", pipeline_.error_message().c_str());
            ImGui::PopTextWrapPos();
        } else {
            ImGui::TextColored(ThemeColors::TextMuted, "%s",
                               pipeline_.current_step_description().c_str());
        }
        if (fonts_.small)
            ImGui::PopFont();

        ImGui::Dummy(ImVec2(0.f, 8.f));

        // Quick launch checkboxes
        ImGui::Checkbox("Pantalla completa", &fullscreen_option_);
        ImGui::SameLine(0, 16.0f);
        ImGui::Checkbox("Filtro CRT", &crt_filter_option_);

        // Learned fallbacks banner if any
        const size_t pending = pipeline_.pending_learn_count();
        if (is_ready && !is_running && pending > 0) {
            ImGui::Dummy(ImVec2(0.f, 8.f));
            ImGui::TextColored(ThemeColors::WarningAmber, "Se aprendieron %zu nuevas rutinas.",
                               pending);
            ImGui::SameLine(0, 10.0f);
            if (ImGui::SmallButton("Optimizar y Recompilar")) {
                pipeline_.optimize_and_recompile();
            }
        }

        const auto game_err = pipeline_.last_game_error();
        if (!game_err.empty() && !is_running) {
            ImGui::Dummy(ImVec2(0.f, 6.f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + info_col_w);
            ImGui::TextColored(ThemeColors::WarningAmber, "Aviso: %s", game_err.c_str());
            ImGui::PopTextWrapPos();
        }

        if (!export_status_message_.empty()) {
            ImGui::Dummy(ImVec2(0.f, 8.f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + info_col_w);
            ImGui::TextColored(
                last_package_result_.success ? ThemeColors::SuccessGreen : ThemeColors::DangerRed,
                "%s", export_status_message_.c_str());
            ImGui::PopTextWrapPos();
            if (last_package_result_.success) {
                if (ImGui::SmallButton("Abrir carpeta")) {
                    open_dir_os(last_package_result_.package_path);
                }
                ImGui::SameLine(0, 8.0f);
                if (ImGui::SmallButton("Ver en Biblioteca")) {
                    current_view_ = StudioView::Library;
                }
            }
        }

        ImGui::PopItemWidth();
        ImGui::EndGroup();

        ImGui::SameLine(0, 18.0f);

        // Right Action Stack inside Recomp-Result Card
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        if (fonts_.body_bold)
            ImGui::PushFont(fonts_.body_bold);

        if (is_compiling) {
            ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::SurfaceAlt);
            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::DangerRed);
            if (ImGui::Button("Cancelar", ImVec2(act_col_w, 38.0f))) {
                pipeline_.cancel();
                active_rom_ = RomInfo{};
            }
            ImGui::PopStyleColor(2);
        } else if (is_failed) {
            ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::Primary);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
            if (ImGui::Button("Reintentar", ImVec2(act_col_w, 38.0f))) {
                pipeline_.start_pipeline(active_rom_);
            }
            ImGui::PopStyleColor(2);
        } else if (is_ready) {
            // 1. Primary Play button
            ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::Primary);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(std::min(ThemeColors::Primary.x * 1.1f, 1.f),
                                         std::min(ThemeColors::Primary.y * 1.1f, 1.f),
                                         std::min(ThemeColors::Primary.z * 1.1f, 1.f), 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColors::Primary);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
            if (is_running) {
                ImGui::Button("● En ejecucion...", ImVec2(act_col_w, 38.0f));
            } else {
                if (ImGui::Button("▶  Jugar (60 FPS)", ImVec2(act_col_w, 38.0f))) {
                    pipeline_.launch_game(fullscreen_option_, crt_filter_option_,
                                          settings_.auto_save_state);
                }
            }
            ImGui::PopStyleColor(4);

            ImGui::Dummy(ImVec2(0.f, 4.f));

            // 2. Export portable (opens modal pre-filled with settings_.export_directory;
            //    never exports until user clicks "Exportar Ahora")
            ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::SurfaceAlt);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColors::SurfaceHover);
            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
            if (ImGui::Button("Exportar portable", ImVec2(act_col_w, 34.0f))) {
                if (!settings_.export_directory.empty()) {
                    set_export_directory(settings_.export_directory.string());
                } else if (export_dir_buf_[0] == '\0') {
                    set_export_directory(SettingsStore::defaults().export_directory.string());
                }
                last_package_result_ = PackageResult{};
                export_status_message_.clear();
                show_package_modal_ = true;
            }
            ImGui::PopStyleColor(3);
        }

        if (fonts_.body_bold)
            ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::EndGroup();

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        // --- Console Controls Card (shown right after recompilation completes) ---
        if (is_ready) {
            ImGui::Dummy(ImVec2(0.f, 12.f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::Surface);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 14.0f));
            ImGui::BeginChild("ConsoleControlsCard", ImVec2(0.0f, 0.0f),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

            if (fonts_.body_bold)
                ImGui::PushFont(fonts_.body_bold);
            ImGui::TextColored(ThemeColors::TextPrimary, "Controles — %s",
                               rom_platform_display_name(active_rom_.platform));
            if (fonts_.body_bold)
                ImGui::PopFont();

            ImGui::Dummy(ImVec2(0.f, 6.f));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0.f, 8.f));

            const auto pad_rows = console_controls_for(active_rom_.platform);
            static const ControlRow kSysRows[] = {
                {"Guardar / Cargar", "F5 / F7 (0-9)"},
                {"Pantalla completa", "F11"},
                {"Filtro CRT / Aspecto", "F9 / F8"},
                {"Salir del juego", "Esc"},
            };

            auto draw_binding_table = [&](const char* id, const char* header,
                                          const ControlRow* rows, size_t count) {
                if (fonts_.small)
                    ImGui::PushFont(fonts_.small);
                ImGui::TextColored(ThemeColors::AccentCyan, "%s", header);
                ImGui::Dummy(ImVec2(0.f, 4.f));
                if (ImGui::BeginTable(id, 2,
                                      ImGuiTableFlags_SizingStretchProp |
                                          ImGuiTableFlags_NoSavedSettings)) {
                    ImGui::TableSetupColumn("Accion", ImGuiTableColumnFlags_WidthStretch, 0.48f);
                    ImGui::TableSetupColumn("Teclas", ImGuiTableColumnFlags_WidthStretch, 0.52f);
                    for (size_t i = 0; i < count; ++i) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextColored(ThemeColors::TextMuted, "%s", rows[i].action);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextColored(ThemeColors::TextPrimary, "%s", rows[i].keys);
                    }
                    ImGui::EndTable();
                }
                if (fonts_.small)
                    ImGui::PopFont();
            };

            const float ctrl_w = ImGui::GetContentRegionAvail().x;
            if (ctrl_w >= 460.0f) {
                if (ImGui::BeginTable("ControlsTwoCol", 2,
                                      ImGuiTableFlags_SizingStretchSame |
                                          ImGuiTableFlags_NoSavedSettings)) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    draw_binding_table("PadTbl", "MANDO", pad_rows.data(), pad_rows.size());
                    ImGui::TableSetColumnIndex(1);
                    draw_binding_table("SysTbl", "ATAJOS", kSysRows,
                                       sizeof(kSysRows) / sizeof(kSysRows[0]));
                    ImGui::EndTable();
                }
            } else {
                draw_binding_table("PadTblStack", "MANDO", pad_rows.data(), pad_rows.size());
                ImGui::Dummy(ImVec2(0.f, 8.f));
                draw_binding_table("SysTblStack", "ATAJOS", kSysRows,
                                   sizeof(kSysRows) / sizeof(kSysRows[0]));
            }

            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        }
    }

    ImGui::EndChild();

    ImGui::SameLine(0, gap);

    // =========================================================================
    // RIGHT COLUMN (276px): Log de compilación + Acciones de recuperación
    // =========================================================================
    ImGui::BeginChild("RecompSide", ImVec2(side_w, 0.0f), false);

    // --- Card 1: Log de compilación ---
    const float avail_h = ImGui::GetContentRegionAvail().y;
    const float log_card_h = std::max(210.0f, avail_h * 0.54f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::Surface);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 14.0f));
    ImGui::BeginChild("CompileLogCard", ImVec2(0.0f, log_card_h), true);

    if (fonts_.body_bold)
        ImGui::PushFont(fonts_.body_bold);
    ImGui::TextColored(ThemeColors::TextPrimary, "Log de compilacion");
    if (fonts_.body_bold)
        ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.f, 6.f));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::BgApp);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::BeginChild("CompileLogInner", ImVec2(0.0f, 0.0f), true);
    if (fonts_.small)
        ImGui::PushFont(fonts_.small);
    const auto logs = pipeline_.compile_log();
    if (logs.empty()) {
        ImGui::TextColored(ThemeColors::TextFaint, "Esperando ROM...");
    } else {
        for (const auto& entry : logs) {
            ImGui::TextColored(ThemeColors::TextFaint, "%s", entry.time.c_str());
            ImGui::SameLine(0, 6.0f);
            ImVec4 msg_col = ThemeColors::TextPrimary;
            if (entry.level == LogLevel::Info)
                msg_col = ThemeColors::AccentCyan;
            else if (entry.level == LogLevel::Success)
                msg_col = ThemeColors::SuccessGreen;
            else if (entry.level == LogLevel::Warning)
                msg_col = ThemeColors::WarningAmber;
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(msg_col, "%s", entry.message.c_str());
            ImGui::PopTextWrapPos();
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    if (fonts_.small)
        ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0.f, 12.f));

    // --- Card 2: Acciones de recuperación ---
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::Surface);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 14.0f));
    ImGui::BeginChild("RecoveryCard", ImVec2(0.0f, 0.0f), true);

    if (fonts_.body_bold)
        ImGui::PushFont(fonts_.body_bold);
    ImGui::TextColored(ThemeColors::TextPrimary, "Acciones de recuperacion");
    if (fonts_.body_bold)
        ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.f, 6.f));

    const char* symptoms[] = {"Pantalla gris",
                              "Pantalla azul",
                              "Juego congelado al cargar",
                              "Juego congelado al jugar",
                              "Juego no abre / Se cierra",
                              "Sin sonido",
                              "Sonido distorsionado",
                              "Colores / Paleta extrana",
                              "Controles no responden",
                              "Graficos corruptos",
                              "Velocidad anormal"};

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("##SymptomCombo", &selected_symptom_idx_, symptoms, IM_ARRAYSIZE(symptoms));
    ImGui::Dummy(ImVec2(0.f, 8.f));

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::Primary);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(std::min(ThemeColors::Primary.x * 1.1f, 1.f),
                                 std::min(ThemeColors::Primary.y * 1.1f, 1.f),
                                 std::min(ThemeColors::Primary.z * 1.1f, 1.f), 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColors::Primary);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
    if (fonts_.body_bold)
        ImGui::PushFont(fonts_.body_bold);
    if (ImGui::Button("Intentar accion", ImVec2(-1.0f, 34.0f))) {
        if (!active_rom_.is_valid()) {
            diagnostic_status_success_ = false;
            diagnostic_status_message_ = "Carga primero una ROM para aplicar una accion.";
        } else {
            DiagnosticResult res = pipeline_.diagnose_and_repair(
                static_cast<DiagnosticSymptom>(selected_symptom_idx_));
            diagnostic_status_success_ = res.success;
            diagnostic_status_message_ = res.summary;
            if (!res.details.empty()) {
                diagnostic_status_message_ += "\n" + res.details;
            }
        }
    }
    if (fonts_.body_bold)
        ImGui::PopFont();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    if (!diagnostic_status_message_.empty()) {
        ImGui::Dummy(ImVec2(0.f, 8.f));
        if (fonts_.small)
            ImGui::PushFont(fonts_.small);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(
            diagnostic_status_success_ ? ThemeColors::SuccessGreen : ThemeColors::WarningAmber,
            "%s", diagnostic_status_message_.c_str());
        ImGui::PopTextWrapPos();
        if (fonts_.small)
            ImGui::PopFont();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::EndChild();
}

void StudioApp::render_preferences() {
    ImGui::BeginChild("PreferencesScroll", ImVec2(0.0f, 0.0f), false);

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float container_w = std::min(avail_w, std::clamp(avail_w * 0.72f, 560.0f, 820.0f));
    const float offset_x = std::max(0.0f, (avail_w - container_w) * 0.5f);

    ImGui::SetCursorPosX(offset_x);
    ImGui::BeginGroup();

    auto section_header = [&](const char* title) {
        if (fonts_.small)
            ImGui::PushFont(fonts_.small);
        ImGui::TextColored(ThemeColors::AccentCyan, "%s", title);
        if (fonts_.small)
            ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.f, 6.f));
    };

    auto begin_setting_card = [&](const char* id) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::Surface);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 14.0f));
        ImGui::BeginChild(id, ImVec2(container_w, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    };

    auto end_setting_card = [&]() {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.f, 16.f));
    };

    // =========================================================================
    // 1. COMPILACION
    // =========================================================================
    section_header("COMPILACION");
    begin_setting_card("PrefCompCard");
    {
        const float inner_right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

        // Row 1: Nucleos maximos
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ThemeColors::TextPrimary, "Nucleos maximos");
        int threads = static_cast<int>(draft_settings_.max_compile_threads);
        const float count_txt_w = ImGui::CalcTextSize("16 nucleos").x;
        const float slider_w = std::clamp(container_w * 0.28f, 130.0f, 220.0f);
        const float row1_ctrl_x = std::max(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 12.0f,
                                           inner_right - slider_w - 10.0f - count_txt_w);
        ImGui::SameLine(row1_ctrl_x);
        ImGui::SetNextItemWidth(slider_w);
        if (ImGui::SliderInt("##PrefThreads", &threads, 1, 16, "")) {
            draft_settings_.max_compile_threads = static_cast<unsigned int>(threads);
        }
        ImGui::SameLine(0, 10.0f);
        ImGui::TextColored(ThemeColors::TextMuted, "%d nucleos", threads);

        ImGui::Dummy(ImVec2(0.f, 8.f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.f, 8.f));

        // Row 2: Nivel de optimizacion
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ThemeColors::TextPrimary, "Nivel de optimizacion");
        const char* opt_items[] = {"-O0 (Sin opt.)", "-O1 (Rapido)", "-O2 (Balanceado)",
                                   "-O3 (Agresivo)"};
        int opt_idx = static_cast<int>(draft_settings_.opt_level);
        const float combo_w =
            std::max(std::clamp(container_w * 0.34f, 195.0f, 250.0f),
                     ImGui::CalcTextSize("-O2 (Balanceado)").x + ImGui::GetFrameHeight() + 24.0f);
        const float row2_ctrl_x = std::max(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 12.0f,
                                           inner_right - combo_w);
        ImGui::SameLine(row2_ctrl_x);
        ImGui::SetNextItemWidth(combo_w);
        if (ImGui::Combo("##PrefOpt", &opt_idx, opt_items, IM_ARRAYSIZE(opt_items))) {
            draft_settings_.opt_level = static_cast<CompileOptLevel>(std::clamp(opt_idx, 0, 3));
        }
    }
    end_setting_card();

    // =========================================================================
    // 2. RUTAS (ONLY Directorio de exportacion — NO Directorio de biblioteca)
    // =========================================================================
    section_header("RUTAS");
    begin_setting_card("PrefPathsCard");
    {
        const float inner_x = ImGui::GetCursorPosX();
        const float inner_w = ImGui::GetContentRegionAvail().x;
        const float btn_w = std::max(98.0f, ImGui::CalcTextSize("Cambiar").x + 30.0f);
        const float btn_h = std::max(32.0f, ImGui::GetFrameHeight() + 4.0f);
        const float text_max_w = std::max(160.0f, inner_w - btn_w - 16.0f);

        const std::string exp_str = draft_settings_.export_directory.empty()
                                        ? SettingsStore::defaults().export_directory.string()
                                        : draft_settings_.export_directory.string();
        ImGui::BeginGroup();
        ImGui::TextColored(ThemeColors::TextPrimary, "Directorio de exportacion");
        if (fonts_.small)
            ImGui::PushFont(fonts_.small);
        ImGui::PushTextWrapPos(inner_x + text_max_w);
        ImGui::TextColored(ThemeColors::TextMuted, "%s", exp_str.c_str());
        ImGui::PopTextWrapPos();
        if (fonts_.small)
            ImGui::PopFont();
        ImGui::EndGroup();

        ImGui::SameLine(inner_x + inner_w - btn_w);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        if (ImGui::Button("Cambiar", ImVec2(btn_w, btn_h))) {
            open_pref_export_dir_dialog();
        }
        ImGui::PopStyleVar();
    }
    end_setting_card();

    // =========================================================================
    // 3. APARIENCIA
    // =========================================================================
    section_header("APARIENCIA");
    begin_setting_card("PrefAppearanceCard");
    {
        const float inner_right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const float toggle_w = 40.0f;

        // Row 1: Modo oscuro
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ThemeColors::TextPrimary, "Modo oscuro");
        ImGui::SameLine(inner_right - toggle_w);
        if (toggle_switch("##PrefDarkMode", &draft_settings_.dark_mode)) {
            apply_studio_theme(draft_settings_.dark_mode, draft_settings_.accent_color,
                               display_scale_);
        }

        ImGui::Dummy(ImVec2(0.f, 8.f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.f, 8.f));

        // Row 2: Color de acento (4 circular swatches)
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ThemeColors::TextPrimary, "Color de acento");
        const float swatches_w = 4.0f * 24.0f + 3.0f * 10.0f;
        ImGui::SameLine(inner_right - swatches_w);

        const ImVec4 swatch_cols[4] = {
            ImVec4(0.941f, 0.486f, 0.118f, 1.0f),  // Orange #F07C1E
            ImVec4(0.118f, 0.784f, 0.831f, 1.0f),  // Cyan   #1EC8D4
            ImVec4(0.608f, 0.349f, 0.714f, 1.0f),  // Purple #9B59B6
            ImVec4(0.180f, 0.800f, 0.443f, 1.0f),  // Green  #2ECC71
        };
        const char* swatch_ids[4] = {"##sw_orange", "##sw_cyan", "##sw_purple", "##sw_green"};

        ImDrawList* sdl = ImGui::GetWindowDrawList();
        for (int i = 0; i < 4; ++i) {
            if (i > 0)
                ImGui::SameLine(0, 10.0f);
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton(swatch_ids[i], ImVec2(24.0f, 24.0f))) {
                draft_settings_.accent_color = static_cast<AccentColor>(i);
                apply_studio_theme(draft_settings_.dark_mode, draft_settings_.accent_color,
                                   display_scale_);
            }
            const ImVec2 c(sp.x + 12.0f, sp.y + 12.0f);
            sdl->AddCircleFilled(c, 10.5f, ImGui::GetColorU32(swatch_cols[i]), 24);
            if (static_cast<int>(draft_settings_.accent_color) == i) {
                sdl->AddCircle(c, 12.0f, ImGui::GetColorU32(ThemeColors::TextPrimary), 24, 2.0f);
            }
        }
    }
    end_setting_card();

    // =========================================================================
    // 4. JUEGO
    // =========================================================================
    section_header("JUEGO");
    begin_setting_card("PrefGameCard");
    {
        const float inner_right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const float toggle_w = 40.0f;

        // Row 1: Autoguardado de estado
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ThemeColors::TextPrimary, "Autoguardado de estado");
        ImGui::SameLine(inner_right - toggle_w);
        toggle_switch("##PrefAutoSave", &draft_settings_.auto_save_state);

        ImGui::Dummy(ImVec2(0.f, 8.f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.f, 8.f));

        // Row 2: Filtro CRT por defecto
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ThemeColors::TextPrimary, "Filtro CRT por defecto");
        ImGui::SameLine(inner_right - toggle_w);
        toggle_switch("##PrefDefaultCrt", &draft_settings_.default_crt_filter);
    }
    end_setting_card();

    // =========================================================================
    // Bottom Actions: Guardar cambios / Restablecer todo
    // =========================================================================
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    if (fonts_.body_bold)
        ImGui::PushFont(fonts_.body_bold);

    const float action_h = std::max(38.0f, ImGui::GetFrameHeight() + 10.0f);
    const float save_w = std::max(170.0f, ImGui::CalcTextSize("Guardar cambios").x + 36.0f);
    const float reset_w = std::max(170.0f, ImGui::CalcTextSize("Restablecer todo").x + 36.0f);

    ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::Primary);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(std::min(ThemeColors::Primary.x * 1.1f, 1.f),
                                 std::min(ThemeColors::Primary.y * 1.1f, 1.f),
                                 std::min(ThemeColors::Primary.z * 1.1f, 1.f), 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColors::Primary);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
    if (ImGui::Button("Guardar cambios", ImVec2(save_w, action_h))) {
        settings_ = draft_settings_;
        apply_settings(true);
        pref_status_message_ = "Cambios guardados correctamente.";
    }
    ImGui::PopStyleColor(4);

    ImGui::SameLine(0, 12.0f);

    ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::SurfaceAlt);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColors::SurfaceHover);
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
    if (ImGui::Button("Restablecer todo", ImVec2(reset_w, action_h))) {
        draft_settings_ = SettingsStore::defaults();
        settings_ = draft_settings_;
        apply_settings(true);
        pref_status_message_ = "Preferencias restablecidas a los valores por defecto.";
    }
    ImGui::PopStyleColor(3);

    if (fonts_.body_bold)
        ImGui::PopFont();
    ImGui::PopStyleVar();

    if (!pref_status_message_.empty()) {
        ImGui::Dummy(ImVec2(0.f, 10.f));
        ImGui::TextColored(ThemeColors::SuccessGreen, "%s", pref_status_message_.c_str());
    }

    ImGui::Dummy(ImVec2(0.f, 12.f));
    ImGui::EndGroup();
    ImGui::EndChild();
}

}  // namespace dragon::studio
