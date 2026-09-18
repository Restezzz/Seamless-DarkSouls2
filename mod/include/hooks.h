// Hook system for DS2 Seamless Co-op
//
// Strategy: Hook at the protobuf serialization layer (verified AOB from ds3os)
// rather than guessing internal game function addresses.
//
// The game sends ALL network messages through protobuf serialize/parse.
// By hooking those two functions, we can intercept and block disconnect
// messages (opcodes 0x03F9, 0x03EB, 0x03E9) to keep sessions alive.

#pragma once
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <cstdint>
#include <atomic>

namespace DS2Coop::Hooks {

// ============================================================================
// Hook Manager (MinHook wrapper)
// ============================================================================
class HookManager {
public:
    static HookManager& GetInstance();

    bool Initialize();
    void Shutdown();

    bool InstallHook(void* targetFunc, void* detourFunc, void** originalFunc);
    bool RemoveHook(void* targetFunc);
    bool EnableHooks();
    bool DisableHooks();

private:
    HookManager() = default;
    ~HookManager() = default;
    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;

    bool m_initialized = false;
};

// ============================================================================
// Protobuf Interception Hooks (core of the seamless co-op mechanism)
//
// ds3os discovered that DS2 routes all network messages through protobuf
// serialize/parse functions with known AOB patterns. By hooking these:
//
// 1. We intercept outgoing SerializeWithCachedSizesToArray calls
// 2. We use RTTI to get the protobuf class name (message type)
// 3. If it's a disconnect/leave message, we block it
// 4. The game thinks it sent the disconnect, but nothing went out
// 5. Session stays alive through boss kills, deaths, area transitions
// ============================================================================
// Place an incoming summon sign under the local player's feet.
//
// A sign carries no coordinates in the protobuf itself — RequestCreateSign has
// only online_area_id, cell_id, sign_type and an opaque player_struct — and the
// position lives inside that blob as four int16 at offset 4, measured by
// placing signs at known spots and diffing the bytes:
//
//   +4  x    +6  y    +8  z    +10 facing      value = 32 * (world - map origin)
//
// Rewriting those three so the sign lands wherever the receiver stands makes
// where it was placed irrelevant, which is what "the sign should appear under
// their feet" asks for, and what the summon automation will need later.
// Off by default; toggled at runtime so a bad rewrite cannot quietly break a
// session.
void SetSignUnderFeet(bool enable);
bool GetSignUnderFeet();

// The map this player is standing in, taken from the sign-list request the
// game sends about once a minute. Shared with the other player so a sign can
// be aimed into their map.
uint32_t GetLocalAreaId();

// How many sign-list requests went out and how many answers came back. Equal
// means nothing is in flight.
uint32_t GetSignListRequestCount();
uint32_t GetSignListResponseCount();

// Sign creations and summon requests that actually went out. The automatic join
// counts a placement or a summon as done only when its message has left.
uint32_t GetSignCreateCount();
uint32_t GetSummonRequestCount();
void NoteSignCreate();
void NoteSummonRequest();
ULONGLONG GetLastSignCreateTime();   // GetTickCount64 of the last RequestCreateSign, 0 if none yet

// Warns (log and screen, once per stall) when a sign or sign-list request has
// been followed by 25 s without any message from the server: the game's line to
// the server has stalled and no summon can work. Called from the game thread.
void ServerWatchTick();
ULONGLONG GetLastServerMessageTime();   // GetTickCount64 of the last message from the server, 0 if none
bool      IsServerLineStalled();        // the warning above is standing

// Aim the next sign this player creates at a given spot instead of the other
// player's feet -- once. Used after a death, to be summoned back beside a
// bonfire. Ignored unless the spot lies in the map the other player is in.
void SetNextSignTarget(uint32_t area, float x, float y, float z);

// Sign coordinates are stored as 32 * (world - origin) with an origin of their
// own per map, so aiming a sign into another map needs that map's origin, and a
// sign that cannot be aimed must not be placed at all.
bool IsMapOriginKnown(uint32_t area);
void NoteRemoteMapOrigin(uint32_t area, float x, float y, float z);   // packet MapOriginInfo
bool GetLocalMapOrigin(uint32_t* area, float* x, float* y, float* z);
void ShareLocalMapOrigin();   // tell the other player about the map I am standing in
// The sign about to be created only measures the map: it is not aimed at anyone and
// the other player is not told (set around the call that creates it).
void SetSignProbe(bool probe);
bool IsGameOriginTrusted();   // the game's own map origins matched a known one

namespace ProtobufHooks {
    bool InstallHooks();
    void UninstallHooks();

    // Function signatures from ds3os reverse engineering
    using SerializeFunc = uint8_t*(__fastcall*)(void* thisPtr, uint8_t* target);
    using ParseFunc = bool(__fastcall*)(void* thisPtr, void* data, int size);

    // Control whether disconnect messages are blocked
    void SetSeamlessActive(bool active);
    bool IsSeamlessActive();

    // Stats for debugging
    uint32_t GetBlockedMessageCount();
    uint32_t GetTotalMessageCount();

    // Sign filtering — only show summon signs from session members
    void AddSessionSteamId(const std::string& steamId);
    void ClearSessionSteamIds();
    std::string GetLocalSteamId();
}

// ============================================================================
// Winsock Hooks (connection monitoring + server redirect)
// Hooks Winsock connect() to detect and redirect game server connections.
// ============================================================================
namespace WinsockHooks {
    bool InstallHooks();
    void UninstallHooks();

    // Server redirect configuration
    void SetServerRedirect(const std::string& ip, uint16_t port);
    bool IsRedirectActive();

    // The address the game's own DNS gave for the login server, before the
    // redirect ("" until the game has connected). 198.18.x.x there means a TUN
    // proxy with fake-IP DNS (net_check.cpp).
    std::string GetLastLoginTarget();
}

// ============================================================================
// Server Redirect (hostname + RSA key patching in game memory)
// Patches the FromSoft server hostname and RSA public key so the game
// connects to our custom ds3os server instead.
// ============================================================================
namespace ServerRedirect {
    bool PatchHostname(const std::string& newHostname);
    bool PatchRSAKey(const std::string& newPublicKey);
    bool Install(const std::string& serverIp, const std::string& publicKeyPath);
}

// ============================================================================
// Game State Hooks (secondary - for detecting events locally)
// These are optional and use pattern scanning to find game functions.
// If they fail, the mod still works through protobuf interception alone.
// ============================================================================
namespace GameState {
    bool InstallHooks();
    void UninstallHooks();

    using PlayerDeathFunc = void(__fastcall*)(void* playerPtr);
    using BossDefeatedFunc = void(__fastcall*)(void* bossPtr);
}

} // namespace DS2Coop::Hooks
