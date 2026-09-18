// Protobuf Interception Hooks - THE CORE OF SEAMLESS CO-OP
//
// This file implements the ds3os approach: instead of finding individual game
// functions (CreateSession, DestroySession, etc.), we hook the protobuf
// serialization layer that ALL network messages pass through.
//
// When the game tries to send a disconnect/leave message after a boss kill
// or player death, we intercept it here and silently drop it. The game
// thinks the message was sent, but the session stays alive.
//
// Sources:
//   - ds3os/Source/Injector/Hooks/DarkSouls2/DS2_LogProtobufsHook.cpp
//   - AOB patterns verified in DS2 SotFS by TLeonardUK

#include "../../include/hooks.h"
#include "../../include/addresses.h"
#include "../../include/session.h"
#include "../../include/sync.h"
#include "../../include/network.h"
#include "../../include/utils.h"
#include "../../include/pattern_scanner.h"
#include "../../include/address_resolver.h"
#include "../../include/ui.h"
#include "../../include/ui_settings.h"
#include "MinHook.h"
#include <typeinfo>
#include <string>
#include <vector>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <mutex>

using namespace DS2Coop::Hooks;
using namespace DS2Coop::Utils;
using namespace DS2Coop::Addresses;

// ============================================================================
// State
// ============================================================================
static std::atomic<bool> g_seamlessActive{false};
static std::atomic<uint32_t> g_blockedCount{0};
static std::atomic<uint32_t> g_totalCount{0};

// Steam ID whitelist — only signs from these players are shown
static std::vector<std::string> g_sessionSteamIds;
static std::mutex g_steamIdMutex;
static std::string g_localSteamId;

// ============================================================================
// Is the game still hearing from the server?
//
// 17.09, 00:28-00:41: a friend's game went on sending -- its signs reached the
// server, and the host saw and summoned them -- but after 00:28:46 got nothing
// back: not the answer to its own sign, not the summon, not one sign list. The
// game said nothing about it, and six joins in a row simply never happened. A
// sign or a sign list is answered at once while the line is alive (106 of 110
// signs and 1117 of 1120 lists in one long log), so a request followed by 25 s
// without a single message from the server names the stall, on screen.
// ============================================================================
static constexpr ULONGLONG kServerSilentMs = 25000;
static std::atomic<ULONGLONG> g_replyAwaitedSince{ 0 };   // the first request since the server last spoke
static std::atomic<bool>      g_serverSilentWarned{ false };
static std::atomic<ULONGLONG> g_lastServerMessageAt{ 0 };

static void NoteRequestAwaitingReply() {
    ULONGLONG Expected = 0;
    g_replyAwaitedSince.compare_exchange_strong(Expected, GetTickCount64());
}

static void NoteServerMessage() {
    g_lastServerMessageAt.store(GetTickCount64());
    const ULONGLONG Since = g_replyAwaitedSince.exchange(0);
    if (!g_serverSilentWarned.exchange(false)) return;
    LOG_INFO("[NET] the server answers again, %llu s after the unanswered request",
             static_cast<unsigned long long>(Since ? (GetTickCount64() - Since) / 1000 : 0));
    DS2Coop::UI::Overlay::GetInstance().ShowNotification(
        DS2Coop::UI::Tr("The server answers again.", "Связь с сервером восстановилась."),
        4.0f, DS2Coop::UI::NotifyKind::Success);
}

namespace DS2Coop::Hooks {
ULONGLONG GetLastServerMessageTime() { return g_lastServerMessageAt.load(); }
bool      IsServerLineStalled() { return g_serverSilentWarned.load(); }

void ServerWatchTick() {
    const ULONGLONG Since = g_replyAwaitedSince.load();
    if (!Since || g_serverSilentWarned.load()) return;
    const ULONGLONG Now = GetTickCount64();
    if (Now < Since + kServerSilentMs || g_serverSilentWarned.exchange(true)) return;
    LOG_WARNING("[NET] no message from the server for %llu s after a request it always answers -- the game's "
                "line to the server has stalled; signs and summons cannot work until it is back",
                static_cast<unsigned long long>((Now - Since) / 1000));
    DS2Coop::UI::Overlay::GetInstance().ShowNotification(
        DS2Coop::UI::Tr("The game gets no answer from the server, so no summon can work. Restart the game.",
                        "Игра не получает ответов от сервера \xE2\x80\x94 призыв не сработает. Перезапусти игру."),
        10.0f, DS2Coop::UI::NotifyKind::Warning);
}
}

// ============================================================================
// Get local Steam ID from steam_api64.dll
// ============================================================================
typedef uint64_t(*SteamID_t)();

static uint64_t SafeCallGetSteamID(void* userIface) {
    typedef uint64_t(__fastcall* Fn)(void*);
    void** vt = *reinterpret_cast<void***>(userIface);
    auto fn = reinterpret_cast<Fn>(vt[2]);
    __try { return fn(userIface); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static std::string GetLocalSteamIdInternal() {
    if (!g_localSteamId.empty()) return g_localSteamId;

    HMODULE hSteam = GetModuleHandleW(L"steam_api64.dll");
    if (!hSteam) return "";

    typedef void*(*GetSteamUser_t)();
    auto SteamUser = reinterpret_cast<GetSteamUser_t>(GetProcAddress(hSteam, "SteamAPI_SteamUser_v023"));
    if (!SteamUser) SteamUser = reinterpret_cast<GetSteamUser_t>(GetProcAddress(hSteam, "SteamAPI_SteamUser_v021"));
    if (!SteamUser) return "";

    void* user = SteamUser();
    if (!user) return "";

    uint64_t steamId = SafeCallGetSteamID(user);
    if (steamId == 0) return "";

    g_localSteamId = std::to_string(steamId);
    LOG_INFO("[STEAM] Local Steam ID: %s", g_localSteamId.c_str());
    return g_localSteamId;
}

// ============================================================================
// Check if raw protobuf bytes contain a steam_id from our session
// Field 5 (player_steam_id) has wire key 0x2A (field 5, type 2 = length-delimited)
// ============================================================================
static bool ContainsSessionSteamId(const uint8_t* data, int size) {
    std::lock_guard<std::mutex> lock(g_steamIdMutex);
    if (g_sessionSteamIds.empty()) return true; // no filter active

    // Scan for wire key 0x2A followed by varint length then steam ID string
    for (int i = 0; i < size - 20; i++) {
        if (data[i] == 0x2A) {
            // Read varint length
            int len = 0;
            int j = i + 1;
            if (j < size) {
                len = data[j] & 0x7F;
                if (data[j] & 0x80 && j + 1 < size) {
                    len |= (data[j + 1] & 0x7F) << 7;
                    j++;
                }
                j++;
            }
            // Steam IDs are 17 digits starting with "7656119"
            if (len >= 17 && len <= 20 && j + len <= size) {
                std::string candidate(reinterpret_cast<const char*>(data + j), len);
                if (candidate.substr(0, 7) == "7656119") {
                    // Found a Steam ID — check if it's in our session
                    for (const auto& sid : g_sessionSteamIds) {
                        if (sid == candidate) return true;
                    }
                    // Steam ID found but not in session — block this sign
                    return false;
                }
            }
        }
    }

    return true; // no steam ID found in message — allow through
}

// Original function pointers
static ProtobufHooks::SerializeFunc g_originalSerialize = nullptr;
static ProtobufHooks::ParseFunc g_originalParse = nullptr;

// ============================================================================
// RTTI Helper - extract class name from MSVC vtable
// This is how ds3os identifies which protobuf message is being serialized.
//
// MSVC x64 RTTI layout:
//   object -> vtable pointer (first 8 bytes)
//   vtable[-1] -> pointer to RTTICompleteObjectLocator
//   COL -> pTypeDescriptor (offset within module)
//   TypeDescriptor -> decorated name string
//
// The ds3os project uses this same technique (GetRttiNameFromObject).
// ============================================================================

// MSVC RTTI structures (simplified)
struct _RTTITypeDescriptor {
    void* pVFTable;
    void* spare;
    char name[1]; // Decorated name, e.g. ".?AVRequestNotifyDisconnectSession@@"
};

struct _RTTICompleteObjectLocator {
    uint32_t signature;       // 1 for x64
    uint32_t offset;
    uint32_t cdOffset;
    int32_t  pTypeDescriptor; // RVA to TypeDescriptor (relative to module base in x64)
    int32_t  pClassHierarchy; // RVA to ClassHierarchyDescriptor
    int32_t  pSelf;           // RVA to this COL (for module base calculation)
};

static const char* GetRttiClassName(void* obj) {
    if (!obj) return "unknown";

    __try {
        // Read vtable pointer
        uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(obj);
        if (!vtable) return "unknown";

        // vtable[-1] points to RTTICompleteObjectLocator
        auto* col = reinterpret_cast<_RTTICompleteObjectLocator*>(vtable[-1]);
        if (!col) return "unknown";

        // For x64 (signature == 1), TypeDescriptor is an RVA from module base.
        // Calculate module base from the COL's pSelf field.
        if (col->signature == 1) {
            uintptr_t colAddr = reinterpret_cast<uintptr_t>(col);
            uintptr_t moduleBase = colAddr - static_cast<uint32_t>(col->pSelf);
            auto* typeDesc = reinterpret_cast<_RTTITypeDescriptor*>(
                moduleBase + static_cast<uint32_t>(col->pTypeDescriptor));
            return typeDesc->name; // ".?AVClassName@@"
        }

        // x86 fallback (signature == 0) - direct pointer
        auto* typeDesc = reinterpret_cast<_RTTITypeDescriptor*>(
            static_cast<uintptr_t>(col->pTypeDescriptor));
        return typeDesc->name;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return "unknown";
    }
}

// ============================================================================
// Check if a message class name corresponds to a disconnect/leave message
// ============================================================================
// Messages to block when SENDING (outgoing — serialize hook)
// Don't block ANY outgoing messages — let them serialize and send normally.
// Blocking outgoing messages corrupts the server's session state.
// Outgoing disconnect blocking is OFF. See crash_history.md for why.
// Instead, we capture the return address when LeaveGuestPlayer is serialized
// to identify the caller function for future NOP patching.
static bool IsOutgoingDisconnect(const char* className) {
    (void)className;
    return false;
}

// Messages to block when RECEIVING (incoming — parse hook)
//
// HISTORY: This list used to also block LeaveSession, LeaveGuestPlayer,
// RemovePlayer, and BreakInTarget. Those entries were the *original* fix for
// boss-kill phantom dismissal — the server would push LeaveGuestPlayer after
// a boss kill and we'd intercept it. That fix was later REPLACED by the
// runtime NOP patch at exe+0x44ef7b (commit 8c0f792, PatchPhantomReturnOnBossKill)
// which prevents the boss-kill code from ever generating the dismiss event in
// the first place. The block-list entries were left in place as vestigial
// belt-and-suspenders, but it turned out they were catching DS2's normal
// death/respawn flow as collateral damage: the death state machine sends a
// LeaveSession-style round-trip and waits for the ack to advance to respawn.
// Blocking it leaves the player permanently dead-but-not-respawning, with
// the camera free to rotate around the corpse and the pause menu unable to
// open (DS2's input arbiter is also waiting on the same round-trip).
//
// CURRENT POLICY: only block the messages tied to documented host-crash
// bugs that have NO other fix:
//   - DisconnectSession  : server-initiated forced disconnect
//   - BanishPlayer       : server kicking us
//   - RemoveSign / RejectSign : crystal/homeward bone host crash
//     (commit dd10dea — phantom departs mid-session, server pushes sign
//     teardown, host processes it as "phantom gone" and crashes)
//
// Phantom join/leave detection now happens AFTER the original parser runs
// via OnPhantomJoined() / OnPhantomLeft() in ParseHook — those still fire
// correctly because we let the message through.
static bool IsIncomingDisconnect(const char* className) {
    if (!className) return false;
    if (strstr(className, "DisconnectSession")) return true;
    if (strstr(className, "BanishPlayer")) return true;
    // When a phantom uses the Black Separation Crystal, the server sends
    // PushRequestRemoveSign to the host. The host's game processes this as
    // "phantom is gone" and tries to tear down the active session — crash.
    if (strstr(className, "RemoveSign")) return true;
    // RejectSign can also carry a "phantom returned home" state that crashes
    // the host when processed mid-session.
    if (strstr(className, "RejectSign")) return true;
    return false;
}

// ============================================================================
// Check if a message is a death notification (we log but don't block)
// ============================================================================
static bool IsDeathMessage(const char* className) {
    if (!className) return false;
    if (strstr(className, "NotifyDeath")) return true;
    if (strstr(className, "NotifyKillPlayer")) return true;
    return false;
}

// Forward declarations for helpers defined below ParseHook
static void OnPhantomJoined();
static void OnPhantomLeft();

// ============================================================================
// HOOKED: SerializeWithCachedSizesToArray
//
// Every outgoing protobuf message passes through this function.
// We inspect the message type via RTTI and block disconnect messages.
// ============================================================================
// Which map this player is in, and where each map's coordinate origin sits.
//
// A sign's position is stored as 32 * (world - origin), with a different origin
// per map, so the difference trick that aims a sign at someone standing in the
// same map falls apart across maps — tested, and the sign landed some three
// hundred metres out. Aiming across maps needs the target map's origin, and that
// can be worked out from any sign this player has placed there:
//
//   origin = my world position - encoded / 32
//
// Learned once per map and kept, so a sign can later be aimed into a map this
// player has been in before, from anywhere.
struct MapOrigin { float x, y, z; };
static std::mutex                             g_originMutex;
static std::unordered_map<uint32_t, MapOrigin> g_mapOrigins;
static std::atomic<uint32_t>                  g_localAreaId{ 0 };

// Sign-list traffic, counted both ways. The automatic summon asks for the list
// again only when nothing is in flight, and "in flight" is these two counters
// disagreeing.
static std::atomic<uint32_t>                  g_signListRequests{ 0 };
static std::atomic<uint32_t>                  g_signListResponses{ 0 };
static bool                                   g_originsLoaded = false;

// Origins survive a restart.
//
// They were kept in memory only, so a map learned yesterday was forgotten on
// the next launch and a sign aimed into it simply refused to move — which is
// exactly what "it used to appear and now it doesn't" turned out to be. A map's
// origin is a property of the game, not of the session, so it belongs on disk.
static const wchar_t* kOriginFile = L"ds2_sign_origins.txt";

static void LoadMapOrigins() {
    if (g_originsLoaded) return;
    g_originsLoaded = true;

    // Two maps measured by hand, so a fresh install can aim into them before
    // ever having placed a sign there. Anything else is learned as it is used.
    g_mapOrigins.emplace(10310000u, MapOrigin{ -13.98f, -15.02f, 163.02f });
    g_mapOrigins.emplace(10100000u, MapOrigin{ 207.99f,  10.00f, -133.02f });

    FILE* f = nullptr;
    if (_wfopen_s(&f, kOriginFile, L"r") != 0 || !f) return;

    unsigned area = 0; float x = 0, y = 0, z = 0;
    int loaded = 0;
    while (fscanf_s(f, "%u %f %f %f", &area, &x, &y, &z) == 4) {
        if (area) { g_mapOrigins[area] = MapOrigin{ x, y, z }; loaded++; }
    }
    fclose(f);
    // With the numbers, not just the count. A stored origin that is wrong is
    // invisible otherwise, and one of them was wrong for two days: Majula's was
    // the position of whoever had placed a sign there, and it went on summoning
    // the guest off the map long after the code was fixed, because the guest's
    // own copy of this file still held it (12.09).
    LOG_INFO("[SIGN] %d map origin(s) read back from disk", loaded);
    for (const auto& kv : g_mapOrigins) {
        LOG_INFO("[SIGN]   map %u origin (%.2f, %.2f, %.2f)", kv.first, kv.second.x, kv.second.y, kv.second.z);
    }
}

static void SaveMapOrigins() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, kOriginFile, L"w") != 0 || !f) {
        LOG_WARNING("[SIGN] could not write ds2_sign_origins.txt");
        return;
    }
    for (const auto& kv : g_mapOrigins) {
        fprintf(f, "%u %.4f %.4f %.4f\n", kv.first, kv.second.x, kv.second.y, kv.second.z);
    }
    fclose(f);
}

// The game's own per-map origin: the one its sign encoder subtracts.
//
// exe+0x210730(spot, out) turns a spot {raw map id, x, y, z} into what a sign
// carries. It finds the area with exe+0x3BCE40([GMImp+0x38], raw map id), takes
// the block at area+0x148 and subtracts the three floats at +0x2F0..+0x2F8
// (SUBPS at exe+0x2107BD). The same floats read here are the origin a sign in
// that map is measured against -- for a map loaded on this machine, which the
// one this player stands in always is.
//
// exe+0x2A9E70 was asked before, and in every session in both players' logs it
// never answered once -- not even for 10100000 with the host standing in it
// (17.09 15:00). So the host kept putting a sign of its own down in every new
// map to measure it, and that sign stayed under its feet after the eagle flight
// (0.2.2 point 10). A measurement from a sign is also only as good as the spot
// the game picked: where it takes a region's spot instead of the player's feet,
// "my position minus the sign" is not the origin, and 10100000 went from
// (208, 10, -133) to (0, 0, 0) and (241, 11, -132) that way (17.09, host log).
//
// Still not taken on faith. Before it is used for a map nobody has stood in, it
// is compared with maps whose origin is known -- two measured by hand, three
// measured by both players independently, on different days, to the same 0.03 --
// and trusted only if it reproduces one within 0.1 (the field on the wire has a
// step of 1/32) with none disagreeing. A sign of this player's own that matches
// it trusts it too. The comparison happens in the game, with real numbers, and
// goes into the log either way.
//
// Game thread only (the sign tick and the sign creation): it walks the map
// manager. The packet from the other player never comes through here.
constexpr uint32_t kGameManagerImpRva = 0x16148F0;   // *(exe+...) = GameManagerImp; +0x38 map manager
constexpr uint32_t kAreaByRawId       = 0x3BCE40;    // (map manager, raw map id) -> area, or 0
using AreaByRawIdFn = uintptr_t(__fastcall*)(uintptr_t, uint32_t);

static std::atomic<int> g_gameOriginTrust{ 0 };   // 0 not checked yet, 1 trusted, -1 no

// 10160000 -> 0x0A100000.
static uint32_t RawMapId(uint32_t area) {
    return ((area / 1000000u) & 0xFF) << 24 | ((area / 10000u % 100u) & 0xFF) << 16 |
           ((area / 100u % 100u) & 0xFF) << 8 | (area % 100u & 0xFF);
}

// Maps whose origin was established in this run -- by the game itself or by a
// sign of this player's own, in that map. Anything else is hearsay off the disk,
// and hearsay has to lose to a number the other player measured just now: a
// wrong value stored for Majula survived every fix to the code because the mod
// refused to overwrite what it already had (12.09).
static std::unordered_map<uint32_t, bool> g_originMeasuredHere;

static bool QueryGameMapOrigin(uint32_t area, MapOrigin* out) {
    if (!area || !out) return false;
    __try {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
        const uintptr_t gm = *reinterpret_cast<const uintptr_t*>(base + kGameManagerImpRva);
        const uintptr_t mapMgr = gm ? *reinterpret_cast<const uintptr_t*>(gm + 0x38) : 0;
        if (!mapMgr) return false;
        const uintptr_t found = reinterpret_cast<AreaByRawIdFn>(base + kAreaByRawId)(mapMgr, RawMapId(area));
        const uintptr_t block = found ? *reinterpret_cast<const uintptr_t*>(found + 0x148) : 0;
        if (!block) return false;
        const float* got = reinterpret_cast<const float*>(block + 0x2F0);
        for (int i = 0; i < 3; i++) {
            if (!(got[i] == got[i]) || got[i] > 1.0e6f || got[i] < -1.0e6f) return false;
        }
        out->x = got[0];
        out->y = got[1];
        out->z = got[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SameOrigin(const MapOrigin& a, const MapOrigin& b) {
    return fabsf(a.x - b.x) < 0.1f && fabsf(a.y - b.y) < 0.1f && fabsf(a.z - b.z) < 0.1f;
}

static void CheckGameOriginOnce() {
    if (g_gameOriginTrust.load() != 0) return;
    struct Known { uint32_t Area; float X, Y, Z; };
    // Majula (0, 0, 0) is left out on purpose: a block of zeros would pass for it.
    static const Known known[] = {
        { 10310000u,  -13.98f, -15.02f,  163.02f },   // by hand
        { 10100000u,  207.99f,  10.00f, -133.02f },   // by hand
        { 10160000u,  -77.99f,   4.02f,  562.00f },   // host 17.09 15:11:57, guest in an earlier session
        { 10180000u,   51.99f, -69.97f,  486.98f },   // host 17.09 14:43:40, guest in an earlier session
        { 10020000u, -497.99f,  29.98f, -259.98f },   // host 00:28:46, guest 00:28:55, each its own sign
    };
    // Per map, and never all-or-nothing. Demanding both of them at once is what
    // made this whole check dead code: the query only answers for a map whose
    // data is loaded, the loop asked for 10310000 first, that map is almost never
    // loaded next to 10100000, and the `return` left trust at "not checked yet"
    // for the entire session -- every time. So the game's own origin was never
    // once used, the stored one always won, and Majula stayed broken through
    // three releases (12.09, both players' logs have not a single line from here).
    int matched = 0, wrong = 0, resolved = 0;
    for (const Known& k : known) {
        MapOrigin got{};
        if (!QueryGameMapOrigin(k.Area, &got)) continue;   // that map is not loaded: try the next
        ++resolved;
        const bool ok = SameOrigin(got, MapOrigin{ k.X, k.Y, k.Z });
        LOG_INFO("[SIGN] the game puts map %u origin at (%.2f, %.2f, %.2f); known as (%.2f, %.2f, %.2f) -- %s",
                 k.Area, got.x, got.y, got.z, k.X, k.Y, k.Z, ok ? "same" : "DIFFERENT");
        if (ok) ++matched; else ++wrong;
    }
    if (!resolved) {
        static bool told = false;
        if (!told) {
            told = true;
            LOG_INFO("[SIGN] none of the maps with a known origin is loaded yet -- the game's origins are not "
                     "used until one is (or until a sign of mine matches)");
        }
        return;   // ask again next time
    }
    const bool trust = wrong == 0 && matched > 0;
    g_gameOriginTrust.store(trust ? 1 : -1);
    LOG_INFO("[SIGN] origins read from the game are %s (%d of %zu maps with a known origin could be checked)",
             trust ? "trusted -- no sign of my own is needed to measure a map"
                   : "NOT trusted -- only maps measured here or sent by the other player",
             resolved, sizeof(known) / sizeof(known[0]));
}

// A sign of this player's own was just written in `area`, and `measured` is
// "my position minus the sign". Where the game put the sign at the player's feet
// that is the origin exactly, so it agreeing with the game's own number trusts
// the game's number; where the game used a region's spot it is not the origin,
// so disagreeing proves nothing and only goes into the log.
static void CompareGameOriginWithSign(uint32_t area, const MapOrigin& measured) {
    MapOrigin fromGame{};
    if (!QueryGameMapOrigin(area, &fromGame)) {
        LOG_INFO("[SIGN] map %u: the game gives no origin to compare my sign with", area);
        return;
    }
    const bool same = SameOrigin(fromGame, measured);
    LOG_INFO("[SIGN] map %u: the game's origin (%.2f, %.2f, %.2f), from my sign (%.2f, %.2f, %.2f) -- %s",
             area, fromGame.x, fromGame.y, fromGame.z, measured.x, measured.y, measured.z,
             same ? "same" : "different (the sign may be at a region's spot, not at my feet)");
    // A zero origin proves nothing, for the same reason Majula is left out of the
    // known maps above: a block of zeros would pass for it.
    if (same && SameOrigin(fromGame, MapOrigin{ 0.0f, 0.0f, 0.0f })) {
        LOG_INFO("[SIGN] map %u: both say (0, 0, 0) -- not taken as proof that the game's origins are right", area);
        return;
    }
    int expected = 0;
    if (same && g_gameOriginTrust.compare_exchange_strong(expected, 1)) {
        LOG_INFO("[SIGN] origins read from the game are trusted -- my own sign agrees with it");
    }
}

// The origin to aim a sign into a map with. The game's own answer comes first,
// because a value measured from a sign can be wrong -- and one of them was.
// Majula came out as (10.53, 5.92, -16.25), which is simply where the player who
// placed that sign stood: a sign in Majula only goes down through the mod's own
// fallback spot, that spot carried coordinates of zero, and "my position minus
// zero" is my position. The wrong number was then kept on disk and handed to the
// other player, and the guest went on being summoned off the map and dying there
// (12.09, 04:52 and 04:54). So the game is asked first and a stored value that
// disagrees with it is thrown away.
static bool LookupMapOrigin(uint32_t area, MapOrigin* out) {
    if (!area || !out) return false;
    MapOrigin stored{};
    bool haveStored = false;
    {
        std::lock_guard<std::mutex> lock(g_originMutex);
        LoadMapOrigins();
        auto it = g_mapOrigins.find(area);
        if (it != g_mapOrigins.end()) {
            stored = it->second;
            haveStored = true;
        }
    }
    CheckGameOriginOnce();
    MapOrigin fromGame{};
    const bool gameAnswers = QueryGameMapOrigin(area, &fromGame);
    if (gameAnswers && g_gameOriginTrust.load() == 0) {
        // Not used yet, only logged: what the next test needs to see (once per map).
        static std::unordered_map<uint32_t, bool> told;
        if (!told[area]) {
            told[area] = true;
            LOG_INFO("[SIGN] map %u: the game says its origin is (%.2f, %.2f, %.2f), stored %s -- not used "
                     "until checked", area, fromGame.x, fromGame.y, fromGame.z,
                     haveStored ? (SameOrigin(stored, fromGame) ? "the same" : "DIFFERENT") : "nothing");
        }
    }
    if (gameAnswers && g_gameOriginTrust.load() == 1) {
        const bool same = haveStored && SameOrigin(stored, fromGame);
        {
            // Called every frame: the file is written only when the number changes.
            std::lock_guard<std::mutex> lock(g_originMutex);
            g_originMeasuredHere[area] = true;
            if (!same) {
                g_mapOrigins[area] = fromGame;
                SaveMapOrigins();
            }
        }
        if (!haveStored) {
            LOG_INFO("[SIGN] map %u origin read from the game: (%.2f, %.2f, %.2f)",
                     area, fromGame.x, fromGame.y, fromGame.z);
        } else if (!same) {
            LOG_WARNING("[SIGN] map %u was stored as (%.2f, %.2f, %.2f) and the game says (%.2f, %.2f, %.2f) "
                        "-- the game wins and the stored one is thrown away",
                        area, stored.x, stored.y, stored.z, fromGame.x, fromGame.y, fromGame.z);
        }
        *out = fromGame;
        return true;
    }
    if (haveStored) {
        *out = stored;
        return true;
    }
    return false;
}

// Read a protobuf varint. Returns the value, advances the offset.
static uint64_t ReadVarint(const uint8_t* data, size_t len, size_t& off) {
    uint64_t value = 0;
    int shift = 0;
    while (off < len && shift < 64) {
        const uint8_t b = data[off++];
        value |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return value;
}

// The game asks for the sign list about once a minute and states which map it is
// asking about, which is the cheapest way to know where this player is standing
// without hunting for it in memory.
static void NoteAreaFromSignListRequest(const uint8_t* data, size_t len) {
    if (!data || len < 2 || data[0] != 0x08) return;   // field 1, varint
    size_t off = 1;
    const uint32_t area = static_cast<uint32_t>(ReadVarint(data, len, off));
    if (!area) return;
    if (g_localAreaId.exchange(area) != area) {
        LOG_INFO("[SIGN] now in map %u", area);
    }
}

// The map a RequestCreateSign puts the sign into: field 1, a varint (10160000 in
// "08 80 8F EC 04", 17.09 15:11:57). 0 when the message does not start with it.
static uint32_t ReadSignArea(const uint8_t* data, size_t len) {
    if (!data || len < 2 || data[0] != 0x08) return 0;
    size_t off = 1;
    return static_cast<uint32_t>(ReadVarint(data, len, off));
}

// Set by the sign tick around its own call that puts a sign down only to measure
// the map (player_sync.cpp, ProbeOwnMapOrigin); the message is built inside that call.
static std::atomic<bool> g_signIsProbe{ false };

static void AimSignAtOtherPlayer(uint8_t* data, size_t len);

static uint8_t* __fastcall SerializeHook(void* thisPtr, uint8_t* target) {
    g_totalCount++;

    const char* className = GetRttiClassName(thisPtr);

    // If seamless mode is active, block disconnect messages
    if (g_seamlessActive.load()) {
        if (IsOutgoingDisconnect(className)) {
            g_blockedCount++;
            LOG_INFO("[SEAMLESS] BLOCKED outgoing: %s (total: %u)", className, g_blockedCount.load());

            // Let the serialize run so the game's internal state stays consistent,
            // but return the original target pointer so zero bytes are added to
            // the output buffer. The network layer sees an empty message and skips it.
            // Previous approach of memset-zeroing corrupted the protobuf stream.
            g_originalSerialize(thisPtr, target);
            return target;
        }

        if (IsDeathMessage(className)) {
            LOG_INFO("[SEAMLESS] Death notification sent (allowed): %s", className);
            // We allow death messages through - the session should survive deaths
            // because we block the disconnect that would normally follow.
        }
    }

    // Log session, item, and interaction messages for debugging
    if (strstr(className, "Session") || strstr(className, "Sign") ||
        strstr(className, "Guest") || strstr(className, "BreakIn") ||
        strstr(className, "Leave") || strstr(className, "Return") ||
        strstr(className, "Banish") || strstr(className, "Remove") ||
        strstr(className, "Phantom") || strstr(className, "Summon") ||
        strstr(className, "Item") || strstr(className, "Buy") ||
        strstr(className, "Chest") || strstr(className, "Treasure") ||
        strstr(className, "Bonfire") || strstr(className, "Visit") ||
        strstr(className, "Boss") || strstr(className, "FogWall")) {
        LOG_INFO("[PROTOBUF >>] %s (seamless=%s)", className,
                 g_seamlessActive.load() ? "ON" : "OFF");
    }

    // Capture call stack when the game tries to send LeaveGuestPlayer or LeaveSession
    // (which function initiates phantom removal after a boss), and when it
    // creates a summon sign.
    //
    // The sign is the interesting one. The soapstone is greyed out in areas the
    // game will not allow a sign in, and greyed out means no refusal message to
    // trace — the probe on the message path caught the locked door (id 0x451)
    // and nothing at all for the soapstone. So the check has to be found from
    // the other end: place a sign somewhere it IS allowed, and the stack at the
    // moment RequestCreateSign goes out names the whole creation path. That path
    // holds both the region test to remove and the position to overwrite, which
    // is what "place anywhere" and "sign under the other player's feet" both need.
    if (strstr(className, "LeaveGuestPlayer") || strstr(className, "LeaveSession") ||
        strstr(className, "CreateSign") || strstr(className, "SummonSign")) {
        // Walk the return addresses on the stack to find the caller chain
        void* callers[16] = {};
        USHORT frames = CaptureStackBackTrace(0, 16, callers, nullptr);
        uintptr_t exeBase = (uintptr_t)GetModuleHandle(nullptr);
        LOG_INFO("[CALLTRACE] %s initiated from (exe base: 0x%llX):", className, exeBase);
        for (int i = 0; i < frames && i < 16; i++) {
            uintptr_t addr = (uintptr_t)callers[i];
            LOG_INFO("[CALLTRACE]   [%d] 0x%llX (exe+0x%llX)", i, addr, addr - exeBase);
        }
    }

    // Dump the bytes of a sign creation.
    //
    // RequestCreateSign carries no coordinates — only online_area_id, cell_id,
    // sign_type, matching_parameter and an opaque player_struct — so the sign's
    // position has to live inside that blob. Two signs placed a few steps apart
    // differ only in position, so dumping the raw message for each and diffing
    // them points straight at the bytes that hold it.
    //
    // That is what both outstanding requests need: the offset to overwrite so a
    // sign lands under the receiving player's feet, and the same structure the
    // creation path builds when placing one without the soapstone.
    if (strstr(className, "RequestGetSignList")) {
        uint8_t* end = g_originalSerialize(thisPtr, target);
        const size_t len = (end > target) ? static_cast<size_t>(end - target) : 0;
        NoteAreaFromSignListRequest(target, len);
        g_signListRequests.fetch_add(1);
        NoteRequestAwaitingReply();

        // Where summoning is not allowed the game asks for no areas at all, so
        // the request is a stub and no sign can come back. Whatever decides that
        // is the same gate that greys out the soapstone, and it cannot be caught
        // from a refusal message because there isn't one. Take the stack the
        // first time a request is built properly and the first time it comes out
        // empty: the two differ at exactly the check worth patching.
        // Every request gets a line. Only the first of each kind carries a
        // stack: the point of the line is to show whether the client is asking
        // at all, and a once-per-session log cannot show that changing.
        const bool empty = (len < 24);
        LOG_INFO("[SIGNREQ] %s request, %zu bytes, map %u",
                 empty ? "EMPTY" : "normal", len, g_localAreaId.load());

        static bool loggedFull = false, loggedEmpty = false;
        if ((empty && !loggedEmpty) || (!empty && !loggedFull)) {
            (empty ? loggedEmpty : loggedFull) = true;
            void* callers[16] = {};
            USHORT frames = CaptureStackBackTrace(0, 16, callers, nullptr);
            uintptr_t exeBase = (uintptr_t)GetModuleHandle(nullptr);
            LOG_INFO("[SIGNREQ] stack for the first %s request:", empty ? "EMPTY" : "normal");
            for (int i = 0; i < frames && i < 16; i++) {
                LOG_INFO("[SIGNREQ]   [%d] exe+0x%llX", i,
                         (unsigned long long)((uintptr_t)callers[i] - exeBase));
            }
        }
        return end;
    }

    if (strstr(className, "RequestCreateSign")) {
        uint8_t* end = g_originalSerialize(thisPtr, target);
        const size_t len = (end > target) ? static_cast<size_t>(end - target) : 0;

        float px = 0, py = 0, pz = 0, prot = 0;
        if (DS2Coop::Sync::GetLocalPlayerPosition(px, py, pz, prot)) {
            LOG_INFO("[SIGNBYTES] %zu bytes | player x=%.2f y=%.2f z=%.2f rot=%.3f",
                     len, px, py, pz, prot);
        } else {
            LOG_INFO("[SIGNBYTES] %zu bytes | player position unavailable", len);
        }
        char line[3 * 32 + 16];
        for (size_t off = 0; off < len && off < 1024; off += 32) {
            int used = snprintf(line, sizeof(line), "%04zX:", off);
            for (size_t i = off; i < len && i < off + 32; i++)
                used += snprintf(line + used, sizeof(line) - used, " %02X", target[i]);
            LOG_INFO("[SIGNBYTES] %s", line);
        }

        AimSignAtOtherPlayer(target, len);
        DS2Coop::Hooks::NoteSignCreate();
        NoteRequestAwaitingReply();

        // Tell the other player straight away. Their client polls for signs on
        // its own schedule, which is about a minute, and that minute is the
        // whole reason a sign can take that long to show up. Not for a sign that
        // only measures the map: nobody is meant to see that one.
        if (!g_signIsProbe.load()) {
            DS2Coop::Network::PacketHeader Ping{};
            Ping.magic = 0x44533243;
            Ping.type = DS2Coop::Network::PacketType::SignPlaced;
            Ping.size = sizeof(Ping);
            Ping.timestamp = GetTickCount64();
            DS2Coop::Network::PeerManager::GetInstance().BroadcastPacket(&Ping);
        }
        return end;
    }

    // Summon requests are counted: the host's automatic summon counts as done
    // only once one has really gone out.
    if (strstr(className, "RequestSummonSign") && !strstr(className, "Push")) {
        DS2Coop::Hooks::NoteSummonRequest();
    }

    // Call original for all non-blocked messages
    return g_originalSerialize(thisPtr, target);
}

// Plain C helper — no C++ objects, safe for SEH.
// Reads phantom name from NetSessionManager into buf (UTF-8, null-terminated).
static void TryReadPhantomName(char* buf, int bufLen) {
    uintptr_t nsm = DS2Coop::AddressResolver::GetInstance().GetNetSessionManager();
    if (!nsm) return;
    __try {
        uintptr_t pp = 0;
        if (!DS2Coop::Utils::Memory::Read<uintptr_t>(nsm + 0x20, &pp) || !pp) return;
        wchar_t wname[24] = {};
        for (int i = 0; i < 23; i++) {
            wchar_t ch = 0;
            if (!DS2Coop::Utils::Memory::Read<wchar_t>(pp + 0x234 + i*2, &ch) || ch == 0) break;
            if (ch < 0x20 || ch > 0x9FFF) return;
            wname[i] = ch;
        }
        if (wname[0])
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, buf, bufLen-1, nullptr, nullptr);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static std::atomic<int> g_phantomCounter{0};

static void OnPhantomJoined() {
    // Don't read name from NSM+0x20+0x234 — that's always the LOCAL player's name.
    // Use a numbered placeholder. P2P handshake will update the name if it connects.
    int num = ++g_phantomCounter;
    char nameBuf[32];
    snprintf(nameBuf, sizeof(nameBuf), "Phantom %d", num);

    // Use a deterministic-ish ID that won't collide with the local player ID
    uint64_t phantomId = static_cast<uint64_t>(GetTickCount64()) ^ (0xDE1F0000ULL + num);
    LOG_INFO("[SEAMLESS] Phantom entered world: %s (id=%llu)", nameBuf, phantomId);
    DS2Coop::Session::SessionManager::GetInstance().AddPlayer(phantomId, nameBuf);
}

static void OnPhantomLeft() {
    LOG_INFO("[SEAMLESS] Phantom left world — removing from session");
    auto& sm = DS2Coop::Session::SessionManager::GetInstance();
    auto players = sm.GetPlayers();
    // The id, not GetLocalPlayer(): that hands out a pointer into the player
    // list, which the network thread changes under its lock.
    uint64_t localId = DS2Coop::Network::PeerManager::GetInstance().GetLocalPlayerId();
    for (const auto& p : players) {
        if (p.playerId != localId) {
            sm.RemovePlayer(p.playerId);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Sign relocation.
// ---------------------------------------------------------------------------
static std::atomic<bool> g_signUnderFeet{ false };

// Aiming happens once, when the sign is created, so there is nothing to freeze:
// the sign is written at the other player's position at that instant and the
// server stores it that way. It cannot drift afterwards, which is what "it
// should stay where it appeared" asked for.

namespace DS2Coop::Hooks {
void SetSignUnderFeet(bool enable) {
    g_signUnderFeet.store(enable);
    LOG_INFO("[SIGN] under-feet relocation %s", enable ? "ON" : "OFF");
}
void SetSignProbe(bool probe) { g_signIsProbe.store(probe); }
bool IsGameOriginTrusted() { return g_gameOriginTrust.load() == 1; }
bool GetSignUnderFeet() { return g_signUnderFeet.load(); }
uint32_t GetLocalAreaId() { return g_localAreaId.load(); }
uint32_t GetSignListRequestCount() { return g_signListRequests.load(); }
uint32_t GetSignListResponseCount() { return g_signListResponses.load(); }

static std::atomic<uint32_t> g_signCreates{ 0 };
static std::atomic<uint32_t> g_summonRequests{ 0 };
static std::atomic<ULONGLONG> g_lastSignCreateAt{ 0 };
uint32_t GetSignCreateCount() { return g_signCreates.load(); }
uint32_t GetSummonRequestCount() { return g_summonRequests.load(); }
ULONGLONG GetLastSignCreateTime() { return g_lastSignCreateAt.load(); }
void NoteSignCreate() {
    g_signCreates.fetch_add(1);
    g_lastSignCreateAt.store(GetTickCount64());
}
void NoteSummonRequest() { g_summonRequests.fetch_add(1); }
}

// A one-off aim for the next sign (see SetNextSignTarget).
struct SignTarget {
    bool     Set;
    uint32_t Area;
    float    X, Y, Z;
};
static std::mutex g_signTargetMutex;
static SignTarget g_signTarget{};

namespace DS2Coop::Hooks {
void SetNextSignTarget(uint32_t area, float x, float y, float z) {
    std::lock_guard<std::mutex> lock(g_signTargetMutex);
    g_signTarget = SignTarget{ true, area, x, y, z };
    LOG_INFO("[SIGN] next sign aimed at map %u (%.2f, %.2f, %.2f)", area, x, y, z);
}

// Can a sign be aimed into this map at all? Its own map always can (the origins
// cancel); any other map needs that map's origin. A sign that cannot be aimed
// must not be placed: its numbers would be read against the other map's origin,
// which is how a guest ended up off the map in Majula and died on arrival.
bool IsMapOriginKnown(uint32_t area) {
    if (!area) return false;
    if (area == g_localAreaId.load()) return true;
    MapOrigin origin{};
    return LookupMapOrigin(area, &origin);
}

// The other player measured the origin of the map it is standing in and sent it
// over. Kept like a locally learned one, on disk included.
void NoteRemoteMapOrigin(uint32_t area, float x, float y, float z) {
    if (!area) return;
    std::lock_guard<std::mutex> lock(g_originMutex);
    LoadMapOrigins();
    auto it = g_mapOrigins.find(area);

    // Only a value established in this run outranks the other player's: it was
    // either the game's own answer or measured by a sign in that very map. A
    // line read from the file is hearsay, and refusing to overwrite hearsay is
    // exactly what kept the guest being summoned off the map in Majula -- the
    // host measured the right origin, sent it every ten seconds, and the guest
    // threw it away in favour of the wrong number in its own file (12.09).
    if (it != g_mapOrigins.end()) {
        if (g_originMeasuredHere.count(area)) return;   // measured here: keep ours
        const bool same = fabsf(it->second.x - x) < 0.1f && fabsf(it->second.y - y) < 0.1f &&
                          fabsf(it->second.z - z) < 0.1f;
        if (same) return;
        LOG_WARNING("[SIGN] map %u was stored as (%.2f, %.2f, %.2f) and the other player measured "
                    "(%.2f, %.2f, %.2f) in it -- taking theirs, ours was only read from the file",
                    area, it->second.x, it->second.y, it->second.z, x, y, z);
        g_mapOrigins[area] = MapOrigin{ x, y, z };
        SaveMapOrigins();
        return;
    }

    g_mapOrigins[area] = MapOrigin{ x, y, z };
    SaveMapOrigins();
    LOG_INFO("[SIGN] map %u origin from the other player: (%.2f, %.2f, %.2f) -- signs can be aimed into it now",
             area, x, y, z);
}

// Tell the other player the origin of the map this player is standing in, so it
// can aim a sign into it. Only whoever has stood in a map knows its origin, so
// this is the only way the other side can get it without having been there.
// Repeated on a slow timer: it costs 24 bytes and it has to survive a join that
// happened before this player arrived in the map.
void ShareLocalMapOrigin() {
    uint32_t area = 0;
    float x = 0, y = 0, z = 0;
    if (!GetLocalMapOrigin(&area, &x, &y, &z)) return;
    static std::atomic<uint32_t>  lastArea{ 0 };
    static std::atomic<ULONGLONG> lastAt{ 0 };
    const ULONGLONG now = GetTickCount64();
    if (lastArea.load() == area && now - lastAt.load() < 10000) return;
    lastArea.store(area);
    lastAt.store(now);
    DS2Coop::Network::MapOriginPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type = DS2Coop::Network::PacketType::MapOriginInfo;
    packet.header.size = sizeof(packet);
    packet.area = area;
    packet.x = x; packet.y = y; packet.z = z;
    DS2Coop::Network::PeerManager::GetInstance().BroadcastPacket(&packet.header);
}

// The origin of the map this player is standing in, to send to the other one.
//
// Through LookupMapOrigin, so the game is asked first. It used to read the
// stored table and nothing else, and two things followed from that: the origin
// handed to the other player was whatever the file happened to hold, and the
// host put a sign of its own down in every map missing from that file just to
// measure it -- which is the summon sign that turned up under the host's feet in
// Majula (12.09). Where the game can answer, neither happens.
bool GetLocalMapOrigin(uint32_t* area, float* x, float* y, float* z) {
    const uint32_t here = g_localAreaId.load();
    if (!here || !area || !x || !y || !z) return false;
    MapOrigin origin{};
    if (!LookupMapOrigin(here, &origin)) return false;
    *area = here;
    *x = origin.x; *y = origin.y; *z = origin.z;
    return true;
}
}

// Write the sign out at the other player's position instead of my own.
//
// Done when the sign is created, not when it is received. Relocating on receipt
// worked from the placer's *current* position, and the sign list only refreshes
// about once a minute, so the sign missed by however far the placer had wandered
// meanwhile — measured at 3.05 across, 0.99 along and 0.18 up, matching the "two
// or three metres off" that came back from testing. At creation the placer's
// position is a fact, not an estimate.
//
// Two cases, because the encoding is 32 * (world - origin) with a per-map origin:
//
//   same map      the origins cancel, so the difference of the two positions is
//                 enough and nothing else needs to be known
//   another map   they do not cancel; the target map's origin is required, and
//                 it is only available if a sign has been placed there before.
//                 Without it the sign is left alone rather than thrown three
//                 hundred metres out, which is what the difference produced.
static void AimSignAtOtherPlayer(uint8_t* data, size_t len) {
    if (!data || len < 16) return;

    float mx = 0, my = 0, mz = 0, mrot = 0;
    if (!DS2Coop::Sync::GetLocalPlayerPosition(mx, my, mz, mrot)) return;

    // Find the position bytes first: player_struct is 80 bytes and starts
    // 06 00 00 00, so "tag, length 0x50, 06 00 00 00" identifies it without
    // walking the protobuf.
    uint8_t* pos = nullptr;
    for (size_t i = 0; i + 14 <= len; i++) {
        if ((data[i] != 0x1A && data[i] != 0x22) || data[i + 1] != 0x50) continue;
        if (data[i + 2] != 0x06 || data[i + 3] || data[i + 4] || data[i + 5]) continue;
        pos = data + i + 6;
        break;
    }
    if (!pos) {
        LOG_WARNING("[SIGN] no player_struct in the outgoing sign — not aimed");
        return;
    }

    int16_t sx = 0, sy = 0, sz = 0;
    memcpy(&sx, pos + 0, 2);
    memcpy(&sy, pos + 2, 2);
    memcpy(&sz, pos + 4, 2);

    constexpr float kScale = 32.0f;

    // This sign can teach us where this map's origin is -- but only where the
    // game cannot say it itself. Measuring it from a sign whose spot the mod had
    // to invent produced the player's own position instead of an origin, and
    // that wrong number is what kept summoning the guest off the map in Majula.
    // A sign the game wrote into another map (field 1 of the message) measures
    // nothing about the map this player stands in.
    const uint32_t myArea = g_localAreaId.load();
    const uint32_t signArea = ReadSignArea(data, len);
    if (myArea && signArea && signArea != myArea) {
        LOG_INFO("[SIGN] the game wrote this sign into map %u, not map %u where I stand -- no origin measured",
                 signArea, myArea);
    } else if (myArea) {
        const MapOrigin o{ mx - sx / kScale, my - sy / kScale, mz - sz / kScale };
        CheckGameOriginOnce();
        CompareGameOriginWithSign(myArea, o);
        MapOrigin fromGame{};
        const bool gameKnows = g_gameOriginTrust.load() == 1 && QueryGameMapOrigin(myArea, &fromGame);
        if (!gameKnows) {
            std::lock_guard<std::mutex> lock(g_originMutex);
            LoadMapOrigins();
            auto it = g_mapOrigins.find(myArea);
            const bool isNew = (it == g_mapOrigins.end());
            g_mapOrigins[myArea] = o;
            g_originMeasuredHere[myArea] = true;   // measured in this map, in this run
            if (isNew) LOG_INFO("[SIGN] map %u origin learned from my own sign: (%.2f, %.2f, %.2f)",
                                myArea, o.x, o.y, o.z);
            SaveMapOrigins();
        }
    }

    // A sign put down only to measure the map is nobody's to summon: aimed at the
    // other player it would stand under their feet instead (0.2.2 point 10).
    if (g_signIsProbe.load()) {
        LOG_INFO("[SIGN] a sign to measure the map with -- not aimed at anyone, taken down once the server has it");
        return;
    }

    if (!g_signUnderFeet.load()) return;

    auto& sm = DS2Coop::Session::SessionManager::GetInstance();
    // The id, not GetLocalPlayer() (a pointer into a list the network thread changes).
    const uint64_t localId = DS2Coop::Network::PeerManager::GetInstance().GetLocalPlayerId();
    float tx = 0, ty = 0, tz = 0;
    uint32_t targetArea = 0;
    bool haveTarget = false;
    for (const auto& p : sm.GetPlayers()) {
        if (p.playerId == localId) continue;
        tx = p.x; ty = p.y; tz = p.z; targetArea = p.onlineAreaId; haveTarget = true;
        break;
    }
    if (!haveTarget) {
        LOG_INFO("[SIGN] nobody else in the session — sign stays where I placed it");
        return;
    }

    // A one-off spot (a bonfire to come back to after a death) replaces their
    // feet when it lies in the map they are in now; in any other map it would be
    // read against the wrong origin.
    {
        std::lock_guard<std::mutex> lock(g_signTargetMutex);
        if (g_signTarget.Set) {
            g_signTarget.Set = false;
            if (g_signTarget.Area && g_signTarget.Area == targetArea) {
                tx = g_signTarget.X; ty = g_signTarget.Y; tz = g_signTarget.Z;
                LOG_INFO("[SIGN] aiming at the respawn spot (%.2f, %.2f, %.2f) instead of their feet", tx, ty, tz);
            } else {
                LOG_INFO("[SIGN] the respawn spot is in map %u and they are in map %u -- under their feet instead",
                         g_signTarget.Area, targetArea);
            }
        }
    }

    int nxi = 0, nyi = 0, nzi = 0;

    if (targetArea && myArea && targetArea != myArea) {
        MapOrigin origin{};
        const bool known = LookupMapOrigin(targetArea, &origin);
        if (!known) {
            LOG_INFO("[SIGN] they are in map %u and I have never placed a sign there, so its "
                     "origin is unknown — sign stays where I placed it", targetArea);
            return;
        }
        nxi = static_cast<int>(lroundf(kScale * (tx - origin.x)));
        nyi = static_cast<int>(lroundf(kScale * (ty - origin.y)));
        nzi = static_cast<int>(lroundf(kScale * (tz - origin.z)));
        LOG_INFO("[SIGN] aiming into map %u using its origin (%.2f,%.2f,%.2f)",
                 targetArea, origin.x, origin.y, origin.z);
    } else {
        // Same map: origins cancel.
        const float dxw = tx - mx, dyw = ty - my, dzw = tz - mz;
        const float distSq = dxw * dxw + dyw * dyw + dzw * dzw;
        constexpr float kMaxMove = 500.0f;
        if (!(distSq == distSq) || distSq > kMaxMove * kMaxMove) {
            LOG_WARNING("[SIGN] other player reads as (%.2f,%.2f,%.2f), %.0f away — leaving the sign put",
                        tx, ty, tz, sqrtf(distSq));
            return;
        }
        nxi = sx + static_cast<int>(lroundf(kScale * dxw));
        nyi = sy + static_cast<int>(lroundf(kScale * dyw));
        nzi = sz + static_cast<int>(lroundf(kScale * dzw));
    }

    auto clamp16 = [](int v) -> int16_t {
        if (v >  32767) return  32767;
        if (v < -32768) return -32768;
        return static_cast<int16_t>(v);
    };
    const int16_t nx = clamp16(nxi), ny = clamp16(nyi), nz = clamp16(nzi);
    if (nxi != nx || nyi != ny || nzi != nz) {
        LOG_WARNING("[SIGN] aimed position does not fit the 16-bit field — leaving the sign put");
        return;
    }

    memcpy(pos + 0, &nx, 2);
    memcpy(pos + 2, &ny, 2);
    memcpy(pos + 4, &nz, 2);

    LOG_INFO("[SIGN] aimed (%d,%d,%d) -> (%d,%d,%d)  me(%.2f,%.2f,%.2f) map %u | them(%.2f,%.2f,%.2f) map %u",
             sx, sy, sz, nx, ny, nz, mx, my, mz, myArea, tx, ty, tz, targetArea);
}


// ============================================================================
// HOOKED: ParseFromArray
//
// Every incoming protobuf message passes through this function.
// We log interesting messages for debugging and event detection.
// ============================================================================
static bool __fastcall ParseHook(void* thisPtr, void* data, int size) {
    // Where does a received sign go?
    //
    // The server offers the sign, the message arrives, and in Majula nothing is
    // drawn — so something between parsing and spawning throws it away. There is
    // no refusal message to trace (the soapstone is simply greyed out there, and
    // the message probe caught nothing), so take the stack instead: the frames
    // above this parse are the code that asked for the list and will handle it,
    // and the region test lives inside them.
    //
    // Captured once for a response that carries signs and once for an empty one,
    // because the difference between those two runs is the test worth patching.
    {
        const char* cn = GetRttiClassName(thisPtr);
        if (cn && strstr(cn, "GetSignListResponse")) {
            g_signListResponses.fetch_add(1);
            static bool loggedFull = false, loggedEmpty = false;
            const bool empty = (size < 8);
            if ((empty && !loggedEmpty) || (!empty && !loggedFull)) {
                (empty ? loggedEmpty : loggedFull) = true;
                void* callers[16] = {};
                USHORT frames = CaptureStackBackTrace(0, 16, callers, nullptr);
                uintptr_t exeBase = (uintptr_t)GetModuleHandle(nullptr);
                LOG_INFO("[SIGNRESP] %s response, %d bytes, map %u:",
                         empty ? "empty" : "WITH SIGNS", size, g_localAreaId.load());
                for (int i = 0; i < frames && i < 16; i++) {
                    LOG_INFO("[SIGNRESP]   [%d] exe+0x%llX", i,
                             (unsigned long long)((uintptr_t)callers[i] - exeBase));
                }
            }
        }
    }

    // Call original first so the object is populated
    bool result = g_originalParse(thisPtr, data, size);

    if (result) {
        const char* className = GetRttiClassName(thisPtr);

        // Anything the server itself sends counts as the line being alive.
        if (strstr(className, "@Frpg2RequestMessage@@")) NoteServerMessage();

        // Log ALL session-related incoming messages (INFO level for debugging)
        if (strstr(className, "Session") || strstr(className, "Guest") ||
            strstr(className, "Sign") || strstr(className, "BreakIn") ||
            strstr(className, "Summon") || strstr(className, "Push") ||
            strstr(className, "Join") || strstr(className, "Leave") ||
            strstr(className, "Phantom") || strstr(className, "Remove")) {
            LOG_INFO("[PROTOBUF <<] %s (size: %d)", className, size);
        }

        // If we receive a disconnect push from the server while seamless is active,
        // return false so the game thinks the parse failed and ignores it.
        if (g_seamlessActive.load() && IsIncomingDisconnect(className)) {
            LOG_INFO("[SEAMLESS] BLOCKED incoming disconnect from server: %s", className);
            g_blockedCount++;
            return false;
        }

        // Sign filtering disabled — we're on a private server, no randoms.
        // The old filter rejected entire SignList responses if ANY sign
        // contained a Steam ID not in the whitelist, breaking sign visibility.

        // Detect phantom joining/leaving world via DS2 soapstone summon.
        // Handled in helper functions to avoid C2712 (__try + C++ objects).
        if (strstr(className, "NotifyJoinGuestPlayer"))
            OnPhantomJoined();
        if (strstr(className, "NotifyLeaveGuestPlayer") || strstr(className, "LeaveGuestPlayer"))
            OnPhantomLeft();
    }

    return result;
}

// ============================================================================
// Installation
// ============================================================================
bool ProtobufHooks::InstallHooks() {
    LOG_INFO("==========================================");
    LOG_INFO("Installing Protobuf Interception Hooks...");
    LOG_INFO("==========================================");

    int hooked = 0;

    // Find SerializeWithCachedSizesToArray via AOB scan
    LOG_INFO("Scanning for SerializeWithCachedSizesToArray...");
    uintptr_t serializeAddr = PatternScanner::FindPattern(
        ProtobufPatterns::SERIALIZE_PATTERN,
        ProtobufPatterns::SERIALIZE_MASK,
        nullptr
    );

    if (serializeAddr) {
        LOG_INFO("  Found at: 0x%p", reinterpret_cast<void*>(serializeAddr));
        if (HookManager::GetInstance().InstallHook(
            reinterpret_cast<void*>(serializeAddr),
            reinterpret_cast<void*>(&SerializeHook),
            reinterpret_cast<void**>(&g_originalSerialize)
        )) {
            LOG_INFO("  HOOKED SerializeWithCachedSizesToArray");
            hooked++;
        } else {
            LOG_ERROR("  Failed to hook SerializeWithCachedSizesToArray");
        }
    } else {
        LOG_ERROR("  SerializeWithCachedSizesToArray pattern NOT FOUND");
        LOG_ERROR("  This is the critical hook - seamless co-op cannot work without it.");
        LOG_ERROR("  Your game version may have a different protobuf implementation.");
    }

    // Find ParseFromArray via AOB scan
    LOG_INFO("Scanning for ParseFromArray...");
    uintptr_t parseAddr = PatternScanner::FindPattern(
        ProtobufPatterns::PARSE_PATTERN,
        ProtobufPatterns::PARSE_MASK,
        nullptr
    );

    if (parseAddr) {
        LOG_INFO("  Found at: 0x%p", reinterpret_cast<void*>(parseAddr));
        if (HookManager::GetInstance().InstallHook(
            reinterpret_cast<void*>(parseAddr),
            reinterpret_cast<void*>(&ParseHook),
            reinterpret_cast<void**>(&g_originalParse)
        )) {
            LOG_INFO("  HOOKED ParseFromArray");
            hooked++;
        } else {
            LOG_ERROR("  Failed to hook ParseFromArray");
        }
    } else {
        LOG_WARNING("  ParseFromArray pattern NOT FOUND (non-critical, logging only)");
    }

    LOG_INFO("==========================================");
    LOG_INFO("Protobuf Hooks Result: %d/2 installed", hooked);
    LOG_INFO("==========================================");

    if (hooked >= 1) {
        LOG_INFO("Protobuf interception active.");
        if (hooked == 1) {
            LOG_WARNING("Only serialize hook installed. Incoming message logging unavailable.");
        }
        return true;
    }

    LOG_ERROR("No protobuf hooks installed. Seamless co-op will NOT work.");
    LOG_ERROR("The mod will still load but cannot prevent disconnections.");
    return false;
}

void ProtobufHooks::UninstallHooks() {
    LOG_INFO("Uninstalling protobuf hooks...");
    g_seamlessActive = false;
}

void ProtobufHooks::SetSeamlessActive(bool active) {
    bool prev = g_seamlessActive.exchange(active);
    if (prev != active) {
        LOG_INFO("[SEAMLESS] Disconnect blocking %s", active ? "ENABLED" : "DISABLED");
    }
}

bool ProtobufHooks::IsSeamlessActive() {
    return g_seamlessActive.load();
}

uint32_t ProtobufHooks::GetBlockedMessageCount() {
    return g_blockedCount.load();
}

uint32_t ProtobufHooks::GetTotalMessageCount() {
    return g_totalCount.load();
}

void ProtobufHooks::AddSessionSteamId(const std::string& steamId) {
    std::lock_guard<std::mutex> lock(g_steamIdMutex);
    // Don't add duplicates
    for (const auto& s : g_sessionSteamIds) {
        if (s == steamId) return;
    }
    g_sessionSteamIds.push_back(steamId);
    LOG_INFO("[SEAMLESS] Added session Steam ID: %s (total: %zu)", steamId.c_str(), g_sessionSteamIds.size());
}

void ProtobufHooks::ClearSessionSteamIds() {
    std::lock_guard<std::mutex> lock(g_steamIdMutex);
    g_sessionSteamIds.clear();
    g_phantomCounter.store(0);
    g_blockedCount.store(0);
    LOG_INFO("[SEAMLESS] Cleared session Steam ID whitelist");
}

std::string ProtobufHooks::GetLocalSteamId() {
    return GetLocalSteamIdInternal();
}
