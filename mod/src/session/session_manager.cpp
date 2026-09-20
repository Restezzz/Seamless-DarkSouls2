// Session manager - tracks players, drives sync loops
//
// Coordinates between PeerManager (networking) and PlayerSync/ProgressSync.

#include "../../include/session.h"
#include "../../include/network.h"
#include "../../include/hooks.h"
#include "../../include/sync.h"
#include "../../include/ui.h"
#include "../../include/utils.h"
#include "../../include/ui_settings.h"
#include <algorithm>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <chrono>

using namespace DS2Coop::Session;
using namespace DS2Coop::Utils;

SessionManager& SessionManager::GetInstance() {
    static SessionManager instance;
    return instance;
}

bool SessionManager::Initialize() {
    if (m_initialized) return true;

    LOG_INFO("Initializing session manager...");

    m_state = SessionState::Disconnected;
    m_initialized = true;

    LOG_INFO("Session manager initialized");
    return true;
}

void SessionManager::Shutdown() {
    if (!m_initialized) return;

    LOG_INFO("Shutting down session manager...");

    LeaveSession();
    m_initialized = false;

    LOG_INFO("Session manager shut down");
}

bool SessionManager::CreateSession(const std::string& password) {
    if (!m_initialized) return false;

    LOG_INFO("Creating new session...");

    TransitionToState(SessionState::Connecting);

    m_isHost = true;
    m_sessionPassword = password;

    // Create network session
    auto& peerMgr = Network::PeerManager::GetInstance();
    if (!peerMgr.CreateSession(password)) {
        LOG_ERROR("Failed to create network session");
        TransitionToState(SessionState::Error);
        return false;
    }

    // Use PeerManager's generated player ID
    m_localPlayerId = peerMgr.GetLocalPlayerId();

    // Add local player
    SessionPlayer localPlayer{};
    localPlayer.playerId = m_localPlayerId;
    localPlayer.isAlive = true;
    localPlayer.isReady = true;
    localPlayer.soulLevel = 0;
    localPlayer.health = 0;
    localPlayer.maxHealth = 0;
    localPlayer.x = localPlayer.y = localPlayer.z = 0.0f;

    // Initialize sync systems first so we can read the character name
    Sync::PlayerSync::GetInstance().Initialize();
    Sync::ProgressSync::GetInstance().Initialize();

    // Read actual character name from game memory
    std::string charName = Sync::PlayerSync::GetInstance().GetLocalCharacterName();
    localPlayer.playerName = charName.empty() ? "Host" : charName;

    {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        m_players.push_back(localPlayer);
    }

    TransitionToState(SessionState::Connected);

    // Set up sign filtering — add our own Steam ID to the whitelist
    Hooks::ProtobufHooks::ClearSessionSteamIds();
    std::string localSteam = Hooks::ProtobufHooks::GetLocalSteamId();
    if (!localSteam.empty()) {
        Hooks::ProtobufHooks::AddSessionSteamId(localSteam);
    }

    LOG_INFO("Session created. Local player ID: %llu", m_localPlayerId);
    return true;
}

bool SessionManager::JoinSession(const std::string& address, const std::string& password) {
    if (!m_initialized) return false;

    LOG_INFO("Joining session at %s...", address.c_str());

    TransitionToState(SessionState::Connecting);

    m_isHost = false;
    m_sessionPassword = password;

    // Parse address (ip or ip:port)
    size_t colonPos = address.find(':');
    std::string ip = address.substr(0, colonPos);
    uint16_t port = 27015;
    if (colonPos != std::string::npos) {
        port = static_cast<uint16_t>(std::stoi(address.substr(colonPos + 1)));
    }

    // Initialize sync systems BEFORE joining P2P so the handshake has the real name
    Sync::PlayerSync::GetInstance().Initialize();
    Sync::ProgressSync::GetInstance().Initialize();

    // Join network session
    auto& peerMgr = Network::PeerManager::GetInstance();
    if (!peerMgr.JoinSession(ip, port, password)) {
        LOG_ERROR("Failed to join network session");
        TransitionToState(SessionState::Error);
        return false;
    }

    // Use PeerManager's generated player ID
    m_localPlayerId = peerMgr.GetLocalPlayerId();

    // Add local player
    SessionPlayer localPlayer{};
    localPlayer.playerId = m_localPlayerId;
    localPlayer.isAlive = true;
    localPlayer.isReady = true;
    localPlayer.soulLevel = 0;
    localPlayer.health = 0;
    localPlayer.maxHealth = 0;
    localPlayer.x = localPlayer.y = localPlayer.z = 0.0f;

    std::string charName = Sync::PlayerSync::GetInstance().GetLocalCharacterName();
    localPlayer.playerName = charName.empty() ? "Player" : charName;

    {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        m_players.push_back(localPlayer);
    }

    TransitionToState(SessionState::Connected);

    // Set up sign filtering
    Hooks::ProtobufHooks::ClearSessionSteamIds();
    std::string localSteam = Hooks::ProtobufHooks::GetLocalSteamId();
    if (!localSteam.empty()) {
        Hooks::ProtobufHooks::AddSessionSteamId(localSteam);
    }

    LOG_INFO("Joined session. Local player ID: %llu", m_localPlayerId);
    return true;
}

void SessionManager::LeaveSession() {
    if (m_state == SessionState::Disconnected) return;

    LOG_INFO("Leaving session...");

    // Leave network session
    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.LeaveSession();

    // Clear players
    {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        m_players.clear();
    }

    TransitionToState(SessionState::Disconnected);
    Sync::ForgetAutoSignPlacement();

    LOG_INFO("Left session");
}

void SessionManager::Update(float deltaTime) {
    if (!IsActive()) return;

    // Update networking (receive packets, send heartbeats)
    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.Update();

    // Update player sync (reads game memory, broadcasts position/state)
    auto& playerSync = Sync::PlayerSync::GetInstance();
    playerSync.Update(deltaTime);
}

// ============================================================================
// Player management
// ============================================================================

void SessionManager::AddPlayer(uint64_t playerId, const std::string& name) {
    if (playerId == m_localPlayerId) return;

    std::lock_guard<std::mutex> lock(m_playersMutex);

    // Don't add duplicates
    for (const auto& p : m_players) {
        if (p.playerId == playerId) {
            LOG_DEBUG("Player %llu already in session", playerId);
            return;
        }
    }

    SessionPlayer newPlayer{};
    newPlayer.playerId = playerId;
    newPlayer.playerName = name;
    newPlayer.isAlive = true;
    newPlayer.isReady = true;
    newPlayer.soulLevel = 0;
    newPlayer.health = 0;
    newPlayer.maxHealth = 0;
    newPlayer.x = newPlayer.y = newPlayer.z = 0.0f;

    m_players.push_back(newPlayer);

    LOG_INFO("Player joined session: %s (ID: %llu) [Total: %zu players]",
             name.c_str(), playerId, m_players.size());

    // Show notification + play sound
    UI::Overlay::GetInstance().ShowNotification(
        UI::Format(UI::Tr("%s joined the lobby", "Игрок %s вошёл в лобби"), name.c_str()),
        4.0f, UI::NotifyKind::Player);
    PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC);

    // Ensure seamless mode is active now that we have a real co-op partner
    Hooks::ProtobufHooks::SetSeamlessActive(true);
}

void SessionManager::RemovePlayer(uint64_t playerId, bool replaced) {
    std::lock_guard<std::mutex> lock(m_playersMutex);

    std::string name;
    for (const auto& p : m_players) {
        if (p.playerId == playerId) { name = p.playerName; break; }
    }

    m_players.erase(
        std::remove_if(m_players.begin(), m_players.end(),
            [playerId](const SessionPlayer& p) { return p.playerId == playerId; }),
        m_players.end()
    );

    LOG_INFO("Player left session: %s (ID: %llu) [Total: %zu players]%s",
             name.c_str(), playerId, m_players.size(), replaced ? " -- replaced by their rejoin" : "");

    // A restarted game replacing its own old entry is a rejoin, not a departure:
    // no "left" and no "everyone has left" in between.
    if (replaced) return;

    if (!name.empty()) {
        UI::Overlay::GetInstance().ShowNotification(
            UI::Format(UI::Tr("%s left the lobby", "Игрок %s покинул лобби"), name.c_str()),
            4.0f, UI::NotifyKind::Player);
        PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC);
    }

    // If we're now alone (only local player remains), seamless blocking is no longer
    // needed — re-enable it on the next connect. Keep it active for now so any
    // in-flight messages from the departing player don't crash us, but show a notice.
    if (m_players.size() <= 1) {
        LOG_INFO("All remote players gone — session effectively solo");
        UI::Overlay::GetInstance().ShowNotification(
            UI::Tr("Everyone has left. Waiting for players\xE2\x80\xA6", "Все вышли. Ждём игроков\xE2\x80\xA6"),
            5.0f, UI::NotifyKind::Info);
    }
}

SessionPlayer* SessionManager::GetPlayer(uint64_t playerId) {
    // Caller must hold m_playersMutex or be on the update thread
    for (auto& p : m_players) {
        if (p.playerId == playerId) return &p;
    }
    return nullptr;
}

SessionPlayer* SessionManager::GetLocalPlayer() {
    return GetPlayer(m_localPlayerId);
}

void SessionManager::UpdatePlayerPosition(uint64_t playerId, float x, float y, float z) {
    std::lock_guard<std::mutex> lock(m_playersMutex);
    SessionPlayer* player = GetPlayer(playerId);
    if (player) {
        player->x = x;
        player->y = y;
        player->z = z;
    }
}

void SessionManager::UpdatePlayerArea(uint64_t playerId, uint32_t onlineAreaId) {
    if (!onlineAreaId) return;   // the sender does not know its own map yet
    std::lock_guard<std::mutex> lock(m_playersMutex);
    SessionPlayer* player = GetPlayer(playerId);
    if (player && player->onlineAreaId != onlineAreaId) {
        LOG_INFO("Player %llu is now in map %u", (unsigned long long)playerId, onlineAreaId);
        player->onlineAreaId = onlineAreaId;
    }
}

void SessionManager::UpdatePlayerName(uint64_t playerId, const std::string& name) {
    std::lock_guard<std::mutex> lock(m_playersMutex);
    for (auto& player : m_players) {
        if (player.playerId == playerId) player.playerName = name;
    }
}

void SessionManager::UpdatePlayerHealth(uint64_t playerId, int32_t health, int32_t maxHealth) {
    std::lock_guard<std::mutex> lock(m_playersMutex);
    SessionPlayer* player = GetPlayer(playerId);
    if (player) {
        player->health = health;
        player->maxHealth = maxHealth;
        player->isAlive = (health > 0);
    }
}

void SessionManager::UpdatePlayerLevel(uint64_t playerId, uint32_t soulLevel) {
    std::lock_guard<std::mutex> lock(m_playersMutex);
    SessionPlayer* player = GetPlayer(playerId);
    if (player) {
        player->soulLevel = soulLevel;
    }
}

void SessionManager::NotifyPlayerDeath(uint64_t playerId) {
    bool shouldBroadcast = false;
    {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        SessionPlayer* player = GetPlayer(playerId);
        if (player) {
            player->isAlive = false;
            LOG_INFO("Player %s died", player->playerName.c_str());
            shouldBroadcast = true;
        }
    }
    // Broadcast OUTSIDE the lock to avoid deadlock with m_peersMutex
    if (shouldBroadcast) {
        Network::PacketHeader deathPacket{};
        deathPacket.magic = 0x44533243;
        deathPacket.type = Network::PacketType::PlayerDeath;
        deathPacket.size = sizeof(Network::PacketHeader);
        deathPacket.timestamp = 0;
        Network::PeerManager::GetInstance().BroadcastPacket(&deathPacket);
    }
}

void SessionManager::NotifyPlayerRespawn(uint64_t playerId) {
    bool shouldBroadcast = false;
    {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        SessionPlayer* player = GetPlayer(playerId);
        if (player) {
            player->isAlive = true;
            LOG_INFO("Player %s respawned", player->playerName.c_str());
            shouldBroadcast = true;
        }
    }
    if (shouldBroadcast) {
        Network::PacketHeader respawnPacket{};
        respawnPacket.magic = 0x44533243;
        respawnPacket.type = Network::PacketType::PlayerRespawn;
        respawnPacket.size = sizeof(Network::PacketHeader);
        respawnPacket.timestamp = 0;
        Network::PeerManager::GetInstance().BroadcastPacket(&respawnPacket);
    }
}

// A partner's own report that they died or came back. Only the local record
// changes: passing it on (NotifyPlayerDeath / NotifyPlayerRespawn broadcast)
// would send it straight back, and the two clients would bounce it forever --
// the same loop the event flags once had.
void SessionManager::SetPlayerAlive(uint64_t playerId, bool alive) {
    std::lock_guard<std::mutex> lock(m_playersMutex);
    SessionPlayer* player = GetPlayer(playerId);
    if (player) {
        player->isAlive = alive;
        LOG_INFO("Player %s %s", player->playerName.c_str(), alive ? "is back" : "died");
    }
}

void SessionManager::OnBossDefeated(uint32_t bossId) {
    LOG_INFO("Boss %u defeated in session", bossId);

    Network::BossDefeatedPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type = Network::PacketType::BossDefeated;
    packet.header.size = sizeof(Network::BossDefeatedPacket);
    packet.bossId = bossId;
    packet.defeatTime = 0;

    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.BroadcastPacket(&packet.header);
}

void SessionManager::OnBonfireRested(uint32_t bonfireId) {
    LOG_INFO("Bonfire %u rested", bonfireId);

    Network::EventFlagPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type = Network::PacketType::BonfireRest;
    packet.header.size = sizeof(Network::EventFlagPacket);
    packet.flagId = bonfireId;
    packet.flagValue = true;

    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.BroadcastPacket(&packet.header);
}

void SessionManager::OnFogGateEntered(uint32_t fogGateId) {
    LOG_INFO("Fog gate %u entered", fogGateId);

    Network::EventFlagPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type = Network::PacketType::FogGateTransition;
    packet.header.size = sizeof(Network::EventFlagPacket);
    packet.flagId = fogGateId;
    packet.flagValue = true;

    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.BroadcastPacket(&packet.header);
}

void SessionManager::PreventDisconnection() {
    m_preventDisconnect = true;
    LOG_INFO("Session disconnection prevention enabled");
}

void SessionManager::AllowDisconnection() {
    m_preventDisconnect = false;
    LOG_INFO("Session disconnection prevention disabled");
}

void SessionManager::TransitionToState(SessionState newState) {
    if (m_state == newState) return;
    LOG_INFO("Session state: %d -> %d", static_cast<int>(m_state), static_cast<int>(newState));
    m_state = newState;
}

void SessionManager::SynchronizePlayers() {
    // Driven by PlayerSync::Update now
}
