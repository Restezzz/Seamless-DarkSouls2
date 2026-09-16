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
constexpr uint32_t kResultSequence = 0x18F9C0;    // (EventResult*, out, arg3, code*, row*): builds the job chain
constexpr uint32_t kBattleStart    = 0x180AF0;    // (boss manager, area index, battle id) -> AL: starts a boss fight
constexpr uint32_t kNetEnemyReset  = 0x517080;    // (NetEnemyManager): the game's own reset of the enemy sync table
constexpr uint32_t kNetEnemyVtable = 0x10FB580;   // NetEnemyManager, *(netRoot)+0x28
constexpr uint32_t kPhantomParam   = 0x16F540;    // (phantom type) -> that type's param row ([GMImp+0x18] lookup)

// The camera (docs §3.23). Offsets, not RVAs: all of them hang off GMImp.
constexpr uint32_t kCameraManager  = 0x20;        // [GMImp+0x20]  CameraManager
constexpr uint32_t kCamActiveKind  = 0x48;        // int: which operator is live (2 = Ingame, 1 = Player)
constexpr uint32_t kCamIngame      = 0x28;        // [CameraManager+0x28]  IngameCameraOperator
constexpr uint32_t kCamFollowed    = 0xF8;        // the character the camera follows
constexpr uint32_t kCamMode        = 0xD0;        // current camera mode (10 = default)
constexpr uint32_t kCamModeWanted  = 0x1520;      // requested camera mode
constexpr uint32_t kCameraCommand  = 0x492080;    // CameraManager::Command(mgr, cmd): id 3 = follow this character

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
using SeqFn     = void*(__fastcall*)(void*, void*, void*, const int*, const uint8_t*);
using ParamRowFn = void*(__fastcall*)(uint32_t);
using CamCmdFn   = void(__fastcall*)(void*, void*);
using BattleStartFn = uint64_t(__fastcall*)(void*, int32_t, int32_t);
using NetEnemyResetFn = void(__fastcall*)(void*);

// Set by MH_CreateHook before the hook goes live, so a detour never sees null.
void* g_phantomBranchOriginal = nullptr;
void* g_hostBranchOriginal    = nullptr;
void* g_lastBonfireOriginal   = nullptr;
void* g_requestWarpOriginal   = nullptr;
void* g_mpNoticeOriginal      = nullptr;
void* g_joinLeaveOriginal     = nullptr;
void* g_resultSeqOriginal     = nullptr;
bool  g_cameraMoved           = false;   // the camera was pointed away from this player
void* g_battleStartOriginal   = nullptr;

std::atomic<bool>      g_enabled{ true };
std::atomic<bool>      g_partnerAlive{ true };     // from the partner's PlayerDeath / PlayerRespawn
std::atomic<ULONGLONG> g_partnerBackAt{ 0 };       // when the partner last got up again
std::atomic<bool>      g_hostTravelPending{ false }; // the host travelled by bonfire (HostTravelled)
std::atomic<int32_t>   g_hostTravelMap{ 0 };
std::atomic<int32_t>   g_hostTravelBonfire{ 0 };
std::atomic<bool>      g_cancelRejoin{ false };    // the player left on purpose
std::atomic<int32_t>   g_partnerBossActive{ 0 };   // the host's boss fight, as it reported it
std::atomic<int32_t>   g_partnerBossPhase{ 0 };
std::atomic<ULONGLONG> g_partnerBossAt{ 0 };
std::atomic<int32_t>   g_partnerBossArea{ -1 };     // the host's battle's event area index
std::atomic<int32_t>   g_partnerBossCount{ 0 };     // the host's participant count
std::atomic<bool>      g_bossSync{ true };          // ini boss_sync
std::atomic<int32_t>   g_guestBattleTriedFor{ 0 };  // one start attempt per host fight
std::atomic<int32_t>   g_guestBattleRunning{ 0 };   // the battle the mod started here

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

uintptr_t BossManagerPtr() {
    uintptr_t Gm = 0, Events = 0, Boss = 0;
    if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0x70, &Events) ||
        !ReadPtr(Events + 0x88, &Boss)) return 0;
    return Boss;
}

// +0x10 the battle's event area index, +0x210 the participant count (one byte):
// what a guest's own game needs from the host to run the same fight (§3.32).
bool ReadBossExtra(int32_t* Area, int32_t* Count) {
    const uintptr_t Boss = BossManagerPtr();
    if (!Boss || !ReadI32(Boss + 0x10, Area)) return false;
    __try {
        *Count = *reinterpret_cast<const uint8_t*>(Boss + 0x210);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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

// Where the partner last said it was standing, and in which map.
bool ReadPartnerSpot(Spot* Out) {
    auto& Players = Session::SessionManager::GetInstance();
    const uint64_t LocalId = Network::PeerManager::GetInstance().GetLocalPlayerId();
    for (const auto& P : Players.GetPlayers()) {
        if (P.playerId == LocalId || !P.onlineAreaId) continue;
        *Out = Spot{ P.onlineAreaId, P.x, P.y, P.z, true };
        return true;
    }
    return false;
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
    bool      WaitHostArrival;    // following a host that travelled: wait until it is somewhere new
    Spot      From;               // where the host was when it travelled
    ULONGLONG HostSettledSince;
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

// Always back at the partner's feet, once the partner is up.
//
// It used to aim at the last bonfire this guest rested at in the host's world
// (else the one beside its arrival point), and on 16.09 that was precisely the
// complaint: after a death the guest came back at "its" bonfire instead of next
// to the host. An invalid target means the sign goes under the partner's feet;
// the rest and arrival spots are still recorded, for the log.
void ArmRejoin(bool OwnDeath, bool AtPartner) {
    Rejoin R{};
    R.Pending = true;
    R.OwnDeath = OwnDeath;
    R.Since = GetTickCount64();
    R.Target = Spot{};
    g_rejoin = R;
    const bool PartnerDown = !g_partnerAlive.load();
    LOG_INFO("[DEATH] after going home: back at the partner's feet%s",
             PartnerDown ? " once they are up (they fell too)"
                         : (AtPartner ? " (after the boss fight)" : ""));
}

void CallPhantomBranch(void* Result, int Reason) {
    reinterpret_cast<BranchFn>(g_phantomBranchOriginal)(Result, Reason);
}

void SendBossState(int32_t Active, int32_t Phase, int32_t AreaIndex, int32_t Participants) {
    Network::BossStatePacket Packet{};
    Packet.header.magic = 0x44533243;
    Packet.header.type = Network::PacketType::BossState;
    Packet.header.size = sizeof(Network::BossStatePacket);
    Packet.active = Active;
    Packet.phase = Phase;
    Packet.areaIndex = AreaIndex;
    Packet.participants = Participants;
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

// The arrival warp of a join, and where it would put this guest.
//
// The request carries the landing position at +0x18/+0x1C/+0x20, in the same
// world coordinates the host reports for itself -- 16.09 measured it on every
// join: each good arrival sat within half a metre of the host, and both bad ones
// were 10.55 m off in X, which is exactly the gap between the origin the host's
// probe sign had learned for map 10300000 and the real one. In Z the gap was
// 135 m: the guest came in under the map, fell to its death (death type 90) and
// was thrown out once it got up again. So a landing far from the host is a sign
// aimed with a wrong origin, and the host's own position is the right answer --
// whatever any map origin says.
constexpr uint32_t kJoinArrivalCaller = 0x2C2E48;
constexpr float    kArrivalFarSq      = 20.0f * 20.0f;

void CorrectArrival(const int32_t* Request, int32_t RawMap) {
    Spot Host{};
    if (!ReadPartnerSpot(&Host)) {
        LOG_INFO("[DEATH] arrival: the host's position is not known here -- landing as the game has it");
        return;
    }
    const uint32_t Area = RawMapToArea(RawMap);
    __try {
        float* Pos = reinterpret_cast<float*>(const_cast<int32_t*>(Request) + 6);
        const float Dx = Pos[0] - Host.X, Dy = Pos[1] - Host.Y, Dz = Pos[2] - Host.Z;
        if (Area != Host.Area) {
            LOG_INFO("[DEATH] arrival at (%.2f, %.2f, %.2f) in map %u while the host reports map %u -- "
                     "not the same map, left alone", Pos[0], Pos[1], Pos[2], Area, Host.Area);
            return;
        }
        if (Dx * Dx + Dy * Dy + Dz * Dz <= kArrivalFarSq) {
            LOG_INFO("[DEATH] arrival at (%.2f, %.2f, %.2f), off the host by (%.2f, %.2f, %.2f) -- as aimed",
                     Pos[0], Pos[1], Pos[2], Dx, Dy, Dz);
            return;
        }
        LOG_WARNING("[DEATH] arrival at (%.2f, %.2f, %.2f) is off the host at (%.2f, %.2f, %.2f) by "
                    "(%.2f, %.2f, %.2f) -- the sign was aimed with a wrong map origin; landing at the host",
                    Pos[0], Pos[1], Pos[2], Host.X, Host.Y, Host.Z, Dx, Dy, Dz);
        Pos[0] = Host.X;
        Pos[1] = Host.Y;
        Pos[2] = Host.Z;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARNING("[DEATH] arrival: the warp request could not be read -- left alone");
    }
}

// A host travelling with a guest still in its session.
//
// The game's network enemy manager keeps raw pointers to the current map's enemy
// statuses from the moment the session gets a second player (exe+0x517BF0), and
// lets go of them only when the guest's last accept controller goes
// (exe+0x2C9BD0 -> exe+0x517080). Vanilla never lets a host travel with a phantom
// in its world; the mod does. On 16.09 the host travelled at 18:46:36, the old
// map's enemy data was freed and its memory reused, and when the guest left at
// 18:47:40 the detach loop exe+0x517E70 set bit 48 of status+0x3C through all
// 255 stale pointers -- one bit flipped in the new map's objects, a
// MapSfxSlotComponent's vtable among them (exe+0x10EB388 became exe+0x10FB388),
// and the save pass jumped into garbage. The host's game crashed.
//
// Running the game's own reset right after the warp is taken -- the map is still
// loaded, so those statuses are still alive -- empties the table exactly as a
// guest's departure would. The detach loop skips null entries and zeroes each
// entry it visits, so the departure that follows writes nothing; the table is
// filled again, for the new map, when the guest joins there.
void ResetEnemySyncForHostWarp() {
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive() || !Lobby.IsHost()) return;
    __try {
        uintptr_t Root = 0, Mgr = 0, Vtbl = 0;
        if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x28, &Mgr) || !ReadPtr(Mgr, &Vtbl)) return;
        if (Vtbl != ExeBase() + kNetEnemyVtable) {
            LOG_WARNING("[DEATH] the enemy sync manager has an unexpected vtable (exe+0x%llX) -- left alone",
                        static_cast<unsigned long long>(Vtbl - ExeBase()));
            return;
        }
        int32_t State = 0;
        if (!ReadI32(Mgr + 8, &State) || State == 0) return;
        reinterpret_cast<NetEnemyResetFn>(ExeBase() + kNetEnemyReset)(reinterpret_cast<void*>(Mgr));
        LOG_INFO("[DEATH] travelling with a guest in the session: emptied the enemy sync table (state %d) "
                 "while this map is still loaded", State);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARNING("[DEATH] emptying the enemy sync table before the warp threw -- left alone");
    }
}

// The host has just travelled by bonfire: tell the guest to follow.
void TellGuestHostTravelled(int32_t RawMap, int32_t Bonfire) {
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive() || !Lobby.IsHost()) return;
    if (Network::PeerManager::GetInstance().GetPeers().empty()) return;
    Network::HostTravelledPacket Packet{};
    Packet.header.magic = 0x44533243;
    Packet.header.type = Network::PacketType::HostTravelled;
    Packet.header.size = sizeof(Packet);
    Packet.header.timestamp = GetTickCount64();
    Packet.map = RawMap;
    Packet.bonfire = Bonfire;
    Network::PeerManager::GetInstance().BroadcastPacket(&Packet.header);
    LOG_INFO("[DEATH] I travelled to bonfire %d (map %u) -- telling the guest to follow", Bonfire,
             RawMapToArea(RawMap));
}

uint64_t __fastcall RequestWarpDetour(void* Gm, const int32_t* Request, uint64_t MpWarp) {
    int32_t F[8] = {};
    const bool Readable = ReadWords(Request, F, 8);
    const uintptr_t Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());

    // A guest may not travel between bonfires on its own inside the host's world.
    //
    // 16.09, three tries, three crashes (18:43:17, 19:30:42, and a process that
    // died mid-line at 19:36:11), each one right after "warp requested: type 3,
    // kind 2 ... multiplayer 0 -> taken" from the travel menu. The guest counts as
    // host-equivalent here, so the game ran the warp the host's way: it told
    // "every guest" to leave -- there are none -- and reloaded the GUEST'S OWN copy
    // of the map while the session still had it in the host's world. Once it did
    // not crash outright, the two ended up in separate copies of Heide's Tower:
    // the host could not see the guest, enemies ignored it and took no damage, and
    // leaving the lobby then brought the host's game down in its item code.
    // Refusing is what the game itself does with a warp it will not take.
    if (Readable && F[0] == 3 && F[1] == 2 && ReadJoinState() == kJoinInWorld) {
        LOG_WARNING("[DEATH] refused: travel to bonfire %d while a guest in the host's world "
                    "(it splits the two worlds and has crashed the game every time) (from exe+0x%llX)",
                    F[6], static_cast<unsigned long long>(Caller - ExeBase()));
        Toast("Only the host can travel between bonfires: for a guest it splits the worlds and crashes the game.",
              "Перемещаться между кострами в мире хоста может только хост: у гостя это разводит миры и роняет игру.",
              UI::NotifyKind::Warning);
        return 0;
    }

    if (Readable && F[0] == 0 && F[1] == 4 && (MpWarp & 0xFF) &&
        Caller - ExeBase() == kJoinArrivalCaller) {
        CorrectArrival(Request, F[2]);
    }

    const uint64_t Result = reinterpret_cast<WarpFn>(g_requestWarpOriginal)(Gm, Request, MpWarp);

    if (Readable && (Result & 0xFF) && F[0] == 3 && F[1] == 2) {
        ResetEnemySyncForHostWarp();
        TellGuestHostTravelled(F[2], F[6]);
    }
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

// The game throwing a guest out of the host's world by itself, as opposed to a
// death (exe+0x2C9246, through the phantom branch) or leaving on purpose.
constexpr uint32_t  kGameEjectCaller = 0x2C385C;
constexpr ULONGLONG kEjectWindowMs   = 5 * 60 * 1000;
constexpr int       kEjectsTolerated = 3;
ULONGLONG           g_ejects[kEjectsTolerated] = {};
std::atomic<bool>   g_leaveLobbyWanted{ false };

// On 16.09 the game threw the guest out through exe+0x2C385C after it had talked
// to the blacksmith, and twice in the very second it arrived -- and every time
// the mod's lobby went on as if nothing had happened: the guest was out of the
// world and still listed in it. Why the game does it is being looked into. Until
// then the guest goes straight back in at the host's feet, and if the game keeps
// throwing it out, the lobby is left for real so both sides agree on who is where.
void NoteGameEject(int Reason) {
    if (!g_enabled.load()) return;
    if (!Session::SessionManager::GetInstance().IsActive() || !PartnerConnected()) return;
    if (g_hold.Active || g_rejoin.Pending) return;   // a death is already being handled
    if (!IsAlive(ReadLocalHp())) return;              // and a dead guest belongs to that path

    const ULONGLONG Now = GetTickCount64();
    int Recent = 0;
    for (ULONGLONG At : g_ejects) {
        if (At && Now - At < kEjectWindowMs) ++Recent;
    }
    if (Recent >= kEjectsTolerated) {
        LOG_WARNING("[DEATH] thrown out of the host's world again (reason %d), %d times in five minutes "
                    "-- leaving the lobby instead of trying again", Reason, Recent + 1);
        Toast("The game keeps throwing you out of the host's world -- leaving the lobby.",
              "Игра раз за разом выкидывает из мира хоста — выхожу из лобби.", UI::NotifyKind::Error);
        g_leaveLobbyWanted.store(true);
        return;
    }
    int Oldest = 0;
    for (int I = 1; I < kEjectsTolerated; ++I) {
        if (g_ejects[I] < g_ejects[Oldest]) Oldest = I;
    }
    g_ejects[Oldest] = Now;
    LOG_INFO("[DEATH] the game threw me out of the host's world (reason %d) -- going straight back in "
             "(%d of %d allowed in five minutes)", Reason, Recent + 1, kEjectsTolerated);
    Toast("Thrown out of the host's world -- going back in...",
          "Выкинуло из мира хоста — захожу обратно…", UI::NotifyKind::Warning);
    ArmRejoin(false, true);
}

void __fastcall JoinLeaveDetour(void* Ctrl, int Reason) {
    const uintptr_t Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    LOG_INFO("[DEATH] join controller asked to leave: reason %d, state %d (from exe+0x%llX)",
             Reason, ReadJoinState(), static_cast<unsigned long long>(Caller - ExeBase()));
    reinterpret_cast<LeaveFn>(g_joinLeaveOriginal)(Ctrl, Reason);
    if (Caller - ExeBase() == kGameEjectCaller) NoteGameEject(Reason);
}

// Read-only probe: what the game is about to do after this death.
//
// The sequence is not hard-coded. exe+0x18F830 looks up a row of 24 bytes by
// "phantom type + code * 100" (falling back to "code * 100 + 99") and this
// function turns that row into a chain of jobs. The constructors name their own
// classes: exe+0x190FA0 is EventResultJob::WaitJob, whose length comes from the
// dying phantom type's param row at +0x28; exe+0x190DF0 is
// ChargeVowContributeJob (row byte 2); exe+0x18F1A0 is the functor job that
// finally hands the phantom to exe+0x2C9220 and sends it home. Two more are
// built from row byte 0, row byte 1 with the dword at +4, and -- through
// exe+0x18FF20 -- one of the dwords at +8 or +0xC with the flag at +0x10.
//
// The banner a guest sees on dying is the MessageInfo job: exe+0x190160 pairs a
// WaitJob with FeFunctorJob<JOB_MEMBER_FUNCTOR_ARG1<EventResult,
// EventResult::MessageInfo, void>> and builds it ONLY when row byte 1 is
// non-zero. Byte 1 also chooses how the text is shown and the dword at +4 is the
// text id: 1 plain text (exe+0x2D6BF0), 2 and 3 text with a player's name
// (exe+0x2D6C50 / exe+0x2D6D30). That one byte is read in exactly one place,
// which is what makes clearing it safe.
//
// The row is still printed on every death: it is what the fix below acts on, and
// nobody has seen one from a live session yet.
void* __fastcall ResultSeqDetour(void* Result, void* Out, void* Arg3, const int* Code, const uint8_t* Row) {
    __try {
        if (Row && Code) {
            static const char Hex[] = "0123456789ABCDEF";
            char Bytes[24 * 3 + 1];
            for (int I = 0; I < 24; ++I) {
                Bytes[I * 3 + 0] = Hex[Row[I] >> 4];
                Bytes[I * 3 + 1] = Hex[Row[I] & 0xF];
                Bytes[I * 3 + 2] = ' ';
            }
            Bytes[24 * 3] = '\0';

            const uint8_t Type = Result
                ? *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(Result) + 0xE0)
                : 0xFF;
            uint32_t Wait = 0;
            void* ParamRow = reinterpret_cast<ParamRowFn>(ExeBase() + kPhantomParam)(Type);
            if (ParamRow) Wait = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(ParamRow) + 0x28);

            LOG_INFO("[DEATH] result sequence: code %d, phantom type %u, wait param %u, row %s",
                     *Code, static_cast<unsigned>(Type), Wait, Bytes);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARNING("[DEATH] reading the result row threw -- leaving it alone");
    }

    // "You were defeated, returning to your world" -- while the mod is keeping
    // this guest in the host's world, that message is simply false (12.09: the
    // guest died at a boss, read it, and stayed where it was). Clearing row byte
    // 1 in a copy leaves out the message job and nothing else: the return home
    // hangs off byte 3, the reward off the dwords at +8/+0xC.
    //
    // Only when the mod really is going to hold, because anywhere else the
    // message is true. The predicate is the one PhantomBranchDetour uses for the
    // same decision -- both run on the same death, so they see the same state.
    uint8_t Copy[24];
    const uint8_t* Use = Row;
    if (Row && Row[1]) {
        const int     Join = ReadJoinState();
        const int32_t Hp   = ReadLocalHp();
        int32_t BossActive = 0, BossPhase = 0;
        ReadBoss(&BossActive, &BossPhase);
        const bool Guest    = Join == kJoinInWorld;
        const bool InBoss   = (BossActive > 0 && BossPhase == 1) || (Guest && PartnerBossFight());
        const bool OwnDeath = IsDead(Hp);
        const bool HoldBack = g_enabled.load() && Guest && InBoss &&
                              (OwnDeath ? g_partnerAlive.load() : IsAlive(Hp));
        if (HoldBack) {
            for (int I = 0; I < 24; ++I) Copy[I] = Row[I];
            Copy[1] = 0;
            Use = Copy;
            LOG_INFO("[DEATH] held in the host's world -- leaving out the game's defeat message "
                     "(row byte 1 was %u)", static_cast<unsigned>(Row[1]));
        }
    }
    return reinterpret_cast<SeqFn>(g_resultSeqOriginal)(Result, Out, Arg3, Code, Use);
}

// Every boss battle start, logged with its caller -- and the one call a guest
// must not get: the game's own script starting a battle the mod already started
// here would find the boss row set and half-reset the fight (exe+0x181D70).
uint64_t __fastcall BattleStartDetour(void* Mgr, int32_t AreaIndex, int32_t BattleId) {
    const uintptr_t Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const int32_t Running = g_guestBattleRunning.load();
    if (Mgr && Running != 0 && Running == BattleId) {
        int32_t Active = 0, Phase = 0;
        if (ReadI32(reinterpret_cast<uintptr_t>(Mgr) + 0x14, &Active) &&
            ReadI32(reinterpret_cast<uintptr_t>(Mgr) + 0x204, &Phase) && Active == BattleId && Phase == 1) {
            LOG_INFO("[BOSS] the game asked to start battle %d again (from exe+0x%llX) -- it already runs here, "
                     "left alone", BattleId, static_cast<unsigned long long>(Caller - ExeBase()));
            return 1;
        }
    }
    const uint64_t R = reinterpret_cast<BattleStartFn>(g_battleStartOriginal)(Mgr, AreaIndex, BattleId);
    LOG_INFO("[BOSS] battle %d in area %d, start asked from exe+0x%llX -> %s", BattleId, AreaIndex,
             static_cast<unsigned long long>(Caller - ExeBase()), (R & 0xFF) ? "yes" : "no");
    return R;
}

bool HookAt(uint32_t Rva, void* Detour, void** Original, const char* What) {
    if (DS2Coop::Hooks::HookManager::GetInstance().InstallHook(
            reinterpret_cast<void*>(ExeBase() + Rva), Detour, Original)) {
        return true;
    }
    LOG_WARNING("[DEATH] could not hook exe+0x%X (%s)", Rva, What);
    return false;
}

// Who the camera is following, and whether the operator that can be retargeted
// is the live one. Reads only -- no game function is called.
//
// Asked for on 12.09: when a guest dies in a boss fight the camera should follow
// whoever is still standing. The game has its own way to do that (command id 3
// to exe+0x492080, which reaches IngameCameraOperator::SetChr), but it only
// bites while the ACTIVE camera kind is 2: kind 1 reads [GMImp+0xD0] for itself
// and would ignore any retarget. Nothing in the disassembly says which kind is
// live during a boss fight, so nothing is written until a real death has printed
// it. The line also settles whether +0xF8 holds a plain character pointer: if it
// equals [GMImp+0xD0] while alive, it does, and the reference helpers are not
// needed at all.
void LogCameraState(const char* When) {
    __try {
        uintptr_t Gm = 0, Mgr = 0, Ingame = 0, Local = 0, Followed = 0;
        if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !Gm) return;
        ReadPtr(Gm + 0xD0, &Local);
        if (!ReadPtr(Gm + kCameraManager, &Mgr) || !Mgr) {
            LOG_INFO("[CAM] %s: no camera manager yet", When);
            return;
        }
        const int Kind = *reinterpret_cast<const int*>(Mgr + kCamActiveKind);
        ReadPtr(Mgr + kCamIngame, &Ingame);
        int Mode = -1, Wanted = -1;
        if (Ingame) {
            ReadPtr(Ingame + kCamFollowed, &Followed);
            Mode   = *reinterpret_cast<const int*>(Ingame + kCamMode);
            Wanted = *reinterpret_cast<const int*>(Ingame + kCamModeWanted);
        }
        LOG_INFO("[CAM] %s: active kind %d (%s), following 0x%llX, local player 0x%llX (%s), mode %d -> %d",
                 When, Kind,
                 Kind == 2 ? "Ingame -- a retarget would bite" : "NOT the Ingame operator",
                 static_cast<unsigned long long>(Followed),
                 static_cast<unsigned long long>(Local),
                 (Followed && Followed == Local) ? "same, so +0xF8 is a plain pointer" : "different",
                 Mode, Wanted);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARNING("[CAM] %s: reading the camera threw -- left alone", When);
    }
}

// Point the camera at a character, the game's own way (docs §3.23).
//
// Command id 3 to exe+0x492080 reaches IngameCameraOperator::SetChr, which is
// the path the game itself uses -- once, at exe+0x1BF536, with the local player.
// Never called with null: SetChr skips its own store for null but still hands it
// to all ten sub-operators, which would leave the operator inconsistent. It only
// bites while the live camera kind is 2, so that is checked here instead of
// assumed, and the reason is written down when it is not.
bool PointCameraAt(uintptr_t Chr, const char* Why, bool Say) {
    if (!Chr) return false;
    __try {
        uintptr_t Gm = 0, Mgr = 0;
        if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !Gm) return false;
        if (!ReadPtr(Gm + kCameraManager, &Mgr) || !Mgr) return false;
        const int Kind = *reinterpret_cast<const int*>(Mgr + kCamActiveKind);
        if (Kind != 2) {
            if (Say) {
                LOG_INFO("[CAM] not moving the camera (%s): the live operator is kind %d, and only kind 2 follows "
                         "a character -- kind 1 reads the local player for itself", Why, Kind);
            }
            return false;
        }
        uint8_t Cmd[0x30] = {};
        *reinterpret_cast<int*>(Cmd) = 3;
        *reinterpret_cast<uintptr_t*>(Cmd + 0x10) = Chr;
        reinterpret_cast<CamCmdFn>(ExeBase() + kCameraCommand)(reinterpret_cast<void*>(Mgr), Cmd);
        if (Say) LOG_INFO("[CAM] camera now follows 0x%llX (%s)", static_cast<unsigned long long>(Chr), Why);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARNING("[CAM] pointing the camera (%s) threw -- left alone", Why);
        return false;
    }
}

// While this player is down and the camera was moved, keep it there. The
// operator binds itself back to the local player whenever its own reference
// resolves to nothing (exe+0x495B60), and whether a death does that is exactly
// what is unknown -- so this re-asks once a second, quietly.
void TickCameraHold(int32_t Hp) {
    if (!g_cameraMoved || Hp == kNoHp || !IsDead(Hp)) return;
    static ULONGLONG s_at = 0;
    const ULONGLONG Now = GetTickCount64();
    if (Now - s_at < 1000) return;
    s_at = Now;
    const uintptr_t Partner = GetPartnerCharacter(5000);
    if (Partner) PointCameraAt(Partner, "still down", false);
}

// --- tick parts ----------------------------------------------------------------
void TickLife(int32_t Hp) {
    if (Hp == kNoHp) return;   // loading: keep the last value
    const bool Died = IsAlive(g_lastHp) && IsDead(Hp);
    const bool Back = IsDead(g_lastHp) && IsAlive(Hp);
    g_lastHp = Hp;
    if (!Died && !Back) return;

    LogCameraState(Died ? "died" : "back up");

    // The camera follows whoever is still standing (asked for on 12.09), and
    // goes back by itself the moment this player is up again. Only in a session,
    // and only while the partner's character has been seen recently -- the mod
    // learns it from the code that draws it (npc_talk.cpp), so it is known
    // exactly while the partner is on screen, which is when this matters.
    if (g_enabled.load() && Session::SessionManager::GetInstance().IsActive()) {
        if (Died) {
            const uintptr_t Partner = GetPartnerCharacter(5000);
            if (Partner) {
                g_cameraMoved = PointCameraAt(Partner, "this player is down", true);
            } else {
                LOG_INFO("[CAM] nobody to follow: the partner's character has not been seen in the last 5 s");
            }
        } else if (g_cameraMoved) {
            uintptr_t Gm = 0, Local = 0;
            if (ReadPtr(ExeBase() + kGameManagerImp, &Gm) && ReadPtr(Gm + 0xD0, &Local) && Local) {
                PointCameraAt(Local, "up again", true);
            }
            g_cameraMoved = false;
        }
    }
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
        int32_t Area = -1, Count = 0;
        ReadBossExtra(&Area, &Count);
        SendBossState(Active, Phase, Area, Count);
    }
}

// The guest's own copy of the host's boss fight.
//
// 16.09, Ancient Dragonslayer: the guest's game never started the battle -- its
// manager read "0 running" through the whole fight while the host's read
// 1031010, phase 1 -- and all three symptoms follow from that one fact. No health
// bar: the bar is made by the battle start, exe+0x180AF0. No damage: while a game
// runs no battle of its own (+0x14 <= 0), exe+0x410280 keeps the boss
// invincible, so the guest's hits died in the damage filter before any team was
// looked at, and were never sent either. And the fog as a wall once the host was
// inside: the door update exe+0x1D1920 turns both prompts off while the
// participant count +0x210 is 0, and the count the host's game should have sent
// never arrived. The start is script-driven (command 0x20469) and the guest's
// script never got there, so the host sends its battle id, area index and count,
// and the guest starts the same battle with the game's own function.
void TickGuestBoss(int Join) {
    if (!g_bossSync.load() || Join != kJoinInWorld) return;
    if (!PartnerBossFight()) {
        g_guestBattleTriedFor.store(0);
        g_guestBattleRunning.store(0);
        return;
    }
    if (!IsAlive(ReadLocalHp())) return;   // a dead guest is a spectator; starting a fight moves the camera
    const uintptr_t Boss = BossManagerPtr();
    if (!Boss) return;
    const int32_t HostBattle = g_partnerBossActive.load();
    const int32_t HostArea   = g_partnerBossArea.load();
    const int32_t HostCount  = g_partnerBossCount.load();
    __try {
        // The fog: at least one participant, or the prompts stay off.
        uint8_t* Count = reinterpret_cast<uint8_t*>(Boss + 0x210);
        if (*Count == 0) {
            *Count = static_cast<uint8_t>(HostCount > 0 && HostCount < 256 ? HostCount : 1);
            LOG_INFO("[BOSS] the host is fighting %d -- participant count here set to %u, so the fog lets me in",
                     HostBattle, static_cast<unsigned>(*Count));
        }

        // The battle: once per host fight, and only from a clean state.
        const int32_t   Active = *reinterpret_cast<const int32_t*>(Boss + 0x14);
        const uintptr_t Row    = *reinterpret_cast<const uintptr_t*>(Boss + 0x18);
        const int32_t   Phase  = *reinterpret_cast<const int32_t*>(Boss + 0x204);
        if (Active != 0 || Phase != 0) return;
        if (g_guestBattleTriedFor.load() == HostBattle) return;
        g_guestBattleTriedFor.store(HostBattle);
        if (Row != 0) {
            LOG_WARNING("[BOSS] not starting battle %d here: a boss row is already set (0x%llX), and starting "
                        "over one half-resets the fight", HostBattle, static_cast<unsigned long long>(Row));
            return;
        }
        if (HostArea < 0 || HostArea > 255) {
            LOG_WARNING("[BOSS] not starting battle %d here: the host sent no usable area index (%d)",
                        HostBattle, HostArea);
            return;
        }
        uintptr_t Gm = 0, Gens = 0, Slot = 0;
        if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0x40, &Gens) ||
            !ReadPtr(Gens + 0x170 + static_cast<uintptr_t>(HostArea) * 8, &Slot)) {
            LOG_WARNING("[BOSS] not starting battle %d here: no enemy generators loaded for area %d",
                        HostBattle, HostArea);
            return;
        }
        const uint64_t Ok = reinterpret_cast<BattleStartFn>(ExeBase() + kBattleStart)(
            reinterpret_cast<void*>(Boss), HostArea, HostBattle);
        const int32_t NowActive = *reinterpret_cast<const int32_t*>(Boss + 0x14);
        const int32_t NowPhase  = *reinterpret_cast<const int32_t*>(Boss + 0x204);
        LOG_INFO("[BOSS] started the host's battle %d in area %d here -> %s (now %d running, phase %d)",
                 HostBattle, HostArea, (Ok & 0xFF) ? "yes" : "no", NowActive, NowPhase);
        if (Ok & 0xFF) g_guestBattleRunning.store(HostBattle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARNING("[BOSS] starting the host's battle here threw -- boss sync is off for the rest of this run");
        g_bossSync.store(false);
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

// The host travelled by bonfire (packet HostTravelled). The game's own answer is
// to throw the guest out -- up to five minutes later: on 16.09 the host travelled
// at 18:38:52, its game sent RequestNotifyLeaveGuestPlayer at 18:40:58, and the
// guest was thrown out at 18:41:01 after two minutes alone in a copy of a world
// the host had left. So the guest follows at once: it leaves that copy and joins
// again where the host went, once the host has actually arrived there.
void TickHostTravel(int Join) {
    if (!g_hostTravelPending.exchange(false)) return;
    const int32_t Bonfire = g_hostTravelBonfire.load();
    if (!g_enabled.load()) return;
    if (Join != kJoinInWorld) {
        LOG_INFO("[DEATH] the host travelled to bonfire %d, and I am not in its world right now -- nothing to follow",
                 Bonfire);
        return;
    }
    if (g_hold.Active) {
        LOG_INFO("[DEATH] the host travelled to bonfire %d while I am held in its boss fight -- the hold decides",
                 Bonfire);
        return;
    }
    Spot From{};
    ReadPartnerSpot(&From);   // still the host's position from before its load
    LOG_INFO("[DEATH] the host travelled to bonfire %d -- following: leaving this copy of its world", Bonfire);
    Toast("The host travelled -- following...", "Хост переместился — иду за ним…", UI::NotifyKind::Player);
    RequestLeaveWorld();
    g_cancelRejoin.store(false);   // this leave starts a follow; it is not a goodbye
    ArmRejoin(false, true);
    g_rejoin.WaitHostArrival = true;
    g_rejoin.From = From;
    g_rejoin.HostSettledSince = 0;
}

void TickRejoin(int Join, int32_t Hp) {
    // Leaving the lobby after the game kept throwing us out -- done here, once
    // the game has finished sending us home, never from inside its own leave.
    if (g_leaveLobbyWanted.load() && Join == -1) {
        g_leaveLobbyWanted.store(false);
        g_rejoin.Pending = false;
        LOG_INFO("[DEATH] home -- leaving the lobby");
        Session::SessionManager::GetInstance().LeaveSession();
        return;
    }
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
    // Following a host that travelled: not before it is somewhere new and has
    // been there a moment, or the sign would be aimed at where it left from.
    if (g_rejoin.WaitHostArrival) {
        Spot Host{};
        const bool Known = ReadPartnerSpot(&Host);
        const float Dx = Host.X - g_rejoin.From.X, Dy = Host.Y - g_rejoin.From.Y, Dz = Host.Z - g_rejoin.From.Z;
        const bool Moved = Known && (!g_rejoin.From.Valid || Host.Area != g_rejoin.From.Area ||
                                     Dx * Dx + Dy * Dy + Dz * Dz > 25.0f);
        if (!Moved) {
            g_rejoin.HostSettledSince = 0;
            return;
        }
        if (!g_rejoin.HostSettledSince) {
            g_rejoin.HostSettledSince = Now;
            return;
        }
        if (Now - g_rejoin.HostSettledSince < kPartnerSettleMs) return;
        g_rejoin.WaitHostArrival = false;
        LOG_INFO("[DEATH] the host is where it travelled (map %u, %.2f, %.2f, %.2f) -- joining it there",
                 Host.Area, Host.X, Host.Y, Host.Z);
    }
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
        HookAt(kResultSequence, reinterpret_cast<void*>(&ResultSeqDetour), &g_resultSeqOriginal, "death result sequence");
        HookAt(kBattleStart, reinterpret_cast<void*>(&BattleStartDetour), &g_battleStartOriginal, "boss battle start");
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
    TickCameraHold(Hp);
    TickBoss(Join);
    TickGuestBoss(Join);
    TickArrival(Join);
    TickTravelList(Join);
    TickBonfireSync(Join);
    TickHold(Join, Hp);
    TickHostTravel(Join);
    TickRejoin(Join, Hp);
}

void NotePartnerLife(bool Alive) {
    const bool Was = g_partnerAlive.exchange(Alive);
    if (Alive && !Was) g_partnerBackAt.store(GetTickCount64());
}

void NoteHostTravelled(int32_t RawMap, int32_t Bonfire) {
    g_hostTravelMap.store(RawMap);
    g_hostTravelBonfire.store(Bonfire);
    g_hostTravelPending.store(true);
    LOG_INFO("[DEATH] the host says it travelled to bonfire %d (map %u)", Bonfire, RawMapToArea(RawMap));
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

void NotePartnerBoss(int32_t Active, int32_t Phase, int32_t AreaIndex, int32_t Participants) {
    const int32_t WasActive = g_partnerBossActive.exchange(Active);
    const int32_t WasPhase = g_partnerBossPhase.exchange(Phase);
    g_partnerBossArea.store(AreaIndex);
    g_partnerBossCount.store(Participants);
    g_partnerBossAt.store(GetTickCount64());
    if (WasActive != Active || WasPhase != Phase) {
        LOG_INFO("[DEATH] the host's boss fight: %d running, phase %d (area %d, %d participant(s))",
                 Active, Phase, AreaIndex, Participants);
    }
}

void SetBossSyncEnabled(bool On) {
    g_bossSync.store(On);
    LOG_INFO("[BOSS] %s", On ? "a guest runs its own copy of the host's boss fight (boss_sync=true)"
                             : "boss fights are left to the game (boss_sync=false)");
}

bool IsHostInBossFight() {
    return PartnerBossFight();
}

void CancelDeathRejoin() {
    g_cancelRejoin.store(true);
    g_forgetSpots.store(true);
}

} // namespace DS2Coop::Sync
