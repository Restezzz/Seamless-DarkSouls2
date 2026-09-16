#pragma once

#include "imgui.h"
#include <cstddef>

// The overlay's look: palette, fonts, style, and the handful of custom widgets
// the menu, the hint and the notifications are drawn with.
//
// Direction: a bonfire at night. Warm charcoal panels, parchment text, thin gold
// lines, and one ember accent reserved for the thing to press. Everything scales
// with the screen height (1.0 at 1080p) and the menu size the player picked.
namespace DS2Coop::UI::Kit {

// The menu window's width at scale 1, and about its tallest page: the host's
// lobby page in Russian measures 614 px with two players, the settings page 601
// (tools/ui_preview, 17.09). The scale never grows past what lets both fit on
// the screen; a page taller still scrolls.
constexpr float kMenuWidth   = 500.0f;
constexpr float kMenuTallest = 640.0f;

// ---- palette ---------------------------------------------------------------
namespace Col {
    constexpr ImU32 Bg        = IM_COL32( 16,  13,  10, 246);
    constexpr ImU32 Surface   = IM_COL32( 28,  23,  18, 255);
    constexpr ImU32 Surface2  = IM_COL32( 38,  31,  24, 255);
    constexpr ImU32 Line      = IM_COL32(176, 141,  87,  70);
    constexpr ImU32 Gold      = IM_COL32(201, 165,  92, 255);
    constexpr ImU32 Ember     = IM_COL32(224, 123,  42, 255);
    constexpr ImU32 EmberHi   = IM_COL32(242, 161,  84, 255);
    constexpr ImU32 Text      = IM_COL32(237, 227, 207, 255);
    constexpr ImU32 TextMuted = IM_COL32(162, 149, 126, 255);
    constexpr ImU32 TextFaint = IM_COL32(111, 101,  86, 255);
    constexpr ImU32 OnEmber   = IM_COL32( 26,  18,  11, 255);
    constexpr ImU32 Green     = IM_COL32(134, 179, 110, 255);
    constexpr ImU32 Amber     = IM_COL32(227, 164,  68, 255);
    constexpr ImU32 Red       = IM_COL32(208,  90,  76, 255);
}

ImU32 Fade(ImU32 color, float alpha);          // multiply the colour's alpha
ImU32 Mix(ImU32 a, ImU32 b, float t);          // blend, t = 0..1
float EaseOut(float t);                        // cubic ease-out, t = 0..1
float Approach(float value, float target, float perSecond);

// ---- fonts and style --------------------------------------------------------
// The scale for a screen of this size: its height against 1080p, times the menu
// size from the settings, no larger than fits, in steps of 0.05 so a window
// dragged by a few pixels changes nothing.
float TargetScale(float displayWidth, float displayHeight);
// Builds the fonts and the style at TargetScale. The first call comes after
// ImGui::CreateContext; a later call rebuilds the font atlas, so it has to come
// between frames, with the renderer's font texture released.
void  Setup(float displayWidth, float displayHeight);
float Scale();
ImFont* FontBody();
ImFont* FontSmall();
ImFont* FontStrong();
ImFont* FontTitle();

// ---- widgets (inside an ImGui window) ---------------------------------------
enum class ButtonKind { Primary, Secondary, Danger, Ghost };

bool Button(const char* label, ButtonKind kind, float width = -1.0f, bool enabled = true);
bool ActionCard(const char* id, int icon, const char* title, const char* subtitle);
bool Tabs(const char* id, const char* const* labels, int count, int& current);
bool LanguageSwitch(const char* id, bool& russian);
bool KeyBindButton(const char* id, const char* keyName, bool capturing);
bool CloseButton(const char* id);
bool InputField(const char* label, const char* id, char* buffer, size_t size,
                const char* hint, bool focus = false);
void SectionLabel(const char* text);
void Paragraph(const char* text, ImU32 color = Col::TextMuted);
void Divider();
void StatusDot(ImDrawList* list, ImVec2 center, ImU32 color, bool pulse);
void Chip(const char* text, ImU32 color);

// ---- free drawing (usable without a window) ---------------------------------
ImVec2 KeycapSize(const char* label);
void   Keycap(ImDrawList* list, ImVec2 pos, const char* label, float alpha);

enum Icon { IconHost = 0, IconJoin = 1 };

} // namespace DS2Coop::UI::Kit
