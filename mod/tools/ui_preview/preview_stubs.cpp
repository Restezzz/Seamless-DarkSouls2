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
}

namespace DS2Coop::Hooks::ProtobufHooks {
void SetSeamlessActive(bool) {}
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
void SeamlessCoopMod::SetUiPreferences(const std::string&, const std::string&) {}
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
