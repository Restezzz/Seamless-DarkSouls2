#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

namespace DS2Coop::Session {

// Session state
enum class SessionState {
    Disconnected,
    Connecting,
    Connected,
    InGame,
    Error
};

// Player information in session
struct SessionPlayer {
    uint64_t playerId;
    std::string playerName;
    uint32_t soulLevel;
    float x, y, z;
    uint32_t onlineAreaId = 0;   // 0 = not known yet
    int32_t health;
    int32_t maxHealth;
    bool isAlive;
    bool isReady;
};

// Session manager for co-op sessions
class SessionManager {
public:
    static SessionManager& GetInstance();
    
    bool Initialize();
    void Shutdown();
    
    bool CreateSession(const std::string& password);
    bool JoinSession(const std::string& address, const std::string& password);
    void LeaveSession();
    
    void Update(float deltaTime);
    
    SessionState GetState() const { return m_state; }
    bool IsActive() const { return m_state == SessionState::Connected || m_state == SessionState::InGame; }
    bool IsHost() const { return m_isHost; }
    
    std::vector<SessionPlayer> GetPlayers() const {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        return m_players; // returns a copy — safe for render thread
    }
    SessionPlayer* GetPlayer(uint64_t playerId);
    SessionPlayer* GetLocalPlayer();

    void AddPlayer(uint64_t playerId, const std::string& name);
    // replaced: the same player is rejoining (a restarted game), so no toasts.
    void RemovePlayer(uint64_t playerId, bool replaced = false);

    void UpdatePlayerPosition(uint64_t playerId, float x, float y, float z);
    void UpdatePlayerArea(uint64_t playerId, uint32_t onlineAreaId);
    void UpdatePlayerHealth(uint64_t playerId, int32_t health, int32_t maxHealth);
    void UpdatePlayerLevel(uint64_t playerId, uint32_t soulLevel);
    // The local player died / came back: recorded and broadcast to the others.
    void NotifyPlayerDeath(uint64_t playerId);
    void NotifyPlayerRespawn(uint64_t playerId);
    // A partner's own report of the same: recorded only, never passed on.
    void SetPlayerAlive(uint64_t playerId, bool alive);
    
    // Session events that should persist
    void OnBossDefeated(uint32_t bossId);
    void OnBonfireRested(uint32_t bonfireId);
    void OnFogGateEntered(uint32_t fogGateId);
    
    // Prevent session disconnection
    void PreventDisconnection();
    void AllowDisconnection();

private:
    SessionManager() = default;
    ~SessionManager() = default;
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;
    
    void TransitionToState(SessionState newState);
    void SynchronizePlayers();
    
    bool m_initialized = false;
    SessionState m_state = SessionState::Disconnected;
    bool m_isHost = false;
    bool m_preventDisconnect = false;
    uint64_t m_localPlayerId = 0;
    std::string m_sessionPassword;
    std::vector<SessionPlayer> m_players;
    mutable std::mutex m_playersMutex; // protects m_players from concurrent access
};

} // namespace DS2Coop::Session

