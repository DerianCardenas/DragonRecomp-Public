#pragma once

#include "imgui.h"
#include "studio/settings.h"

namespace dragon::studio {

// Typography scale. Loaded once at startup from Inter / Montserrat / System TTF.
// Using real rasterized sizes instead of ImGui::SetWindowFontScale keeps every
// heading and stat number crisp instead of blurry upscaled bitmaps.
struct StudioFonts {
    ImFont* body = nullptr;       // ~16px regular  — default body text
    ImFont* body_bold = nullptr;  // ~16px bold     — emphasis inline
    ImFont* small = nullptr;      // ~13px regular  — captions / section labels
    ImFont* h2 = nullptr;         // ~20px bold     — card / section titles
    ImFont* h1 = nullptr;         // ~26px bold     — screen titles
    ImFont* display = nullptr;    // ~32px bold     — hero numbers / drop-zone
};

// Loads the font scale into io.Fonts and returns the handles. Falls back to the
// built-in font when no TTF is found. Supports high-DPI display scale.
StudioFonts load_studio_fonts(ImGuiIO& io, float scale = 1.0f);

void apply_studio_theme(bool dark_mode, AccentColor accent, float scale = 1.0f);
void apply_modern_dark_theme(float scale = 1.0f);

// Single source of truth for Studio's dynamic palette (Obsidian Dark / Light + Accent).
namespace ThemeColors {
// Elevation ramp (Obsidian dark default: #0C0E14 -> #161922 -> #1E2230 -> #252A3A).
inline ImVec4 BgApp = ImVec4(0.047f, 0.055f, 0.078f, 1.00f);         // #0C0E14
inline ImVec4 Surface = ImVec4(0.086f, 0.098f, 0.133f, 1.00f);       // #161922
inline ImVec4 SurfaceAlt = ImVec4(0.118f, 0.133f, 0.188f, 1.00f);    // #1E2230
inline ImVec4 SurfaceHover = ImVec4(0.145f, 0.165f, 0.227f, 1.00f);  // #252A3A
inline ImVec4 Border = ImVec4(0.145f, 0.165f, 0.227f, 1.00f);        // #252A3A
inline ImVec4 BorderStrong = ImVec4(0.220f, 0.247f, 0.325f, 1.00f);  // #383F53

// Brand & semantic accents.
inline ImVec4 Primary = ImVec4(0.941f, 0.486f, 0.118f, 1.00f);        // #F07C1E
inline ImVec4 PrimaryDim = ImVec4(0.941f, 0.486f, 0.118f, 0.14f);     // rgba(240,124,30,0.14)
inline ImVec4 AccentCyan = ImVec4(0.118f, 0.784f, 0.831f, 1.00f);     // #1EC8D4
inline ImVec4 AccentCyanDim = ImVec4(0.118f, 0.784f, 0.831f, 0.12f);  // rgba(30,200,212,0.12)
inline ImVec4 AccentBlue = ImVec4(0.094f, 0.627f, 0.984f, 1.00f);     // #18A0FB
inline ImVec4 SuccessGreen = ImVec4(0.180f, 0.800f, 0.443f, 1.00f);   // #2ECC71
inline ImVec4 WarningAmber = ImVec4(0.941f, 0.486f, 0.118f, 1.00f);   // #F07C1E
inline ImVec4 DangerRed = ImVec4(0.906f, 0.298f, 0.235f, 1.00f);      // #E74C3C

// Text ramp.
inline ImVec4 TextPrimary = ImVec4(0.910f, 0.918f, 0.941f, 1.00f);  // #E8EAF0
inline ImVec4 TextMuted = ImVec4(0.486f, 0.510f, 0.600f, 1.00f);    // #7C8299
inline ImVec4 TextFaint = ImVec4(0.290f, 0.314f, 0.408f, 1.00f);    // #4A5068

// Legacy aliases kept so existing call sites compile unchanged.
inline ImVec4 BgDark = BgApp;
inline ImVec4 CardBg = Surface;
inline ImVec4 CardBorder = Border;
}  // namespace ThemeColors

}  // namespace dragon::studio
