#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

namespace DS2Coop {

// Version information
constexpr const char* MOD_VERSION = "0.1.3";  // keep in step with VERSION and CMakeLists.txt
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
    // off / log / on. Writes into the local save, so it starts at "log":
    // changes are detected and logged, but nothing is sent or applied.
    std::string flag_sync = "log";
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
    // Let a guest's own game finish putting characters into the world it joined.
    // The generator system skips that step in multiplayer, which is why NPCs are
    // missing or half-there for a guest and there is nobody to talk to. On by
    // default; Delete flips it in game, so the inversion can be seen either way.
    bool npc_spawn = true;
    // Make a boss fog hold a guest back until the other player has gone in.
    // Off: the fog behaves for a guest as it does for a host, the way 0.1.0 had
    // it. On, a guest who would otherwise walk in first cannot wake the boss by
    // itself -- but a host waiting at the fog then locks both players out.
    bool boss_fog_wait = false;
    // Test keys F2-F11, Home and End (they place signs, flip patches, dump
    // memory). Off by default so nobody sets them off by habit; the menu key,
    // Insert and Esc always work.
    bool debug_hotkeys = false;
    // Interface: "auto" follows the Windows language, "en"/"ru" is the
    // player's choice from the menu. The menu key is stored by name ("F1").
    std::string language = "auto";
    std::string menu_key = "F1";
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

    // Called by the menu when the language or menu key changes; writes the ini.
    void SetUiPreferences(const std::string& language, const std::string& menuKey);

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

