#include "studio/theme.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace dragon::studio {

namespace {

// Verifies that a file exists and has a valid TrueType or OpenType header.
bool is_valid_font_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
        return false;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < 12) {
        return false;
    }
    FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) {
        return false;
    }
    unsigned char header[4] = {0};
    const size_t n = std::fread(header, 1, 4, f);
    std::fclose(f);
    if (n < 4) {
        return false;
    }
    // TrueType 1.0 (0x00010000)
    if (header[0] == 0x00 && header[1] == 0x01 && header[2] == 0x00 && header[3] == 0x00) {
        return true;
    }
    // OpenType ('OTTO')
    if (header[0] == 'O' && header[1] == 'T' && header[2] == 'T' && header[3] == 'O') {
        return true;
    }
    // TrueType ('true')
    if (header[0] == 't' && header[1] == 'r' && header[2] == 'u' && header[3] == 'e') {
        return true;
    }
    // TrueType Collection ('ttcf')
    if (header[0] == 't' && header[1] == 't' && header[2] == 'c' && header[3] == 'f') {
        return true;
    }
    return false;
}

// Finds the first existing font file among candidate filenames across common search roots.
std::string find_font_file(const std::vector<std::string>& filenames) {
    std::vector<std::filesystem::path> base_dirs = {
        "assets/fonts",
        "../assets/fonts",
        "../../assets/fonts",
    };

#if defined(_WIN32)
    char buf[MAX_PATH];
    if (GetModuleFileNameA(NULL, buf, sizeof(buf)) > 0) {
        const std::filesystem::path exe_dir = std::filesystem::path(buf).parent_path();
        base_dirs.push_back(exe_dir / "assets" / "fonts");
        base_dirs.push_back(exe_dir / ".." / "assets" / "fonts");
        base_dirs.push_back(exe_dir / ".." / ".." / "assets" / "fonts");
    }
    base_dirs.push_back("C:\\Windows\\Fonts");
#elif defined(__linux__)
    base_dirs.push_back("/usr/share/fonts/truetype/inter");
    base_dirs.push_back("/usr/share/fonts/truetype/montserrat");
    base_dirs.push_back("/usr/share/fonts/truetype/dejavu");
    base_dirs.push_back("/usr/share/fonts/TTF");
    base_dirs.push_back("/usr/share/fonts/truetype/noto");
    base_dirs.push_back("/usr/share/fonts/truetype");
#endif

    for (const auto& fn : filenames) {
        for (const auto& dir : base_dirs) {
            std::filesystem::path candidate = dir / fn;
            if (is_valid_font_file(candidate)) {
                return candidate.string();
            }
        }
    }
    return {};
}

// Slightly darkens/mixes a color toward another by t in [0,1].
ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

// Returns the same color with a replaced alpha.
ImVec4 alpha(const ImVec4& c, float a) {
    return ImVec4(c.x, c.y, c.z, a);
}

}  // namespace

StudioFonts load_studio_fonts(ImGuiIO& io, float scale) {
    if (scale < 1.0f) {
        scale = 1.0f;
    }

    // Preferred font order: Inter -> Montserrat -> Segoe UI (Windows) -> DejaVu (Linux)
    const std::string regular = find_font_file({
        "Inter-Regular.ttf",
        "Montserrat-Regular.ttf",
        "segoeui.ttf",
        "DejaVuSans.ttf",
        "NotoSans-Regular.ttf",
        "LiberationSans-Regular.ttf",
    });

    const std::string bold = find_font_file({
        "Inter-Bold.ttf",
        "Montserrat-Bold.ttf",
        "segoeuib.ttf",
        "DejaVuSans-Bold.ttf",
        "NotoSans-Bold.ttf",
        "LiberationSans-Bold.ttf",
    });

    // Dedicated symbol font for geometric shapes, arrows, and UI dingbats
    const std::string symbol = find_font_file({
        "seguisym.ttf",
        "segoeui.ttf",
        "DejaVuSans.ttf",
        "NotoSansSymbols-Regular.ttf",
    });

    StudioFonts fonts;

    if (regular.empty()) {
        ImFont* def = io.Fonts->AddFontDefault();
        fonts.body = fonts.body_bold = fonts.small = def;
        fonts.h2 = fonts.h1 = fonts.display = def;
        return fonts;
    }

    // Build comprehensive glyph ranges including Spanish and UI symbols
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    builder.AddText("▶●→·⚡✔✕✖ℹ⚠⚙✦★—¿¡áéíóúñÁÉÍÓÚÑ↻");
    static ImVector<ImWchar> ranges;
    ranges.clear();
    builder.BuildRanges(&ranges);

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = true;

    const char* reg = regular.c_str();
    const char* bld = bold.empty() ? reg : bold.c_str();

    static const ImWchar sym_ranges[] = {
        0x2000, 0x206F,  // General Punctuation (— em-dash, · middot)
        0x2100, 0x214F,  // Letterlike (ℹ)
        0x2190, 0x21FF,  // Arrows (→, ↻)
        0x25A0, 0x25FF,  // Geometric Shapes (▶ 0x25B6, ● 0x25CF)
        0x2600, 0x26FF,  // Misc Symbols (⚡ 0x26A1, ⚙ 0x2699, ⚠ 0x26A0, ★ 0x2605)
        0x2700, 0x27BF,  // Dingbats (✔ 0x2714, ✕ 0x2715, ✖ 0x2716, ✦ 0x2726)
        0,
    };

    auto add_font_scale = [&](const char* path, float size_pt) -> ImFont* {
        ImFont* f = io.Fonts->AddFontFromFileTTF(path, size_pt * scale, &cfg, ranges.Data);
        if (!symbol.empty() && symbol != path) {
            ImFontConfig sym_cfg;
            sym_cfg.MergeMode = true;
            sym_cfg.OversampleH = 2;
            sym_cfg.OversampleV = 2;
            sym_cfg.PixelSnapH = true;
            io.Fonts->AddFontFromFileTTF(symbol.c_str(), size_pt * scale, &sym_cfg, sym_ranges);
        }
        return f;
    };

    // Generous, crisp, modern typography scale
    fonts.small = add_font_scale(reg, 15.0f);
    fonts.body = add_font_scale(reg, 19.0f);
    fonts.body_bold = add_font_scale(bld, 19.0f);
    fonts.h2 = add_font_scale(bld, 25.0f);
    fonts.h1 = add_font_scale(bld, 32.0f);
    fonts.display = add_font_scale(bld, 42.0f);

    io.FontDefault = fonts.body;
    return fonts;
}

void apply_modern_dark_theme(float scale) {
    using namespace ThemeColors;
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Scale geometry for high-DPI displays
    if (scale > 1.0f) {
        style.ScaleAllSizes(scale);
    }

    // --- Geometry: generous, consistent rhythm --------------------------------
    style.WindowPadding = ImVec2(18.0f, 18.0f);
    style.FramePadding = ImVec2(12.0f, 8.0f);
    style.CellPadding = ImVec2(10.0f, 8.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 13.0f;
    style.GrabMinSize = 11.0f;

    style.WindowRounding = 10.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 7.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 7.0f;
    style.TabRounding = 7.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;

    // Center window titles and align section headers for a tidier look.
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    // --- Colors derived from the shared palette -------------------------------
    colors[ImGuiCol_Text] = TextPrimary;
    colors[ImGuiCol_TextDisabled] = TextFaint;
    colors[ImGuiCol_WindowBg] = BgApp;
    colors[ImGuiCol_ChildBg] = Surface;
    colors[ImGuiCol_PopupBg] = mix(Surface, BgApp, 0.3f);
    colors[ImGuiCol_Border] = alpha(Border, 0.80f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    colors[ImGuiCol_FrameBg] = SurfaceAlt;
    colors[ImGuiCol_FrameBgHovered] = SurfaceHover;
    colors[ImGuiCol_FrameBgActive] = mix(SurfaceHover, AccentCyan, 0.12f);

    colors[ImGuiCol_TitleBg] = BgApp;
    colors[ImGuiCol_TitleBgActive] = Surface;
    colors[ImGuiCol_TitleBgCollapsed] = alpha(BgApp, 0.60f);
    colors[ImGuiCol_MenuBarBg] = Surface;

    colors[ImGuiCol_ScrollbarBg] = alpha(BgApp, 0.40f);
    colors[ImGuiCol_ScrollbarGrab] = BorderStrong;
    colors[ImGuiCol_ScrollbarGrabHovered] = mix(BorderStrong, AccentCyan, 0.4f);
    colors[ImGuiCol_ScrollbarGrabActive] = AccentCyan;

    colors[ImGuiCol_CheckMark] = AccentCyan;
    colors[ImGuiCol_SliderGrab] = AccentCyanDim;
    colors[ImGuiCol_SliderGrabActive] = AccentCyan;

    // Buttons: calm neutral surface that lights up cyan on hover.
    colors[ImGuiCol_Button] = SurfaceAlt;
    colors[ImGuiCol_ButtonHovered] = mix(SurfaceAlt, AccentCyan, 0.28f);
    colors[ImGuiCol_ButtonActive] = mix(SurfaceAlt, AccentCyan, 0.45f);

    colors[ImGuiCol_Header] = alpha(SurfaceAlt, 0.90f);
    colors[ImGuiCol_HeaderHovered] = SurfaceHover;
    colors[ImGuiCol_HeaderActive] = mix(SurfaceHover, AccentCyan, 0.25f);

    colors[ImGuiCol_Separator] = alpha(Border, 0.60f);
    colors[ImGuiCol_SeparatorHovered] = alpha(AccentCyan, 0.70f);
    colors[ImGuiCol_SeparatorActive] = AccentCyan;

    colors[ImGuiCol_ResizeGrip] = alpha(Border, 0.40f);
    colors[ImGuiCol_ResizeGripHovered] = alpha(AccentCyan, 0.60f);
    colors[ImGuiCol_ResizeGripActive] = alpha(AccentCyan, 0.90f);

    colors[ImGuiCol_Tab] = Surface;
    colors[ImGuiCol_TabHovered] = SurfaceHover;
    colors[ImGuiCol_TabSelected] = mix(Surface, AccentCyan, 0.55f);
    colors[ImGuiCol_TabDimmed] = mix(Surface, BgApp, 0.4f);
    colors[ImGuiCol_TabDimmedSelected] = SurfaceAlt;

    colors[ImGuiCol_PlotLines] = AccentCyan;
    colors[ImGuiCol_PlotLinesHovered] = WarningAmber;
    colors[ImGuiCol_PlotHistogram] = AccentCyan;
    colors[ImGuiCol_PlotHistogramHovered] = AccentBlue;

    colors[ImGuiCol_TableHeaderBg] = SurfaceAlt;
    colors[ImGuiCol_TableBorderStrong] = BorderStrong;
    colors[ImGuiCol_TableBorderLight] = alpha(Border, 0.60f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);

    colors[ImGuiCol_TextSelectedBg] = alpha(AccentCyan, 0.35f);
    colors[ImGuiCol_DragDropTarget] = alpha(AccentCyan, 0.90f);
    colors[ImGuiCol_NavHighlight] = AccentCyan;
}

}  // namespace dragon::studio
