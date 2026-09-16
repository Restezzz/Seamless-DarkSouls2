#pragma once

#include <cstdint>
#include <string>
#include <mutex>
#include <unordered_set>
#include <unordered_map>

namespace DS2Coop::Sync {

// Progress synchronization manager
// Event flag synchronisation (implemented in player_sync.cpp).
//
// Flags are what two players disagree about: world items, fog gates, defeated
// bosses and the bonfire travel list all read from the same table, so syncing
// them is what makes one world out of two. Writes land in the receiving
// player's save, so this only does anything when flag_sync=on in the ini.
bool ApplyRemoteEventFlag(uint32_t flagId, bool value);
bool GetLocalPlayerPosition(float& x, float& y, float& z, float& rotY);

// Ask the server for the sign list at the next opportunity, rather than
// waiting for the client's own once-a-minute poll.
void RequestImmediateSignPoll();

// Automatic join: the joiner places a sign once the host accepts it, and the
// host summons that sign as soon as it arrives. Both obey auto_summon in the ini.
void RequestAutoSignPlacement();
void ArmAutoSummon();

// Leave the other player's world through the game's own return path -- the one
// a dropped session takes -- instead of only closing the mod's channel.
void RequestLeaveWorld();

// Resting at a bonfire resets the world (enemies, map objects, event state).
// With rest sync on, the reset runs for every player in the session: the one
// who rested tells the others, and they replay the game's own reset.
bool InstallWorldSync();
void SetWorldSyncEnabled(bool enabled);
void RequestWorldReset(const std::string& fromName);
void SetFlagSyncMode(const std::string& mode);

// World items (loot_sync.cpp). In multiplayer the game gives whoever loads an
// area no world items at all; with loot sync on, every player sees the items
// their own save has not picked up, and what they pick up in someone else's
// world stays picked up in their own.
bool InstallLootSync();
void SetLootSyncEnabled(bool enabled);
bool IsLootSyncEnabled();
void ToggleLootSyncNow();   // F4: flip it and show or hide the items in the loaded areas
void LootSyncGameTick();    // runs the queued F4 work on the game thread

// Being summoned from anywhere (summon_accept.cpp). The game turns down a summon
// on its own at once where signs are not allowed (Majula and the like); a sign
// the mod put down is let through there. Armed for a while by every placement.
bool InstallSummonAccept();
void ArmSummonAccept();
// The game declines every summon right now: at a bonfire, in a menu, in an
// event (the multiplayer manager's busy counter). No sign goes down meanwhile.
bool IsSummonBusy();
void WarnBusyForSign();         // tells the player the game is busy (bonfire, menu, event)
// Around the mod's own call of the game's sign creation (player_sync.cpp):
// where the game finds no spot for a sign the player's own spot is used, and a
// live-sign flag a failed create left behind is taken down first.
void SetModSignPlacement(bool on);
bool ClearStaleLiveSign(void* signManager);
// Game thread: a summon declined while busy is followed by a fresh sign once free.
void SummonAcceptGameTick();

// Free travel (free_travel.cpp): no co-op fog walls at area borders (the doors
// behave as in a solo game) and walking through one does not send anyone home.
// Installs the probes once and sets both patch groups from the ini; the two
// toggles flip one group each for testing by inversion.
bool InstallFreeTravel(bool enabled);
void ToggleFreeTravelDoors();
void ToggleFreeTravelCrossing();
void FreeTravelGameTick();   // writes the requested bytes, on the game thread only
// A boss fog the host has walked through (packet BossDoorCrossed): the guest's
// own copy of that door cannot see it, so without this it stays a wall.
void NoteHostCrossedBossFog(uint32_t flag);
// Whether a boss fog makes a guest wait for the other player at all. Off: the
// fog behaves as it does for a host, which is what 0.1.0 did.
void SetBossFogWait(bool on);
// Every bonfire the other player has lit (packet BonfireList). A guest's travel
// list reads the session's set, which the game fills with the current map only
// and sixteen entries at most; this fills the rest. The byte it writes is never
// saved, so the guest's own progress is untouched.
void NotePartnerBonfires(const void* entries, uint32_t count);
// The host travelled by bonfire (packet HostTravelled). A guest in its world
// leaves at once and joins again where the host went, once it has arrived.
void NoteHostTravelled(int32_t map, int32_t bonfire);

// Progress already made, handed over once when a session starts (packet
// FlagBulk). The flag diff only reports what changes while both are connected,
// so a guest joining later would never learn about the host's bonfires, fog
// gates and bosses. Set-only, and written on the game's own thread.
void SendFlagCatchUp();
void NoteRemoteFlagBulk(uint32_t group, const uint8_t* bits, uint32_t bytes);
// Whether the player agreed to progress being written into this save (ini
// flag_sync). Asked by anything that would change the save rather than the
// session: the partner's bonfires in byte +0x02, for one.
bool IsProgressSharingOn();

// Deaths in co-op (death_sync.cpp). A guest who dies -- or whose host dies --
// goes home the game's way and is brought straight back to the partner's world,
// at the last bonfire rested at there (or the one nearest to where it arrived);
// in a boss fight the return waits until the fight is decided or both are dead.
bool InstallDeathSync(bool enabled);
// NPCs for a guest (npc_talk.cpp). Both symptoms -- see-through characters and
// no prompt to talk -- come out of the same field, the phantom id and its
// neighbours in the character's PlayerType (docs §3.24).
bool InstallNpcTalk();
// The world's other characters drawn solid rather than as ghosts. This also
// makes the partner solid on the host's screen.
void SetNpcSolidEnabled(bool on);
// Whether a guest is asked about an NPC's prompt the way a host is, instead of
// as a phantom the game turns down before looking at anything else. The answer
// is cached until the guest leaves and re-enters the NPC's zone (ini npc_talk).
void SetNpcTalkEnabled(bool on);
// The partner's character object, as last seen by the code that draws it, or 0
// if nothing that recent is known. The camera needs it (docs §3.23).
uintptr_t GetPartnerCharacter(uint64_t maxAgeMs);

// How the two players stand towards each other -- no damage, friendly fire
// without lock-on, or a real fight (pvp_modes.cpp, docs §3.25). For now this
// only reads: it names both players' team bytes and the relation the game has
// between them, because the meaning of those numbers has never been measured
// and the ones in the old notes came from a bad memory scan.
void PvpModesGameTick();   // game thread

// Groundwork for a join with no summon sign at all (join_direct.cpp, docs
// §3.27). A guest's entry is exe+0x2C6330, and three of the four paths into it
// never involve a sign -- but the session descriptor it takes carries an opaque
// payload that has to describe a genuinely live session. This watches a normal
// summon and writes that descriptor down. It reads only.
bool InstallJoinProbe();

// A character's own death, below the session layer (chr_death.cpp, docs §3.28).
// The game has no way to stand a dead character up again, but a death is only
// *requested* by one byte and consumed later -- so it can be denied before it
// happens. This only watches the local character's HP, that byte and the death
// bits; it writes nothing.
void ChrDeathTick();   // game thread
void DeathSyncGameTick();             // game thread, every frame
void NotePartnerLife(bool alive);     // the partner's own PlayerDeath / PlayerRespawn
// The host's boss fight (BossState): battle id, phase, the event area it belongs
// to and the participant count -- what a guest's own game needs to run it too.
void NotePartnerBoss(int32_t active, int32_t phase, int32_t areaIndex, int32_t participants);
// Whether a guest starts its own copy of the host's boss fight (ini boss_sync).
void SetBossSyncEnabled(bool on);
bool IsHostInBossFight();             // a recent BossState from the host says a fight is on
void CancelDeathRejoin();             // leaving on purpose: no automatic return
// Put a sign down again for the host to summon, without the once-per-handshake
// limit of the automatic join (player_sync.cpp).
void RequestRejoinSignPlacement();

class ProgressSync {
public:
    static ProgressSync& GetInstance();
    
    bool Initialize();
    void Shutdown();
    
    // Event flags synchronization
    void SyncEventFlag(uint32_t flagId, bool value);
    bool GetEventFlag(uint32_t flagId);
    void RequestEventFlagSync();
    
    // Boss defeat synchronization
    void SyncBossDefeat(uint32_t bossId);
    bool IsBossDefeated(uint32_t bossId);
    
    // Bonfire synchronization
    void SyncBonfire(uint32_t bonfireId, bool lit);
    bool IsBonfireLit(uint32_t bonfireId);
    void SyncAllBonfires();
    
    // Item pickup synchronization (optional)
    void SyncItemPickup(uint32_t itemId, uint32_t locationId);
    bool IsItemPickedUp(uint32_t itemId, uint32_t locationId);
    
    // Fog gate synchronization
    void NotifyFogGateEntry(uint32_t fogGateId);
    void WaitForPartyAtFogGate(uint32_t fogGateId);

private:
    ProgressSync() = default;
    ~ProgressSync() = default;
    ProgressSync(const ProgressSync&) = delete;
    ProgressSync& operator=(const ProgressSync&) = delete;
    
    bool m_initialized = false;
    std::recursive_mutex m_mutex;
    std::unordered_map<uint32_t, bool> m_eventFlags;
    std::unordered_set<uint32_t> m_defeatedBosses;
    std::unordered_set<uint32_t> m_litBonfires;
    std::unordered_map<uint64_t, bool> m_pickedItems;
};

// Player synchronization manager
class PlayerSync {
public:
    static PlayerSync& GetInstance();
    
    bool Initialize();
    void Shutdown();
    
    void Update(float deltaTime);
    
    // Position synchronization
    void SyncLocalPlayerPosition();
    void ApplyRemotePlayerPosition(uint64_t playerId, float x, float y, float z, float rotX, float rotY, float rotZ);
    
    // State synchronization
    void SyncLocalPlayerState();
    void ApplyRemotePlayerState(uint64_t playerId, int32_t health, int32_t maxHealth, int32_t stamina, int32_t maxStamina);
    
    // Animation synchronization
    void SyncAnimation(uint64_t playerId, uint32_t animationId);
    
    // Equipment synchronization
    void SyncEquipment(uint64_t playerId);

    // Seamless helpers
    bool GrantSoapstones();
    bool MaxPhantomTimer();
    void EnableSummoning();
    std::string GetLocalCharacterName();

private:
    PlayerSync() = default;
    ~PlayerSync() = default;
    PlayerSync(const PlayerSync&) = delete;
    PlayerSync& operator=(const PlayerSync&) = delete;
    
    bool m_initialized = false;
    float m_positionSyncTimer = 0.0f;
    float m_stateSyncTimer = 0.0f;
    float m_phantomTimerRefresh = 0.0f;
    
    static constexpr float POSITION_SYNC_INTERVAL = 0.05f; // 20 times per second
    static constexpr float STATE_SYNC_INTERVAL = 0.5f;     // 2 times per second
};

} // namespace DS2Coop::Sync

