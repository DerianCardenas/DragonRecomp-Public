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
#elif defined(__linux__) || defined(__unix__)
#include <unistd.h>
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
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        const std::filesystem::path exe_dir = std::filesystem::path(buf).parent_path();
        base_dirs.push_back(exe_dir / "assets" / "fonts");
        base_dirs.push_back(exe_dir / ".." / "assets" / "fonts");
        base_dirs.push_back(exe_dir / ".." / ".." / "assets" / "fonts");
    }
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

    // Generous, crisp, modern typography scale matching the Obsidian mockup
    fonts.small = add_font_scale(reg, 13.5f);
    fonts.body = add_font_scale(reg, 16.0f);
    fonts.body_bold = add_font_scale(bld, 16.0f);
    fonts.h2 = add_font_scale(bld, 20.0f);
    fonts.h1 = add_font_scale(bld, 26.0f);
    fonts.display = add_font_scale(bld, 32.0f);

    io.FontDefault = fonts.body;
    return fonts;
}

void apply_studio_theme(bool dark_mode, AccentColor accent, float scale) {
    using namespace ThemeColors;

    // 1. Update Accent Primary token
    switch (accent) {
        case AccentColor::Cyan:
            Primary = ImVec4(0.118f, 0.784f, 0.831f, 1.00f);  // #1EC8D4
            break;
        case AccentColor::Purple:
            Primary = ImVec4(0.608f, 0.349f, 0.714f, 1.00f);  // #9B59B6
            break;
        case AccentColor::Green:
            Primary = ImVec4(0.180f, 0.800f, 0.443f, 1.00f);  // #2ECC71
            break;
        case AccentColor::Orange:
        default:
            Primary = ImVec4(0.941f, 0.486f, 0.118f, 1.00f);  // #F07C1E
            break;
    }
    PrimaryDim = ImVec4(Primary.x, Primary.y, Primary.z, dark_mode ? 0.15f : 0.14f);

    // 2. Update Dark / Light Elevation & Text tokens
    if (dark_mode) {
        BgApp = ImVec4(0.047f, 0.055f, 0.078f, 1.00f);         // #0C0E14
        Surface = ImVec4(0.086f, 0.098f, 0.133f, 1.00f);       // #161922
        SurfaceAlt = ImVec4(0.118f, 0.133f, 0.188f, 1.00f);    // #1E2230
        SurfaceHover = ImVec4(0.145f, 0.165f, 0.227f, 1.00f);  // #252A3A
        Border = ImVec4(0.145f, 0.165f, 0.227f, 1.00f);        // #252A3A
        BorderStrong = ImVec4(0.220f, 0.247f, 0.325f, 1.00f);  // #383F53

        AccentCyan = ImVec4(0.118f, 0.784f, 0.831f, 1.00f);     // #1EC8D4
        AccentCyanDim = ImVec4(0.118f, 0.784f, 0.831f, 0.12f);  // rgba(30,200,212,0.12)
        SuccessGreen = ImVec4(0.180f, 0.800f, 0.443f, 1.00f);   // #2ECC71
        WarningAmber = ImVec4(0.941f, 0.486f, 0.118f, 1.00f);   // #F07C1E
        DangerRed = ImVec4(0.906f, 0.298f, 0.235f, 1.00f);      // #E74C3C

        TextPrimary = ImVec4(0.910f, 0.918f, 0.941f, 1.00f);  // #E8EAF0
        TextMuted = ImVec4(0.486f, 0.510f, 0.600f, 1.00f);    // #7C8299
        TextFaint = ImVec4(0.290f, 0.314f, 0.408f, 1.00f);    // #4A5068
    } else {
        BgApp = ImVec4(0.953f, 0.957f, 0.973f, 1.00f);         // #F3F4F8
        Surface = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);       // #FFFFFF
        SurfaceAlt = ImVec4(0.910f, 0.925f, 0.957f, 1.00f);    // #E8ECF4
        SurfaceHover = ImVec4(0.855f, 0.878f, 0.925f, 1.00f);  // #DAE0EC
        Border = ImVec4(0.816f, 0.839f, 0.894f, 1.00f);        // #D0D6E4
        BorderStrong = ImVec4(0.680f, 0.714f, 0.788f, 1.00f);  // #ADB6C9

        AccentCyan = ImVec4(0.055f, 0.600f, 0.655f, 1.00f);
        AccentCyanDim = ImVec4(0.055f, 0.600f, 0.655f, 0.12f);
        SuccessGreen = ImVec4(0.153f, 0.682f, 0.376f, 1.00f);  // #27AE60
        WarningAmber = ImVec4(0.851f, 0.420f, 0.086f, 1.00f);
        DangerRed = ImVec4(0.839f, 0.188f, 0.192f, 1.00f);  // #D63031

        TextPrimary = ImVec4(0.086f, 0.098f, 0.133f, 1.00f);  // #161922
        TextMuted = ImVec4(0.353f, 0.384f, 0.471f, 1.00f);    // #5A6278
        TextFaint = ImVec4(0.549f, 0.580f, 0.667f, 1.00f);    // #8C94AA
    }

    BgDark = BgApp;
    CardBg = Surface;
    CardBorder = Border;

    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Reset geometry before applying scale (so re-calling at runtime does not compound scaling)
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(12.0f, 7.0f);
    style.CellPadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 8.0f;
    style.GrabMinSize = 14.0f;

    style.WindowRounding = 8.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 7.0f;
    style.TabRounding = 6.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    if (scale > 1.0f) {
        style.ScaleAllSizes(scale);
    }

    colors[ImGuiCol_Text] = TextPrimary;
    colors[ImGuiCol_TextDisabled] = TextFaint;
    colors[ImGuiCol_WindowBg] = BgApp;
    colors[ImGuiCol_ChildBg] = Surface;
    colors[ImGuiCol_PopupBg] = dark_mode ? mix(Surface, BgApp, 0.25f) : Surface;
    colors[ImGuiCol_Border] = Border;
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    colors[ImGuiCol_FrameBg] = SurfaceAlt;
    colors[ImGuiCol_FrameBgHovered] = SurfaceHover;
    colors[ImGuiCol_FrameBgActive] = mix(SurfaceHover, Primary, 0.18f);

    colors[ImGuiCol_TitleBg] = BgApp;
    colors[ImGuiCol_TitleBgActive] = Surface;
    colors[ImGuiCol_TitleBgCollapsed] = alpha(BgApp, 0.60f);
    colors[ImGuiCol_MenuBarBg] = Surface;

    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab] = Border;
    colors[ImGuiCol_ScrollbarGrabHovered] = mix(Border, Primary, 0.4f);
    colors[ImGuiCol_ScrollbarGrabActive] = Primary;

    colors[ImGuiCol_CheckMark] = Primary;
    colors[ImGuiCol_SliderGrab] = Primary;
    colors[ImGuiCol_SliderGrabActive] = Primary;

    colors[ImGuiCol_Button] = SurfaceAlt;
    colors[ImGuiCol_ButtonHovered] = SurfaceHover;
    colors[ImGuiCol_ButtonActive] = mix(SurfaceAlt, Primary, 0.35f);

    colors[ImGuiCol_Header] = SurfaceAlt;
    colors[ImGuiCol_HeaderHovered] = SurfaceHover;
    colors[ImGuiCol_HeaderActive] = PrimaryDim;

    colors[ImGuiCol_Separator] = Border;
    colors[ImGuiCol_SeparatorHovered] = Primary;
    colors[ImGuiCol_SeparatorActive] = Primary;

    colors[ImGuiCol_ResizeGrip] = alpha(Border, 0.40f);
    colors[ImGuiCol_ResizeGripHovered] = alpha(Primary, 0.60f);
    colors[ImGuiCol_ResizeGripActive] = Primary;

    colors[ImGuiCol_Tab] = Surface;
    colors[ImGuiCol_TabHovered] = SurfaceHover;
    colors[ImGuiCol_TabSelected] = Primary;
    colors[ImGuiCol_TabDimmed] = Surface;
    colors[ImGuiCol_TabDimmedSelected] = SurfaceAlt;

    colors[ImGuiCol_PlotLines] = Primary;
    colors[ImGuiCol_PlotLinesHovered] = AccentCyan;
    colors[ImGuiCol_PlotHistogram] = Primary;
    colors[ImGuiCol_PlotHistogramHovered] = AccentCyan;

    colors[ImGuiCol_TableHeaderBg] = SurfaceAlt;
    colors[ImGuiCol_TableBorderStrong] = BorderStrong;
    colors[ImGuiCol_TableBorderLight] = Border;
    colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TableRowBgAlt] =
        dark_mode ? ImVec4(1.0f, 1.0f, 1.0f, 0.02f) : ImVec4(0.0f, 0.0f, 0.0f, 0.02f);

    colors[ImGuiCol_TextSelectedBg] = alpha(Primary, 0.30f);
    colors[ImGuiCol_DragDropTarget] = alpha(AccentCyan, 0.90f);
    colors[ImGuiCol_NavHighlight] = Primary;
}

void apply_modern_dark_theme(float scale) {
    apply_studio_theme(true, AccentColor::Orange, scale);
}

}  // namespace dragon::studio
