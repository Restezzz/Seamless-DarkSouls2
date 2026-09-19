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
void KeepFlagLocal(uint32_t flagId, bool value);   // a flag of my own the partner is not told about
bool GetLocalPlayerPosition(float& x, float& y, float& z, float& rotY);

// Ask the server for the sign list at the next opportunity, rather than
// waiting for the client's own once-a-minute poll.
void RequestImmediateSignPoll();

// Automatic join: the joiner places a sign once the host accepts it, and the
// host summons that sign as soon as it arrives. Both obey auto_summon in the ini.
bool RequestAutoSignPlacement();   // false when no sign was queued
void ForgetAutoSignPlacement();    // the lobby was left: the next join places a sign at once
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
// In this player's own world, right before a map's generators are made: once-only enemies it
// killed in someone else's world and took the drop of are counted as killed (loot_sync.cpp).
void ApplyHomeKillsBeforeArea(void* genMgr, int32_t areaIndex);

// Being summoned from anywhere (summon_accept.cpp). The game turns down a summon
// on its own at once where signs are not allowed (Majula and the like); a sign
// the mod put down is let through there. Armed for a while by every placement.
bool InstallSummonAccept();
void ArmSummonAccept();
// The game declines every summon right now: at a bonfire, in a menu, in an
// event (the multiplayer manager's busy counter). No sign goes down meanwhile.
bool IsSummonBusy();
// The host confirms a guest's summon slot on arrival; a slot the game dropped is reserved again,
// and the lobby's join goes through if it still says no (summon_accept.cpp, ini join_slot_confirm).
void SetJoinSlotConfirm(bool on);
void WarnBusyForSign();         // tells the player the game is busy (bonfire, menu, event)
// Around the mod's own call of the game's sign creation (player_sync.cpp):
// where the game finds no spot for a sign the player's own spot is used, and a
// live-sign flag a failed create left behind is taken down first.
void SetModSignPlacement(bool on);
bool ClearStaleLiveSign(void* signManager);
// Game thread: take the manager's live sign down once the server has created it.
enum class PlacedSignTakeDown { TakenDown, NotCreatedYet, NoSign, Failed };
PlacedSignTakeDown TakeDownPlacedSign(void* signManager);
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
// Either player travelled (packet PlayerTravelled): a notification on the game thread
// saying where to. Network thread.
void NotePartnerTravelled(int32_t map, int32_t target, int32_t type, const std::string& from);

// Progress already made, handed over once when a session starts (packet
// FlagBulk). The flag diff only reports what changes while both are connected,
// so a guest joining later would never learn about the host's bonfires, fog
// gates and bosses. Set-only, and written on the game's own thread.
void SendFlagCatchUp();
void NoteRemoteFlagBulk(uint32_t group, const uint8_t* bits, uint32_t bytes, uint32_t offset);
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
// A character one of the game's five network player slots holds ([netRoot+0x20]+0x1E8+i*0xD0):
// another player of this session, never an NPC phantom (npc_talk.cpp).
bool IsSessionPlayer(uintptr_t chr);
// A talk with an NPC is open: EventTalkManager's int32 handle names a character standing
// within a few metres of this player (npc_talk.cpp). Game thread.
bool IsTalkOpenNearby();

// How the two players stand towards each other -- no damage, friendly fire
// without lock-on, or a real fight (pvp_modes.cpp, docs §3.25). The host picks
// the mode; every machine then gives the guest the same team, so each side's
// damage filter agrees with the other's.
bool InstallPvpModes();    // the status flag getter behind "no lock-on in friendly fire"

// NPC progress for both players (npc_progress.cpp): a guest's talk progress is kept
// in the host's world, and what an NPC's talk gives goes to the partner as well if
// the partner has none of it (ini npc_progress).
bool InstallNpcProgress(bool enabled);
void NotePartnerNpcGift(const void* items, uint32_t count, const std::string& from);   // network thread
void NpcProgressGameTick();   // game thread

// The Estus Flask from the menu for a player who has none (estus_grant.cpp).
bool InstallEstusGrant();
int  GetEstusFlaskState();   // -1 unknown (loading, not hooked), 0 no flask, 1 has one
void RequestEstusGrant();    // any thread; given on the game thread if still missing
void EstusGameTick();        // game thread
void PvpModesGameTick();   // game thread
enum : uint8_t { kDamageNone = 0, kDamageFriendlyFire = 1, kDamagePvp = 2 };
void    SetDamageMode(uint8_t mode);      // the host's choice (ini damage_mode, the menu)
uint8_t GetDamageMode();                  // what applies here now: the host's own, or the host's as sent
uint8_t GetChosenDamageMode();            // this player's own choice, whatever role it has
void    NoteHostDamageMode(uint8_t mode); // network thread
// The team a guest keeps for itself in the host's world: 0 unless a damage mode
// says otherwise. EnableSummoning writes this instead of a flat 0.
uint8_t GuestOwnTeam();

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
void NotePartnerStateHp(int32_t hp, int32_t maxHp);   // network thread: the partner's PlayerState, every 0.5 s
// The host's boss fight (BossState): battle id, phase, the event area it belongs
// to and the participant count -- what a guest's own game needs to run it too.
void NotePartnerBoss(int32_t active, int32_t phase, int32_t areaIndex, int32_t participants);
// Whether a guest starts its own copy of the host's boss fight (ini boss_sync).
void SetBossSyncEnabled(bool on);
bool IsHostInBossFight();             // a recent BossState from the host says a fight is on
// A guest's copy of a boss fight the host won is handing out its reward right
// now (phase 3 of that battle here): the owner-only gates may let this guest in.
bool GuestBossRewardDue();

// A guest's world as the game builds it (guest_world.cpp, docs §3.37): NPC scripts
// and the NPC factory asked "multiplayer world?" answered as for the owner, and a
// joining guest's characters put in only once the host's world has arrived.
// liveStates: the host's enemy live states from the join snapshot, kept when the join
// map's generators are not made yet and applied once they are (ini enemy_states_at_join).
bool InstallGuestWorld(bool talkScripts, bool npcLocal, bool waitForSnapshot, bool liveStates);
void GuestWorldTick();   // game thread: probe, the applied states counted again later
// Map event scripts asking "someone else's multiplayer world?" (ESD 130602) get the game's answer
// for a guest; true answers them "no" like the talk scripts, as up to 0.2.1 (ini
// guest_event_scripts_owner).
void SetGuestLiftFix(bool on);
// A guest's hits on the host's world's characters do not count towards their anger, so an NPC hit
// a few times still talks (npc_progress.cpp, ini guest_npc_hits_ignored).
void SetGuestNpcHitsIgnored(bool on);
void SetNpcGiftShare(bool on);                 // what an NPC gives goes to the partner as well
void SetMapObjectStates(bool on);              // map_objects.cpp: the map's objects as the host has them
void MapObjectsTick();                         // game thread
void NoteHostMapObjects(int32_t map, uint8_t source, const void* entries, uint16_t count);   // network thread
void SetNpcEventsAfterTalk(bool on);
uintptr_t CurrentEventTask();                                           // mp_gates.cpp, 0 outside one
bool ReadEventTaskKey(uintptr_t task, uint32_t* map, int32_t* event);   // [task+0x28], [[task+8]+0x18]
// Enemies as the host has them in every map a guest loads (enemy_reconcile.cpp, docs §3.47):
// the host's killed generator records and its kill counters, pushed by the host every few
// seconds when they change, applied by the guest (dead wins, the higher count wins).
bool InstallEnemyReconcile(bool deadRecords, bool killCounts);
void EnemyReconcileTick();                                        // game thread
void EnemyReconcileBeforeArea(void* genMgr, int32_t areaIndex);   // game thread, before an area's generators are made
void EnemyReconcileAfterArea(void* genMgr, int32_t areaIndex);    // game thread, right after
void EnemyReconcileNow();                                         // game thread, before the enemy sync is armed
void ForgetHostEnemyStates(const char* why);                      // game thread: a world reset here, or out of the world
void ForgetHostEnemyStatesAfterRest(const char* why);             // game thread: the same, and the host's next lists waited out
void NoteHostEnemyDeadList(int32_t map, uint8_t source, const uint16_t* ids, uint16_t count);      // network thread
void NoteHostKillCounts(int32_t map, const uint16_t* index, const uint8_t* kills, uint16_t count); // network thread
// A boss fight that ends while one of the players is down (boss_down.cpp, docs §3.47): the
// host's event scripts carry on, and the decided fight's phases 2-3 run for a player who is
// down, so the souls and the reward are handed out.
bool InstallBossDown(bool enabled);
// A guest standing in a boss's arena wakes the boss on the host, as the host would (boss_arena.cpp,
// docs §3.47, ini boss_guest_starts).
bool InstallBossArena(bool enabled);
// A guest down far from its partner in the same map: the world is loaded around the partner and
// the camera follows it (death_camera.cpp, ini far_death_camera). Game thread.
bool InstallFarDeathCamera(bool enabled);
bool StartFarSpectate(uintptr_t partner);
void StopFarSpectate(const char* why);
bool FarSpectating();
bool PartnerAliveReported();               // the partner's own PlayerDeath / PlayerRespawn
bool GuestHeldForBattle(int32_t battle);   // a guest's return is held for this battle
// A bonfire the partner lit where both players are: this player's respawn too (death_sync.cpp).
void NotePartnerLitForRespawn(int32_t bonfireId, int32_t rawMap);   // game thread
// A join's arrival in a map the host has already left lands where the host stands (ini
// arrival_follow_host).
void SetArrivalFollowHost(bool on);
// The partner's copy left in the bonfire travel pose after its travel is put back to standing
// (travel_sync.cpp, ini travel_pose_fix).
void NotePartnerTravelForPose();   // any thread
void SetTravelPoseFix(bool on);
// The partner's rest replayed here without the event and flag reset (world_sync.cpp, ini
// rest_replay_full); the host world's flags carried into a guest's own world (player_sync.cpp,
// ini flags_carry_home).
void SetRestReplayFull(bool on);
void SetFlagsCarryHome(bool on);
// Game thread, a map of this player's own world being made: the host world's flags written in before
// that map's event scripts start (player_sync.cpp).
void CarryHostWorldFlagsHomeNow(const char* why);
// mp_gates.cpp: the event manager's hold counter (+0x1B4) and the byte it drives, logged as they
// change (19.09 probe: the host's black screen after making its character).
void EventViewProbeTick();
// The partner's copy made again from its new look (partner_look.cpp, travel_sync.cpp, docs §3.51).
void PartnerLookTick();                                        // game thread: my own look, sent when new
void InstallMapStateAct(bool Local);                           // map_state_act.cpp: map object states in a session
void MapStateActTick();                                        // game thread: the Majula gate and its levers, logged
void SetPartnerLookRefresh(bool on);
void SetPartnerLookSwap(bool on);
void NotePartnerLook(const uint8_t* look, uint32_t seq);       // network thread: record +0x00..+0x2DB
void ForgetPartnerCharacter();                                 // npc_talk.cpp: the kept pointer, gone with the copy
void ForgetPartnerCopy();                                      // pvp_modes.cpp: the team written on the old copy
// Chests the host has open, opened for a guest in every map it loads (chest_lids.cpp, docs
// §3.47); what lies in them follows the guest's own save (loot_sync.cpp).
void SetChestLidsReconcile(bool on);
void ChestLidsTick();                        // game thread
void ApplyHostChestLids(uint32_t areaId);    // game thread, right after an area's objects were restored
void NoteHostChestLids(int32_t map, uint8_t source, const void* entries, uint16_t count);   // network thread, ChestLidEntry[]
// A guest's kills in the host's world counted in the session's kill counters (player_sync.cpp,
// the multiplayer predicate asked from exe+0x40FE79).
void SetGuestKillCounts(bool on);
// A guest's death result is never built as the host's while in the host's world
// (death_result_type.cpp, ini guest_result_type_fix; off: logged only).
bool InstallDeathResultType(bool enabled);

// Enemy drops for a guest from every kill (guest_drops.cpp, docs §3.40).
bool InstallGuestDrops(bool enabled);
void GuestDropsTick();          // game thread
void ForgetGuestDropRolls();    // a rest: every enemy can drop again

// Gates a session puts on things a player alone can do (mp_gates.cpp, docs §3.39):
// covenants while talking, a host summoning from Majula; probes for the ship table
// and Pharros contraptions.
bool InstallMpGates(bool enabled);
// The area's protection against invaders (a burnt Human Effigy) and the lobby partner's
// summon (mp_gates.cpp): the partner is let through, invaders are not (ini effigy_summon).
void SetEffigySummon(bool on);
// Cutscene transfers the game hides while in multiplayer (the Pursuer's eagle, the
// Wharf ship, portals, DLC entrances): offered in co-op (ini transfer_events_solo).
void SetTransferEventsSolo(bool on);
void SetPartnerSummonStarting(bool on);   // game thread, around the host's own start of the partner's summon
void NotePartnerSummonSent();              // RequestSummonSign went out for the partner
bool ReadAreaProtection(int32_t* area, float* secondsLeft);   // the current area: protected?
bool RecentSearchHit();   // an object was "searched" in the last two seconds

// Travelling in a co-op session without leaving it (travel_sync.cpp, docs §3.38).
// detachWhenApart: the enemy sync table is let go while the players stand in different
// maps (ini enemy_detach_when_apart).
bool InstallTravelSync(bool enabled, bool detachWhenApart);
void NoteLocalTravel(int32_t rawMap);      // a travel warp was just taken here
void NotePartnerRawMap(int32_t rawMap);    // network thread: the map the partner stands in
void TravelResyncTick();                   // game thread
// Game thread: zeroes every entry of the network enemy table that does not point at
// a generator record the game has loaded right now for the table's map, so the reset
// that follows writes through none of them. Returns how many, or -1 if unreadable.
int ForgetStaleEnemyEntries(uintptr_t enemyManager);
// Both players stand in the same map, by the game's own map value on each side (the
// partner's from its PlayerMap packets, if one came in the last few seconds).
bool PlayersShareMap();
// The game's own names in its current language (game_text.cpp): "" when not known yet
// or not showable with the overlay's font. Game thread only.
std::string GameBonfireName(int32_t bonfireId);
// A bonfire one player lights is lit for the other (bonfire_lit.cpp).
bool InstallBonfireLit();
void NotePartnerBonfireLit(int32_t id, int32_t map, const std::string& from);   // network thread
void BonfireLitGameTick();   // game thread
std::string GameMapName(int32_t rawMap);
// Probe: both characters' bonfire-travel pose numbers for a while after either
// player travels (travel_sync.cpp). Game thread.
void WatchPoses();
void PoseProbeTick();
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
    void EnableSummoning();
    std::string GetLocalCharacterName();
    // This player's own name only ([[GMImp+0xA8]+0x114]), "" while the character is not made yet --
    // never someone else's name from the session (loot_sync.cpp keys its records by it).
    std::string GetOwnCharacterName();
    // The name and the save slot, "name#slot" (the slot unreadable: the name alone) -- what loot_sync.cpp
    // keys its records by, so two characters of one name in two slots never share them.
    std::string GetOwnCharacterKey();

private:
    PlayerSync() = default;
    ~PlayerSync() = default;
    PlayerSync(const PlayerSync&) = delete;
    PlayerSync& operator=(const PlayerSync&) = delete;
    
    bool m_initialized = false;
    float m_positionSyncTimer = 0.0f;
    float m_stateSyncTimer = 0.0f;
    
    static constexpr float POSITION_SYNC_INTERVAL = 0.05f; // 20 times per second
    static constexpr float STATE_SYNC_INTERVAL = 0.5f;     // 2 times per second
};

} // namespace DS2Coop::Sync

