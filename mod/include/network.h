#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <mutex>

namespace DS2Coop::Network {

// Packet types for communication
enum class PacketType : uint8_t {
    // Connection management
    Handshake = 0x01,
    Disconnect = 0x02,
    Heartbeat = 0x03,
    
    // Session management
    SessionCreate = 0x10,
    SessionJoin = 0x11,
    SessionLeave = 0x12,
    SessionUpdate = 0x13,
    
    // Player synchronization
    PlayerPosition = 0x20,
    PlayerAction = 0x21,
    PlayerState = 0x22,
    PlayerDeath = 0x23,
    PlayerRespawn = 0x24,
    
    // Game state synchronization
    BossDefeated = 0x30,
    BonfireRest = 0x31,
    FogGateTransition = 0x32,
    ItemPickup = 0x33,
    EventFlag = 0x34,
    // "I just put a sign down." Carries nothing: the point is the timing, not
    // the contents. The other side asks the server for the list the moment it
    // arrives instead of waiting for its own poll to come round.
    SignPlaced = 0x35,
    // "I rested at a bonfire." The receiver replays the game's rest reset, so
    // enemies respawn on both sides at once (world_sync.cpp).
    WorldReset = 0x36,
    // The host's boss fight (running, phase), on every change and every few
    // seconds while it runs. The guest's own game does not run the host's
    // battle, so this is how the guest knows one is on (death_sync.cpp).
    BossState = 0x37,
    // The origin a map's sign coordinates are measured against, as measured by
    // the player standing in that map. Without it a sign cannot be aimed into
    // that map at all: on 12.09 a guest was summoned into Majula with its sign
    // still written in its own map's frame, landed off the map and died on
    // arrival, twice in a row (session_hooks.cpp).
    MapOriginInfo = 0x38,
    // Every bonfire this player has lit. The game syncs a session's bonfires
    // itself, but only for the map the players are in and at most sixteen of
    // them: measured on 12.09, the host had five lit and the set the guest's
    // travel list reads held two (death_sync.cpp, docs §3.18).
    BonfireList = 0x3A,
    // "I have walked through the boss fog whose event flag is this one." The
    // guest's copy of that door cannot see the host go through, so it stayed a
    // wall until a fight was already running -- on 12.09 the guest stood at the
    // fog for five minutes (free_travel.cpp).
    BossDoorCrossed = 0x39,

    // Custom data
    ChatMessage = 0x40,
    CustomData = 0x41
};

// Base packet structure
#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;          // Magic number for validation
    PacketType type;         // Packet type
    uint32_t size;           // Total packet size including header
    uint32_t sequence;       // Sequence number
    uint64_t timestamp;      // Timestamp
};
#pragma pack(pop)

// Packet data structures
#pragma pack(push, 1)
struct HandshakePacket {
    PacketHeader header;
    uint32_t version;
    uint64_t playerId;
    char playerName[32];
    char password[64];
};

struct PlayerPositionPacket {
    PacketHeader header;
    uint64_t playerId;
    float x, y, z;
    float rotX, rotY, rotZ;
    uint32_t animation;
    // Which map the player is standing in. Needed because a summon sign's
    // coordinates are encoded relative to a per-map origin, so aiming a sign at
    // someone in a different map means knowing which map that is.
    uint32_t onlineAreaId;
};

struct PlayerStatePacket {
    PacketHeader header;
    uint64_t playerId;
    int32_t health;
    int32_t maxHealth;
    int32_t stamina;
    int32_t maxStamina;
    uint32_t souls;
    uint32_t soulLevel;
};

struct BossDefeatedPacket {
    PacketHeader header;
    uint32_t bossId;
    uint64_t defeatTime;
};

struct EventFlagPacket {
    PacketHeader header;
    uint32_t flagId;
    bool flagValue;
};

struct BossStatePacket {
    PacketHeader header;
    int32_t active;   // EventBossBattleManager+0x14: the battle running (0 none)
    int32_t phase;    // +0x204: 1 fighting, 2 won, 3 cleanup
};

struct MapOriginPacket {
    PacketHeader header;
    uint32_t area;      // online area id (10040000 = Majula)
    float x, y, z;      // sign coordinate = 32 * (world - origin)
};

struct BossDoorPacket {
    PacketHeader header;
    uint32_t flag;      // the door param's event flag: the same number in both games
};

// One bonfire as the sender's own save has it: bit 0 of flags = lit, bits 1-7
// the kindle level, exactly the byte the game keeps at record +0x02.
struct BonfireEntry {
    uint16_t id;
    uint8_t  flags;
};

struct BonfireListPacket {
    PacketHeader header;
    uint32_t     count;          // how many entries are filled
    BonfireEntry entries[256];   // a save holds well under this (77 records measured)
};
#pragma pack(pop)

// Peer information
struct PeerInfo {
    uint64_t playerId;
    std::string playerName;
    uint32_t address;
    uint16_t port;
    uint64_t lastHeartbeat;
    bool connected;
};

// Peer manager for handling connections
class PeerManager {
public:
    static PeerManager& GetInstance();
    
    bool Initialize(uint16_t port);
    void Shutdown();
    
    bool CreateSession(const std::string& password);
    bool JoinSession(const std::string& address, uint16_t port, const std::string& password);
    void LeaveSession();
    
    void Update();
    
    bool SendPacket(const PacketHeader* packet, uint64_t targetPlayerId = 0);
    void BroadcastPacket(const PacketHeader* packet);
    
    const std::vector<PeerInfo>& GetPeers() const { return m_peers; }
    bool IsHost() const { return m_isHost; }
    bool IsConnected() const { return m_connected; }
    bool IsHandshakeConfirmed() const { return m_handshakeConfirmed; }

    uint64_t GetLocalPlayerId() const { return m_localPlayerId; }
    const std::string& GetSessionPassword() const { return m_sessionPassword; }

private:
    PeerManager() = default;
    ~PeerManager() = default;
    PeerManager(const PeerManager&) = delete;
    PeerManager& operator=(const PeerManager&) = delete;

    void HandleIncomingPackets();
    void HandleHandshakePacket(const struct HandshakePacket* hs, const struct sockaddr_in& senderAddr);
    void SendHeartbeats();
    void CheckTimeouts();

    bool m_initialized = false;
    bool m_isHost = false;
    bool m_connected = false;
    uint64_t m_localPlayerId = 0;
    uint16_t m_port = 27015;
    std::string m_sessionPassword;
    std::vector<PeerInfo> m_peers;
    mutable std::recursive_mutex m_peersMutex;
    void* m_socket = nullptr;
    uint64_t m_lastHeartbeatMs = 0;
    uint64_t m_connectingTimestampMs = 0; // for handshake timeout
    bool m_handshakeConfirmed = false;    // set true when host responds
};

// Packet handler for processing received packets
class PacketHandler {
public:
    static PacketHandler& GetInstance();
    
    void HandlePacket(const PacketHeader* packet, const PeerInfo& sender);

private:
    PacketHandler() = default;
    ~PacketHandler() = default;
    PacketHandler(const PacketHandler&) = delete;
    PacketHandler& operator=(const PacketHandler&) = delete;
    
    void HandleHandshake(const HandshakePacket* packet, const PeerInfo& sender);
    void HandlePlayerPosition(const PlayerPositionPacket* packet);
    void HandlePlayerState(const PlayerStatePacket* packet);
    void HandleBossDefeated(const BossDefeatedPacket* packet);
    void HandleEventFlag(const EventFlagPacket* packet);
};

} // namespace DS2Coop::Network

