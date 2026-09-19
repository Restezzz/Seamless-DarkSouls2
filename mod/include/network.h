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
    // Every event flag this player already has set, one group at a time. The
    // flag diff only carries what changes during a session, so without this a
    // guest never hears about the bonfires, fog gates and bosses the host
    // cleared before it joined. Sent once per join, applied set-only.
    FlagBulk = 0x3B,
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
    // "I, the host, just travelled by bonfire." Vanilla answers a host's warp by
    // dropping every guest -- five minutes later, after the guest has spent them
    // in a copy of a world the host already left (16.09: host warp 18:38:52,
    // guest thrown out 18:41:01). The guest follows at once instead.
    HostTravelled = 0x3C,
    // The host's choice of damage between the players: 0 none, 1 friendly fire
    // without lock-on, 2 PvP. Sent every few seconds; a guest that has not heard
    // it recently plays with none (pvp_modes.cpp, docs §3.25).
    DamageMode = 0x3D,
    // The map this player really stands in, the game's own id, every two seconds.
    // The sign map the mod knows is a guest's home map while it is in the host's
    // world, so travelling in a session needs this (travel_sync.cpp).
    PlayerMap = 0x3E,
    // The connection check from the menu (net_check.cpp): datagrams of a given
    // size one way, small answers the other, to find the sizes the path between
    // the two players loses. 17.09 nothing over 1472 bytes reached a friend.
    NetProbe = 0x3F,
    // What an NPC's talk gave this player (npc_progress.cpp): the partner's game adds
    // each item it has none of, so a key or the Estus Flask reaches both players.
    NpcGift = 0x42,
    // A travel either player just took, for the other's notification (17.09, 0.2.2
    // point 6): the host's HostTravelled drives the guest's own logic, this one only
    // says where the partner went.
    PlayerTravelled = 0x43,
    // A bonfire the sender just lit (bonfire_lit.cpp): lit for the receiver too.
    BonfireLit = 0x44,
    // The host's killed enemies of one map, by generator id (enemy_reconcile.cpp): a guest
    // gets the host's enemies from the join snapshot for the join map only.
    EnemyDeadList = 0x45,
    // The host's kill counters of one map (enemy_reconcile.cpp): what keeps a boss dead.
    KillCounts = 0x46,
    // The host's open chests of one map (chest_lids.cpp): a chest open at the host's is opened
    // for the guest too, its contents by the guest's own save.
    ChestLids = 0x47,
    // The sender's own character as the game builds it for a summon, record +0x00..+0x2DB
    // (partner_look.cpp): the copy on the other side is made again when its look changed.
    PartnerLook = 0x48,
    // host -> guest: the states of a map's objects -- gates, bridges, lifts, statues (map_objects.cpp).
    MapObjectStates = 0x49,

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
    int32_t active;        // EventBossBattleManager+0x14: the battle running (0 none)
    int32_t phase;         // +0x204: 1 fighting, 2 won, 3 cleanup
    int32_t areaIndex;     // +0x10: the event area the battle belongs to
    int32_t participants;  // +0x210 (a byte): a guest's fog stays shut while its own is 0
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

// One group of the event-flag table: flag id = group * 10000 + bit index, and
// inside each byte the game counts bits from the top (bit of id N is
// 1 << (7 - N % 8)). Measured 12.09: five groups, 2575 bytes in all.
struct FlagBulkPacket {
    PacketHeader header;
    uint32_t     group;
    uint32_t     bytes;      // how many of the array below are filled
    uint32_t     offset;     // where in the group's bytes the array starts (0.2.2: groups 10 and 20 are 1250 bytes)
    uint8_t      bits[640];
};

struct BonfireLitPacket {
    PacketHeader header;
    int32_t      id;        // the bonfire id
    int32_t      map;       // the sender's raw map id when it lit it
};

// Generator records the host has killed and not respawned yet, in one map: kind
// [rec+0x76]&3 set, stay [rec+0x7A] bit 1 set, no-respawn [rec+0x76]&0xC clear.
struct EnemyDeadListPacket {
    PacketHeader header;
    int32_t      map;       // raw map id
    uint16_t     count;     // ids filled
    uint8_t      source;    // 0 generators loaded on the host, 1 the host's cache of maps it left
    uint8_t      reserved;
    uint16_t     ids[256];  // (u16)[rec+0x68]
};

// The host's kill counters of one map: its store slot's sorted generator indices and
// how many times each was killed.
struct KillCountsPacket {
    PacketHeader header;
    int32_t      map;       // raw map id
    uint16_t     count;     // entries filled
    uint16_t     reserved;
    uint16_t     index[256];
    uint8_t      kills[256];
};

// One chest: its object id and the lid's state at the host's.
struct ChestLidEntry {
    uint32_t id;
    uint8_t  state;
};

struct ChestLidsPacket {
    PacketHeader  header;
    int32_t       map;       // raw map id
    uint16_t      count;     // entries filled
    uint8_t       source;    // 0 loaded on the host, 1 the host's save record
    uint8_t       reserved;
    ChestLidEntry entries[200];
};

struct MapObjectState {
    uint32_t id;
    uint8_t  state;
};

struct MapObjectStatesPacket {
    PacketHeader    header;
    int32_t         map;        // raw map id
    uint16_t        count;      // entries filled
    uint8_t         source;     // 0 loaded on the host, 1 the host's save record
    uint8_t         reserved;
    MapObjectState  entries[250];
};

struct PartnerLookPacket {
    PacketHeader header;
    uint32_t     seq;              // the sender's look number; the same number is the same look
    uint8_t      record[0x2DC];    // exe+0x51BA70's record, +0x00..+0x2DB
};

struct PlayerTravelledPacket {
    PacketHeader header;
    int32_t      map;       // the warp request's raw map id
    int32_t      target;    // the bonfire id (type 3), or the request's id (type 4: ship, cutscene)
    int32_t      type;      // the warp request's type
};

struct HostTravelledPacket {
    PacketHeader header;
    int32_t      map;       // the warp request's raw map id (0x0A1F0000 = map 10310000)
    int32_t      bonfire;   // the bonfire travelled to
};

struct DamageModePacket {
    PacketHeader header;
    uint8_t      mode;         // 0 none, 1 friendly fire without lock-on, 2 PvP
    uint8_t      reserved[3];
};

struct PlayerMapPacket {
    PacketHeader header;
    int32_t      rawMap;       // 0x0A1F0000 = map 10310000
};

// One item as the game's ItemGive takes it (16 bytes).
struct NpcGiftItem {
    int32_t      kind;
    int32_t      id;
    float        durability;
    int16_t      count;
    uint8_t      upgrade;
    uint8_t      infusion;
};

struct NpcGiftPacket {
    PacketHeader header;
    uint32_t     count;        // how many items are filled
    NpcGiftItem  items[16];
};

// header.size is the whole datagram, padding included: a probe of 4000 bytes is
// this struct and 3955 bytes after it.
struct NetProbePacket {
    PacketHeader header;
    uint8_t      kind;         // NetProbeKind
    uint8_t      reserved[3];
    uint32_t     run;          // the check this belongs to
    uint32_t     round;        // which pass over the sizes
    uint32_t     bytes;        // the datagram size the probe is about
    uint64_t     sentAt;       // the asker's clock (ms), sent back as it came
};
#pragma pack(pop)

enum class NetProbeKind : uint8_t {
    Out  = 0,   // a probe of `bytes` bytes: answer with a small Ack
    Ack  = 1,   // "your probe of `bytes` bytes arrived"
    Ask  = 2,   // a small request: send me a datagram of `bytes` bytes
    Back = 3,   // the answer to Ask, `bytes` bytes long
};

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

