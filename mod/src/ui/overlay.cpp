// In-game co-op menu.
//
// One window, two tabs. "Lobby": host or join, and once in a lobby the players
// and the way out. "Settings": the language, the menu size and the key that
// opens all this.
// The window fades and rises in, dims the game a little behind it, and is drawn
// with the widgets in ui_kit.h. The hint and notifications live in
// overlay_hud.cpp.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>

#include "imgui.h"
#include "../../include/ui.h"
#include "../../include/ui_kit.h"
#include "../../include/ui_settings.h"
#include "../../include/mod.h"
#include "../../include/session.h"
#include "../../include/network.h"
#include "../../include/net_check.h"
#include "../../include/hooks.h"
#include "../../include/sync.h"
#include "../../include/utils.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

using namespace DS2Coop::Utils;
using namespace DS2Coop::Session;
using namespace DS2Coop::Network;

namespace DS2Coop::UI {

namespace {

// ---- addresses to share ------------------------------------------------------

// As Unicode: the connection report is Russian, and CF_TEXT would hand the
// messenger it is pasted into question marks.
void CopyToClipboard(const std::string& text) {
    const int Wide = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (Wide <= 0 || !OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL Mem = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(Wide) * sizeof(wchar_t))) {
        if (auto* Dst = static_cast<wchar_t*>(GlobalLock(Mem))) {
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, Dst, Wide);
            GlobalUnlock(Mem);
            if (!SetClipboardData(CF_UNICODETEXT, Mem)) GlobalFree(Mem);
        } else {
            GlobalFree(Mem);
        }
    }
    CloseClipboard();
}

std::string       g_publicIP;
std::atomic<bool> g_publicIPFetched{ false };
std::atomic<bool> g_publicIPFetching{ false };

void FetchPublicIPThread() {
    // Winsock is already up (PeerManager); no WSAStartup/Cleanup here.
    addrinfo Hints{}, *Result = nullptr;
    Hints.ai_family = AF_INET;
    Hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("api.ipify.org", "80", &Hints, &Result) != 0) { g_publicIPFetched = true; return; }

    SOCKET Sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Sock == INVALID_SOCKET || connect(Sock, Result->ai_addr, static_cast<int>(Result->ai_addrlen)) != 0) {
        if (Sock != INVALID_SOCKET) closesocket(Sock);
        freeaddrinfo(Result);
        g_publicIPFetched = true;
        return;
    }
    freeaddrinfo(Result);

    const char* Request = "GET / HTTP/1.1\r\nHost: api.ipify.org\r\nConnection: close\r\n\r\n";
    send(Sock, Request, static_cast<int>(strlen(Request)), 0);
    char Buf[512] = {};
    int Total = 0;
    while (Total < static_cast<int>(sizeof(Buf)) - 1) {
        const int N = recv(Sock, Buf + Total, sizeof(Buf) - 1 - Total, 0);
        if (N <= 0) break;
        Total += N;
    }
    closesocket(Sock);

    if (char* Body = strstr(Buf, "\r\n\r\n")) {
        Body += 4;
        while (*Body == ' ' || *Body == '\r' || *Body == '\n') Body++;
        char* End = Body;
        while (*End && *End != '\r' && *End != '\n' && *End != ' ') End++;
        *End = '\0';
        const size_t Len = strlen(Body);
        if (Len >= 7 && Len <= 15) g_publicIP = Body;
    }
    g_publicIPFetched = true;
}

void EnsurePublicIPFetched() {
    bool Expected = false;
    if (g_publicIPFetching.compare_exchange_strong(Expected, true)) std::thread(FetchPublicIPThread).detach();
}

struct Address { std::string Kind; std::string Ip; bool Vpn; };

// The addresses a friend could type, from the adapters that are up. Virtual
// switches (WSL, Docker, Hyper-V, VirtualBox, VMware) are left out; the VPNs
// people share for games come first, by name.
std::vector<Address> LocalAddresses() {
    const ULONG Flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG Size = 32 * 1024;
    std::vector<unsigned char> Buffer(Size);
    ULONG Rc = GetAdaptersAddresses(AF_INET, Flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(Buffer.data()), &Size);
    if (Rc == ERROR_BUFFER_OVERFLOW) {
        Buffer.resize(Size);
        Rc = GetAdaptersAddresses(AF_INET, Flags, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(Buffer.data()), &Size);
    }
    if (Rc != NO_ERROR) return {};

    std::vector<Address> Vpn, Lan;
    for (auto* Ad = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(Buffer.data()); Ad; Ad = Ad->Next) {
        if (Ad->OperStatus != IfOperStatusUp || Ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        const std::wstring Name = Ad->FriendlyName ? Ad->FriendlyName : L"";
        const std::wstring Desc = Ad->Description ? Ad->Description : L"";
        auto Has = [&](const wchar_t* Word) {
            return Name.find(Word) != std::wstring::npos || Desc.find(Word) != std::wstring::npos;
        };
        if (Has(L"vEthernet") || Has(L"WSL") || Has(L"Docker") || Has(L"Hyper-V") ||
            Has(L"VirtualBox") || Has(L"VMware") || Has(L"Loopback")) continue;

        std::string Kind = "LAN";
        if (Has(L"Radmin"))         Kind = "Radmin";
        else if (Has(L"Hamachi"))   Kind = "Hamachi";
        else if (Has(L"ZeroTier"))  Kind = "ZeroTier";
        else if (Has(L"Tailscale")) Kind = "Tailscale";

        for (auto* U = Ad->FirstUnicastAddress; U; U = U->Next) {
            char Ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(U->Address.lpSockaddr)->sin_addr, Ip, sizeof(Ip));
            const std::string S(Ip);
            if (S.rfind("169.254.", 0) == 0 || S.rfind("127.", 0) == 0) continue;
            std::string K = Kind;
            if (K == "LAN" && (S.rfind("26.", 0) == 0 || S.rfind("25.", 0) == 0)) K = "VPN";
            const bool IsVpn = K != "LAN";
            (IsVpn ? Vpn : Lan).push_back({ K, S, IsVpn });
        }
    }
    Vpn.insert(Vpn.end(), Lan.begin(), Lan.end());
    return Vpn;
}

// ---- small drawing helpers -------------------------------------------------------

ImU32 A(ImU32 color) { return Kit::Fade(color, ImGui::GetStyle().Alpha); }

void DrawText(ImDrawList* list, ImFont* font, ImVec2 pos, ImU32 color, const char* text) {
    list->AddText(font, font->FontSize, pos, A(color), text);
}

ImVec2 TextSize(ImFont* font, const char* text) {
    return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text);
}

// A pill with a label, drawn in place; returns its width.
float DrawChip(ImDrawList* list, ImVec2 pos, const char* text, ImU32 color) {
    const float S = Kit::Scale();
    ImFont* Font = Kit::FontSmall();
    const ImVec2 T = TextSize(Font, text);
    const ImVec2 Max(pos.x + T.x + 16.0f * S, pos.y + T.y + 6.0f * S);
    const float R = (Max.y - pos.y) * 0.5f;
    list->AddRectFilled(pos, Max, A(Kit::Fade(color, 0.14f)), R);
    list->AddRect(pos, Max, A(Kit::Fade(color, 0.55f)), R, 0, 1.0f);
    DrawText(list, Font, ImVec2(pos.x + 8.0f * S, pos.y + 3.0f * S), color, text);
    return Max.x - pos.x;
}

void PageTitle(const char* text) {
    ImGui::PushFont(Kit::FontStrong());
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

bool BackLink() {
    return Kit::Button(Tr("\xE2\x80\xB9  Back", "\xE2\x80\xB9  Назад"), Kit::ButtonKind::Ghost, 0.0f);
}

} // namespace

// ============================================================================
// Singleton and entry points
// ============================================================================
Overlay& Overlay::GetInstance() {
    static Overlay instance;
    return instance;
}

void Overlay::Initialize() {
    if (m_initialized) return;
    m_initialized = true;   // ImGui itself starts on the first Present (renderer.cpp)
    LOG_INFO("Overlay initialized");
}

void Overlay::Shutdown() {
    if (!m_initialized) return;
    m_initialized = false;
    std::lock_guard<std::mutex> lock(m_notifMutex);
    m_notifications.clear();
}

void Overlay::ShowConnectionMenu() {
    m_visible = true;
    m_tab = 0;
    m_page = Page::Home;
}

void Overlay::ShowPlayerList() {
    m_visible = true;
    m_tab = 0;
}

void Overlay::HandleInput() {}   // input is read in PollMenuKey and the WndProc hook

void Overlay::Render() {
    RenderHint();
    RenderNotifications();
    RenderMenu();
}

// ============================================================================
// Window
// ============================================================================
void Overlay::RenderMenu() {
    if (!m_visible) m_capturingKey = false;
    m_menuAnim = Kit::Approach(m_menuAnim, m_visible ? 1.0f : 0.0f, m_visible ? 7.0f : 9.0f);
    if (m_menuAnim <= 0.001f) return;

    const float S = Kit::Scale();
    const float Ease = Kit::EaseOut(m_menuAnim);
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize,
                                                  IM_COL32(0, 0, 0, static_cast<int>(90.0f * Ease)));

    // Anchored by the top edge, so switching pages does not make it jump -- unless
    // the page would run off the bottom of the screen: then it moves up, and a page
    // taller than the whole screen scrolls.
    const float Margin = 12.0f * S;
    const float MaxH = std::max(io.DisplaySize.y - Margin * 2.0f, 120.0f * S);
    float Top = io.DisplaySize.y * 0.15f;
    if (Top + m_menuHeight > io.DisplaySize.y - Margin) {
        Top = std::max(Margin, io.DisplaySize.y - Margin - m_menuHeight);
    }
    const float Width = std::max(std::min(Kit::kMenuWidth * S, io.DisplaySize.x - Margin * 2.0f), 100.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, Top + (1.0f - Ease) * 14.0f * S),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(Width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(Width, 0.0f), ImVec2(Width, MaxH));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, Ease);

    ImGuiWindowFlags Flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings;
    // No scrollbar while the page fits: an auto-sized window lags its content by a
    // frame, and a bar flickering in on every page switch would shift the layout.
    if (m_menuContent <= MaxH) Flags |= ImGuiWindowFlags_NoScrollbar;
    if (!m_visible) Flags |= ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("##seamless_menu", nullptr, Flags)) {
        // An ember line along the top edge.
        const ImVec2 P = ImGui::GetWindowPos();
        const float W = ImGui::GetWindowWidth(), Mid = P.x + W * 0.5f;
        const ImU32 C0 = A(Kit::Fade(Kit::Col::Ember, 0.0f)), C1 = A(Kit::Fade(Kit::Col::Ember, 0.9f));
        ImDrawList* L = ImGui::GetWindowDrawList();
        L->AddRectFilledMultiColor(ImVec2(P.x + 40.0f * S, P.y + 1.0f), ImVec2(Mid, P.y + 1.0f + 2.0f * S), C0, C1, C1, C0);
        L->AddRectFilledMultiColor(ImVec2(Mid, P.y + 1.0f), ImVec2(P.x + W - 40.0f * S, P.y + 1.0f + 2.0f * S), C1, C0, C0, C1);

        RenderHeader();

        const char* TabNames[2] = { Tr("Lobby", "Лобби"), Tr("Settings", "Настройки") };
        if (Kit::Tabs("tabs", TabNames, 2, m_tab)) m_capturingKey = false;
        ImGui::Dummy(ImVec2(0.0f, 2.0f * S));

        if (m_tab == 1) {
            RenderSettingsPage();
        } else if (m_page == Page::NetCheck) {
            RenderNetCheckPage();
        } else if (SessionManager::GetInstance().IsActive()) {
            RenderSessionPage();
        } else if (m_page == Page::Host) {
            RenderHostPage();
        } else if (m_page == Page::Join) {
            RenderJoinPage();
        } else {
            RenderHomePage();
        }

        RenderFooter();

        const ImGuiStyle& Style = ImGui::GetStyle();
        m_menuContent = ImGui::GetCursorPosY() - Style.ItemSpacing.y + Style.WindowPadding.y;
        m_menuHeight = ImGui::GetWindowHeight();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::RenderHeader() {
    const float S = Kit::Scale();
    ImDrawList* L = ImGui::GetWindowDrawList();
    const ImVec2 P = ImGui::GetCursorScreenPos();
    const float W = ImGui::GetContentRegionAvail().x;
    ImFont* Title = Kit::FontTitle();
    ImFont* Small = Kit::FontSmall();

    // An ember diamond, the title, and where we stand underneath it.
    const ImVec2 D(P.x + 6.0f * S, P.y + Title->FontSize * 0.5f + 1.0f * S);
    const float R = 5.0f * S;
    L->AddQuadFilled(ImVec2(D.x, D.y - R), ImVec2(D.x + R, D.y), ImVec2(D.x, D.y + R), ImVec2(D.x - R, D.y),
                     A(Kit::Col::Ember));
    DrawText(L, Title, ImVec2(P.x + 20.0f * S, P.y), Kit::Col::Text, "SEAMLESS CO-OP");

    auto& Mgr = SessionManager::GetInstance();
    std::string Status;
    if (!Mgr.IsActive())  Status = Tr("Not in a lobby", "Вне лобби");
    else if (Mgr.IsHost()) Status = Format(Tr("Hosting \xC2\xB7 players: %zu", "Хост \xC2\xB7 игроков: %zu"), Mgr.GetPlayers().size());
    else if (!PeerManager::GetInstance().IsHandshakeConfirmed())
                           Status = Tr("Connecting\xE2\x80\xA6", "Подключение\xE2\x80\xA6");
    else                   Status = Tr("In the host's world", "В мире хоста");
    DrawText(L, Small, ImVec2(P.x + 20.0f * S, P.y + Title->FontSize + 2.0f * S), Kit::Col::TextMuted, Status.c_str());

    ImGui::SetCursorScreenPos(ImVec2(P.x + W - 30.0f * S, P.y));
    if (Kit::CloseButton("close")) m_visible = false;

    ImGui::SetCursorScreenPos(ImVec2(P.x, P.y + Title->FontSize + Small->FontSize + 18.0f * S));
}

// ============================================================================
// Lobby tab
// ============================================================================
void Overlay::RenderHomePage() {
    Kit::Paragraph(Tr("Host a lobby or join a friend \xE2\x80\x94 the summon happens by itself.",
                      "Создайте лобби или подключитесь к другу \xE2\x80\x94 призыв произойдёт сам."));
    ImGui::Dummy(ImVec2(0.0f, 2.0f * Kit::Scale()));

    if (Kit::ActionCard("host", Kit::IconHost, Tr("Host a lobby", "Создать лобби"),
                        Tr("Friends join you with a password", "Друзья заходят к вам по паролю"))) {
        m_page = Page::Host;
        m_inputPassword[0] = '\0';
        m_focusField = true;
        m_showAddresses = false;
    }
    if (Kit::ActionCard("join", Kit::IconJoin, Tr("Join a friend", "Подключиться к другу"),
                        Tr("You need the host's address and password", "Нужны адрес хоста и пароль"))) {
        m_page = Page::Join;
        m_inputPassword[0] = '\0';
        m_focusField = true;
    }
    if (Kit::Button(Tr("Check the connection", "Проверка связи"), Kit::ButtonKind::Ghost, 0.0f)) {
        m_page = Page::NetCheck;
    }
}

void Overlay::RenderHostPage() {
    const float S = Kit::Scale();
    if (BackLink()) { m_page = Page::Home; return; }
    PageTitle(Tr("Create a lobby", "Создать лобби"));

    Kit::InputField(Tr("Lobby password", "Пароль лобби"), "##pw", m_inputPassword, sizeof(m_inputPassword),
                    Tr("Any word \xE2\x80\x94 your friend types it", "Любое слово \xE2\x80\x94 его введёт друг"), m_focusField);
    m_focusField = false;
    ImGui::Dummy(ImVec2(0.0f, 2.0f * S));

    Kit::SectionLabel(Tr("YOUR ADDRESS", "ВАШ АДРЕС"));
    if (!m_showAddresses) {
        if (Kit::Button(Tr("Show addresses", "Показать адреса"), Kit::ButtonKind::Secondary)) m_showAddresses = true;
    } else {
        static std::vector<Address> Addresses;
        static double LastRefresh = -100.0;
        if (ImGui::GetTime() - LastRefresh > 10.0) {
            Addresses = LocalAddresses();
            LastRefresh = ImGui::GetTime();
        }
        EnsurePublicIPFetched();

        ImDrawList* L = ImGui::GetWindowDrawList();
        ImFont* Strong = Kit::FontStrong();
        const float W = ImGui::GetContentRegionAvail().x;
        const float CopyW = TextSize(Kit::FontBody(), Tr("Copy", "Копировать")).x + 36.0f * S;

        auto Row = [&](int Index, const char* Kind, const std::string& Ip, ImU32 Color) {
            ImGui::PushID(Index);
            const ImVec2 P = ImGui::GetCursorScreenPos();
            const float RowH = Kit::FontBody()->FontSize + 8.0f * S;
            const float ChipW = DrawChip(L, ImVec2(P.x, P.y + (RowH - Kit::FontSmall()->FontSize - 6.0f * S) * 0.5f), Kind, Color);
            DrawText(L, Strong, ImVec2(P.x + ChipW + 10.0f * S, P.y + (RowH - Strong->FontSize) * 0.5f), Kit::Col::Text, Ip.c_str());
            ImGui::SetCursorScreenPos(ImVec2(P.x + W - CopyW, P.y));
            if (Kit::Button(Tr("Copy##c", "Копировать##c"), Kit::ButtonKind::Ghost, CopyW)) {
                CopyToClipboard(Ip);
                ShowNotification(Format(Tr("Copied %s", "Скопировано: %s"), Ip.c_str()), 2.5f, NotifyKind::Info);
            }
            ImGui::PopID();
        };

        int Index = 0;
        for (const Address& Ad : Addresses) Row(Index++, Ad.Kind.c_str(), Ad.Ip, Ad.Vpn ? Kit::Col::Ember : Kit::Col::Gold);
        if (g_publicIPFetched && !g_publicIP.empty()) {
            Row(Index++, Tr("PUBLIC", "ПУБЛИЧНЫЙ"), g_publicIP, Kit::Col::TextMuted);
        } else if (!g_publicIPFetched) {
            Kit::Paragraph(Tr("Looking up the public address\xE2\x80\xA6", "Определяем публичный адрес\xE2\x80\xA6"), Kit::Col::TextFaint);
        }
        if (Kit::Button(Tr("Hide", "Скрыть"), Kit::ButtonKind::Ghost, 0.0f)) m_showAddresses = false;
    }
    Kit::Paragraph(Tr("On one network or a VPN (Radmin, Hamachi) your friend uses the VPN or LAN address; "
                      "over the internet, the public one with port 27015 open.",
                      "В одной сети или через VPN (Radmin, Hamachi) другу нужен VPN- или LAN-адрес, "
                      "через интернет \xE2\x80\x94 публичный, с открытым портом 27015."),
                   Kit::Col::TextFaint);
    ImGui::Dummy(ImVec2(0.0f, 2.0f * S));

    if (Kit::Button(Tr("Create lobby", "Создать лобби"), Kit::ButtonKind::Primary, -1.0f, m_inputPassword[0] != '\0')) {
        if (SessionManager::GetInstance().CreateSession(m_inputPassword)) {
            ShowNotification(Tr("Lobby created. Waiting for players\xE2\x80\xA6", "Лобби создано. Ждём игроков\xE2\x80\xA6"),
                             5.0f, NotifyKind::Success);
            m_page = Page::Home;
        } else {
            ShowNotification(Tr("Could not create the lobby \xE2\x80\x94 see the log.",
                                "Не удалось создать лобби \xE2\x80\x94 подробности в логе."), 5.0f, NotifyKind::Error);
        }
    }
}

void Overlay::RenderJoinPage() {
    const float S = Kit::Scale();
    if (BackLink()) { m_page = Page::Home; return; }
    PageTitle(Tr("Join a friend", "Подключиться к другу"));

    const bool FocusIp = m_focusField && m_inputIP[0] == '\0';
    const bool FocusPw = m_focusField && !FocusIp;
    m_focusField = false;
    Kit::InputField(Tr("Host address", "Адрес хоста"), "##ip", m_inputIP, sizeof(m_inputIP),
                    Tr("e.g. 26.12.34.56", "например, 26.12.34.56"), FocusIp);
    Kit::InputField(Tr("Lobby password", "Пароль лобби"), "##pw", m_inputPassword, sizeof(m_inputPassword),
                    Tr("The password the host chose", "Пароль, который задал хост"), FocusPw);
    Kit::Paragraph(Tr("A port can follow the address: 26.12.34.56:27015.",
                      "К адресу можно дописать порт: 26.12.34.56:27015."), Kit::Col::TextFaint);
    ImGui::Dummy(ImVec2(0.0f, 2.0f * S));

    const bool CanJoin = m_inputIP[0] != '\0' && m_inputPassword[0] != '\0';
    if (Kit::Button(Tr("Connect", "Подключиться"), Kit::ButtonKind::Primary, -1.0f, CanJoin)) {
        if (SessionManager::GetInstance().JoinSession(m_inputIP, m_inputPassword)) {
            ShowNotification(Tr("Connecting to the host\xE2\x80\xA6", "Подключаемся к хосту\xE2\x80\xA6"), 4.0f, NotifyKind::Info);
            m_page = Page::Home;
        } else {
            ShowNotification(Tr("Could not connect. Check the address and password.",
                                "Не удалось подключиться. Проверьте адрес и пароль."), 5.0f, NotifyKind::Error);
        }
    }
}

void Overlay::RenderSessionPage() {
    const float S = Kit::Scale();
    auto& Mgr = SessionManager::GetInstance();
    const bool IsHost = Mgr.IsHost();
    const bool Waiting = !IsHost && !PeerManager::GetInstance().IsHandshakeConfirmed();
    const auto Players = Mgr.GetPlayers();
    const uint64_t LocalId = PeerManager::GetInstance().GetLocalPlayerId();
    ImDrawList* L = ImGui::GetWindowDrawList();
    const float W = ImGui::GetContentRegionAvail().x;

    // Status card.
    {
        const ImVec2 P = ImGui::GetCursorScreenPos();
        const float H = 60.0f * S;
        ImGui::Dummy(ImVec2(W, H));
        L->AddRectFilled(P, ImVec2(P.x + W, P.y + H), A(Kit::Col::Surface), 10.0f * S);
        L->AddRect(P, ImVec2(P.x + W, P.y + H), A(Kit::Col::Line), 10.0f * S, 0, 1.0f);
        Kit::StatusDot(L, ImVec2(P.x + 22.0f * S, P.y + H * 0.5f), Waiting ? Kit::Col::Amber : Kit::Col::Green, true);

        const char* Title = Waiting ? Tr("Connecting to the host\xE2\x80\xA6", "Подключение к хосту\xE2\x80\xA6")
                          : IsHost  ? Tr("Your lobby is open", "Ваше лобби открыто")
                                    : Tr("You are in the host's world", "Вы в мире хоста");
        const std::string Sub = IsHost ? Format(Tr("Players: %zu", "Игроков: %zu"), Players.size())
                                       : std::string(Tr("You can leave from here at any time", "Выйти можно здесь в любой момент"));
        ImFont* Strong = Kit::FontStrong();
        ImFont* Small = Kit::FontSmall();
        const float Top = P.y + (H - Strong->FontSize - Small->FontSize - 2.0f * S) * 0.5f;
        DrawText(L, Strong, ImVec2(P.x + 42.0f * S, Top), Kit::Col::Text, Title);
        DrawText(L, Small, ImVec2(P.x + 42.0f * S, Top + Strong->FontSize + 2.0f * S), Kit::Col::TextMuted, Sub.c_str());

        const char* Role = IsHost ? Tr("HOST", "ХОСТ") : Tr("GUEST", "ГОСТЬ");
        const float RoleW = TextSize(Small, Role).x + 16.0f * S;
        DrawChip(L, ImVec2(P.x + W - RoleW - 14.0f * S, P.y + (H - Small->FontSize - 6.0f * S) * 0.5f), Role,
                 IsHost ? Kit::Col::Ember : Kit::Col::Gold);
    }

    Kit::SectionLabel(Format(Tr("PLAYERS \xC2\xB7 %zu", "ИГРОКИ \xC2\xB7 %zu"), Players.size()).c_str());
    for (const SessionPlayer& Pl : Players) {
        const bool Me = Pl.playerId == 0 || Pl.playerId == LocalId;
        const bool Dead = !Pl.isAlive && Pl.maxHealth > 0;
        const bool HasHp = Pl.maxHealth > 0;
        const ImVec2 P = ImGui::GetCursorScreenPos();
        ImFont* Body = Kit::FontBody();
        const float H = Body->FontSize + (HasHp ? 12.0f : 6.0f) * S;
        ImGui::Dummy(ImVec2(W, H));

        DrawText(L, Body, P, Dead ? Kit::Col::TextFaint : Kit::Col::Text, Pl.playerName.c_str());
        if (Me) {
            const float NameW = TextSize(Body, Pl.playerName.c_str()).x;
            DrawChip(L, ImVec2(P.x + NameW + 8.0f * S,
                               P.y + (Body->FontSize - Kit::FontSmall()->FontSize - 6.0f * S) * 0.5f + 1.0f * S),
                     Tr("you", "вы"), Kit::Col::Gold);
        }
        if (HasHp) {
            const float Frac = std::clamp(static_cast<float>(Pl.health) / static_cast<float>(Pl.maxHealth), 0.0f, 1.0f);
            const ImU32 Bar = Frac > 0.5f ? Kit::Col::Green : Frac > 0.25f ? Kit::Col::Amber : Kit::Col::Red;
            const float Y = P.y + Body->FontSize + 5.0f * S;
            L->AddRectFilled(ImVec2(P.x, Y), ImVec2(P.x + W, Y + 3.0f * S), A(Kit::Col::Surface2), 2.0f * S);
            L->AddRectFilled(ImVec2(P.x, Y), ImVec2(P.x + W * Frac, Y + 3.0f * S), A(Bar), 2.0f * S);
        }
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f * S));

    // Damage between the players: the host picks, guests follow (pvp_modes.cpp).
    {
        Kit::SectionLabel(Tr("DAMAGE BETWEEN PLAYERS", "УРОН МЕЖДУ ИГРОКАМИ"));
        const char* Names[3] = { Tr("None", "Нет"), Tr("Friendly fire", "Огонь по своим"), Tr("PvP", "PvP") };
        const char* Hints[3] = {
            Tr("Hits between you do nothing.", "Удары друг по другу ничего не делают."),
            Tr("Hits land, but you cannot lock on to each other; enemies still fight both of you.",
               "Удары проходят, но навестись друг на друга нельзя; враги по-прежнему бьют обоих."),
            Tr("The guest is an evil spirit: lock-on works both ways, and the host's enemies leave the guest alone.",
               "Гость \xE2\x80\x94 злой дух: наводиться можно в обе стороны, а враги хоста гостя не трогают."),
        };
        if (IsHost) {
            int Current = DS2Coop::Sync::GetChosenDamageMode();
            if (Current < 0 || Current > 2) Current = 0;
            if (Kit::Tabs("damage", Names, 3, Current)) {
                DS2Coop::Sync::SetDamageMode(static_cast<uint8_t>(Current));
                DS2Coop::SeamlessCoopMod::GetInstance().SetDamageModeSetting(static_cast<uint8_t>(Current));
                ShowNotification(Format(Tr("Damage between players: %s", "Урон между игроками: %s"), Names[Current]),
                                 3.0f, NotifyKind::Info);
            }
            Kit::Paragraph(Hints[Current], Kit::Col::TextFaint);
        } else {
            int Mode = DS2Coop::Sync::GetDamageMode();
            if (Mode < 0 || Mode > 2) Mode = 0;
            Kit::Paragraph(Format(Tr("Chosen by the host: %s. %s", "Выбрал хост: %s. %s"), Names[Mode], Hints[Mode]).c_str(),
                           Kit::Col::TextMuted);
        }
    }

    if (Kit::Button(Tr("Check the connection", "Проверить связь"), Kit::ButtonKind::Secondary)) {
        m_page = Page::NetCheck;
    }
    if (Kit::Button(Tr("Give me soapstones", "Выдать мелки"), Kit::ButtonKind::Secondary)) {
        if (DS2Coop::Sync::PlayerSync::GetInstance().GrantSoapstones()) {
            ShowNotification(Tr("Soapstones added to your inventory.", "Мелки добавлены в инвентарь."), 4.0f, NotifyKind::Success);
        } else {
            ShowNotification(Tr("Could not add them \xE2\x80\x94 try again in game.",
                                "Не удалось выдать \xE2\x80\x94 попробуйте уже в игре."), 4.0f, NotifyKind::Warning);
        }
    }
    if (Kit::Button(Tr("Leave the lobby", "Покинуть лобби"), Kit::ButtonKind::Danger)) {
        // Closing the mod's channel alone left the phantom standing in the
        // host's world; the game has to be told as well.
        DS2Coop::Sync::RequestLeaveWorld();
        Mgr.LeaveSession();
        DS2Coop::Hooks::ProtobufHooks::SetSeamlessActive(false);
        m_visible = false;
        m_page = Page::Home;
        ShowNotification(Tr("You left the lobby.", "Вы покинули лобби."), 4.0f, NotifyKind::Info);
    }
}

// ============================================================================
// Connection check (net_check.cpp)
// ============================================================================
void Overlay::RenderNetCheckPage() {
    using DS2Coop::Network::CheckLevel;
    const float S = Kit::Scale();
    if (BackLink()) { m_page = Page::Home; return; }
    PageTitle(Tr("Connection check", "Проверка связи"));

    const DS2Coop::Network::NetCheckView View = DS2Coop::Network::GetNetCheckView();
    if (!View.running && !View.done) {
        Kit::Paragraph(Tr("Looks at the route to the server, VPNs and proxies, DNS and the server's ports. In a lobby it also "
                          "sends packets of every size between you and the partner, both ways. The partner needs this version of the mod.",
                          "Проверяет маршрут к серверу, VPN и прокси, DNS и порты сервера. В лобби ещё гоняет пакеты всех "
                          "размеров между вами и напарником в обе стороны — у напарника должна быть эта же версия мода."),
                       Kit::Col::TextMuted);
    }

    const char* Label = View.running ? Tr("Checking\xE2\x80\xA6", "Проверяю\xE2\x80\xA6")
                      : View.done    ? Tr("Check again", "Проверить ещё раз")
                                     : Tr("Start the check", "Проверить");
    if (Kit::Button(Label, Kit::ButtonKind::Primary, -1.0f, !View.running)) DS2Coop::Network::StartNetCheck();

    ImDrawList* L = ImGui::GetWindowDrawList();
    if (View.running) {
        const ImVec2 P = ImGui::GetCursorScreenPos();
        const float W = ImGui::GetContentRegionAvail().x, H = 6.0f * S;
        ImGui::Dummy(ImVec2(W, H));
        L->AddRectFilled(P, ImVec2(P.x + W, P.y + H), A(Kit::Col::Surface2), H * 0.5f);
        L->AddRectFilled(P, ImVec2(P.x + W * std::clamp(View.progress, 0.02f, 1.0f), P.y + H), A(Kit::Col::Ember), H * 0.5f);
    }

    const float Indent = 18.0f * S;
    for (const DS2Coop::Network::CheckLine& Line : View.lines) {
        const ImU32 Color = Line.level == CheckLevel::Ok   ? Kit::Col::Green
                          : Line.level == CheckLevel::Warn ? Kit::Col::Amber
                          : Line.level == CheckLevel::Fail ? Kit::Col::Red
                                                           : Kit::Col::Gold;
        const ImVec2 P = ImGui::GetCursorScreenPos();
        Kit::StatusDot(L, ImVec2(P.x + 5.0f * S, P.y + Kit::FontStrong()->FontSize * 0.5f + 1.0f * S), Color, false);
        ImGui::Indent(Indent);
        ImGui::PushFont(Kit::FontStrong());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(Line.title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        if (!Line.detail.empty()) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 7.0f * S);
            ImGui::PushFont(Kit::FontSmall());
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Kit::Col::TextMuted));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(Line.detail.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        ImGui::Unindent(Indent);
    }

    if (View.done) {
        ImGui::Dummy(ImVec2(0.0f, 2.0f * S));
        if (Kit::Button(Tr("Copy the report", "Скопировать отчёт"), Kit::ButtonKind::Secondary)) {
            CopyToClipboard(View.report);
            ShowNotification(Tr("Report copied: paste it to the host.", "Отчёт скопирован: отправь его хосту."),
                             3.0f, NotifyKind::Success);
        }
    }
}

// ============================================================================
// Settings tab
// ============================================================================
void Overlay::RenderSettingsPage() {
    const float S = Kit::Scale();
    ImDrawList* L = ImGui::GetWindowDrawList();
    const float W = ImGui::GetContentRegionAvail().x;
    ImFont* Strong = Kit::FontStrong();
    ImFont* Small = Kit::FontSmall();
    const float RowH = 54.0f * S;

    auto RowText = [&](ImVec2 P, const char* Title, const char* Desc) {
        const float Top = P.y + (RowH - Strong->FontSize - Small->FontSize - 2.0f * S) * 0.5f;
        DrawText(L, Strong, ImVec2(P.x, Top), Kit::Col::Text, Title);
        DrawText(L, Small, ImVec2(P.x, Top + Strong->FontSize + 2.0f * S), Kit::Col::TextMuted, Desc);
    };
    auto EndRow = [&](ImVec2 P) {
        ImGui::SetCursorScreenPos(ImVec2(P.x, P.y + RowH));
        ImGui::Dummy(ImVec2(W, 0.0f));
    };

    // Language.
    {
        const ImVec2 P = ImGui::GetCursorScreenPos();
        RowText(P, Tr("Language", "Язык"), Tr("Menu, hint and notifications", "Меню, подсказка и уведомления"));
        ImGui::SetCursorScreenPos(ImVec2(P.x + W - 108.0f * S, P.y + (RowH - 34.0f * S) * 0.5f));
        bool Russian = GetLanguage() == Language::Russian;
        if (Kit::LanguageSwitch("lang", Russian)) SetLanguage(Russian ? Language::Russian : Language::English);
        EndRow(P);
    }
    Kit::Divider();

    // Menu size: 100% is what suits the screen; the whole overlay follows the
    // choice from the next frame (renderer.cpp builds the fonts again).
    {
        const ImVec2 P = ImGui::GetCursorScreenPos();
        RowText(P, Tr("Menu size", "Размер меню"),
                Tr("Follows the screen; make it larger or smaller here",
                   "Подстраивается под экран; здесь можно крупнее или мельче"));
        EndRow(P);
        static constexpr int kSizes[] = { 85, 100, 115, 130, 150 };
        static constexpr const char* kLabels[] = { "85%", "100%", "115%", "130%", "150%" };
        constexpr int kCount = static_cast<int>(sizeof(kSizes) / sizeof(kSizes[0]));
        int Current = 0;
        for (int I = 1; I < kCount; I++) {
            if (std::abs(kSizes[I] - GetMenuSize()) < std::abs(kSizes[Current] - GetMenuSize())) Current = I;
        }
        if (Kit::Tabs("menu_size", kLabels, kCount, Current)) SetMenuSize(kSizes[Current]);
        ImGui::Dummy(ImVec2(0.0f, 4.0f * S));
    }
    Kit::Divider();

    // Menu key.
    {
        const ImVec2 P = ImGui::GetCursorScreenPos();
        RowText(P, Tr("Menu key", "Клавиша меню"),
                m_capturingKey ? Tr("Press any key \xC2\xB7 Esc to cancel", "Нажмите клавишу \xC2\xB7 Esc \xE2\x80\x94 отмена")
                               : Tr("Opens and closes this menu", "Открывает и закрывает это меню"));
        const std::string Key = KeyName(GetMenuKey());
        const char* Label = m_capturingKey ? Tr("Press\xE2\x80\xA6", "Нажмите\xE2\x80\xA6") : Key.c_str();
        const float KeyW = std::max(TextSize(Strong, Label).x + 32.0f * S, 88.0f * S);
        ImGui::SetCursorScreenPos(ImVec2(P.x + W - KeyW, P.y + (RowH - 38.0f * S) * 0.5f));
        if (Kit::KeyBindButton("bind", Label, m_capturingKey)) {
            m_capturingKey = !m_capturingKey;
            m_captureError.clear();
            for (int Vk = 0; Vk < 256; Vk++) m_keyWasDown[Vk] = (GetAsyncKeyState(Vk) & 0x8000) != 0;
        }
        EndRow(P);
        if (m_capturingKey) UpdateKeyCapture();
    }
    if (!m_captureError.empty()) Kit::Paragraph(m_captureError.c_str(), Kit::Col::Red);

    // Spare key: fixed, shown as a key rather than a button.
    {
        const ImVec2 P = ImGui::GetCursorScreenPos();
        RowText(P, Tr("Spare key", "Запасная клавиша"),
                Tr("Always opens the menu, whatever the main key is",
                   "Открывает меню всегда, какой бы ни была основная"));
        const ImVec2 Cap = Kit::KeycapSize("Insert");
        Kit::Keycap(L, ImVec2(P.x + W - Cap.x, P.y + (RowH - Cap.y) * 0.5f), "Insert", ImGui::GetStyle().Alpha);
        EndRow(P);
    }
    Kit::Divider();

    Kit::Paragraph(Tr("The hint in the corner shows while you are not in a lobby, "
                      "and again after a minute of standing still.",
                      "Подсказка в углу видна, пока вы не в лобби, "
                      "и снова \xE2\x80\x94 если простоять на месте минуту."),
                   Kit::Col::TextFaint);
}

void Overlay::RenderFooter() {
    const float S = Kit::Scale();
    ImGui::Dummy(ImVec2(0.0f, 2.0f * S));
    Kit::Divider();

    ImDrawList* L = ImGui::GetWindowDrawList();
    const ImVec2 P = ImGui::GetCursorScreenPos();
    const float W = ImGui::GetContentRegionAvail().x, H = 26.0f * S;
    ImGui::Dummy(ImVec2(W, H));

    const std::string Key = KeyName(GetMenuKey());
    const ImVec2 Cap = Kit::KeycapSize(Key.c_str());
    const ImVec2 EscCap = Kit::KeycapSize("Esc");
    const float Alpha = ImGui::GetStyle().Alpha;
    Kit::Keycap(L, ImVec2(P.x, P.y + (H - Cap.y) * 0.5f), Key.c_str(), Alpha);
    Kit::Keycap(L, ImVec2(P.x + Cap.x + 6.0f * S, P.y + (H - EscCap.y) * 0.5f), "Esc", Alpha);
    ImFont* Small = Kit::FontSmall();
    DrawText(L, Small, ImVec2(P.x + Cap.x + 6.0f * S + EscCap.x + 8.0f * S, P.y + (H - Small->FontSize) * 0.5f - 1.0f * S),
             Kit::Col::TextMuted, Tr("close", "закрыть"));

    const std::string Version = std::string("v") + DS2Coop::MOD_VERSION;
    const float VersionW = TextSize(Small, Version.c_str()).x;
    DrawText(L, Small, ImVec2(P.x + W - VersionW, P.y + (H - Small->FontSize) * 0.5f - 1.0f * S),
             Kit::Col::TextFaint, Version.c_str());
}

} // namespace DS2Coop::UI
