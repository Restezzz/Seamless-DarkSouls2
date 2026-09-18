#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

namespace DS2Coop {

// Version information
constexpr const char* MOD_VERSION = "0.2.2";  // keep in step with VERSION and CMakeLists.txt
constexpr const char* MOD_NAME = "Dark Souls 2 Seamless Co-op";

// Game version support
enum class GameVersion {
    Unknown,
    SteamLatest,      // Latest Steam version
    CalibrationVer112 // Calibration 1.12
};

// Configuration
struct ModConfig {
    bool enabled = true;
    bool debug_logging = false;
    uint16_t max_players = 6;
    uint16_t port = 27015;
    bool allow_invasions = false;
    bool sync_bonfires = true;
    bool sync_items = true;
    bool sync_enemies = false;
    // on / off. Progress sharing: lit bonfires, opened fog gates, defeated
    // bosses and collected items are event flags, and with this on they are
    // shared both ways, plus the host hands a joining guest everything it has
    // already done. This writes into the receiving player's save (backed up to
    // ds2_flags_backup.bin first). On by default since 0.1.3.
    std::string flag_sync = "on";
    // Put an incoming summon sign under the receiving player's feet instead of
    // where it was placed. On by default: it removes the walk to the sign and
    // is the groundwork for summoning without the ritual.
    bool sign_under_feet = true;
    // Joining through the mod menu does the whole summon by itself: the joiner
    // places a sign, the host summons it. On by default.
    bool auto_summon = true;
    // One player resting at a bonfire resets the world for everyone in the
    // session, so enemies respawn on both sides. On by default.
    bool rest_sync = true;
    // In someone else's world, show the world items this player's own save
    // has not picked up, and keep what is picked up there picked up at home.
    // On by default.
    bool loot_sync = true;
    // Travel anywhere with a partner: no co-op fog walls at area borders, and
    // walking through one does not end the session. On by default.
    bool free_travel = true;
    // Deaths in co-op: a guest who dies (or whose host dies) is brought back to
    // the partner's world at its respawn bonfire; in a boss fight nobody comes
    // back until the fight is decided or both are dead. On by default.
    bool death_respawn = true;
    // Let a guest's own game finish putting characters into the world it joined
    // (the generator system skips that step in multiplayer). Measured 12.09: the
    // call site fires, 24 000 times in one session, and the NPCs stay exactly as
    // missing and as see-through as before -- so this is not what hides them.
    // Off by default now: it changes what the game does for no gain we can see.
    // Delete still flips it in game for another look.
    bool npc_spawn = false;
    // Draw the world's other characters solid for a guest instead of as ghosts,
    // and the partner solid on the host's screen. The row a character is drawn
    // with comes from its own PlayerType, per character, so this answers 0 for
    // everything that is not the local player (docs §3.24). On by default.
    bool npc_solid = true;
    // Ask an NPC's prompt on a guest's behalf the way it is asked for a host.
    // The game refuses every phantom id before it looks at anything else, which
    // is why no NPC ever offered a guest "Talk". The first attempt overruled the
    // answer afterwards and failed -- the refusal is cached in the prompt's own
    // bookkeeping -- so the phantom id is zeroed for the duration of the call
    // instead. On by default; the answer is only re-taken when the guest leaves
    // and re-enters the NPC's zone.
    bool npc_talk = true;
    // Make a boss fog hold a guest back until the other player has gone in.
    // Off: the fog behaves for a guest as it does for a host, the way 0.1.0 had
    // it. On, a guest who would otherwise walk in first cannot wake the boss by
    // itself -- but a host waiting at the fog then locks both players out.
    bool boss_fog_wait = false;
    // A guest runs its own copy of the host's boss fight. The guest's game never
    // started one on 16.09, and the health bar, damage to the boss and the fog
    // once the host is inside all hang on that: the host now sends its battle id,
    // area and participant count, and the guest starts the same battle with the
    // game's own function (docs §3.32). On by default.
    bool boss_sync = true;
    // A guest's world (docs §3.37): NPC talk scripts asked as for the world's
    // owner; NPCs run on the guest's machine (they can be talked to and hit); a
    // joining guest's characters put in once the host's world has arrived, so the
    // enemies the host killed stay dead. All on by default.
    bool guest_npc_talk_scripts = true;
    bool guest_npc_local = true;
    bool guest_wait_for_snapshot = true;
    // Enemies for a guest (0.2.2, docs §3.45): the host's enemy states from the join
    // snapshot applied even though the join map's characters wait for it (killed
    // enemies stay dead); the shared enemy table let go while the players stand in
    // different maps (enemies there are not left frozen); a guest's death in the host's
    // world never handled as the host's (no endless wait watching the partner). All on.
    bool enemy_states_at_join = true;
    bool enemy_detach_when_apart = true;
    bool guest_result_type_fix = true;
    // Enemies as the host has them in every map a guest loads, not only the join map
    // (enemy_reconcile.cpp, docs §3.47): the host's killed enemies, and its kill counters,
    // which keep a boss killed long ago out of its arena. A guest's own kills counted in
    // the session's counters as the host counts them. All on.
    bool enemy_dead_reconcile = true;
    bool kill_counts_reconcile = true;
    bool guest_kill_counts = true;
    // A boss fight that ends while one of the players is down (boss_down.cpp, docs §3.47):
    // the host's event scripts keep running, and the fight's last phases hand out the souls
    // and the reward to a player who was down when the boss died. On.
    bool boss_while_down = true;
    // A guest thrown out right on arrival because the host's summon slot was dropped (a summon
    // where the area says no, the host crossing a border meanwhile): reserved again and let
    // through (summon_accept.cpp, docs §3.47). false: logged only.
    bool join_slot_confirm = true;
    // The lift's init event (m10_30, 1030) gets the game's own answer to "someone else's world?" for
    // a guest instead of "no": it no longer runs the world owner's branch on the guest. Every other
    // event and talk script still gets "no" (guest_world.cpp, docs §3.47-3.48). On.
    bool guest_lift_fix = true;
    // 0.2.2 test of 18.09 (docs §3.48): a join's arrival in a map the host has already left lands
    // where the host stands; the partner's copy stuck in the travel pose is put back; a bonfire the
    // partner lit is this player's respawn too; the partner's rest is replayed without the event and
    // flag reset (true: the full reset, as before); the host world's flags are carried into the
    // guest's own world.
    bool arrival_follow_host = true;
    bool travel_pose_fix = true;
    bool rest_replay_full = false;
    bool flags_carry_home = true;
    // A guest's hits on NPCs of the host's world are not counted towards their anger: an NPC hit
    // a few times still talks to the guest (npc_progress.cpp, docs §3.47). On.
    bool guest_npc_hits_ignored = true;
    // Chests the host has open are opened for a guest in every map it loads -- old chests the host
    // opened long ago could not be opened by the guest at all (chest_lids.cpp, docs §3.47). On.
    bool chest_lids_reconcile = true;
    // A guest who walks into a boss's arena first wakes the boss on the host, as the host would
    // (boss_arena.cpp, docs §3.47). On.
    bool boss_guest_starts = true;
    // A guest who dies far from its partner (same map): the world is loaded around the partner and
    // the camera follows it, instead of staying at the body (death_camera.cpp, docs §3.47). On.
    bool far_death_camera = true;
    // Travelling in a session (docs §3.38): after either player travels, the
    // partner's character and the shared enemies are put back once both stand in
    // the same map. On by default.
    bool travel_resync = true;
    // Script and menu gates that keep a host with a guest from doing things alone
    // players can: joining a covenant, summoning from Majula (docs §3.39).
    bool mp_gates = true;
    // NPCs for both players (docs §3.44): a guest's talk progress in the host's world
    // is kept instead of being dropped by the game, and an item an NPC's talk gives
    // reaches the partner too if the partner has none of it. On by default.
    bool npc_progress = true;
    // The area's protection against invaders (a burnt Human Effigy) keeps invaders out
    // but lets the lobby partner be summoned (docs §3.45). On by default.
    bool effigy_summon = true;
    // Cutscene transfers the game hides in multiplayer (the eagle at the Pursuer's nest,
    // the ship at No-man's Wharf, portals, DLC entrances) are offered in co-op.
    bool transfer_events_solo = true;
    // Damage between the players when this player hosts: "off" (none), "ff"
    // (friendly fire, no lock-on on each other) or "pvp" (the guest counts as an
    // evil spirit: it can be locked on and hit, the enemies leave it alone).
    // Changed from the lobby menu; guests follow their host's choice.
    std::string damage_mode = "off";
    // Test keys F2-F11, Home and End (they place signs, flip patches, dump
    // memory). Off by default so nobody sets them off by habit; the menu key,
    // Insert and Esc always work.
    bool debug_hotkeys = false;
    // Interface: "auto" follows the Windows language, "en"/"ru" is the
    // player's choice from the menu. The menu key is stored by name ("F1").
    // menu_size is the overlay's size in percent of what suits the screen.
    std::string language = "auto";
    std::string menu_key = "F1";
    int menu_size = 100;
    // Custom server redirect
    std::string server_ip = "127.0.0.1";    // IP of the ds3os custom server
    uint16_t server_port = 50031;            // Login port of custom server
    bool use_custom_server = true;           // Enable server redirect
};

// Main mod class
class SeamlessCoopMod {
public:
    static SeamlessCoopMod& GetInstance();
    
    bool Initialize();
    void Shutdown();
    
    bool IsInitialized() const { return m_initialized; }
    GameVersion GetGameVersion() const { return m_gameVersion; }
    const ModConfig& GetConfig() const { return m_config; }
    
    void LoadConfig();
    void SaveConfig();

    // Called by the menu when the language, menu key or menu size changes;
    // writes the ini.
    void SetUiPreferences(const std::string& language, const std::string& menuKey, int menuSize);
    // Damage between the players (0 none, 1 friendly fire, 2 PvP): read from and
    // written to damage_mode in the ini.
    uint8_t GetDamageModeSetting() const;
    void    SetDamageModeSetting(uint8_t mode);

private:
    SeamlessCoopMod() = default;
    ~SeamlessCoopMod() = default;
    SeamlessCoopMod(const SeamlessCoopMod&) = delete;
    SeamlessCoopMod& operator=(const SeamlessCoopMod&) = delete;
    
    bool DetectGameVersion();
    bool InstallHooks();
    void UninstallHooks();
    
    bool m_initialized = false;
    GameVersion m_gameVersion = GameVersion::Unknown;
    ModConfig m_config;
    HANDLE m_updateThread = nullptr;
};

} // namespace DS2Coop

