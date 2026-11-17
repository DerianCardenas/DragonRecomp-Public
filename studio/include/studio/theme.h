#pragma once

#include "imgui.h"

namespace dragon::studio {

// Typography scale. Loaded once at startup from Inter / Montserrat / System TTF.
// Using real rasterized sizes instead of ImGui::SetWindowFontScale keeps every
// heading and stat number crisp instead of blurry upscaled bitmaps.
struct StudioFonts {
    ImFont* body = nullptr;       // ~20px regular  — default body text
    ImFont* body_bold = nullptr;  // ~20px bold     — emphasis inline
    ImFont* small = nullptr;      // ~16px regular  — captions / section labels
    ImFont* h2 = nullptr;         // ~25px bold     — card / section titles
    ImFont* h1 = nullptr;         // ~32px bold     — screen titles
    ImFont* display = nullptr;    // ~42px bold     — hero numbers / drop-zone
};

// Loads the font scale into io.Fonts and returns the handles. Falls back to the
// built-in font when no TTF is found. Supports high-DPI display scale.
StudioFonts load_studio_fonts(ImGuiIO& io, float scale = 1.0f);

void apply_modern_dark_theme(float scale = 1.0f);

// Single source of truth for Studio's palette. theme.cpp derives the ImGui
// style colors from these so the raw draw-list code (headers, banners) and the
// widget theme never drift apart.
namespace ThemeColors {
// Elevation ramp (darkest app background -> raised surfaces).
inline constexpr ImVec4 BgApp = ImVec4(0.055f, 0.067f, 0.086f, 1.00f);         // #0E1116
inline constexpr ImVec4 Surface = ImVec4(0.086f, 0.106f, 0.133f, 1.00f);       // #161B22
inline constexpr ImVec4 SurfaceAlt = ImVec4(0.118f, 0.145f, 0.180f, 1.00f);    // #1E252E
inline constexpr ImVec4 SurfaceHover = ImVec4(0.153f, 0.188f, 0.235f, 1.00f);  // #27303C
inline constexpr ImVec4 Border = ImVec4(0.180f, 0.216f, 0.267f, 1.00f);        // #2E3744
inline constexpr ImVec4 BorderStrong = ImVec4(0.235f, 0.282f, 0.345f, 1.00f);  // #3C4858

// Brand & semantic accents.
inline constexpr ImVec4 AccentCyan = ImVec4(0.000f, 0.824f, 0.831f, 1.00f);     // #00D2D4
inline constexpr ImVec4 AccentCyanDim = ImVec4(0.000f, 0.631f, 0.682f, 1.00f);  // #00A1AE
inline constexpr ImVec4 AccentBlue = ImVec4(0.094f, 0.627f, 0.984f, 1.00f);     // #18A0FB
inline constexpr ImVec4 SuccessGreen = ImVec4(0.063f, 0.725f, 0.506f, 1.00f);   // #10B981
inline constexpr ImVec4 WarningAmber = ImVec4(0.961f, 0.620f, 0.043f, 1.00f);   // #F59E0B
inline constexpr ImVec4 DangerRed = ImVec4(0.937f, 0.325f, 0.314f, 1.00f);      // #EF5350

// Text ramp.
inline constexpr ImVec4 TextPrimary = ImVec4(0.910f, 0.925f, 0.945f, 1.00f);  // #E8ECF1
inline constexpr ImVec4 TextMuted = ImVec4(0.541f, 0.580f, 0.639f, 1.00f);    // #8A94A3
inline constexpr ImVec4 TextFaint = ImVec4(0.361f, 0.408f, 0.471f, 1.00f);    // #5C6878

// Legacy aliases kept so existing call sites compile unchanged.
inline constexpr ImVec4 BgDark = BgApp;
inline constexpr ImVec4 CardBg = Surface;
inline constexpr ImVec4 CardBorder = Border;
}  // namespace ThemeColors

}  // namespace dragon::studio
