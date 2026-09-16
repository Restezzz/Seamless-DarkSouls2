// Palette helpers, fonts and the base ImGui style for the overlay.
//
// ImGui's built-in font has no Cyrillic, so the Russian menu needs real fonts:
// Windows' own Segoe UI for text and Palatino Linotype for the title, both of
// which carry Cyrillic. They are loaded from the Windows font folder, sized for
// the screen, with the next font in each list as a fallback.
//
// The size is not settled once: the game often shows its first frames at
// 1280x720 and only then switches to the real resolution, which left the menu
// drawn for 720p on a 1440p screen. The renderer calls Setup again whenever the
// back buffer settles at a size that wants another scale, or the player picks
// another menu size.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../../include/ui_kit.h"
#include "../../include/ui_settings.h"
#include "../../include/utils.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>

using namespace DS2Coop::Utils;

namespace DS2Coop::UI::Kit {

namespace {

float   g_scale  = 1.0f;
ImFont* g_body   = nullptr;
ImFont* g_small  = nullptr;
ImFont* g_strong = nullptr;
ImFont* g_title  = nullptr;

// Latin, Latin-1, Cyrillic, and the punctuation the texts use (— … • ‹ › №).
const ImWchar kRanges[] = {
    0x0020, 0x00FF,
    0x0400, 0x052F,
    0x2010, 0x2027,
    0x2039, 0x203A,
    0x2116, 0x2116,
    0x2190, 0x2193,
    0,
};

std::string FontPath(const char* file) {
    char Dir[MAX_PATH] = {};
    GetWindowsDirectoryA(Dir, MAX_PATH);
    return std::string(Dir) + "\\Fonts\\" + file;
}

ImFont* LoadFirst(std::initializer_list<const char*> files, float size, float multiply) {
    ImGuiIO& io = ImGui::GetIO();
    for (const char* File : files) {
        const std::string Path = FontPath(File);
        if (GetFileAttributesA(Path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        ImFontConfig Cfg;
        Cfg.OversampleH = 2;
        Cfg.OversampleV = 1;
        Cfg.RasterizerMultiply = multiply;
        if (ImFont* Font = io.Fonts->AddFontFromFileTTF(Path.c_str(), size, &Cfg, kRanges)) {
            LOG_INFO("[UI] font %s at %.0f px", File, size);
            return Font;
        }
    }
    return nullptr;
}

ImVec4 V(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void ApplyStyle() {
    ImGuiStyle& S = ImGui::GetStyle();
    S = ImGuiStyle();
    ImGui::StyleColorsDark(&S);

    S.WindowRounding    = 12.0f;
    S.ChildRounding     = 8.0f;
    S.FrameRounding     = 8.0f;
    S.PopupRounding     = 8.0f;
    S.GrabRounding      = 6.0f;
    S.TabRounding       = 6.0f;
    S.ScrollbarRounding = 6.0f;
    S.WindowBorderSize  = 1.0f;
    S.FrameBorderSize   = 1.0f;
    S.PopupBorderSize   = 1.0f;
    S.WindowPadding     = ImVec2(24.0f, 20.0f);
    S.FramePadding      = ImVec2(12.0f, 9.0f);
    S.ItemSpacing       = ImVec2(10.0f, 10.0f);
    S.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    S.DisabledAlpha     = 0.45f;
    S.ScaleAllSizes(g_scale);

    ImVec4* C = S.Colors;
    C[ImGuiCol_WindowBg]         = V(Col::Bg);
    C[ImGuiCol_PopupBg]          = V(Col::Bg);
    C[ImGuiCol_Border]           = V(Col::Line);
    C[ImGuiCol_FrameBg]          = V(IM_COL32(10, 8, 6, 255));
    C[ImGuiCol_FrameBgHovered]   = V(IM_COL32(20, 16, 12, 255));
    C[ImGuiCol_FrameBgActive]    = V(IM_COL32(24, 19, 14, 255));
    C[ImGuiCol_Text]             = V(Col::Text);
    C[ImGuiCol_TextDisabled]     = V(Col::TextFaint);
    C[ImGuiCol_TextSelectedBg]   = V(Fade(Col::Ember, 0.35f));
    C[ImGuiCol_Separator]        = V(Col::Line);
    C[ImGuiCol_Button]           = V(Col::Surface);
    C[ImGuiCol_ButtonHovered]    = V(Col::Surface2);
    C[ImGuiCol_ButtonActive]     = V(Fade(Col::Ember, 0.6f));
    C[ImGuiCol_Header]           = V(Col::Surface);
    C[ImGuiCol_HeaderHovered]    = V(Col::Surface2);
    C[ImGuiCol_HeaderActive]     = V(Fade(Col::Ember, 0.6f));
    C[ImGuiCol_CheckMark]        = V(Col::Ember);
    C[ImGuiCol_SliderGrab]       = V(Col::Gold);
    C[ImGuiCol_SliderGrabActive] = V(Col::Ember);
    C[ImGuiCol_ScrollbarBg]      = V(IM_COL32(0, 0, 0, 0));
    C[ImGuiCol_ScrollbarGrab]    = V(Col::Surface2);
}

} // namespace

ImU32 Fade(ImU32 color, float alpha) {
    const float A = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(A + 0.5f) << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    ImU32 Out = 0;
    for (int Shift = 0; Shift < 32; Shift += 8) {
        const float Ca = static_cast<float>((a >> Shift) & 0xFF);
        const float Cb = static_cast<float>((b >> Shift) & 0xFF);
        Out |= static_cast<ImU32>(Ca + (Cb - Ca) * t + 0.5f) << Shift;
    }
    return Out;
}

float EaseOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float U = 1.0f - t;
    return 1.0f - U * U * U;
}

float Approach(float value, float target, float perSecond) {
    const float Step = perSecond * ImGui::GetIO().DeltaTime;
    return value < target ? std::min(value + Step, target) : std::max(value - Step, target);
}

float TargetScale(float displayWidth, float displayHeight) {
    if (displayWidth < 1.0f || displayHeight < 1.0f) return g_scale;
    const float Screen = std::clamp(displayHeight / 1080.0f, 0.85f, 3.0f);
    float Wanted = Screen * static_cast<float>(GetMenuSize()) / 100.0f;
    Wanted = std::min(Wanted, displayWidth * 0.92f / kMenuWidth);
    if (kMenuTallest > 0.0f) Wanted = std::min(Wanted, displayHeight * 0.94f / kMenuTallest);
    Wanted = std::clamp(Wanted, 0.6f, 4.0f);
    return std::round(Wanted * 20.0f) / 20.0f;
}

void Setup(float displayWidth, float displayHeight) {
    g_scale = TargetScale(displayWidth, displayHeight);
    const float S = g_scale;

    // Fonts built for the previous scale go; ImFont pointers are only ever
    // asked for through the accessors below, never kept across frames.
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_body = g_small = g_strong = g_title = nullptr;

    g_body   = LoadFirst({ "segoeui.ttf", "arial.ttf" }, 17.0f * S, 1.0f);
    g_small  = LoadFirst({ "segoeui.ttf", "arial.ttf" }, 14.0f * S, 1.1f);
    g_strong = LoadFirst({ "seguisb.ttf", "segoeuib.ttf", "arialbd.ttf" }, 17.0f * S, 1.0f);
    g_title  = LoadFirst({ "palab.ttf", "georgiab.ttf", "timesbd.ttf" }, 25.0f * S, 1.0f);

    if (!g_body) {
        LOG_WARNING("[UI] no system font found; Cyrillic will not display");
        g_body = io.Fonts->AddFontDefault();
    }
    if (!g_small)  g_small  = g_body;
    if (!g_strong) g_strong = g_body;
    if (!g_title)  g_title  = g_strong;
    io.FontDefault = g_body;

    ApplyStyle();
}

float   Scale()      { return g_scale; }
ImFont* FontBody()   { return g_body   ? g_body   : ImGui::GetFont(); }
ImFont* FontSmall()  { return g_small  ? g_small  : ImGui::GetFont(); }
ImFont* FontStrong() { return g_strong ? g_strong : ImGui::GetFont(); }
ImFont* FontTitle()  { return g_title  ? g_title  : ImGui::GetFont(); }

} // namespace DS2Coop::UI::Kit
