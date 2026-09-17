#pragma once

#include <Windows.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <cstdint>

namespace DS2Coop::UI {

// Title bar notifier — shows in the window title that the mod is running.
class TitleScreenNotifier {
public:
    static TitleScreenNotifier& GetInstance();
    void Start();
    void Stop();

private:
    TitleScreenNotifier() = default;
    ~TitleScreenNotifier();
    HWND FindGameWindow();
    void UpdateThread();
    bool m_running = false;
    std::thread m_thread;
};

// What a notification is about; it picks the accent colour.
enum class NotifyKind { Info, Success, Warning, Error, Player };

// ImGui overlay — the co-op menu, the key hint and the notifications.
class Overlay {
public:
    static Overlay& GetInstance();

    void Initialize();
    void Shutdown();
    void Render();
    void HandleInput();

    // Opens and closes the menu on the configured key. Called every frame
    // before the ImGui frame starts.
    void PollMenuKey();

    void ShowConnectionMenu();
    void ShowPlayerList();
    void ShowNotification(const std::string& message, float duration = 3.0f,
                          NotifyKind kind = NotifyKind::Info);

    bool IsVisible() const { return m_visible; }
    void SetVisible(bool visible) { m_visible = visible; }
    void Toggle() { m_visible = !m_visible; }

private:
    Overlay() = default;
    ~Overlay() = default;
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    void RenderMenu();
    void RenderHeader();
    void RenderHomePage();
    void RenderHostPage();
    void RenderJoinPage();
    void RenderSessionPage();
    void RenderNetCheckPage();
    void RenderSettingsPage();
    void RenderFooter();
    void RenderNotifications();
    void RenderHint();
    void UpdateIdle();
    void UpdateKeyCapture();

    bool m_visible = false;
    bool m_initialized = false;

    enum class Page { Home, Host, Join, NetCheck };
    Page  m_page = Page::Home;
    int   m_tab = 0;              // 0 lobby, 1 settings
    float m_menuAnim = 0.0f;      // 0 closed .. 1 open
    float m_menuHeight = 0.0f;    // the window, last frame
    float m_menuContent = 0.0f;   // what it holds, last frame; more than fits scrolls
    float m_hintAlpha = 0.0f;
    bool  m_focusField = false;
    bool  m_showAddresses = false;

    // Menu key and rebinding. Insert always opens the menu as well, so a key
    // rebound out of reach cannot lock anyone out; Esc closes it.
    bool        m_menuKeyWasDown = false;
    bool        m_escWasDown = false;
    bool        m_releaseGuard = false;   // input stays held until closing keys are up
    bool        m_capturingKey = false;
    bool        m_keyWasDown[256] = {};
    std::string m_captureError;

    // Standing still, for the hint.
    bool   m_haveAnchor = false;
    float  m_anchor[3] = {};
    double m_stillSince = 0.0;
    double m_nextIdleSample = 0.0;

    char m_inputIP[128]       = {0};
    char m_inputPassword[128] = {0};

    struct Notification {
        std::string message;
        float       timeRemaining = 0.0f;
        float       age = 0.0f;
        uint32_t    id = 0;
        NotifyKind  kind = NotifyKind::Info;
    };
    std::vector<Notification> m_notifications;
    uint32_t m_nextNotifId = 1;
    mutable std::mutex m_notifMutex;   // notifications arrive from other threads
};

// DX11 Present hook — installs the ImGui renderer.
class OverlayRenderer {
public:
    static OverlayRenderer& GetInstance();

    bool Initialize();
    void Shutdown();

private:
    OverlayRenderer() = default;
    ~OverlayRenderer() = default;
    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    bool m_initialized = false;
};

} // namespace DS2Coop::UI
