// Shelves-by-console library view (layout 3 of plan-studio-library-ui).
//
// Boxart covers are loaded dynamically via CoverManager (libretro-thumbnails or user local files).
// When unavailable, cards fall back to a procedural per-console gradient tinted by the SHA-1.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

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
    return ImGui::GetColorU32(
        ImVec4(std::clamp(c.x * k, 0.f, 1.f), std::clamp(c.y * k, 0.f, 1.f),
               std::clamp(c.z * k, 0.f, 1.f), a));
}

ImVec4 platform_color(const std::string& key) {
    if (key == "nes") return ImVec4(0.75f, 0.22f, 0.17f, 1.f);
    if (key == "snes") return ImVec4(0.56f, 0.27f, 0.68f, 1.f);
    if (key == "gb") return ImVec4(0.50f, 0.60f, 0.17f, 1.f);
    if (key == "gbc") return ImVec4(0.10f, 0.62f, 0.55f, 1.f);
    if (key == "gba") return ImVec4(0.17f, 0.37f, 0.70f, 1.f);
    return ImVec4(0.35f, 0.38f, 0.45f, 1.f);
}

const char* platform_label(const std::string& key) {
    if (key == "nes") return "NES";
    if (key == "snes") return "SNES";
    if (key == "gb") return "GB";
    if (key == "gbc") return "GBC";
    if (key == "gba") return "GBA";
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
    if (s <= 0) return "Sin jugar";
    const int64_t h = s / 3600;
    const int64_t m = (s % 3600) / 60;
    char buf[48];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%lld h %02lld m", static_cast<long long>(h),
                      static_cast<long long>(m));
    } else {
        std::snprintf(buf, sizeof(buf), "%lld min", static_cast<long long>(std::max<int64_t>(m, 1)));
    }
    return buf;
}

void draw_pill(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg) {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const ImVec2 p1(pos.x + sz.x + 12.f, pos.y + sz.y + 4.f);
    dl->AddRectFilled(pos, p1, bg, 99.f);
    dl->AddText(ImVec2(pos.x + 6.f, pos.y + 2.f), fg, text);
}

// Draws one game card. Returns true when clicked. Context menu outputs action flags.
bool draw_game_card(SDL_Renderer* renderer, CoverManager& covers, const GameEntry& g,
                    ImVec2 size, bool* remove, bool* req_download, bool* req_manual,
                    bool* req_reset) {
    ImGui::PushID(g.sha1.c_str());
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const bool clicked = ImGui::InvisibleButton("##card", size);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 base = platform_color(g.platform);
    const float tint = sha_tint(g.sha1);
    const float lift = hovered ? -4.f : 0.f;
    const ImVec2 q0(p0.x, p0.y + lift);
    const ImVec2 q1(p1.x, p1.y + lift);

    SDL_Texture* tex = covers.get_texture(renderer, g.sha1, g.cover_path);
    if (tex) {
        dl->AddImageRounded((ImTextureID)(intptr_t)tex, q0, q1, ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, 8.f);
        // Bottom overlay to make title and playtime legible over any cover artwork
        dl->AddRectFilled(ImVec2(q0.x, q0.y + size.y * 0.40f), q1, IM_COL32(0, 0, 0, 190), 8.f,
                          ImDrawFlags_RoundCornersBottom);
    } else {
        dl->AddRectFilled(q0, q1, scaled(base, tint), 8.f);
        dl->AddRectFilled(ImVec2(q0.x, q0.y + size.y * 0.45f), q1, IM_COL32(0, 0, 0, 150), 8.f,
                          ImDrawFlags_RoundCornersBottom);
    }

    dl->AddRect(q0, q1,
                hovered ? ImGui::GetColorU32(ThemeColors::AccentCyan)
                        : ImGui::GetColorU32(ThemeColors::Border),
                8.f, 0, hovered ? 2.f : 1.f);

    float badge_y = q0.y + 8.f;
    draw_pill(dl, ImVec2(q0.x + 8.f, badge_y), platform_label(g.platform),
              IM_COL32(0, 0, 0, 180), IM_COL32(255, 255, 255, 255));
    badge_y += 24.f;

    if (g.mode == GameMode::Interpreter) {
        draw_pill(dl, ImVec2(q0.x + 8.f, badge_y), "Interprete", IM_COL32(60, 40, 5, 220),
                  ImGui::GetColorU32(ThemeColors::WarningAmber));
        badge_y += 24.f;
    } else if (g.mode == GameMode::Native) {
        draw_pill(dl, ImVec2(q0.x + 8.f, badge_y), "Nativo", IM_COL32(5, 50, 30, 220),
                  ImGui::GetColorU32(ThemeColors::SuccessGreen));
        badge_y += 24.f;
    }

    if (covers.is_downloading(g.sha1)) {
        draw_pill(dl, ImVec2(q0.x + 8.f, badge_y), "Descargando...", IM_COL32(10, 30, 50, 230),
                  ImGui::GetColorU32(ThemeColors::AccentCyan));
    }

    // Title, wrapped and clipped to the card.
    dl->PushClipRect(ImVec2(q0.x + 6.f, q0.y), ImVec2(q1.x - 6.f, q1.y - 22.f), true);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(q0.x + 8.f, q1.y - 22.f - ImGui::GetFontSize() * 2.6f),
                IM_COL32(255, 255, 255, 255), g.title.c_str(), nullptr, size.x - 16.f);
    dl->PopClipRect();
    dl->AddText(ImVec2(q0.x + 8.f, q1.y - 20.f), ImGui::GetColorU32(ThemeColors::TextMuted),
                format_playtime(g.playtime_s).c_str());

    if (ImGui::BeginPopupContextItem("##ctx")) {
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
    return clicked;
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
}

void StudioApp::render_library() {
    ImGui::BeginChild("LibraryView", ImVec2(0, 0), false);

    std::string open_sha;
    std::string remove_sha;
    std::string download_sha;
    std::string manual_sha;
    std::string reset_sha;

    auto open_entry = [&](const GameEntry& g) {
        if (library_.relocate_rom(g.sha1, g.rom_path.parent_path())) {
            library_notice_.clear();
            open_sha = g.sha1;
        } else {
            library_notice_ = "No se encuentra la ROM de \"" + g.title +
                              "\". Usa \"Abrir ROM...\" para volver a seleccionarla.";
        }
    };

    // --- Continue playing -------------------------------------------------
    if (const GameEntry* last = library_.last_played()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::SurfaceAlt);
        ImGui::BeginChild("Hero", ImVec2(0, 120.0f), true, ImGuiWindowFlags_NoScrollbar);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const ImVec4 base = platform_color(last->platform);
        const ImVec2 hero_sz(72.f, 96.f);

        SDL_Texture* hero_tex = cover_mgr_.get_texture(renderer_, last->sha1, last->cover_path);
        if (hero_tex) {
            dl->AddImageRounded((ImTextureID)(intptr_t)hero_tex, o,
                                ImVec2(o.x + hero_sz.x, o.y + hero_sz.y),
                                ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 6.f);
        } else {
            dl->AddRectFilled(o, ImVec2(o.x + hero_sz.x, o.y + hero_sz.y),
                              scaled(base, sha_tint(last->sha1)), 6.f);
            dl->AddText(ImVec2(o.x + 8.f, o.y + 8.f), IM_COL32(255, 255, 255, 255),
                        platform_label(last->platform));
        }
        dl->AddRect(o, ImVec2(o.x + hero_sz.x, o.y + hero_sz.y),
                    ImGui::GetColorU32(ThemeColors::Border), 6.f, 0, 1.f);

        ImGui::SetCursorScreenPos(ImVec2(o.x + hero_sz.x + 16.f, o.y));
        ImGui::BeginGroup();
        ImGui::PushFont(fonts_.small);
        ImGui::TextColored(ThemeColors::TextMuted, "CONTINUAR JUGANDO");
        ImGui::PopFont();
        ImGui::PushFont(fonts_.h2);
        ImGui::TextColored(ThemeColors::TextPrimary, "%s", last->title.c_str());
        ImGui::PopFont();
        ImGui::TextColored(ThemeColors::TextMuted, "Tiempo jugado: %s",
                           format_playtime(last->playtime_s).c_str());
        ImGui::Dummy(ImVec2(0, 4.f));
        if (ImGui::Button("▶ Abrir", ImVec2(140.f, 36.f))) {
            open_entry(*last);
        }
        ImGui::EndGroup();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    if (!library_notice_.empty()) {
        ImGui::TextColored(ThemeColors::WarningAmber, "%s", library_notice_.c_str());
        ImGui::Spacing();
    }

    // --- Library controls ---
    if (!library_.entries().empty()) {
        if (ImGui::Button("⬇ Descargar caratulas faltantes")) {
            size_t count = 0;
            for (const auto& entry : library_.entries()) {
                if (entry.cover_path.empty() && !cover_mgr_.is_downloading(entry.sha1)) {
                    count++;
                    cover_mgr_.download_cover_async(
                        entry, [this, sha1 = entry.sha1](bool ok, const std::filesystem::path& p) {
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
        ImGui::SameLine();
        ImGui::PushFont(fonts_.small);
        ImGui::TextColored(ThemeColors::TextMuted,
                           "(Descarga libre desde libretro-thumbnails; las imagenes se guardan localmente)");
        ImGui::PopFont();
        ImGui::Spacing();
    }

    // --- Shelves ----------------------------------------------------------
    struct Shelf {
        const char* title;
        std::vector<std::string> keys;
    };
    const Shelf shelves[] = {
        {"Nintendo Entertainment System", {"nes"}},
        {"Super Nintendo", {"snes"}},
        {"Game Boy / Game Boy Color", {"gb", "gbc"}},
        {"Game Boy Advance", {"gba"}},
    };

    const ImVec2 card_size(132.f, 176.f);
    bool any = false;
    for (const auto& shelf : shelves) {
        std::vector<const GameEntry*> items;
        for (const auto& k : shelf.keys) {
            const auto part = library_.shelf(k);
            items.insert(items.end(), part.begin(), part.end());
        }
        if (items.empty()) {
            continue;
        }
        any = true;
        std::sort(items.begin(), items.end(), [](const GameEntry* a, const GameEntry* b) {
            return a->title < b->title;
        });

        ImGui::PushFont(fonts_.body_bold);
        ImGui::TextColored(ThemeColors::TextPrimary, "%s", shelf.title);
        ImGui::PopFont();
        ImGui::SameLine(0, 8.f);
        ImGui::TextColored(ThemeColors::TextMuted, "%zu", items.size());

        ImGui::PushID(shelf.title);
        ImGui::BeginChild("row", ImVec2(0, card_size.y + 26.f), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) {
                ImGui::SameLine(0, 12.f);
            }
            bool remove = false;
            bool req_download = false;
            bool req_manual = false;
            bool req_reset = false;
            if (draw_game_card(renderer_, cover_mgr_, *items[i], card_size, &remove,
                               &req_download, &req_manual, &req_reset)) {
                open_entry(*items[i]);
            }
            if (remove) {
                remove_sha = items[i]->sha1;
            }
            if (req_download) {
                download_sha = items[i]->sha1;
            }
            if (req_manual) {
                manual_sha = items[i]->sha1;
            }
            if (req_reset) {
                reset_sha = items[i]->sha1;
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::Spacing();
    }

    if (!any) {
        ImGui::TextColored(ThemeColors::TextMuted,
                           "Tu biblioteca esta vacia. Abre una ROM para anadirla.");
    }

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
    if (!open_sha.empty()) {
        if (const GameEntry* g = library_.find(open_sha)) {
            load_rom(g->rom_path);
        }
    }
}
}  // namespace dragon::studio
