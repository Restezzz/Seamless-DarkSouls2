// Stand-ins for the parts of the mod the overlay talks to, so the menu can be
// drawn outside the game. Private data members are opened up only in this file
// and in preview_main.cpp, to fake a session; no private function is called.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define private public
#include "../../include/utils.h"
#include "../../include/session.h"
#include "../../include/network.h"
#include "../../include/net_check.h"
#include "../../include/ui_settings.h"
#include "../../include/sync.h"
#include "../../include/hooks.h"
#include "../../include/mod.h"
#undef private

namespace DS2Coop::Utils {
Logger& Logger::GetInstance() { static Logger Instance; return Instance; }
static void Print(const char* tag, const char* fmt, va_list args) {
    std::printf("[%s] ", tag);
    std::vprintf(fmt, args);
    std::printf("\n");
}
void Logger::LogDebug(const char* f, ...)   { va_list a; va_start(a, f); Print("DEBUG", f, a); va_end(a); }
void Logger::LogInfo(const char* f, ...)    { va_list a; va_start(a, f); Print("INFO", f, a); va_end(a); }
void Logger::LogWarning(const char* f, ...) { va_list a; va_start(a, f); Print("WARN", f, a); va_end(a); }
void Logger::LogError(const char* f, ...)   { va_list a; va_start(a, f); Print("ERROR", f, a); va_end(a); }
}

namespace DS2Coop::Session {
SessionManager& SessionManager::GetInstance() { static SessionManager Instance; return Instance; }
bool SessionManager::CreateSession(const std::string&) { m_state = SessionState::Connected; m_isHost = true; return true; }
bool SessionManager::JoinSession(const std::string&, const std::string&) { m_state = SessionState::Connected; m_isHost = false; return true; }
void SessionManager::LeaveSession() { m_state = SessionState::Disconnected; m_players.clear(); }
}

namespace DS2Coop::Network {
PeerManager& PeerManager::GetInstance() { static PeerManager Instance; return Instance; }
}

namespace DS2Coop::Sync {
PlayerSync& PlayerSync::GetInstance() { static PlayerSync Instance; return Instance; }
bool PlayerSync::GrantSoapstones() { return true; }
void RequestLeaveWorld() {}
bool GetLocalPlayerPosition(float& x, float& y, float& z, float& r) { x = y = z = r = 0.0f; return true; }
static uint8_t s_damageMode = 1;   // friendly fire, so the preview shows a choice other than the default
void    SetDamageMode(uint8_t mode) { s_damageMode = mode; }
uint8_t GetDamageMode() { return s_damageMode; }
int  GetEstusFlaskState() { return 0; }
void RequestEstusGrant() {}
uint8_t GetChosenDamageMode() { return s_damageMode; }
}

namespace DS2Coop::Hooks::ProtobufHooks {
void SetSeamlessActive(bool) {}
}

// The connection check as it read on 17.09, made up only in the names.
namespace DS2Coop::Network {
void StartNetCheck() {}
NetCheckView GetNetCheckView() {
    using DS2Coop::UI::Tr;
    NetCheckView V;
    V.done = true;
    V.progress = 1.0f;
    V.lines = {
        { CheckLevel::Warn, Tr("VPN or proxy adapters are up", "Работают VPN или прокси-адаптеры"),
          Tr("happ-tun (Wintun Userspace Tunnel). A VPN or a proxy in TUN mode can carry Radmin's own traffic and lose large packets.",
             "happ-tun (Wintun Userspace Tunnel). VPN или прокси в режиме TUN может пропускать через себя сам туннель Radmin и терять крупные пакеты.") },
        { CheckLevel::Ok, Tr("Route to the server 26.12.34.56", "Маршрут к серверу 26.12.34.56"), Tr("through Radmin VPN (MTU 1500).", "через Radmin VPN (MTU 1500).") },
        { CheckLevel::Warn, Tr("Internet traffic", "Интернет-трафик"),
          Tr("goes through happ-tun. Radmin VPN's own tunnel may go through it too: put RvControlSvc.exe and DarkSoulsII.exe into its exceptions (direct).",
             "идёт через happ-tun. Сам туннель Radmin VPN тоже может идти через него: добавь RvControlSvc.exe и DarkSoulsII.exe в исключения (напрямую, direct).") },
        { CheckLevel::Ok, "DNS", Tr("ordinary answers.", "обычные ответы.") },
        { CheckLevel::Ok, Tr("Server ports", "Порты сервера"), Tr("login 50031: 2 ms, auth 50000: 1 ms.", "вход 50031: 2 мс, авторизация 50000: 1 мс.") },
        { CheckLevel::Ok, Tr("Game server", "Игровой сервер"), Tr("last message 4 s ago.", "последнее сообщение 4 с назад.") },
        { CheckLevel::Fail, Tr("Packets from me to Lucatiel", "Пакеты от меня к Lucatiel"),
          Tr("64: 8/8 \xC2\xB7 512: 8/8 \xC2\xB7 1024: 8/8 \xC2\xB7 1200: 8/8 \xC2\xB7 1300: 8/8 \xC2\xB7 1400: 8/8 \xC2\xB7 1472: 8/8 \xC2\xB7 2000: 0/8 \xC2\xB7 4000: 0/8 \xC2\xB7 8000: 0/8. Nothing over 1472 bytes gets through.",
             "64: 8/8 \xC2\xB7 512: 8/8 \xC2\xB7 1024: 8/8 \xC2\xB7 1200: 8/8 \xC2\xB7 1300: 8/8 \xC2\xB7 1400: 8/8 \xC2\xB7 1472: 8/8 \xC2\xB7 2000: 0/8 \xC2\xB7 4000: 0/8 \xC2\xB7 8000: 0/8. Больше 1472 байт не проходит ничего.") },
        { CheckLevel::Ok, Tr("Packets from Lucatiel to me", "Пакеты от Lucatiel ко мне"),
          "64: 8/8 \xC2\xB7 512: 8/8 \xC2\xB7 1024: 8/8 \xC2\xB7 1200: 8/8 \xC2\xB7 1300: 8/8 \xC2\xB7 1400: 8/8 \xC2\xB7 1472: 8/8 \xC2\xB7 2000: 8/8 \xC2\xB7 4000: 8/8 \xC2\xB7 8000: 8/8" },
        { CheckLevel::Info, Tr("Delay", "Задержка"), Tr("about 38 ms there and back.", "около 38 мс туда и обратно.") },
        { CheckLevel::Fail, Tr("Verdict", "Вывод"),
          Tr("datagrams over 1472 bytes do not get through between you. The game server's large packets die the same way, and after a few failed retries it drops the player. The usual cause is a VPN or proxy in TUN mode on the path (happ-tun): put Radmin VPN (RvControlSvc.exe) and DarkSoulsII.exe into its exceptions on both computers, or turn it off while playing.",
             "пакеты больше 1472 байт между вами не проходят. Крупные пакеты игрового сервера гибнут так же, и после нескольких неудачных повторов сервер отключает игрока. Обычная причина — VPN или прокси в режиме TUN на пути (happ-tun): добавь Radmin VPN (RvControlSvc.exe) и DarkSoulsII.exe в его исключения на обоих компьютерах или выключай его на время игры.") },
    };
    V.report = "report";
    return V;
}
}

namespace DS2Coop::UI {
void SetGameInputBlocked(bool) {}
bool IsGameInputBlocked() { return false; }
void InstallInputCapture() {}
void SetMenuCursorWanted(bool) {}
void ApplyMenuCursor(HWND) {}
HWND GetGameWindow() { return nullptr; }
}

namespace DS2Coop {
SeamlessCoopMod& SeamlessCoopMod::GetInstance() { static SeamlessCoopMod Instance; return Instance; }
void SeamlessCoopMod::SetUiPreferences(const std::string&, const std::string&, int) {}
uint8_t SeamlessCoopMod::GetDamageModeSetting() const { return 1; }
void SeamlessCoopMod::SetDamageModeSetting(uint8_t) {}
}

// 0: no lobby. 1: hosting with a guest. 2: guest still waiting for the host.
// 3: guest in the host's world. The names are made up (they go into the README).
void PreviewSetSession(int mode) {
    using namespace DS2Coop::Session;
    auto& S = SessionManager::GetInstance();
    auto& P = DS2Coop::Network::PeerManager::GetInstance();
    S.m_players.clear();
    if (mode == 0) { S.m_state = SessionState::Disconnected; return; }
    S.m_state = SessionState::Connected;
    S.m_isHost = mode == 1;
    P.m_localPlayerId = 111;
    P.m_handshakeConfirmed = mode != 2;
    SessionPlayer Me{};
    Me.playerId = 111; Me.playerName = "Bearer"; Me.isAlive = true; Me.health = 826; Me.maxHealth = 826;
    SessionPlayer Friend{};
    Friend.playerId = 222; Friend.playerName = "Lucatiel"; Friend.isAlive = true; Friend.health = 310; Friend.maxHealth = 720;
    S.m_players.push_back(Me);
    if (mode != 2) S.m_players.push_back(Friend);
}
