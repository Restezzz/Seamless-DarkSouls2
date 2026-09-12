// Dying in co-op without losing the partner.
//
// Every death -- host, guest, solo -- goes through EventResult: result records
// (1 you died, 4 host died, 8 phantom died, 0x12 boss killed, ...) run a job
// chain (WaitSessionJob exe+0x191210, then waiting for the menus) that ends in a
// callback jumping on the local phantom type ([EventResult+0xE0]) to one of two
// branches, both (EventResult*, reason):
//
//   exe+0x190920  host / solo: exe+0x44FDE0 -> warp to the last bonfire
//   exe+0x190950  phantom:     exe+0x2C9220(mp, reason) -> the join controller
//                              leaves (state 8) and warps home
//
// A respawn is always a full reload (GMImp::RequestWarp, vtable+0x40 =
// exe+0x1C2A80); the game has no revive in place. The host's respawn waits in
// WaitSessionJob until the session is over, and the host's warp drops every
// guest (exe+0x2C7EC0 -> the accept controllers, reason 4).
//
// So nothing in the game brings a guest back inside the host's world. What this
// does instead, with the game's own paths only:
//
//   * the guest goes home the game's way, and the mod joins the host again as
//     soon as the guest stands at home alive and the host is up too -- the sign
//     aimed at the respawn spot: where the guest last rested at a bonfire in the
//     host's world, else beside the bonfire nearest to where the guest first
//     appeared there, else the host's feet. When it was the HOST who died, the
//     spot is where the guest was standing; after a boss fight, and whenever the
//     host was down as well, it is the host's feet once the host is up again.
//   * in a boss fight the phantom branch is held back: a dead guest stays a
//     spectator until the fight is over or the host is dead too, and a guest
//     whose host died fights on until the fight is over or it dies as well.
//     Then the held branch runs and both come back as above. The host needs
//     nothing: its own respawn already waits until the guest has gone.
//
// The guest's own game does not run the host's boss battle (its manager stayed
// at 0 through the whole Last Giant fight of 12.09 while the host's read
// 1010010, phase 1), so the host sends its battle state (BossState) and the
// guest decides on that.
//
// Probes log every respawn branch, every warp request (type, kind, map, id),
// the host's warp notice to its guests, every leave request made of the join
// controller, the boss-fight state and the local player's life.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <intrin.h>

#include "../../include/sync.h"
#include "../../include/hooks.h"
#include "../../include/network.h"
#include "../../include/session.h"
#include "../../include/ui.h"
#include "../../include/ui_settings.h"
#include "../../include/utils.h"

#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdint>

#pragma intrinsic(_ReturnAddress)

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

// --- game functions and data (RVA) -------------------------------------------
constexpr uint32_t kGameManagerImp = 0x16148F0;   // *(exe+...) = GameManagerImp
constexpr uint32_t kNetRoot        = 0x1616CF8;   // *(exe+...) = network root
constexpr uint32_t kJoinCtrlVtable = 0x10D7BD8;   // NetSummonJoinMultiplayCtrl
constexpr uint32_t kPhantomBranch  = 0x190950;    // (EventResult*, reason): phantom goes home
constexpr uint32_t kHostBranch     = 0x190920;    // (EventResult*, reason): warp to the last bonfire
constexpr uint32_t kLastBonfire    = 0x44FE30;    // (EventManager*, {map, kind, id}): rest / travel
constexpr uint32_t kRequestWarp    = 0x1C2A80;    // GMImp::RequestWarp(GMImp*, request*, multiplayer warp)
constexpr uint32_t kMpWarpNotice   = 0x2C7EC0;    // (mp, kind): the host's warp, reason 4 to every guest
constexpr uint32_t kJoinLeave      = 0x2C2F20;    // join controller slot A0 (ctrl, reason): leave

constexpr int       kJoinInWorld        = 7;      // join controller state: in the host's world
constexpr ULONGLONG kRejoinSettleMs     = 3000;   // home and alive this long before joining again
constexpr ULONGLONG kPartnerSettleMs    = 4000;   // the partner up this long, so its position is the new one
constexpr ULONGLONG kRejoinGiveUpMs     = 3 * 60 * 1000;
constexpr ULONGLONG kHoldGiveUpMs       = 15 * 60 * 1000;
constexpr ULONGLONG kPartnerBossFreshMs = 12000;  // a BossState from the host counts this long
constexpr ULONGLONG kBossResendMs       = 4000;   // the host repeats a running fight this often
constexpr float     kBesideBonfire      = 1.5f;   // metres from the bonfire, towards the arrival point

// No PlayerCtrl (loading). A dead player's HP goes below zero -- -910 for ee at
// 22:22:57 -- so dead is "<= 0", and "none" needs a value no HP can take.
constexpr int32_t kNoHp = INT32_MIN;

using BranchFn  = void(__fastcall*)(void*, int);
using BonfireFn = void(__fastcall*)(void*, const int32_t*);
using WarpFn    = uint64_t(__fastcall*)(void*, const int32_t*, uint64_t);
using NoticeFn  = void(__fastcall*)(void*, int);
using LeaveFn   = void(__fastcall*)(void*, int);

// Set by MH_CreateHook before the hook goes live, so a detour never sees null.
void* g_phantomBranchOriginal = nullptr;
void* g_hostBranchOriginal    = nullptr;
void* g_lastBonfireOriginal   = nullptr;
void* g_requestWarpOriginal   = nullptr;
void* g_mpNoticeOriginal      = nullptr;
void* g_joinLeaveOriginal     = nullptr;

std::atomic<bool>      g_enabled{ true };
std::atomic<bool>      g_partnerAlive{ true };     // from the partner's PlayerDeath / PlayerRespawn
std::atomic<ULONGLONG> g_partnerBackAt{ 0 };       // when the partner last got up again
std::atomic<bool>      g_cancelRejoin{ false };    // the player left on purpose
std::atomic<int32_t>   g_partnerBossActive{ 0 };   // the host's boss fight, as it reported it
std::atomic<int32_t>   g_partnerBossPhase{ 0 };
std::atomic<ULONGLONG> g_partnerBossAt{ 0 };

// The executable's base never moves: asked for once, not every frame.
uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

bool IsDead(int32_t Hp)  { return Hp != kNoHp && Hp <= 0; }
bool IsAlive(int32_t Hp) { return Hp != kNoHp && Hp > 0; }

// --- reads (SEH, plain data only) --------------------------------------------
bool ReadPtr(uintptr_t Addr, uintptr_t* Out) {
    __try {
        *Out = *reinterpret_cast<const uintptr_t*>(Addr);
        return *Out != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadI32(uintptr_t Addr, int32_t* Out) {
    __try {
        *Out = *reinterpret_cast<const int32_t*>(Addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Join controller state: 7 in the host's world, 8+ on the way out;
// -1 no join controller (at home, or the host), -2 a controller of another kind.
int ReadJoinState() {
    uintptr_t Net = 0, Mp = 0, Ctrl = 0, Vtbl = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Net) || !ReadPtr(Net + 0x18, &Mp)) return -1;
    if (!ReadPtr(Mp + 0x40, &Ctrl)) return -1;
    if (!ReadPtr(Ctrl, &Vtbl) || Vtbl != ExeBase() + kJoinCtrlVtable) return -2;
    int32_t State = -1;
    return ReadI32(Ctrl + 0xF8, &State) ? State : -1;
}

// Local HP (PlayerCtrl+0x168), kNoHp while there is no PlayerCtrl.
int32_t ReadLocalHp() {
    uintptr_t Gm = 0, Player = 0;
    if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0xD0, &Player)) return kNoHp;
    int32_t Hp = kNoHp;
    return ReadI32(Player + 0x168, &Hp) ? Hp : kNoHp;
}

// EventBossBattleManager [[GMImp+0x70]+0x88]: +0x14 the battle running (what
// the game's own IsBossBattle check reads), +0x204 phase (1 fighting,
// 2 defeated, 3 cleanup).
bool ReadBoss(int32_t* Active, int32_t* Phase) {
    uintptr_t Gm = 0, Events = 0, Boss = 0;
    if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0x70, &Events) ||
        !ReadPtr(Events + 0x88, &Boss)) return false;
    return ReadI32(Boss + 0x14, Active) && ReadI32(Boss + 0x204, Phase);
}

bool InBossFight() {
    int32_t Active = 0, Phase = 0;
    return ReadBoss(&Active, &Phase) && Active > 0 && Phase == 1;
}

// The host's fight as the host last reported it.
bool PartnerBossFight() {
    const ULONGLONG At = g_partnerBossAt.load();
    return At && GetTickCount64() - At < kPartnerBossFreshMs &&
           g_partnerBossActive.load() > 0 && g_partnerBossPhase.load() == 1;
}

// A boss fight is on for this player: its own, or -- a guest in the host's
// world -- the host's.
bool BossFightOn(int Join) {
    return InBossFight() || (Join == kJoinInWorld && PartnerBossFight());
}

// The EventResult now in charge ([[[GMImp+0x70]+0x78]+0x10]); a new one is made
// on every map load, so a held one that is no longer current is gone.
uintptr_t ReadCurrentEventResult() {
    uintptr_t Gm = 0, Events = 0, Manager = 0, Result = 0;
    if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0x70, &Events) ||
        !ReadPtr(Events + 0x78, &Manager) || !ReadPtr(Manager + 0x10, &Result)) return 0;
    return Result;
}

// Bonfires of the loaded map: [[GMImp+0x70]+0x58] +8 first component, +0x10
// count, next at component+0x60; position at [component+8]+0x70.
bool FindNearestBonfire(float X, float Y, float Z, float* Out) {
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        const uintptr_t Events = *reinterpret_cast<const uintptr_t*>(Gm + 0x70);
        const uintptr_t List = *reinterpret_cast<const uintptr_t*>(Events + 0x58);
        uintptr_t Node = *reinterpret_cast<const uintptr_t*>(List + 0x08);
        int Count = *reinterpret_cast<const int32_t*>(List + 0x10);
        if (Count < 0 || Count > 512) Count = 512;
        float Best = -1.0f;
        for (int I = 0; I < Count && Node; ++I) {
            const uintptr_t Obj = *reinterpret_cast<const uintptr_t*>(Node + 0x08);
            if (Obj) {
                const float* P = reinterpret_cast<const float*>(Obj + 0x70);
                const float Dx = P[0] - X, Dy = P[1] - Y, Dz = P[2] - Z;
                const float D = Dx * Dx + Dy * Dy + Dz * Dz;
                if (D == D && (Best < 0.0f || D < Best)) {
                    Best = D;
                    Out[0] = P[0]; Out[1] = P[1]; Out[2] = P[2];
                }
            }
            Node = *reinterpret_cast<const uintptr_t*>(Node + 0x60);
        }
        return Best >= 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadWords(const int32_t* From, int32_t* Out, int Count) {
    __try {
        for (int I = 0; I < Count; ++I) Out[I] = From[I];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- where to come back ------------------------------------------------------
struct Spot {
    uint32_t Area;
    float    X, Y, Z;
    bool     Valid;
};

// Maps come in two spellings: the game's own id, as in the last-bonfire record
// (0x0A0A0000, Forest of Fallen Giants), and the online area number the sign
// messages use (10100000) -- the same four bytes, 10 10 00 00.
uint32_t RawMapToArea(int32_t Raw) {
    const uint32_t R = static_cast<uint32_t>(Raw);
    return ((R >> 24) & 0xFF) * 1000000u + ((R >> 16) & 0xFF) * 10000u + ((R >> 8) & 0xFF) * 100u + (R & 0xFF);
}

// The map the partner last reported standing in, 0 if unknown. A guest in the
// host's world sends no sign-list requests, so the map the mod knows for itself
// (GetLocalAreaId) is still its home map there: on 12.09 every spot was stamped
// 10310000 while the host stood in 10100000, and the aim threw them all away.
// Where the guest arrives or stands, the host is.
uint32_t PartnerArea() {
    auto& Players = Session::SessionManager::GetInstance();
    // The id, not GetLocalPlayer(): that hands out a pointer into the player
    // list, which the network thread changes under its lock.
    const uint64_t LocalId = Network::PeerManager::GetInstance().GetLocalPlayerId();
    for (const auto& P : Players.GetPlayers()) {
        if (P.playerId != LocalId && P.onlineAreaId) return P.onlineAreaId;
    }
    return 0;
}

bool ReadLocalSpot(Spot* Out) {
    float X = 0, Y = 0, Z = 0, Rot = 0;
    if (!GetLocalPlayerPosition(X, Y, Z, Rot)) return false;
    uint32_t Area = PartnerArea();
    if (!Area) Area = Hooks::GetLocalAreaId();
    *Out = Spot{ Area, X, Y, Z, true };
    return Out->Area != 0;
}

// Everything below is touched on the game thread only (the detours and the
// tick all run there); other threads talk through the atomics above.
Spot g_restSpot{};      // last rest at a bonfire in the host's world
Spot g_arrivalSpot{};   // beside the bonfire nearest to where we arrived
int  g_lastJoinState = -1;
std::atomic<bool> g_forgetSpots{ false };

struct Hold {
    bool      Active;
    void*     Result;
    int       Reason;
    bool      OwnDeath;
    ULONGLONG Since;
};
Hold g_hold{};

struct Rejoin {
    bool      Pending;
    bool      OwnDeath;
    Spot      Target;
    ULONGLONG Since;
    ULONGLONG AliveSince;
    bool      WaitLogged;
};
Rejoin g_rejoin{};

int32_t   g_lastHp = kNoHp;
int32_t   g_lastBossActive = -100, g_lastBossPhase = -100;
ULONGLONG g_bossSentAt = 0;

bool PartnerConnected() {
    auto& Peers = Network::PeerManager::GetInstance();
    return Peers.IsConnected() && Peers.IsHandshakeConfirmed();
}

void Toast(const char* En, const char* Ru, UI::NotifyKind Kind) {
    UI::Overlay::GetInstance().ShowNotification(UI::Tr(En, Ru), 5.0f, Kind);
}

// AtPartner: come back at the partner's feet once the partner is up -- after a
// boss fight (won, or the host fell and got up at its bonfire) and whenever the
// partner is down right now.
void ArmRejoin(bool OwnDeath, bool AtPartner) {
    Rejoin R{};
    R.Pending = true;
    R.OwnDeath = OwnDeath;
    R.Since = GetTickCount64();
    const bool PartnerDown = !g_partnerAlive.load();
    if (AtPartner || PartnerDown) {
        R.Target = Spot{};
    } else if (OwnDeath) {
        R.Target = g_restSpot.Valid ? g_restSpot : g_arrivalSpot;
    } else if (!ReadLocalSpot(&R.Target)) {
        R.Target = Spot{};
    }
    g_rejoin = R;
    if (AtPartner || PartnerDown) {
        LOG_INFO("[DEATH] after going home: back at the partner's feet once they are up (%s)",
                 PartnerDown ? "they fell too" : "after the boss fight");
    } else if (R.Target.Valid) {
        LOG_INFO("[DEATH] after going home: back to the partner, sign aimed at %s -- map %u (%.2f, %.2f, %.2f)",
                 OwnDeath ? (g_restSpot.Valid ? "the last rest" : "the bonfire by the arrival point")
                          : "where I stood",
                 R.Target.Area, R.Target.X, R.Target.Y, R.Target.Z);
    } else {
        LOG_INFO("[DEATH] after going home: back to the partner, at their feet (no respawn spot known)");
    }
}

void CallPhantomBranch(void* Result, int Reason) {
    reinterpret_cast<BranchFn>(g_phantomBranchOriginal)(Result, Reason);
}

void SendBossState(int32_t Active, int32_t Phase) {
    Network::BossStatePacket Packet{};
    Packet.header.magic = 0x44533243;
    Packet.header.type = Network::PacketType::BossState;
    Packet.header.size = sizeof(Network::BossStatePacket);
    Packet.active = Active;
    Packet.phase = Phase;
    Network::PeerManager::GetInstance().BroadcastPacket(&Packet.header);
}

// --- detours -----------------------------------------------------------------
void __fastcall PhantomBranchDetour(void* Result, int Reason) {
    const int     Join    = ReadJoinState();
    const int32_t Hp      = ReadLocalHp();
    int32_t BossActive = 0, BossPhase = 0;
    ReadBoss(&BossActive, &BossPhase);
    const bool OwnDeath = IsDead(Hp);
    const bool Guest    = Join == kJoinInWorld;
    const bool HostBoss = Guest && PartnerBossFight();
    const bool InBoss   = (BossActive > 0 && BossPhase == 1) || HostBoss;
    const bool Partner  = g_partnerAlive.load();

    // Held: the fight goes on while someone is still standing in it.
    const bool HoldBack = g_enabled.load() && Guest && InBoss && (OwnDeath ? Partner : IsAlive(Hp));
    const bool Back     = g_enabled.load() && Guest && !HoldBack;
    LOG_INFO("[DEATH] phantom branch: reason %d, my HP %d, join state %d, boss here %d (phase %d), host's fight %s, partner %s -> %s",
             Reason, Hp, Join, BossActive, BossPhase, HostBoss ? "on" : "off", Partner ? "alive" : "dead",
             HoldBack ? "HELD until the fight is decided" : Back ? "home, then straight back" : "the game's way");

    if (HoldBack) {
        g_hold = Hold{ true, Result, Reason, OwnDeath, GetTickCount64() };
        if (OwnDeath) {
            Toast("You fell in the boss fight -- you come back when it is over or your partner falls too",
                  "Ты погиб в бою с боссом — вернёшься, когда бой кончится или погибнет и напарник",
                  UI::NotifyKind::Warning);
        } else {
            Toast("Your partner fell -- the fight goes on", "Напарник погиб — бой продолжается",
                  UI::NotifyKind::Warning);
        }
        return;
    }
    if (Back) ArmRejoin(OwnDeath, InBoss);
    CallPhantomBranch(Result, Reason);
}

void __fastcall HostBranchDetour(void* Result, int Reason) {
    LOG_INFO("[DEATH] host branch (respawn at the last bonfire): reason %d, my HP %d, join state %d",
             Reason, ReadLocalHp(), ReadJoinState());
    reinterpret_cast<BranchFn>(g_hostBranchOriginal)(Result, Reason);
}

void __fastcall LastBonfireDetour(void* Events, const int32_t* Record) {
    reinterpret_cast<BonfireFn>(g_lastBonfireOriginal)(Events, Record);
    int32_t Fields[3] = { -1, -1, -1 };
    if (!ReadWords(Record, Fields, 3)) Fields[0] = Fields[1] = Fields[2] = -1;   // never half a record
    if (ReadJoinState() != kJoinInWorld) return;
    Spot Here{};
    if (!ReadLocalSpot(&Here)) return;
    if (Fields[0] > 0) Here.Area = RawMapToArea(Fields[0]);   // the bonfire's own map: no guess needed
    g_restSpot = Here;
    LOG_INFO("[DEATH] rested at bonfire %d (map %d) in the partner's world -- respawn spot map %u (%.2f, %.2f, %.2f)",
             Fields[2], Fields[0], Here.Area, Here.X, Here.Y, Here.Z);
}

uint64_t __fastcall RequestWarpDetour(void* Gm, const int32_t* Request, uint64_t MpWarp) {
    int32_t F[8] = {};
    const bool Readable = ReadWords(Request, F, 8);
    const uintptr_t Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uint64_t Result = reinterpret_cast<WarpFn>(g_requestWarpOriginal)(Gm, Request, MpWarp);
    if (!Readable) {
        LOG_INFO("[DEATH] warp requested (request unreadable), multiplayer %u -> %s (from exe+0x%llX)",
                 static_cast<unsigned>(MpWarp & 0xFF), (Result & 0xFF) ? "taken" : "refused",
                 static_cast<unsigned long long>(Caller - ExeBase()));
        return Result;
    }
    LOG_INFO("[DEATH] warp requested: type %d, kind %d, map %d, fade %d, id %d, multiplayer %u -> %s (from exe+0x%llX)",
             F[0], F[1], F[2], F[5] & 0xFF, F[6], static_cast<unsigned>(MpWarp & 0xFF),
             (Result & 0xFF) ? "taken" : "refused",
             static_cast<unsigned long long>(Caller - ExeBase()));
    return Result;
}

void __fastcall MpWarpNoticeDetour(void* Mp, int Kind) {
    LOG_INFO("[DEATH] my warp: every guest gets reason 4 (warp kind %d)", Kind);
    reinterpret_cast<NoticeFn>(g_mpNoticeOriginal)(Mp, Kind);
}

void __fastcall JoinLeaveDetour(void* Ctrl, int Reason) {
    const uintptr_t Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    LOG_INFO("[DEATH] join controller asked to leave: reason %d, state %d (from exe+0x%llX)",
             Reason, ReadJoinState(), static_cast<unsigned long long>(Caller - ExeBase()));
    reinterpret_cast<LeaveFn>(g_joinLeaveOriginal)(Ctrl, Reason);
}

bool HookAt(uint32_t Rva, void* Detour, void** Original, const char* What) {
    if (DS2Coop::Hooks::HookManager::GetInstance().InstallHook(
            reinterpret_cast<void*>(ExeBase() + Rva), Detour, Original)) {
        return true;
    }
    LOG_WARNING("[DEATH] could not hook exe+0x%X (%s)", Rva, What);
    return false;
}

// --- tick parts ----------------------------------------------------------------
void TickLife(int32_t Hp) {
    if (Hp == kNoHp) return;   // loading: keep the last value
    const bool Died = IsAlive(g_lastHp) && IsDead(Hp);
    const bool Back = IsDead(g_lastHp) && IsAlive(Hp);
    g_lastHp = Hp;
    if (!Died && !Back) return;
    auto& Players = Session::SessionManager::GetInstance();
    // The id, not GetLocalPlayer(): that hands out a pointer into the player
    // list, which the network thread changes under its lock.
    const uint64_t LocalId = Network::PeerManager::GetInstance().GetLocalPlayerId();
    LOG_INFO("[DEATH] I %s (HP %d)", Died ? "died" : "am back", Hp);
    if (!LocalId) return;
    if (Died) Players.NotifyPlayerDeath(LocalId);
    else Players.NotifyPlayerRespawn(LocalId);
}

// Logs the local fight when it changes; the host also tells its guest, on every
// change and again every few seconds while a fight runs.
void TickBoss(int Join) {
    int32_t Active = -1, Phase = -1;
    if (!ReadBoss(&Active, &Phase)) return;
    const bool Changed = Active != g_lastBossActive || Phase != g_lastBossPhase;
    if (Changed) {
        g_lastBossActive = Active;
        g_lastBossPhase = Phase;
        LOG_INFO("[DEATH] boss fight: %d running, phase %d", Active, Phase);
    }
    auto& Lobby = Session::SessionManager::GetInstance();
    if (Join == kJoinInWorld || !Lobby.IsActive() || !Lobby.IsHost()) return;
    const ULONGLONG Now = GetTickCount64();
    if (Changed || (Active > 0 && Now - g_bossSentAt >= kBossResendMs)) {
        g_bossSentAt = Now;
        SendBossState(Active, Phase);
    }
}

void TickArrival(int Join) {
    const bool Arrived = Join == kJoinInWorld && g_lastJoinState != kJoinInWorld;
    g_lastJoinState = Join;
    if (!Arrived) return;
    Spot Here{};
    if (!ReadLocalSpot(&Here)) return;
    if (g_arrivalSpot.Valid && g_arrivalSpot.Area == Here.Area) return;   // same map: keep the first
    float B[3] = {};
    if (!FindNearestBonfire(Here.X, Here.Y, Here.Z, B)) {
        LOG_INFO("[DEATH] arrived in the partner's world (map %u); no bonfire found nearby", Here.Area);
        return;
    }
    // Stand beside it rather than in it, on the side we came from (Y is up).
    const float Dx = Here.X - B[0], Dz = Here.Z - B[2];
    const float Len = sqrtf(Dx * Dx + Dz * Dz);
    if (Len > 0.1f) {
        B[0] += Dx / Len * kBesideBonfire;
        B[2] += Dz / Len * kBesideBonfire;
    }
    g_arrivalSpot = Spot{ Here.Area, B[0], B[1], B[2], true };
    LOG_INFO("[DEATH] arrived in the partner's world at (%.2f, %.2f, %.2f), map %u (mine on record: %u); nearest bonfire spot (%.2f, %.2f, %.2f), %.0f m away",
             Here.X, Here.Y, Here.Z, Here.Area, Hooks::GetLocalAreaId(), B[0], B[1], B[2], Len);
}

// --- probe: the travel list at a bonfire --------------------------------------
// A guest's travel list holds its own bonfires and not the host's (12.09). The
// bonfire manager [[GMImp+0x70]+0x58] keeps, besides the list of loaded bonfire
// objects at +0x08 that FindNearestBonfire walks, an array of 0x18-byte records
// at +0x20 (how many at +0x28), sorted by the u16 bonfire id at +0x00. Each
// record carries two availability bytes -- +0x02 for the set this save owns and
// +0x03 for the set a session hands over -- and the int at +0x44 says which one
// the list reads (0 -> +0x02, 1 -> +0x03; exe+0x17E6F0 reads rec + view + 2,
// bit 0 = available, bits 1-7 = the kindle level).
//
// exe+0x17E890 sets that view and empties the session set on its way through,
// so filling byte +0x03 would be all a mod has to do -- and that byte is never
// written to the save, so a guest's own progress cannot come to harm by it. None
// of which is worth anything until the numbers are in: view 1 with an empty
// session set is the explanation, view 0 means exe+0x17E890 never ran here and
// the answer is a different one. This reads only, a few times per session, for
// both roles, so the two can be held side by side.
constexpr uint32_t kTravelView   = 0x44;
constexpr uint32_t kTravelArray  = 0x20;
constexpr uint32_t kTravelCount  = 0x28;
constexpr uint32_t kTravelStride = 0x18;

struct TravelSummary {
    int32_t  View;
    uint32_t Count;
    uint32_t OwnLit;
    uint32_t SessionLit;
    int      Differs;
    uint16_t DifferId[6];
    uint8_t  DifferOwn[6];
    uint8_t  DifferSession[6];
};

bool ReadTravelList(TravelSummary* Out) {
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        if (!Gm) return false;
        const uintptr_t Events = *reinterpret_cast<const uintptr_t*>(Gm + 0x70);
        if (!Events) return false;
        const uintptr_t List = *reinterpret_cast<const uintptr_t*>(Events + 0x58);
        if (!List) return false;
        Out->View = *reinterpret_cast<const int32_t*>(List + kTravelView);
        Out->Count = *reinterpret_cast<const uint32_t*>(List + kTravelCount);
        const uintptr_t Records = *reinterpret_cast<const uintptr_t*>(List + kTravelArray);
        if (!Records || Out->Count > 4096) return false;
        for (uint32_t I = 0; I < Out->Count; ++I) {
            const uintptr_t Record = Records + I * kTravelStride;
            const uint16_t Id = *reinterpret_cast<const uint16_t*>(Record);
            const uint8_t Own = *reinterpret_cast<const uint8_t*>(Record + 2);
            const uint8_t Session = *reinterpret_cast<const uint8_t*>(Record + 3);
            if (Own & 1) ++Out->OwnLit;
            if (Session & 1) ++Out->SessionLit;
            if ((Own & 1) != (Session & 1) && Out->Differs < 6) {
                Out->DifferId[Out->Differs] = Id;
                Out->DifferOwn[Out->Differs] = Own;
                Out->DifferSession[Out->Differs] = Session;
                ++Out->Differs;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TickTravelList(int Join) {
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive()) return;
    // Game thread only (the mod's tick).
    static ULONGLONG s_nextAt = 0;
    static int       s_dumps = 0;
    if (s_dumps >= 5) return;
    const ULONGLONG Now = GetTickCount64();
    if (!s_nextAt) {
        s_nextAt = Now + 15000;   // let the join settle first
        return;
    }
    if (Now < s_nextAt) return;
    s_nextAt = Now + 60000;
    TravelSummary Summary{};
    if (!ReadTravelList(&Summary)) {
        LOG_INFO("[BONFIRE] the travel list cannot be read yet");
        return;
    }
    ++s_dumps;
    LOG_INFO("[BONFIRE] travel list: view %d (%s), %u records, lit in my own set %u, in the session set %u -- %s",
             Summary.View,
             Summary.View == 0 ? "reads +0x02, my own" : (Summary.View == 1 ? "reads +0x03, the session's" : "unexpected"),
             Summary.Count, Summary.OwnLit, Summary.SessionLit,
             Join == kJoinInWorld ? "I am a guest in the host's world" : "in my own world");
    for (int I = 0; I < Summary.Differs; ++I) {
        LOG_INFO("[BONFIRE]   bonfire %u: my own byte 0x%02X, the session byte 0x%02X",
                 Summary.DifferId[I], Summary.DifferOwn[I], Summary.DifferSession[I]);
    }
}

// --- the other player's bonfires in this player's travel list -----------------
// The game syncs a session's bonfires by itself, but only for the map the
// players are in and at most sixteen of them (exe+0x17E910 packs, exe+0x17EA40
// applies; docs §3.18). Measured on 12.09: the host had five lit and the set the
// guest's list actually reads held two. So each player sends its whole set and
// the other writes it into byte +0x03 of every matching record -- the session's
// set, which the game zeroes by itself whenever it sets the view and never
// writes to the save. A guest's own progress cannot be touched through it.
//
// Written from the game thread only; the packet arrives on the network thread
// and is parked in the buffer below.
constexpr uint32_t kMaxBonfires = 256;

std::mutex        g_partnerBonfireMutex;
uint16_t          g_partnerBonfireId[kMaxBonfires] = {};
uint8_t           g_partnerBonfireFlags[kMaxBonfires] = {};
uint32_t          g_partnerBonfireCount = 0;
std::atomic<bool> g_partnerBonfiresNew{ false };

// This player's own lit bonfires, out of byte +0x02 of every record.
int CollectOwnBonfires(Network::BonfireEntry* Out, uint32_t Max) {
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        if (!Gm) return -1;
        const uintptr_t Events = *reinterpret_cast<const uintptr_t*>(Gm + 0x70);
        if (!Events) return -1;
        const uintptr_t List = *reinterpret_cast<const uintptr_t*>(Events + 0x58);
        if (!List) return -1;
        const uint32_t Records = *reinterpret_cast<const uint32_t*>(List + kTravelCount);
        const uintptr_t Array = *reinterpret_cast<const uintptr_t*>(List + kTravelArray);
        if (!Array || Records > 4096) return -1;
        uint32_t Found = 0;
        for (uint32_t I = 0; I < Records && Found < Max; ++I) {
            const uintptr_t Record = Array + I * kTravelStride;
            const uint8_t Own = *reinterpret_cast<const uint8_t*>(Record + 2);
            if ((Own & 1) == 0) continue;
            Out[Found].id = *reinterpret_cast<const uint16_t*>(Record);
            Out[Found].flags = Own;
            ++Found;
        }
        return static_cast<int>(Found);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The other player's set into byte +0x03. Returns how many bytes changed.
//
// With progress sharing on it also lights those bonfires in byte +0x02 -- this
// player's own set, the one that is saved. The session byte alone was not
// enough: measured 12.09, the host's five bonfires all reached the guest and
// bonfire 10670 sat there with its session byte 0x01, and it still was not in
// the guest's travel menu. The menu reads more than that byte; owning the
// bonfire outright is what the menu cannot argue with. Only the lit bit is set,
// never the kindle level above it.
int WriteSessionBonfires(const uint16_t* Ids, const uint8_t* Flags, uint32_t Count,
                         bool Share, int* Unlocked) {
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        if (!Gm) return -1;
        const uintptr_t Events = *reinterpret_cast<const uintptr_t*>(Gm + 0x70);
        if (!Events) return -1;
        const uintptr_t List = *reinterpret_cast<const uintptr_t*>(Events + 0x58);
        if (!List) return -1;
        const uint32_t Records = *reinterpret_cast<const uint32_t*>(List + kTravelCount);
        const uintptr_t Array = *reinterpret_cast<const uintptr_t*>(List + kTravelArray);
        if (!Array || Records > 4096) return -1;
        int Written = 0;
        for (uint32_t I = 0; I < Records; ++I) {
            const uintptr_t Record = Array + I * kTravelStride;
            const uint16_t Id = *reinterpret_cast<const uint16_t*>(Record);
            for (uint32_t K = 0; K < Count; ++K) {
                if (Ids[K] != Id) continue;
                uint8_t* Session = reinterpret_cast<uint8_t*>(Record + 3);
                if (*Session != Flags[K]) {
                    *Session = Flags[K];
                    ++Written;
                }
                if (Share && (Flags[K] & 1)) {
                    uint8_t* Own = reinterpret_cast<uint8_t*>(Record + 2);
                    if (!(*Own & 1)) {
                        *Own |= 1;
                        if (Unlocked) ++(*Unlocked);
                    }
                }
                break;
            }
        }
        return Written;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void SendOwnBonfires() {
    Network::BonfireListPacket Packet{};
    const int Found = CollectOwnBonfires(Packet.entries, kMaxBonfires);
    if (Found <= 0) return;
    Packet.header.magic = 0x44533243;
    Packet.header.type = Network::PacketType::BonfireList;
    Packet.header.size = sizeof(Packet);
    Packet.count = static_cast<uint32_t>(Found);
    Network::PeerManager::GetInstance().BroadcastPacket(&Packet.header);
    static int s_lastSent = -1;
    if (Found != s_lastSent) {
        s_lastSent = Found;
        LOG_INFO("[BONFIRE] telling the other player about %d lit bonfires of mine", Found);
    }
}

// No lock is held while the game's memory is written: the copy is taken first.
void ApplyPartnerBonfires() {
    uint16_t Ids[kMaxBonfires] = {};
    uint8_t  Flags[kMaxBonfires] = {};
    uint32_t Count = 0;
    {
        std::lock_guard<std::mutex> Lock(g_partnerBonfireMutex);
        Count = g_partnerBonfireCount;
        for (uint32_t I = 0; I < Count; ++I) {
            Ids[I] = g_partnerBonfireId[I];
            Flags[I] = g_partnerBonfireFlags[I];
        }
    }
    if (!Count) return;
    const bool Share = IsProgressSharingOn();
    int Unlocked = 0;
    const int Written = WriteSessionBonfires(Ids, Flags, Count, Share, &Unlocked);
    static int s_lastWritten = -2;
    if (Written != s_lastWritten) {
        s_lastWritten = Written;
        if (Written < 0) {
            LOG_WARNING("[BONFIRE] could not write the other player's bonfires into the travel list");
        } else {
            LOG_INFO("[BONFIRE] the other player's %u bonfires are in my travel list (%d record(s) changed)",
                     Count, Written);
        }
    }
    if (Unlocked > 0) {
        LOG_INFO("[BONFIRE] %d of the other player's bonfires lit in my own set as well -- progress sharing is on",
                 Unlocked);
    }
}

// The host sends its set; a guest writes the one it was sent, again and again,
// because the game empties that set itself whenever it decides which one the
// list reads.
void TickBonfireSync(int Join) {
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive()) return;
    const ULONGLONG Now = GetTickCount64();
    static ULONGLONG s_sentAt = 0;
    static ULONGLONG s_appliedAt = 0;
    if (Lobby.IsHost()) {
        if (Now - s_sentAt < 5000) return;
        s_sentAt = Now;
        SendOwnBonfires();
        return;
    }
    if (Join != kJoinInWorld) return;
    if (!g_partnerBonfiresNew.exchange(false) && Now - s_appliedAt < 3000) return;
    s_appliedAt = Now;
    ApplyPartnerBonfires();
}

void TickHold(int Join, int32_t Hp) {
    if (!g_hold.Active) return;
    const ULONGLONG Now = GetTickCount64();
    const char* Why = nullptr;
    if (!BossFightOn(Join))                              Why = "the boss fight is over";
    else if (g_hold.OwnDeath && !g_partnerAlive.load())  Why = "the partner fell too";
    else if (!g_hold.OwnDeath && IsDead(Hp))             Why = "I fell too";
    else if (Join != kJoinInWorld)                        Why = "the session ended";
    else if (!PartnerConnected())                         Why = "the partner is gone";
    else if (Now - g_hold.Since > kHoldGiveUpMs)          Why = "waited 15 minutes";
    if (!Why) return;

    const Hold Held = g_hold;
    g_hold = Hold{};
    const bool Own = Held.OwnDeath || IsDead(Hp);
    // The held branch only runs while its EventResult still exists (a new one
    // is made on every map load) and the session it would leave is still up;
    // otherwise the game has already sent us home by itself.
    const bool Current = reinterpret_cast<uintptr_t>(Held.Result) == ReadCurrentEventResult();
    LOG_INFO("[DEATH] held return released: %s%s", Why,
             Current && Join == kJoinInWorld ? "" : " (the game already took us home)");
    ArmRejoin(Own, true);
    if (Current && Join == kJoinInWorld) CallPhantomBranch(Held.Result, Held.Reason);
}

void TickRejoin(int Join, int32_t Hp) {
    if (g_cancelRejoin.exchange(false) && g_rejoin.Pending) {
        g_rejoin.Pending = false;
        LOG_INFO("[DEATH] left on purpose -- not joining again");
    }
    if (!g_rejoin.Pending) return;
    const ULONGLONG Now = GetTickCount64();
    if (Now - g_rejoin.Since > kRejoinGiveUpMs) {
        g_rejoin.Pending = false;
        LOG_WARNING("[DEATH] no way back in 3 minutes -- not joining again by myself");
        return;
    }
    if (Join != -1 || !IsAlive(Hp)) {   // still on the way home, or not alive yet
        g_rejoin.AliveSince = 0;
        return;
    }
    if (!g_rejoin.AliveSince) {
        g_rejoin.AliveSince = Now;
        return;
    }
    if (Now - g_rejoin.AliveSince < kRejoinSettleMs) return;
    // The partner has to be up, and up long enough for its position to be the
    // new one: on 12.09 the sign was aimed at the host still lying at the boss,
    // a second before the host's respawn arrived.
    if (!g_partnerAlive.load()) {
        if (!g_rejoin.WaitLogged) {
            g_rejoin.WaitLogged = true;
            LOG_INFO("[DEATH] home and alive -- waiting for the partner to get up");
        }
        return;
    }
    const ULONGLONG BackAt = g_partnerBackAt.load();
    if (BackAt && Now - BackAt < kPartnerSettleMs) return;
    g_rejoin.Pending = false;
    if (!PartnerConnected()) {
        LOG_INFO("[DEATH] home -- the partner is gone, staying here");
        return;
    }
    if (g_rejoin.Target.Valid) {
        Hooks::SetNextSignTarget(g_rejoin.Target.Area, g_rejoin.Target.X, g_rejoin.Target.Y, g_rejoin.Target.Z);
    }
    LOG_INFO("[DEATH] home and alive -- joining the partner again");
    Toast("Back to your partner's world...", "Возвращаюсь в мир напарника…", UI::NotifyKind::Player);
    RequestRejoinSignPlacement();
}

} // namespace

bool InstallDeathSync(bool Enabled) {
    static bool Installed = false;
    g_enabled.store(Enabled);
    if (!Installed) {
        Installed = true;
        HookAt(kPhantomBranch, reinterpret_cast<void*>(&PhantomBranchDetour), &g_phantomBranchOriginal, "phantom respawn branch");
        HookAt(kHostBranch, reinterpret_cast<void*>(&HostBranchDetour), &g_hostBranchOriginal, "host respawn branch");
        HookAt(kLastBonfire, reinterpret_cast<void*>(&LastBonfireDetour), &g_lastBonfireOriginal, "last bonfire");
        HookAt(kRequestWarp, reinterpret_cast<void*>(&RequestWarpDetour), &g_requestWarpOriginal, "warp request");
        HookAt(kMpWarpNotice, reinterpret_cast<void*>(&MpWarpNoticeDetour), &g_mpNoticeOriginal, "host warp notice");
        HookAt(kJoinLeave, reinterpret_cast<void*>(&JoinLeaveDetour), &g_joinLeaveOriginal, "join controller leave");
    }
    LOG_INFO("[DEATH] death handling %s", Enabled ? "ON: back in the partner's world after a death, boss fights wait for both"
                                                  : "off (death_respawn=false): the game's own way, probes only");
    return g_phantomBranchOriginal != nullptr;
}

void DeathSyncGameTick() {
    if (g_forgetSpots.exchange(false)) {
        g_restSpot = Spot{};
        g_arrivalSpot = Spot{};
    }
    const int     Join = ReadJoinState();
    const int32_t Hp   = ReadLocalHp();
    TickLife(Hp);
    TickBoss(Join);
    TickArrival(Join);
    TickTravelList(Join);
    TickBonfireSync(Join);
    TickHold(Join, Hp);
    TickRejoin(Join, Hp);
}

void NotePartnerLife(bool Alive) {
    const bool Was = g_partnerAlive.exchange(Alive);
    if (Alive && !Was) g_partnerBackAt.store(GetTickCount64());
}

void NotePartnerBonfires(const void* entries, uint32_t count) {
    if (!entries) return;
    const auto* From = static_cast<const Network::BonfireEntry*>(entries);
    std::lock_guard<std::mutex> Lock(g_partnerBonfireMutex);
    g_partnerBonfireCount = count > kMaxBonfires ? kMaxBonfires : count;
    for (uint32_t I = 0; I < g_partnerBonfireCount; ++I) {
        g_partnerBonfireId[I] = From[I].id;
        g_partnerBonfireFlags[I] = From[I].flags;
    }
    g_partnerBonfiresNew.store(true);
}

void NotePartnerBoss(int32_t Active, int32_t Phase) {
    const int32_t WasActive = g_partnerBossActive.exchange(Active);
    const int32_t WasPhase = g_partnerBossPhase.exchange(Phase);
    g_partnerBossAt.store(GetTickCount64());
    if (WasActive != Active || WasPhase != Phase) {
        LOG_INFO("[DEATH] the host's boss fight: %d running, phase %d", Active, Phase);
    }
}

bool IsHostInBossFight() {
    return PartnerBossFight();
}

void CancelDeathRejoin() {
    g_cancelRejoin.store(true);
    g_forgetSpots.store(true);
}

} // namespace DS2Coop::Sync
