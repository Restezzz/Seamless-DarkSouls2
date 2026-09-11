// The overlay outside the menu window: the menu key (and rebinding it), the
// small hint in the corner, and the notifications.
//
// The hint used to be a badge pinned over the game's health bars at all times.
// Now it is a quiet pill in the top-right corner that shows only while there is
// no lobby yet, or once the player has stood still for a minute, and fades out
// the moment they move or open the menu. Notifications stack under it.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "imgui.h"
#include "../../include/ui.h"
#include "../../include/ui_kit.h"
#include "../../include/ui_settings.h"
#include "../../include/input_capture.h"
#include "../../include/session.h"
#include "../../include/sync.h"
#include "../../include/utils.h"

#include <algorithm>
#include <cfloat>

using namespace DS2Coop::Utils;

namespace DS2Coop::UI {

namespace {

constexpr double kIdleSeconds = 60.0;
constexpr float  kMoveEpsilon = 0.25f;   // metres; idle sway stays well under this
constexpr size_t kMaxToasts   = 5;

float g_hintReserve = 0.0f;              // vertical space the hint takes, for the stack

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

ImU32 KindColor(NotifyKind kind) {
    switch (kind) {
    case NotifyKind::Success: return Kit::Col::Green;
    case NotifyKind::Warning: return Kit::Col::Amber;
    case NotifyKind::Error:   return Kit::Col::Red;
    case NotifyKind::Player:  return Kit::Col::Ember;
    default:                  return Kit::Col::Gold;
    }
}

} // namespace

void Overlay::ShowNotification(const std::string& message, float duration, NotifyKind kind) {
    {
        std::lock_guard<std::mutex> lock(m_notifMutex);
        Notification N;
        N.message = message;
        N.timeRemaining = duration;
        N.kind = kind;
        N.id = m_nextNotifId++;
        m_notifications.push_back(N);
        if (m_notifications.size() > kMaxToasts) m_notifications.erase(m_notifications.begin());
    }
    LOG_INFO("Notification: %s", message.c_str());
}

// The configured key toggles the menu, and Insert always does as well, so a key
// rebound out of reach cannot lock anyone out. Esc closes it.
//
// While the menu is open, and afterwards until every key that could have closed
// it is up again, the game gets no input at all -- otherwise the Esc that closed
// the menu would reach the game a frame later and open its pause menu.
void Overlay::PollMenuKey() {
    const bool Down = KeyDown(GetMenuKey()) || KeyDown(VK_INSERT);
    if (!m_capturingKey && Down && !m_menuKeyWasDown) m_visible = !m_visible;
    m_menuKeyWasDown = Down;

    const bool Esc = KeyDown(VK_ESCAPE);
    if (m_visible && !m_capturingKey && Esc && !m_escWasDown) m_visible = false;
    m_escWasDown = Esc;

    const bool Held = Esc || Down || KeyDown(VK_LBUTTON) || KeyDown(VK_RBUTTON);
    if (m_visible) m_releaseGuard = true;
    else if (!Held) m_releaseGuard = false;
    SetGameInputBlocked(m_visible || m_releaseGuard);

    // The menu uses the real Windows cursor (input_capture.cpp): the system
    // draws it at the monitor's rate instead of once per game frame, and it is
    // there even when the game window never saw the mouse move. ImGui only picks
    // its shape; the rest of the time the game's cursor is left alone.
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    if (m_visible) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else           io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    SetMenuCursorWanted(m_visible);

    // Where the cursor is, from Windows every frame: the window's own mouse
    // messages do not always arrive while the game holds the mouse.
    if (m_visible) {
        POINT Pos{};
        HWND Hwnd = GetGameWindow();
        if (Hwnd && GetCursorPos(&Pos) && ScreenToClient(Hwnd, &Pos)) {
            io.AddMousePosEvent(static_cast<float>(Pos.x), static_cast<float>(Pos.y));
        }
    }
}

// Rebinding: the first key that goes down after the button was pressed. Keys
// already held at that moment are snapshotted and ignored until released.
void Overlay::UpdateKeyCapture() {
    for (int Vk = 0x08; Vk <= 0xFE; Vk++) {
        const bool Down = KeyDown(Vk);
        const bool Was = m_keyWasDown[Vk];
        m_keyWasDown[Vk] = Down;
        if (!Down || Was) continue;

        if (Vk == VK_ESCAPE) {
            m_capturingKey = false;
            m_captureError.clear();
            return;
        }
        if (const char* Why = WhyNotBindable(Vk)) {
            m_captureError = Why;
            continue;
        }
        SetMenuKey(Vk);
        m_capturingKey = false;
        m_captureError.clear();
        m_menuKeyWasDown = true;   // the new key is still held; do not toggle on it
        ShowNotification(Format(Tr("Menu key: %s", "Клавиша меню: %s"), KeyName(Vk).c_str()),
                         3.0f, NotifyKind::Success);
        return;
    }
}

void Overlay::UpdateIdle() {
    const double Now = ImGui::GetTime();
    if (Now < m_nextIdleSample) return;
    m_nextIdleSample = Now + 0.25;

    float X = 0.0f, Y = 0.0f, Z = 0.0f, Rot = 0.0f;
    if (!DS2Coop::Sync::GetLocalPlayerPosition(X, Y, Z, Rot)) {
        m_haveAnchor = false;
        m_stillSince = Now;
        return;
    }
    const float Dx = X - m_anchor[0], Dy = Y - m_anchor[1], Dz = Z - m_anchor[2];
    if (!m_haveAnchor || Dx * Dx + Dy * Dy + Dz * Dz > kMoveEpsilon * kMoveEpsilon) {
        m_anchor[0] = X;
        m_anchor[1] = Y;
        m_anchor[2] = Z;
        m_haveAnchor = true;
        m_stillSince = Now;
    }
}

void Overlay::RenderHint() {
    UpdateIdle();

    const auto State = Session::SessionManager::GetInstance().GetState();
    const bool NoLobby = State == Session::SessionState::Disconnected ||
                         State == Session::SessionState::Error;
    const bool Idle = m_haveAnchor && ImGui::GetTime() - m_stillSince >= kIdleSeconds;
    const bool Want = !m_visible && m_menuAnim < 0.01f && (NoLobby || Idle);
    m_hintAlpha = Kit::Approach(m_hintAlpha, Want ? 1.0f : 0.0f, Want ? 1.6f : 4.0f);
    if (m_hintAlpha <= 0.001f) {
        g_hintReserve = 0.0f;
        return;
    }

    const float S = Kit::Scale();
    const ImGuiIO& io = ImGui::GetIO();
    const std::string Key = KeyName(GetMenuKey());
    const char* Label = Tr("Co-op menu", "Кооп-меню");
    ImFont* Font = Kit::FontSmall();

    const ImVec2 Cap = Kit::KeycapSize(Key.c_str());
    const ImVec2 Text = Font->CalcTextSizeA(Font->FontSize, FLT_MAX, 0.0f, Label);
    const float PadX = 10.0f * S, PadY = 6.0f * S, Gap = 8.0f * S, Margin = 20.0f * S;
    const float W = PadX + Cap.x + Gap + Text.x + PadX + 2.0f * S;
    const float H = std::max(Cap.y, Text.y) + PadY * 2.0f;
    const float A = Kit::EaseOut(m_hintAlpha) * 0.85f;

    const ImVec2 Max(io.DisplaySize.x - Margin, Margin + H);
    const ImVec2 Min(Max.x - W, Margin);
    ImDrawList* L = ImGui::GetBackgroundDrawList();
    L->AddRectFilled(Min, Max, Kit::Fade(IM_COL32(12, 10, 8, 255), 0.55f * A), H * 0.5f);
    L->AddRect(Min, Max, Kit::Fade(Kit::Col::Line, A), H * 0.5f, 0, 1.0f);
    Kit::Keycap(L, ImVec2(Min.x + PadX, Min.y + (H - Cap.y) * 0.5f), Key.c_str(), A);
    L->AddText(Font, Font->FontSize,
               ImVec2(Min.x + PadX + Cap.x + Gap, Min.y + (H - Text.y) * 0.5f - 1.0f * S),
               Kit::Fade(Kit::Col::TextMuted, A), Label);

    g_hintReserve = (H + 10.0f * S) * Kit::EaseOut(m_hintAlpha);
}

void Overlay::RenderNotifications() {
    std::vector<Notification> Snapshot;
    {
        std::lock_guard<std::mutex> lock(m_notifMutex);
        if (m_notifications.empty()) return;
        Snapshot = m_notifications;
    }

    const float S = Kit::Scale();
    const ImGuiIO& io = ImGui::GetIO();
    ImFont* Font = Kit::FontBody();
    ImDrawList* L = ImGui::GetBackgroundDrawList();

    const float W = 360.0f * S, PadY = 12.0f * S, TextX = 42.0f * S, PadR = 16.0f * S;
    const float Right = io.DisplaySize.x - 20.0f * S;
    const float Wrap = W - TextX - PadR;
    float Y = 20.0f * S + g_hintReserve;

    for (const Notification& N : Snapshot) {
        const float In = Kit::EaseOut(N.age / 0.3f);
        const float Out = std::clamp(N.timeRemaining / 0.45f, 0.0f, 1.0f);
        const float A = In * Out;
        if (A <= 0.001f) continue;

        const ImVec2 Text = Font->CalcTextSizeA(Font->FontSize, FLT_MAX, Wrap, N.message.c_str());
        const float H = std::max(Text.y + PadY * 2.0f, 46.0f * S);
        const float Slide = (1.0f - In) * 28.0f * S;
        const ImVec2 Min(Right - W + Slide, Y), Max(Right + Slide, Y + H);
        const ImU32 Accent = KindColor(N.kind);
        const float R = 10.0f * S;

        L->AddRectFilled(ImVec2(Min.x, Min.y + 4.0f * S), ImVec2(Max.x, Max.y + 4.0f * S),
                         Kit::Fade(IM_COL32(0, 0, 0, 255), 0.30f * A), R + 2.0f * S);
        L->AddRectFilled(Min, Max, Kit::Fade(Kit::Col::Bg, 0.97f * A), R);
        L->AddRect(Min, Max, Kit::Fade(Kit::Col::Line, A), R, 0, 1.0f);
        L->AddRectFilled(ImVec2(Min.x + 1.0f * S, Min.y + R), ImVec2(Min.x + 4.0f * S, Max.y - R),
                         Kit::Fade(Accent, A), 2.0f * S);

        const ImVec2 Dot(Min.x + 23.0f * S, Min.y + PadY + Font->FontSize * 0.5f + 1.0f * S);
        L->AddCircleFilled(Dot, 9.0f * S, Kit::Fade(Accent, 0.16f * A));
        L->AddCircleFilled(Dot, 4.0f * S, Kit::Fade(Accent, A));
        L->AddText(Font, Font->FontSize, ImVec2(Min.x + TextX, Min.y + PadY),
                   Kit::Fade(Kit::Col::Text, A), N.message.c_str(), nullptr, Wrap);

        Y += (H + 8.0f * S) * std::min(1.0f, Out * 1.5f);   // the gap closes as it fades
    }

    const float Dt = io.DeltaTime;
    std::lock_guard<std::mutex> lock(m_notifMutex);
    for (Notification& N : m_notifications) {
        N.age += Dt;
        N.timeRemaining -= Dt;
    }
    m_notifications.erase(std::remove_if(m_notifications.begin(), m_notifications.end(),
                              [](const Notification& N) { return N.timeRemaining <= 0.0f; }),
                          m_notifications.end());
}

} // namespace DS2Coop::UI
