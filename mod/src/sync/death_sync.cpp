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
constexpr uint32_t kAcceptEvent    = 0x2BD0D0;    // NetSummonAcceptMultiplayCtrl slot E0 (ctrl, reason): a host event for one guest
constexpr uint32_t kBossPhaseTwo   = 0x1810E0;    // (boss manager): script command 0x2046A, "the boss is dead" -> phase 2
constexpr uint32_t kBossAbort      = 0x180EC0;    // (boss manager): script command 0x2046C, the fight called off (phase 1 only)
constexpr uint32_t kBonfireView    = 0x17E890;    // (bonfire list): picks the set the list reads, empties the session set

constexpr int kAcceptBossKilled = 1;              // accept controller reason: a boss died in the host's world
constexpr int kBranchDutyDone   = 1;              // phantom branch reason: the boss is dead, duty fulfilled
constexpr uint32_t kResultLeaving  = 0xCE;        // EventResult: a sequence that ends in leaving; new records dropped
constexpr uint32_t kResultFrozen   = 0xCF;        // EventResult: set when such a sequence ends; no updates after
constexpr int32_t  kResultBossKilled = 0x12;      // result code of a boss kill
constexpr int32_t  kResultHostDied   = 4;         // result code "the world's host died"
constexpr int      kLeaveHostDied    = 3;         // the reason that result hands the join controller (exe+0x2C9220)
constexpr ULONGLONG kHostDeadUnseenMs = 3000;     // the host dead this long with no result of the game's own here
constexpr ULONGLONG kStateAliveAfterDeathMs = 3000;   // a state packet this soon after a death packet may be older
constexpr ULONGLONG kHostKillFreshMs  = 30000;    // a kill the host reported counts this long here
constexpr ULONGLONG kHostEndGraceMs   = 5000;     // a fight that ended with no kill heard of: wait this long
constexpr ULONGLONG kHostKillRepeatMs = 10000;    // the host repeats a kill this long after the fight is over

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
constexpr ULONGLONG kJoinFlightMs       = 45000;   // a sign put down for the way back gets this long to become a join
constexpr ULONGLONG kJoinStuckMs        = 120000;  // a join under way this long without arriving is given up
constexpr int       kJoinTriesMax       = 3;
constexpr float     kCameraFollowMaxM   = 30.0f;   // farther than this the death camera stays with the body
constexpr ULONGLONG kHoldGiveUpMs       = 15 * 60 * 1000;
constexpr ULONGLONG kPartnerBossFreshMs = 12000;  // a BossState from the host counts this long
constexpr ULONGLONG kBossResendMs       = 4000;   // the host repeats a running fight this often
constexpr float     kBesideBonfire      = 1.5f;   // metres from the bonfire, towards the arrival point

// No PlayerCtrl (loading). A dead player's HP goes below zero -- -910 for ee at
// 22:22:57 -- so dead is "<= 0", and "none" needs a value no HP can take.
constexpr int32_t kNoHp = INT32_MIN;
constexpr ULONGLONG kPayOutMs = 20000;   // how long a guest's copy may keep the hold while paying out

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
using AcceptEventFn = void(__fastcall*)(void*, int);
using BossMgrFn = void(__fastcall*)(void*);

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
void* g_acceptEventOriginal   = nullptr;

std::atomic<bool>      g_enabled{ true };
std::atomic<bool>      g_partnerAlive{ true };     // from the partner's PlayerDeath / PlayerRespawn / PlayerState
std::atomic<ULONGLONG> g_partnerDiedAt{ 0 };
std::atomic<ULONGLONG> g_partnerBackAt{ 0 };       // when the partner last got up again
std::atomic<bool>      g_hostTravelPending{ false }; // the host travelled by bonfire (HostTravelled)
std::atomic<int32_t>   g_hostTravelMap{ 0 };
std::atomic<int32_t>   g_hostTravelBonfire{ 0 };
std::atomic<bool>      g_cancelRejoin{ false };    // the player left on purpose
std::atomic<bool>      g_flightOpen{ false };      // a sign for the way back is out (JoinFlight below)
std::atomic<bool>      g_hostTravelledInFlight{ false };
std::atomic<int32_t>   g_partnerBossActive{ 0 };   // the host's boss fight, as it reported it
std::atomic<int32_t>   g_partnerBossPhase{ 0 };
std::atomic<ULONGLONG> g_partnerBossAt{ 0 };
std::atomic<int32_t>   g_partnerBossArea{ -1 };     // the host's battle's event area index
std::atomic<int32_t>   g_partnerBossCount{ 0 };     // the host's participant count
std::atomic<bool>      g_bossSync{ true };          // ini boss_sync
std::atomic<int32_t>   g_guestBattleTriedFor{ 0 };  // one start attempt per host fight
std::atomic<int32_t>   g_guestBattleRunning{ 0 };   // the battle the mod started here
std::atomic<int32_t>   g_hostKilledBattle{ 0 };     // guest: the host reported this fight in phase 2 or 3
std::atomic<ULONGLONG> g_hostKilledAt{ 0 };
std::atomic<int32_t>   g_hostEndedBattle{ 0 };      // guest: the host's fight went from running to none
std::atomic<ULONGLONG> g_hostEndedAt{ 0 };
std::atomic<int32_t>   g_guestKilled{ 0 };          // guest: the fight driven to its end here
std::atomic<ULONGLONG> g_guestKilledAt{ 0 };
std::atomic<ULONGLONG> g_hostDeathResultAt{ 0 };    // guest: the game here built a "host died" result
void*                  g_phaseTwoOriginal = nullptr;

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

// A boss fight is on for this player: a host's own, or -- for a guest in the
// host's world -- the host's, as the host reports it. Never the guest's own copy
// of the battle: on 16.09 evening that copy stayed in phase 1 after the boss had
// died on both machines, and a hold keyed to it would never have been released.
bool BossFightOn(int Join) {
    return Join == kJoinInWorld ? PartnerBossFight() : InBossFight();
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
ULONGLONG g_talkFlagsAgainAt = 0;   // the second pass of my own talk flags after an arrival
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

// The way back after a death, from the sign on (17.09, point 10). The rejoin used to
// end the moment its sign went down: at 12:08:44 the guest's sign was out, the host
// summoned at 12:08:45 and travelled at 12:08:47 -- its warp ends every join still on
// the way (the accept controller gets reason 4) -- the sign was removed at 12:08:51,
// and nothing tried again. So the flight lasts until the guest is in the host's world
// (join state 7); a join that appears and vanishes, a host travel in between, or no
// join at all within kJoinFlightMs sends the guest back to waiting -- for the host to
// arrive wherever it went, when it travelled -- up to kJoinTriesMax signs.
struct JoinFlight {
    bool      Active;
    ULONGLONG Since;
    int       Try;
    bool      SawJoin;
    Spot      HostFrom;
};
JoinFlight g_flight{};

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

// A guest that stays after a won boss fight: the "duty fulfilled" result must
// not leave the EventResult marked as leaving. A sequence whose row sends the
// phantom home sets +0xCE when it is built (exe+0x18F9C0); while that is set every
// new record is dropped (exe+0x190410), and when the sequence ends +0xCF is set
// (exe+0x1906B0), which stops the object's update until the next map load
// (exe+0x190480). A guest that stayed and died later would get no death at all.
// ResultSeqDetour keeps the leave out of the row in the first place; this is for
// a branch that still arrives.
void KeepResultAlive(void* Result) {
    __try {
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(Result) + kResultLeaving) = 0;
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(Result) + kResultFrozen) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARNING("[DEATH] could not clear the result's leaving marks");
    }
}

// Staying after a won fight: a guest in the host's world, on its feet, with the
// death handling on. The branch decision and the row fix both ask this.
bool GuestStaysAfterWin(int Join, int32_t Hp) {
    return g_enabled.load() && Join == kJoinInWorld && IsAlive(Hp);
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
    // A guest goes by the host's fight alone -- see BossFightOn: its own copy of
    // the battle can sit in phase 1 after the boss is dead, and on 16.09 evening
    // that turned a won fight into "your partner fell, the fight goes on".
    const bool InBoss   = Guest ? HostBoss : (BossActive > 0 && BossPhase == 1);
    const bool Partner  = g_partnerAlive.load();

    // The boss is dead and this guest is on its feet: reason 1 is the game's "duty
    // fulfilled, back to your world" (16.09: the Dragonrider died at 18:59:45, the
    // guest's result ran at 18:59:52 and this branch sent it home at 19:00:04, so
    // it had to join again). In seamless co-op the partner stays. The rest of that
    // result -- the message, and the reward jobs built from the row's dwords at
    // +8/+0xC -- runs as before; only the trip home is left out.
    const bool Stay     = Reason == kBranchDutyDone && GuestStaysAfterWin(Join, Hp);
    // Held: the fight goes on while someone is still standing in it.
    const bool HoldBack = !Stay && g_enabled.load() && Guest && InBoss && (OwnDeath ? Partner : IsAlive(Hp));
    const bool Back     = !Stay && g_enabled.load() && Guest && !HoldBack;
    LOG_INFO("[DEATH] phantom branch: reason %d, my HP %d, join state %d, boss here %d (phase %d), host's fight %s, partner %s -> %s",
             Reason, Hp, Join, BossActive, BossPhase, HostBoss ? "on" : "off", Partner ? "alive" : "dead",
             Stay ? "STAYS: the boss is dead, the partner's world goes on"
                  : HoldBack ? "HELD until the fight is decided" : Back ? "home, then straight back" : "the game's way");

    if (Stay) {
        // Normally the row reaches the game without its leave and this branch never
        // comes (ResultSeqDetour). If it does, the result was built as leaving:
        // clear that, or the next death of this guest would have no result.
        KeepResultAlive(Result);
        Toast("Boss defeated -- you stay in the host's world", "Босс повержен — остаёшься в мире хоста",
              UI::NotifyKind::Player);
        return;
    }
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
std::atomic<bool>  g_arrivalFollowHost{ true };   // ini arrival_follow_host

int32_t AreaToRawMap(uint32_t Area) {
    return static_cast<int32_t>(((Area / 1000000u) % 100u) << 24 | ((Area / 10000u) % 100u) << 16 |
                                ((Area / 100u) % 100u) << 8 | (Area % 100u));
}

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
            // The host crossed a border between the summon and the arrival (18.09 00:12: the host
            // stepped from Majula into 10310000 and straight back). The arrival names the map the host
            // was in when its descriptor went out; the guest was put into 10310000 at the host's
            // coordinates, fell through (HP 0, death type 90, six seconds after arriving) and was sent
            // home. The host's own report is newer: the map it stands in, and where.
            if (g_arrivalFollowHost.load() && Host.Area >= 10000000u && Host.Area < 70000000u) {
                int32_t* Words = const_cast<int32_t*>(Request);
                const int32_t Raw = AreaToRawMap(Host.Area);
                LOG_WARNING("[DEATH] arrival at (%.2f, %.2f, %.2f) in map %u while the host reports map %u at "
                            "(%.2f, %.2f, %.2f) -- the host crossed a border meanwhile; landing where it stands",
                            Pos[0], Pos[1], Pos[2], Area, Host.Area, Host.X, Host.Y, Host.Z);
                Words[2] = Raw;
                Pos[0] = Host.X;
                Pos[1] = Host.Y;
                Pos[2] = Host.Z;
                return;
            }
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
//
// A table that is armed but not attached yet is reset as well: the game's tick
// (exe+0x5170E0) would attach it on the next frame, to the map this warp is about
// to free. Entries that already point at nothing loaded are zeroed first.
void ResetEnemySyncForHostWarp() {
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive()) return;   // host or guest: a client's table points into the old map just the same
    __try {
        uintptr_t Root = 0, Mgr = 0, Vtbl = 0;
        if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x28, &Mgr) || !ReadPtr(Mgr, &Vtbl)) return;
        if (Vtbl != ExeBase() + kNetEnemyVtable) {
            LOG_WARNING("[DEATH] the enemy sync manager has an unexpected vtable (exe+0x%llX) -- left alone",
                        static_cast<unsigned long long>(Vtbl - ExeBase()));
            return;
        }
        int32_t State = 0;
        if (!ReadI32(Mgr + 8, &State)) return;
        const uint8_t Armed = *reinterpret_cast<const uint8_t*>(Mgr + 0x74);
        if (State == 0 && !Armed) return;
        // The zeroing only ever makes the reset write less; the reset runs whatever it
        // found, because the table must not stay attached to the map this warp frees.
        const int Stale = State != 0 ? ForgetStaleEnemyEntries(Mgr) : 0;
        reinterpret_cast<NetEnemyResetFn>(ExeBase() + kNetEnemyReset)(reinterpret_cast<void*>(Mgr));
        LOG_INFO("[DEATH] travelling in a co-op session: emptied the enemy sync table (state %d, armed %d, "
                 "%d entries into freed memory%s) while this map is still loaded", State, static_cast<int>(Armed),
                 Stale, Stale < 0 ? ", unreadable" : "");
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

// A travel target as a player would say it: the bonfire's own name (the game's text,
// category BonfireName), else the map's, else numbers. Game thread.
std::string TravelPlaceName(int32_t RawMap, int32_t Target, int32_t Type) {
    const std::string Map = GameMapName(RawMap);
    char Buffer[192];
    if (Type == 3) {
        const std::string Bonfire = GameBonfireName(Target);
        if (!Bonfire.empty()) {
            std::snprintf(Buffer, sizeof(Buffer), "%s \xC2\xAB%s\xC2\xBB", UI::Tr("bonfire", "костёр"), Bonfire.c_str());
        } else {
            std::snprintf(Buffer, sizeof(Buffer), "%s %d", UI::Tr("bonfire", "костёр"), Target);
        }
        if (!Map.empty() && Bonfire != Map) {
            const size_t Used = std::strlen(Buffer);
            std::snprintf(Buffer + Used, sizeof(Buffer) - Used, " (%s)", Map.c_str());
        }
    } else if (!Map.empty()) {
        std::snprintf(Buffer, sizeof(Buffer), "%s", Map.c_str());
    } else {
        std::snprintf(Buffer, sizeof(Buffer), "%s %u", UI::Tr("map", "карта"), RawMapToArea(RawMap));
    }
    return Buffer;
}

// Where this player just travelled, for the partner's notification (both roles).
void TellPartnerITravelled(int32_t RawMap, int32_t Target, int32_t Type) {
    if (!Session::SessionManager::GetInstance().IsActive()) return;
    if (Network::PeerManager::GetInstance().GetPeers().empty()) return;
    Network::PlayerTravelledPacket Packet{};
    Packet.header.magic = 0x44533243;
    Packet.header.type = Network::PacketType::PlayerTravelled;
    Packet.header.size = sizeof(Packet);
    Packet.header.timestamp = GetTickCount64();
    Packet.map = RawMap;
    Packet.target = Target;
    Packet.type = Type;
    Network::PeerManager::GetInstance().BroadcastPacket(&Packet.header);
}

// The host's side of "a guest is in my world": an accept controller at state 0x10.
bool GuestFullyInSafe() {
    uintptr_t Root = 0, Mp = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp)) return false;
    __try {
        uintptr_t It  = *reinterpret_cast<const uintptr_t*>(Mp + 0x48);
        uintptr_t End = *reinterpret_cast<const uintptr_t*>(Mp + 0x50);
        for (int Guard = 0; It && It < End && Guard < 16; It += 8, ++Guard) {
            const uintptr_t Ctrl = *reinterpret_cast<const uintptr_t*>(It);
            if (Ctrl && *reinterpret_cast<const int32_t*>(Ctrl + 0x150) == 0x10) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// A guest still on its way into the host's world: an accept controller that has
// not reached state 0x10. A host warp now would make it drop the guest with code 6
// on the spot (accept slot E0, reason 4, when slot 88 says "not fully in").
bool AGuestIsStillJoining(int32_t* StateOut) {
    uintptr_t Root = 0, Mp = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp)) return false;
    __try {
        uintptr_t It  = *reinterpret_cast<const uintptr_t*>(Mp + 0x48);
        uintptr_t End = *reinterpret_cast<const uintptr_t*>(Mp + 0x50);
        for (int Guard = 0; It && It < End && Guard < 16; It += 8, ++Guard) {
            const uintptr_t Ctrl = *reinterpret_cast<const uintptr_t*>(It);
            if (!Ctrl) continue;
            const int32_t State = *reinterpret_cast<const int32_t*>(Ctrl + 0x150);
            if (State >= 1 && State < 0x10) {
                *StateOut = State;
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

uint64_t __fastcall RequestWarpDetour(void* Gm, const int32_t* Request, uint64_t MpWarp) {
    int32_t F[8] = {};
    const bool Readable = ReadWords(Request, F, 8);
    const uintptr_t Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());

    // Travel in a co-op session (docs §3.38). Bonfire travel is type 3 kind 2, the
    // ship at No-man's Wharf type 4 kind 3.
    const bool Travel = Readable && ((F[0] == 3 && F[1] == 2) || (F[0] == 4 && F[1] == 3));
    const bool Guest  = ReadJoinState() == kJoinInWorld;
    auto& Lobby = Session::SessionManager::GetInstance();
    uint64_t UseMp = MpWarp;

    // A guest travels on its own and stays in the host's world. The refusal of
    // 16.09 and the "follow the host" that went with it are gone (asked for on
    // 16.09 evening): the crashes came from what a reload leaves behind -- the
    // enemy sync table pointing into the old map and the partner's avatar gone --
    // and that is handled after the warp and by TravelResyncTick. The load is a
    // multiplayer one, so the world stays the host's (its flags, its rules); loaded
    // as its own, as on 16.09, the guest ended up in a copy of the map of its own.
    if (Travel && Guest && g_enabled.load() && !(MpWarp & 0xFF)) {
        UseMp = (MpWarp & ~static_cast<uint64_t>(0xFF)) | 1;
        LOG_INFO("[TRAVEL] travelling as a guest (bonfire %d, map %u) -- loaded as a multiplayer warp, "
                 "so the world stays the host's", F[6], RawMapToArea(F[2]));
    }
    // A host does not travel while a guest is half-way in: the game would drop it.
    if (Travel && !Guest && g_enabled.load() && Lobby.IsActive() && Lobby.IsHost()) {
        int32_t JoiningState = 0;
        if (AGuestIsStillJoining(&JoiningState)) {
            LOG_WARNING("[TRAVEL] refused: travel while a guest is still joining (accept state 0x%X) -- "
                        "the game would drop it", JoiningState);
            Toast("Wait a moment: your partner is still entering your world.",
                  "Подожди немного: напарник ещё входит в твой мир.", UI::NotifyKind::Warning);
            return 0;
        }
    }

    if (Readable && F[0] == 0 && F[1] == 4 && (MpWarp & 0xFF) &&
        Caller - ExeBase() == kJoinArrivalCaller) {
        CorrectArrival(Request, F[2]);
    }

    const uint64_t Result = reinterpret_cast<WarpFn>(g_requestWarpOriginal)(Gm, Request, UseMp);

    if (Travel && (Result & 0xFF) && Lobby.IsActive()) {
        ResetEnemySyncForHostWarp();
        NoteLocalTravel(F[2]);
        if (!Guest) TellGuestHostTravelled(F[2], F[6]);
        TellPartnerITravelled(F[2], F[6], F[0]);
    }
    if (!Readable) {
        LOG_INFO("[DEATH] warp requested (request unreadable), multiplayer %u -> %s (from exe+0x%llX)",
                 static_cast<unsigned>(MpWarp & 0xFF), (Result & 0xFF) ? "taken" : "refused",
                 static_cast<unsigned long long>(Caller - ExeBase()));
        return Result;
    }
    LOG_INFO("[DEATH] warp requested: type %d, kind %d, map %d, fade %d, id %d, multiplayer %u -> %s (from exe+0x%llX)",
             F[0], F[1], F[2], F[5] & 0xFF, F[6], static_cast<unsigned>(UseMp & 0xFF),
             (Result & 0xFF) ? "taken" : "refused",
             static_cast<unsigned long long>(Caller - ExeBase()));
    return Result;
}

void __fastcall MpWarpNoticeDetour(void* Mp, int Kind) {
    LOG_INFO("[DEATH] my warp: every guest gets reason 4 (warp kind %d)", Kind);
    reinterpret_cast<NoticeFn>(g_mpNoticeOriginal)(Mp, Kind);
}

// An event in the host's world, handed to each guest's accept controller
// (NetSummonAcceptMultiplayCtrl, slot E0) with a reason: 0 from exe+0x2C7B00,
// 1 a boss died, 2 and 3 door crossings, 4 the host's warp, 5 a death record.
//
// Reason 1 is how the game lets phantoms go after a boss. exe+0x181490, cleaning
// up after the kill -- right after the boss reward, exe+0x181850 -- calls
// exe+0x2C7D40, which gives reason 1 to every controller; unless the battle is
// one of the two the game keeps phantoms for (ids at exe+0x157C1E0: 1021021 and
// 1021031), the controller writes code 0xB and state 0x11 and the guest is let
// go. 16.09: the Dragonrider died at 18:59:45, the host sent
// RequestNotifyLeaveGuestPlayer at 19:00:02, the guest went home at 19:00:04.
//
// In seamless co-op the partner stays, so a host in a lobby does not pass reason
// 1 on: the controller is left exactly as those two battles leave it, where the
// same case changes nothing. The guest's own "duty fulfilled" is kept from
// sending it home in PhantomBranchDetour. Other reasons go through untouched;
// all of them are logged, they are rare.
//
// Reason 4 is the host's own warp. For a guest that is fully in (controller state
// 0x10) it only marks the controller ([+0x1B8] |= 0x10), and exe+0x2BE090 drops the
// guest 300 s later (16.09: host warp 18:38:52, guest thrown out 18:41:01). A guest
// now stays when the host travels, so that mark is not made; a guest still on its
// way in cannot get here -- the host's travel is refused while one is (RequestWarpDetour).
constexpr int kAcceptHostWarp = 4;

void __fastcall AcceptEventDetour(void* Ctrl, int Reason) {
    auto& Lobby = Session::SessionManager::GetInstance();
    const bool HostInLobby = g_enabled.load() && Lobby.IsActive() && Lobby.IsHost();
    int32_t State = -1;
    if (Ctrl) ReadI32(reinterpret_cast<uintptr_t>(Ctrl) + 0x150, &State);
    const bool KeepBoss = Reason == kAcceptBossKilled && HostInLobby;
    const bool KeepWarp = Reason == kAcceptHostWarp && HostInLobby && State == 0x10;
    LOG_INFO("[DEATH] a guest's accept controller (state 0x%X) gets reason %d%s", State, Reason,
             KeepBoss ? " (a boss died here) -- not passed on, the guest stays in my world"
             : KeepWarp ? " (my warp) -- not passed on, the guest is not dropped five minutes later" : "");
    if (KeepBoss || KeepWarp) return;
    reinterpret_cast<AcceptEventFn>(g_acceptEventOriginal)(Ctrl, Reason);
}

// Probe (host): every guest's accept controller ([mp+0x48..0x50]), each change of its
// state (+0x150) or code (+0x198), with the multiplayer manager's two busy counters
// ([mp+8], [mp+9]). Its state 10 ends the guest's join with code 6 while [mp+9] is up
// (exe+0x2BF440), and on 17.09 a guest was thrown out on arrival six times with the host's
// controller gone in the same second -- the code it ended with was never logged.
struct AcceptSeen {
    uintptr_t Ctrl;
    int32_t   State;
    int32_t   Code;
};
AcceptSeen g_acceptSeen[4] = {};

int ReadAcceptControllersSafe(uintptr_t* Ctrls, int Max, uint8_t* Busy1, uint8_t* Busy2) {
    int Count = 0;
    __try {
        const uintptr_t Root = *reinterpret_cast<const uintptr_t*>(ExeBase() + kNetRoot);
        const uintptr_t Mp = Root ? *reinterpret_cast<const uintptr_t*>(Root + 0x18) : 0;
        if (!Mp) return 0;
        *Busy1 = *reinterpret_cast<const uint8_t*>(Mp + 8);
        *Busy2 = *reinterpret_cast<const uint8_t*>(Mp + 9);
        uintptr_t It = *reinterpret_cast<const uintptr_t*>(Mp + 0x48);
        const uintptr_t End = *reinterpret_cast<const uintptr_t*>(Mp + 0x50);
        for (; It && It < End && Count < Max; It += 8) {
            const uintptr_t Ctrl = *reinterpret_cast<const uintptr_t*>(It);
            if (Ctrl) Ctrls[Count++] = Ctrl;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return Count;
}

void AcceptProbeTick() {
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive() || !Lobby.IsHost()) return;
    uintptr_t Ctrls[4] = {};
    uint8_t Busy1 = 0, Busy2 = 0;
    const int Count = ReadAcceptControllersSafe(Ctrls, 4, &Busy1, &Busy2);
    AcceptSeen Now[4] = {};
    for (int I = 0; I < Count; ++I) {
        Now[I].Ctrl = Ctrls[I];
        Now[I].State = -1;
        Now[I].Code = -1;
        ReadI32(Ctrls[I] + 0x150, &Now[I].State);
        ReadI32(Ctrls[I] + 0x198, &Now[I].Code);
        const AcceptSeen* Was = nullptr;
        for (const AcceptSeen& S : g_acceptSeen) {
            if (S.Ctrl == Ctrls[I]) Was = &S;
        }
        if (!Was || Was->State != Now[I].State || Was->Code != Now[I].Code) {
            LOG_INFO("[JOIN] a guest's accept controller %p: state 0x%X, code %d (was %s0x%X, %d); busy [mp+8] %u, [mp+9] %u",
                     reinterpret_cast<void*>(Ctrls[I]), Now[I].State, Now[I].Code, Was ? "" : "new, ",
                     Was ? Was->State : 0, Was ? Was->Code : 0, Busy1, Busy2);
        }
    }
    for (const AcceptSeen& S : g_acceptSeen) {
        if (!S.Ctrl) continue;
        bool Still = false;
        for (int I = 0; I < Count; ++I) Still = Still || Ctrls[I] == S.Ctrl;
        if (!Still) {
            LOG_INFO("[JOIN] a guest's accept controller %p is gone (last state 0x%X, code %d); busy [mp+8] %u, [mp+9] %u",
                     reinterpret_cast<void*>(S.Ctrl), S.State, S.Code, Busy1, Busy2);
        }
    }
    for (int I = 0; I < 4; ++I) g_acceptSeen[I] = Now[I];
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

// The numbers behind a leave: the code the join controller keeps at +0x120 (written by
// its event slot, a host's P2P packet 5, or state 6's own check: 0xF), the phantom type
// at +0xD8 and the "mode" byte state 6 checks it against ([[GMImp+0xD0]+0x490]+0x1AD).
struct LeaveNumbers {
    int32_t Code;
    int32_t Type;
    int32_t Mode;
};

LeaveNumbers ReadLeaveNumbersSafe(void* Ctrl) {
    LeaveNumbers N{ -1, -1, -1 };
    __try {
        const uintptr_t C = reinterpret_cast<uintptr_t>(Ctrl);
        N.Code = *reinterpret_cast<const int32_t*>(C + 0x120);
        N.Type = *reinterpret_cast<const uint8_t*>(C + 0xD8);
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        const uintptr_t Player = Gm ? *reinterpret_cast<const uintptr_t*>(Gm + 0xD0) : 0;
        const uintptr_t Param = Player ? *reinterpret_cast<const uintptr_t*>(Player + 0x490) : 0;
        if (Param) N.Mode = *reinterpret_cast<const int8_t*>(Param + 0x1AD);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return N;
}

void __fastcall JoinLeaveDetour(void* Ctrl, int Reason) {
    const uintptr_t Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const LeaveNumbers N = ReadLeaveNumbersSafe(Ctrl);
    LOG_INFO("[DEATH] join controller asked to leave: reason %d, state %d (from exe+0x%llX); code +0x120 = %d, "
             "phantom type %d, mode %d", Reason, ReadJoinState(), static_cast<unsigned long long>(Caller - ExeBase()),
             N.Code, N.Type, N.Mode);
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
    //
    // And only for a death. Row byte 3 is the reason the phantom branch will get
    // (logs of 12.09-16.09: code 1 "you died" -> 2, code 4 "the host died" -> 3,
    // code 3 "duty fulfilled" -> 1). On 16.09 evening a won boss fight had its
    // message taken away because the guest's stale battle copy still read as a
    // fight; the boss being dead is exactly what that message says.
    uint8_t Copy[24];
    const uint8_t* Use = Row;
    if (Code && *Code == kResultHostDied) g_hostDeathResultAt.store(GetTickCount64());

    // A won boss fight while this guest stays: "duty fulfilled" (row byte 3 = 1)
    // and the guest's own boss kill (code 0x12, whose row a guest has never been
    // seen to build) go to the game without their leave. A row with byte 3 set
    // marks the whole EventResult as leaving and, once its sequence ends, frozen
    // until the next map load (KeepResultAlive says why) -- and nobody is leaving.
    // Everything else in the row runs: the message, and the reward jobs the dwords
    // at +8/+0xC build (humanity restored with the flag at +0x10).
    if (Row && Code && Row[3] != 0 && (Row[3] == kBranchDutyDone || *Code == kResultBossKilled) &&
        GuestStaysAfterWin(ReadJoinState(), ReadLocalHp())) {
        for (int I = 0; I < 24; ++I) Copy[I] = Row[I];
        Copy[3] = 0;
        Use = Copy;
        LOG_INFO("[DEATH] a won boss fight while I stay in the host's world -- code %d built without its "
                 "return home (row byte 3 was %u)", *Code, static_cast<unsigned>(Row[3]));
    }

    if (Row && Row[1] && (Row[3] == 2 || Row[3] == 3)) {
        const int     Join = ReadJoinState();
        const int32_t Hp   = ReadLocalHp();
        int32_t BossActive = 0, BossPhase = 0;
        ReadBoss(&BossActive, &BossPhase);
        const bool Guest    = Join == kJoinInWorld;
        const bool InBoss   = Guest ? PartnerBossFight() : (BossActive > 0 && BossPhase == 1);
        const bool OwnDeath = IsDead(Hp);
        const bool HoldBack = g_enabled.load() && Guest && InBoss &&
                              (OwnDeath ? g_partnerAlive.load() : IsAlive(Hp));
        if (HoldBack) {
            // From Use, not Row: a change made above stays made.
            if (Use != Copy) {
                for (int I = 0; I < 24; ++I) Copy[I] = Row[I];
            }
            Copy[1] = 0;
            Use = Copy;
            LOG_INFO("[DEATH] held in the host's world -- leaving out the game's defeat message "
                     "(row byte 1 was %u)", static_cast<unsigned>(Row[1]));
        }
    }
    return reinterpret_cast<SeqFn>(g_resultSeqOriginal)(Result, Out, Arg3, Code, Use);
}

// Phase 2 of a boss fight, "the boss is dead" (exe+0x1810E0, script command
// 0x2046A): logged with its caller on both sides. Refused for a manager with no
// fight (no boss row) or not in phase 1: the function writes phase 2 whatever
// the phase was, so a late call would take a finished fight back to phase 2 and
// post its result -- and its rewards -- a second time. Starts with
// CMP dword [rcx+0x204],2 (83 B9 04 02 00 00 02): seven whole bytes, no RIP use.
void __fastcall BossPhaseTwoDetour(void* Mgr) {
    const uintptr_t Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    int32_t Active = 0, Phase = -1;
    uintptr_t Row = 0;
    const bool Read = Mgr && ReadI32(reinterpret_cast<uintptr_t>(Mgr) + 0x14, &Active) &&
                      ReadI32(reinterpret_cast<uintptr_t>(Mgr) + 0x204, &Phase);
    if (Read) ReadPtr(reinterpret_cast<uintptr_t>(Mgr) + 0x18, &Row);
    if (Read && (Row == 0 || Phase != 1)) {
        LOG_INFO("[BOSS] phase 2 asked for battle %d in phase %d%s (from exe+0x%llX) -- refused", Active, Phase,
                 Row ? "" : " with no boss row", static_cast<unsigned long long>(Caller - ExeBase()));
        return;
    }
    LOG_INFO("[BOSS] battle %d: the boss is dead, phase 2 (from exe+0x%llX)", Active,
             static_cast<unsigned long long>(Caller - ExeBase()));
    reinterpret_cast<BossMgrFn>(g_phaseTwoOriginal)(Mgr);
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
    if (R & 0xFF) NoteBossStartTask(BattleId);   // boss_arena.cpp: so a guest can wake this one as well
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
// A far partner watched while this guest is down: let go as soon as that no longer holds.
void TickFarSpectate(int Join, int32_t Hp) {
    if (!FarSpectating()) return;
    const char* Why = nullptr;
    if (Hp != kNoHp && !IsDead(Hp))            Why = "up again";
    else if (Join != kJoinInWorld)              Why = "out of the host's world";
    else if (!PlayersShareMap())                Why = "the partner is in another map now";
    else if (!GetPartnerCharacter(3000))        Why = "the partner's character is gone";
    if (!Why) return;
    StopFarSpectate(Why);
    uintptr_t Gm = 0, Local = 0;
    if (ReadPtr(ExeBase() + kGameManagerImp, &Gm) && ReadPtr(Gm + 0xD0, &Local) && Local) {
        PointCameraAt(Local, Why, true);
    }
    g_cameraMoved = false;
}

void TickCameraHold(int32_t Hp) {
    if (!g_cameraMoved || Hp == kNoHp || !IsDead(Hp)) return;
    static ULONGLONG s_at = 0;
    const ULONGLONG Now = GetTickCount64();
    if (Now - s_at < 1000) return;
    s_at = Now;
    const uintptr_t Partner = GetPartnerCharacter(5000);
    if (Partner) PointCameraAt(Partner, "still down", false);
}

// Whether the death camera may go to the partner (17.09, points 5 and 15). The
// world around a player is loaded from that player's own position (exe+0x3BE060
// asks [GMImp+0xD0] for it, never the camera), and the death camera (mode 6,
// FallDead, exe+0x4A1E90) moves its eye only for half a second, 10% a frame, in X and
// Z, stopped by collision. A partner far away got a camera stuck part of the way,
// over ground that was never loaded. So the camera moves only to a partner in the
// same map and within kCameraFollowMaxM.
bool PartnerCloseEnoughToWatch(float* Distance) {
    *Distance = -1.0f;
    if (!PlayersShareMap()) return false;
    Spot Partner{};
    if (!ReadPartnerSpot(&Partner)) return false;
    float X = 0, Y = 0, Z = 0, Rot = 0;
    if (!GetLocalPlayerPosition(X, Y, Z, Rot)) return false;
    const float Dx = Partner.X - X, Dy = Partner.Y - Y, Dz = Partner.Z - Z;
    *Distance = std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz);
    return *Distance <= kCameraFollowMaxM;
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
            float Distance = -1.0f;
            const bool Close = Partner && PartnerCloseEnoughToWatch(&Distance);
            if (Partner && !Close && Distance >= 0.0f && ReadJoinState() == kJoinInWorld &&
                StartFarSpectate(Partner)) {
                LOG_INFO("[CAM] the partner is %.0f m away -- while I am down the world is loaded around it, and the "
                         "camera follows it", Distance);
                g_cameraMoved = PointCameraAt(Partner, "this player is down, the partner far away", true);
                if (!g_cameraMoved) StopFarSpectate("the camera could not be pointed at the partner");
            } else if (Partner && !Close) {
                if (Distance < 0.0f) {
                    LOG_INFO("[CAM] the camera stays with me: the partner is in another map or its position is not known");
                } else {
                    LOG_INFO("[CAM] the camera stays with me: the partner is %.0f m away -- the world is loaded around "
                             "me, not around it", Distance);
                }
            } else if (Partner) {
                g_cameraMoved = PointCameraAt(Partner, "this player is down", true);
            } else {
                LOG_INFO("[CAM] nobody to follow: the partner's character has not been seen in the last 5 s");
            }
        } else if (g_cameraMoved) {
            StopFarSpectate("up again");
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
int32_t   g_hostKillBattle      = 0;   // host, game thread: the fight that ended in a kill
ULONGLONG g_hostKillRepeatUntil = 0;
ULONGLONG g_hostKillSentAt      = 0;

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

    // A kill is said again every two seconds for ten after the fight is over.
    // Phases 2 and 3 go out once each, and a guest that heard neither would take
    // "no fight" for a fight called off and end its copy without the reward.
    if (Active > 0 && (Phase == 2 || Phase == 3)) {
        g_hostKillBattle = Active;
        g_hostKillRepeatUntil = 0;
    } else if (Active <= 0 && g_hostKillBattle > 0 && g_hostKillRepeatUntil == 0) {
        g_hostKillRepeatUntil = Now + kHostKillRepeatMs;
    }
    if (g_hostKillRepeatUntil) {
        if (Now >= g_hostKillRepeatUntil) {
            g_hostKillBattle = 0;
            g_hostKillRepeatUntil = 0;
        } else if (Now - g_hostKillSentAt >= 2000) {
            g_hostKillSentAt = Now;
            SendBossState(g_hostKillBattle, 3, -1, 0);
        }
    }
}

// A guest's copy of the host's fight, once the host's has ended.
//
// The phase moves 1 -> 2 in one place only, exe+0x1810E0, and only the boss
// script's command 0x2046A calls it; exe+0x181490 itself never does, whatever the
// slots' HP. The guest's copy was started by the mod, its script never got into
// the fight, and on 16.09 evening the copy sat in phase 1 with an empty bar after
// the boss had died on both machines -- phase 3, where the souls are given to the
// local player (exe+0x181950) and the reward item (exe+0x181850), never came.
// So when the host reports the kill, the same function runs here; when the host's
// fight just stops with no kill heard of, the game's own abort ends the copy.
void TickGuestBossEnd(uintptr_t Boss) {
    if (!Boss) return;
    __try {
        const int32_t   Active = *reinterpret_cast<const int32_t*>(Boss + 0x14);
        const int32_t   Phase  = *reinterpret_cast<const int32_t*>(Boss + 0x204);
        const uintptr_t Row    = *reinterpret_cast<const uintptr_t*>(Boss + 0x18);
        if (Active <= 0 || Phase != 1 || Row == 0) return;
        const ULONGLONG Now = GetTickCount64();

        const ULONGLONG KilledAt = g_hostKilledAt.load();
        if (g_hostKilledBattle.load() == Active && KilledAt && Now - KilledAt < kHostKillFreshMs) {
            const BossMgrFn PhaseTwo = g_phaseTwoOriginal ? reinterpret_cast<BossMgrFn>(g_phaseTwoOriginal)
                                                          : reinterpret_cast<BossMgrFn>(ExeBase() + kBossPhaseTwo);
            PhaseTwo(reinterpret_cast<void*>(Boss));
            const int32_t After = *reinterpret_cast<const int32_t*>(Boss + 0x204);
            g_guestKilled.store(Active);
            g_guestKilledAt.store(Now);
            g_hostEndedBattle.store(0);
            LOG_INFO("[BOSS] the host killed %d -- this copy of the fight goes on to its end here (phase 1 -> %d)",
                     Active, After);
            return;
        }

        const ULONGLONG EndedAt = g_hostEndedAt.load();
        if (g_hostEndedBattle.load() == Active && EndedAt && Now - EndedAt >= kHostEndGraceMs &&
            !PartnerBossFight()) {
            g_hostEndedBattle.store(0);
            reinterpret_cast<BossMgrFn>(ExeBase() + kBossAbort)(reinterpret_cast<void*>(Boss));
            const int32_t NowActive = *reinterpret_cast<const int32_t*>(Boss + 0x14);
            const int32_t NowPhase  = *reinterpret_cast<const int32_t*>(Boss + 0x204);
            LOG_INFO("[BOSS] the host's fight %d ended with no kill -- called off here too (now %d running, phase %d)",
                     Active, NowActive, NowPhase);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARNING("[BOSS] ending the copy of the host's fight here threw -- boss sync is off for the rest of this run");
        g_bossSync.store(false);
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
    TickGuestBossEnd(BossManagerPtr());
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
    // The four values arrive together but are stored one by one on the network
    // thread: a packet saying the fight is over can land between the reads.
    if (HostBattle <= 0) return;
    // A late or repeated "running" for a boss already killed here must not start
    // a fight with a dead boss.
    const ULONGLONG KilledAt = g_guestKilledAt.load();
    if (HostBattle == g_guestKilled.load() && KilledAt && GetTickCount64() - KilledAt < 5 * 60 * 1000) return;
    __try {
        // The fog: at least one participant, or the prompts stay off.
        uint8_t* Count = reinterpret_cast<uint8_t*>(Boss + 0x210);
        if (*Count == 0) {
            *Count = static_cast<uint8_t>(HostCount > 0 && HostCount < 256 ? HostCount : 1);
            // Once per fight in the log: if something here zeroes the count again
            // every frame, this would otherwise write a line every frame.
            static int32_t s_countLoggedFor = 0;
            if (s_countLoggedFor != HostBattle) {
                s_countLoggedFor = HostBattle;
                LOG_INFO("[BOSS] the host is fighting %d -- participant count here set to %u, so the fog lets me in",
                         HostBattle, static_cast<unsigned>(*Count));
            }
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
        // Through the trampoline: the hooked address would run BattleStartDetour's
        // own guard on the mod's call as well.
        const BattleStartFn Start = g_battleStartOriginal
            ? reinterpret_cast<BattleStartFn>(g_battleStartOriginal)
            : reinterpret_cast<BattleStartFn>(ExeBase() + kBattleStart);
        const uint64_t Ok = Start(reinterpret_cast<void*>(Boss), HostArea, HostBattle);
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

// A summon heals: the game hands a phantom full health when it arrives, because the world it comes to
// is not the one it wore itself down in. A seamless join keeps the character exactly as it stood, so on
// 21.09 a guest came in with the sliver of health it had at home, sat at the host's bonfire, and when
// the host fell it was sent home to that same sliver (report 6). The arrival heals now, as the game's
// own summon does; estus and everything else is left alone.
void HealOnArrival() {
    uintptr_t Gm = 0, Player = 0;
    if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0xD0, &Player)) return;
    __try {
        const int32_t Hp = *reinterpret_cast<const int32_t*>(Player + 0x168);
        const int32_t Max = *reinterpret_cast<const int32_t*>(Player + 0x170);
        if (Max <= 0 || Hp <= 0 || Hp >= Max) return;
        *reinterpret_cast<int32_t*>(Player + 0x168) = Max;
        LOG_INFO("[JOIN] arrived in the partner's world with %d of %d HP -- healed, the way a summon does", Hp, Max);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARNING("[JOIN] could not heal on arrival");
    }
}

void TickArrival(int Join) {
    const bool Arrived = Join == kJoinInWorld && g_lastJoinState != kJoinInWorld;
    g_lastJoinState = Join;
    // The flag table the game hands a guest is the host's copy, and it is swapped in around the arrival:
    // once now and once a few seconds later, so the writes land in the copy that stays.
    if (!Arrived) {
        if (g_talkFlagsAgainAt && GetTickCount64() >= g_talkFlagsAgainAt && Join == kJoinInWorld) {
            g_talkFlagsAgainAt = 0;
            ReapplyMyTalkFlagsOnArrival();
        }
        return;
    }
    HealOnArrival();
    ReapplyMyTalkFlagsOnArrival();   // player_sync.cpp: the lines I have already heard stay heard
    g_talkFlagsAgainAt = GetTickCount64() + 5000;
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
                         bool Share, int* Unlocked, uintptr_t List = 0) {
    __try {
        if (!List) {
            const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
            if (!Gm) return -1;
            const uintptr_t Events = *reinterpret_cast<const uintptr_t*>(Gm + 0x70);
            if (!Events) return -1;
            List = *reinterpret_cast<const uintptr_t*>(Events + 0x58);
            if (!List) return -1;
        }
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
// List 0: the travel list the game manager holds now.
void ApplyPartnerBonfires(uintptr_t List = 0, bool AtLoad = false) {
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
    // ...and the ones this player lit over there itself, which the game's own refill knows nothing
    // about (bonfire_lit.cpp, report 2 of 21.09 evening).
    const int Mine = Count < kMaxBonfires
                         ? MyLitBonfiresInHostWorld(Ids + Count, Flags + Count, static_cast<int>(kMaxBonfires - Count))
                         : 0;
    Count += static_cast<uint32_t>(Mine);
    if (!Count) return;
    const bool Share = IsProgressSharingOn();
    int Unlocked = 0;
    const int Written = WriteSessionBonfires(Ids, Flags, Count, Share, &Unlocked, List);
    if (AtLoad) {
        LOG_INFO("[BONFIRE] a load emptied the session set -- the host's %u bonfires written back at once "
                 "(%d record(s) changed), before this map's bonfires are built", Count, Written);
        return;
    }
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

// A load empties the session set here (exe+0x17E890, from the event manager's
// set-up at exe+0x44F316) and the map's bonfire objects read it once, when they are
// built: exe+0x1CB310 registers "kindle" for a bonfire exe+0x17E6F0 calls unlit. The
// tick's refill came after that (17.09 11:32:16): the bonfire the guest stood at
// offered only "kindle", and kindling failed at its own lit check, which by then
// read the refilled byte -- no kindling, no resting. So a guest's view of the host's
// set is written back the moment the game empties it.
using BonfireViewFn = void(__fastcall*)(void* list);
BonfireViewFn g_bonfireViewOriginal = nullptr;

int ReadViewSafe(void* List) {
    __try {
        return *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(List) + kTravelView);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void __fastcall BonfireViewDetour(void* List) {
    g_bonfireViewOriginal(List);
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!List || !Lobby.IsActive() || Lobby.IsHost() || ReadViewSafe(List) != 1) return;
    ApplyPartnerBonfires(reinterpret_cast<uintptr_t>(List), true);
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

// A guest's own copy of the fight it drove to its end is paying out right now: phase 2 or 3
// of that battle, within twenty seconds of the end. The hold has to outlast it, or the way
// home starts before exe+0x181950 and exe+0x181850 have handed out the souls and the reward
// (boss_down.cpp lets those phases run while the guest is down).
bool CopyStillPayingOut() {
    const int32_t   Killed = g_guestKilled.load();
    const ULONGLONG At     = g_guestKilledAt.load();
    if (Killed <= 0 || !At || GetTickCount64() - At > kPayOutMs) return false;
    int32_t Active = 0, Phase = 0;
    if (!ReadBoss(&Active, &Phase) || Active != Killed || (Phase != 2 && Phase != 3)) return false;
    static int32_t s_told = 0;
    if (s_told != Killed) {
        s_told = Killed;
        LOG_INFO("[DEATH] held a little longer: my copy of battle %d is in phase %d and hands out the reward first",
                 Killed, Phase);
    }
    return true;
}

void TickHold(int Join, int32_t Hp) {
    if (!g_hold.Active) return;
    const ULONGLONG Now = GetTickCount64();
    const char* Why = nullptr;
    // Both of us being down used to end the hold at once ("the partner fell too" / "I fell too"), and
    // that is how a guest lost the souls of a fight it took part in: on 21.09 evening the host fell at
    // 19:50:22, the guest at 19:51:19 -- released, sent home -- and the host went on alone and killed
    // the boss at 19:54:03, with the guest's reward nowhere. The fight itself decides now: while the
    // host's battle is still running the guest waits where it fell, and the reward finds it there
    // (boss_down.cpp gives the souls to a player who is down). A host that gives up and respawns ends
    // the battle, which lands on "the boss fight is over" a moment later.
    if (CopyStillPayingOut())                            Why = nullptr;
    else if (!BossFightOn(Join))                         Why = "the boss fight is over";
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

// The host died where this guest's game cannot see it (0.2.2, second report point 1).
//
// A guest learns of the host's death from the host's character in its own copy of the
// world: the result "host died" (code 4, posted at exe+0x19216E) comes when that character
// dies here. In another map there is no such character -- it goes with the map it was
// loaded in -- so nothing comes: on 17.09 at 16:28:25 the host fell in 10310000 with the
// guest in Majula, the host's respawn waited for the session to end (WaitSessionJob
// exe+0x191210, the host's camera frozen at its body), and only once the guest travelled
// to 10310000 (16:28:54) did its game see the body, post the result (16:28:57) and send it
// home (16:29:03). So with the host dead by its own PlayerDeath packet for kHostDeadUnseenMs,
// no shared map, no partner character here and no result of the game's own, the guest
// leaves the way that result makes it leave -- the join controller's leave with reason 3,
// which exe+0x2C9220 passes on from the phantom branch -- and joins again once the host is up.
bool CallJoinLeaveSafe(void* Ctrl, int Reason) {
    __try {
        reinterpret_cast<LeaveFn>(g_joinLeaveOriginal)(Ctrl, Reason);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t ReadJoinCtrl() {
    uintptr_t Root = 0, Mp = 0, Ctrl = 0, Vtbl = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp) || !ReadPtr(Mp + 0x40, &Ctrl)) return 0;
    return ReadPtr(Ctrl, &Vtbl) && Vtbl == ExeBase() + kJoinCtrlVtable ? Ctrl : 0;
}

void TickHostDiedElsewhere(int Join, int32_t Hp) {
    static ULONGLONG s_since = 0;
    const ULONGLONG Now = GetTickCount64();
    const ULONGLONG ResultAt = g_hostDeathResultAt.load();
    const bool GameSawIt = ResultAt && Now - ResultAt < 30000;
    const bool Unseen = g_enabled.load() && g_joinLeaveOriginal && Join == kJoinInWorld && IsAlive(Hp) &&
                        !g_partnerAlive.load() && PartnerConnected() && !g_hold.Active && !g_rejoin.Pending &&
                        !GameSawIt && !PlayersShareMap() && !GetPartnerCharacter(2000);
    if (!Unseen) {
        s_since = 0;
        return;
    }
    if (!s_since) s_since = Now;
    if (Now - s_since < kHostDeadUnseenMs) return;
    s_since = 0;
    const uintptr_t Ctrl = ReadJoinCtrl();
    if (!Ctrl) return;
    LOG_INFO("[DEATH] the host died in map %u while I stand in another -- my game cannot see it (no \"host died\" "
             "result here), so I leave its world the way that result would (reason %d) and come back once it is up",
             PartnerArea(), kLeaveHostDied);
    Toast("The host fell elsewhere -- back to your world until it is up",
          "Хост погиб в другой локации \xE2\x80\x94 возвращаюсь к себе, пока он не встанет", UI::NotifyKind::Warning);
    ArmRejoin(false, false);
    if (!CallJoinLeaveSafe(reinterpret_cast<void*>(Ctrl), kLeaveHostDied)) {
        g_rejoin.Pending = false;
        LOG_WARNING("[DEATH] leaving the host's world threw -- staying");
    }
}

// The host travelled by bonfire (packet HostTravelled). The game's own answer is
// to throw the guest out -- up to five minutes later: on 16.09 the host travelled
// at 18:38:52, its game sent RequestNotifyLeaveGuestPlayer at 18:40:58, and the
// guest was thrown out at 18:41:01 after two minutes alone in a copy of a world
// the host had left. So the guest follows at once: it leaves that copy and joins
// again where the host went, once the host has actually arrived there.
// The host travelled. The guest used to leave and be summoned again where the
// host went -- "idiotic", in the words of the one who plays it, and rightly: it is
// a trip home and back for something the guest may not even want. Now the guest
// stays where it is, in the host's world; it can travel after the host by itself,
// and TravelResyncTick puts the two back together when they share a map.
void TickHostTravel(int Join) {
    if (!g_hostTravelPending.exchange(false)) return;
    const int32_t Bonfire = g_hostTravelBonfire.load();
    LOG_INFO("[TRAVEL] the host travelled to bonfire %d (map %u)%s", Bonfire,
             RawMapToArea(g_hostTravelMap.load()),
             Join == kJoinInWorld ? " -- I stay where I am; travel there to join up again" : "");
}

// The partner's travel, told on the game thread: the bonfire's name is looked up here.
struct PartnerTravel {
    int32_t Map;
    int32_t Target;
    int32_t Type;
    char    From[32];
};
std::mutex    g_partnerTravelMutex;
PartnerTravel g_partnerTravel{};
bool          g_partnerTravelNew = false;

void TickPartnerTravel(int Join) {
    PartnerTravel T{};
    {
        std::lock_guard<std::mutex> Lock(g_partnerTravelMutex);
        if (!g_partnerTravelNew) return;
        g_partnerTravelNew = false;
        T = g_partnerTravel;
    }
    const std::string Place = TravelPlaceName(T.Map, T.Target, T.Type);
    const bool HostWent = Join == kJoinInWorld;   // only the host's travel reaches a guest in its world
    LOG_INFO("[TRAVEL] %s travelled: %s (map %u, id %d, type %d)", T.From, Place.c_str(), RawMapToArea(T.Map),
             T.Target, T.Type);
    UI::Overlay::GetInstance().ShowNotification(
        HostWent ? UI::Format(UI::Tr("%s travelled to %s \xE2\x80\x94 travel there too to stay together.",
                                     "%s переместился: %s \xE2\x80\x94 переместись туда же, чтобы быть вместе."),
                              T.From, Place.c_str())
                 : UI::Format(UI::Tr("%s travelled to %s.", "%s переместился: %s."), T.From, Place.c_str()),
        6.0f, UI::NotifyKind::Player);
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
    const int Try = g_flight.Try + 1;
    g_flight = JoinFlight{ true, Now, Try, false, Spot{} };
    g_hostTravelledInFlight.store(false);
    g_flightOpen.store(true);
}

void EndFlight() {
    g_flight = JoinFlight{};
    g_flightOpen.store(false);
    g_hostTravelledInFlight.store(false);
}

void TickJoinFlight(int Join) {
    if (!g_flight.Active) return;
    if (g_cancelRejoin.load() || !Session::SessionManager::GetInstance().IsActive()) {
        EndFlight();
        return;
    }
    if (Join == kJoinInWorld) {
        if (g_flight.Try > 1) LOG_INFO("[DEATH] back in the host's world on sign %d", g_flight.Try);
        EndFlight();
        return;
    }
    if (!PartnerConnected()) {
        LOG_INFO("[DEATH] the partner is gone -- the way back is not followed any more");
        EndFlight();
        return;
    }
    if (Join >= 0) g_flight.SawJoin = true;
    const ULONGLONG Now = GetTickCount64();
    // A join that started and neither arrived nor ended: nothing more to do by itself
    // (code review 17.09 -- Lost and Late alone left the flight open for good).
    if (g_flight.SawJoin && Join >= 0 && Now - g_flight.Since > kJoinStuckMs) {
        LOG_WARNING("[DEATH] the way back has been under way for %llu s without arriving (join state %d, sign %d) -- "
                    "not waiting for it any more", static_cast<unsigned long long>((Now - g_flight.Since) / 1000),
                    Join, g_flight.Try);
        Toast("Could not get back to your partner \xE2\x80\x94 join again from the menu.",
              "Не получилось вернуться к напарнику \xE2\x80\x94 зайди снова через меню.", UI::NotifyKind::Warning);
        EndFlight();
        return;
    }
    const bool HostMoved = g_hostTravelledInFlight.exchange(false);
    if (HostMoved && !g_flight.HostFrom.Valid) {
        Spot From{};
        if (ReadPartnerSpot(&From)) g_flight.HostFrom = From;
    }
    const bool Lost = g_flight.SawJoin && Join < 0;
    const bool Late = !g_flight.SawJoin && Now - g_flight.Since > kJoinFlightMs;
    if (!Lost && !Late) return;

    const JoinFlight Was = g_flight;
    g_flight.Active = false;
    g_flightOpen.store(false);
    const char* Why = Lost ? (Was.HostFrom.Valid ? "the host travelled while it was on the way" : "the join did not go through")
                           : "no summon came";
    if (Was.Try >= kJoinTriesMax) {
        LOG_WARNING("[DEATH] the way back into the host's world failed (%s, sign %d) -- not trying again by myself",
                    Why, Was.Try);
        Toast("Could not get back to your partner \xE2\x80\x94 join again from the menu.",
              "Не получилось вернуться к напарнику \xE2\x80\x94 зайди снова через меню.", UI::NotifyKind::Warning);
        EndFlight();
        return;
    }
    ArmRejoin(false, false);
    g_rejoin.WaitHostArrival = Was.HostFrom.Valid;
    g_rejoin.From = Was.HostFrom;
    LOG_INFO("[DEATH] the way back did not complete (%s, sign %d) -- trying again%s", Why, Was.Try,
             Was.HostFrom.Valid ? " once the host has arrived where it went" : "");
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
        HookAt(kAcceptEvent, reinterpret_cast<void*>(&AcceptEventDetour), &g_acceptEventOriginal, "guest accept controller event");
        HookAt(kBossPhaseTwo, reinterpret_cast<void*>(&BossPhaseTwoDetour), &g_phaseTwoOriginal, "boss phase 2");
        HookAt(kBonfireView, reinterpret_cast<void*>(&BonfireViewDetour),
               reinterpret_cast<void**>(&g_bonfireViewOriginal), "bonfire list view");
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
    TickFarSpectate(Join, Hp);
    TickBoss(Join);
    TickGuestBoss(Join);
    TickArrival(Join);
    TickTravelList(Join);
    TickBonfireSync(Join);
    TickHold(Join, Hp);
    TickHostDiedElsewhere(Join, Hp);
    AcceptProbeTick();
    TickHostTravel(Join);
    TickPartnerTravel(Join);
    TickRejoin(Join, Hp);
    TickJoinFlight(Join);
    TravelResyncTick();
    PoseProbeTick();
    GuestWorldTick();
}

void NotePartnerLife(bool Alive) {
    const bool Was = g_partnerAlive.exchange(Alive);
    if (!Alive) g_partnerDiedAt.store(GetTickCount64());
    if (Alive && !Was) g_partnerBackAt.store(GetTickCount64());
}

// The partner's HP from its PlayerState packets (19.09: the guest was sent home after every travel of its
// own). The host fell at 21:31:14, the guest was sent home at 21:31:18 and so missed the host's
// PlayerRespawn (19:31:29 on the host's clock) -- "the partner is dead" stayed for the rest of the session,
// and TickHostDiedElsewhere took each map the guest travelled to without the host for "the host died
// there": home at 21:34:54, 21:35:59, 21:47:19, 21:49:38. The state packets come every 0.5 s: HP above 0
// makes the partner alive again, though not in the first seconds after its death packet (a state packet
// sent before the death may come in after it).
void NotePartnerStateHp(int32_t Hp, int32_t MaxHp) {
    if (Hp <= 0 || MaxHp <= 0 || g_partnerAlive.load()) return;
    if (GetTickCount64() - g_partnerDiedAt.load() < kStateAliveAfterDeathMs) return;
    NotePartnerLife(true);
    LOG_INFO("[DEATH] the partner reports HP %d/%d -- alive (its \"back\" packet never came here)", Hp, MaxHp);
}

void NoteHostTravelled(int32_t RawMap, int32_t Bonfire) {
    g_hostTravelMap.store(RawMap);
    g_hostTravelBonfire.store(Bonfire);
    g_hostTravelPending.store(true);
    if (g_flightOpen.load()) g_hostTravelledInFlight.store(true);
    WatchPoses();
    LOG_INFO("[DEATH] the host says it travelled to bonfire %d (map %u)", Bonfire, RawMapToArea(RawMap));
}

void NotePartnerTravelled(int32_t Map, int32_t Target, int32_t Type, const std::string& From) {
    NotePartnerTravelForPose();   // travel_sync.cpp: the partner's copy here may keep the travel pose
    std::lock_guard<std::mutex> Lock(g_partnerTravelMutex);
    g_partnerTravel.Map = Map;
    g_partnerTravel.Target = Target;
    g_partnerTravel.Type = Type;
    strncpy_s(g_partnerTravel.From, sizeof(g_partnerTravel.From), From.c_str(), _TRUNCATE);
    g_partnerTravelNew = true;
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
    const ULONGLONG Now = GetTickCount64();
    g_partnerBossAt.store(Now);
    // The end of the host's fight, for the guest's copy (TickGuestBossEnd): a kill
    // is phase 2 or 3 of that battle, repeated by the host for ten seconds; a fight
    // that simply stops is "running" followed by "none".
    if (Active > 0 && (Phase == 2 || Phase == 3)) {
        g_hostKilledBattle.store(Active);
        g_hostKilledAt.store(Now);
    } else if (Active <= 0 && WasActive > 0 && WasPhase == 1) {
        g_hostEndedBattle.store(WasActive);
        g_hostEndedAt.store(Now);
    }
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

bool PartnerAliveReported() {
    return g_partnerAlive.load();
}

void SetArrivalFollowHost(bool On) {
    g_arrivalFollowHost.store(On);
}

// A bonfire the partner lit where both of us are: this player's respawn too (18.09, point 3: the
// guest lit a bonfire, the host died and woke at the map's start, never having rested there).
// Lighting sets the lighter's last bonfire itself -- exe+0x1CAF50 -> exe+0x44FE30([GMImp+0x70],
// {map, 0, id}), only outside a multiplayer world -- and nobody else's. The host takes the same
// record; a guest in the host's world comes back after a death by the mod's rest spot, so that one
// moves to the bonfire (found among the loaded ones by its id, exe+0x3BA6A0).
bool FindBonfireByIdSafe(int32_t Id, float* Out) {
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        const uintptr_t Events = *reinterpret_cast<const uintptr_t*>(Gm + 0x70);
        const uintptr_t List = *reinterpret_cast<const uintptr_t*>(Events + 0x58);
        uintptr_t Node = *reinterpret_cast<const uintptr_t*>(List + 0x08);
        int Count = *reinterpret_cast<const int32_t*>(List + 0x10);
        if (Count < 0 || Count > 512) Count = 512;
        for (int I = 0; I < Count && Node; ++I) {
            const uintptr_t Obj = *reinterpret_cast<const uintptr_t*>(Node + 0x08);
            if (Obj) {
                const int32_t* Its = reinterpret_cast<const int32_t*(__fastcall*)(uintptr_t)>(ExeBase() + 0x3BA6A0)(Obj);
                if (Its && *Its == Id) {
                    const float* P = reinterpret_cast<const float*>(Obj + 0x70);
                    Out[0] = P[0];
                    Out[1] = P[1];
                    Out[2] = P[2];
                    return true;
                }
            }
            Node = *reinterpret_cast<const uintptr_t*>(Node + 0x60);
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SetLastBonfireSafe(int32_t RawMap, int32_t Id) {
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        const uintptr_t Events = Gm ? *reinterpret_cast<const uintptr_t*>(Gm + 0x70) : 0;
        if (!Events) return false;
        const int32_t Record[3] = { RawMap, 0, Id };
        reinterpret_cast<BonfireFn>(ExeBase() + kLastBonfire)(reinterpret_cast<void*>(Events), Record);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void NotePartnerLitForRespawn(int32_t Id, int32_t RawMap) {
    if (!g_enabled.load() || Id <= 0) return;
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive()) return;
    if (Lobby.IsHost()) {
        int32_t Joining = 0;
        if (!GuestFullyInSafe() || AGuestIsStillJoining(&Joining)) return;
        const bool Done = SetLastBonfireSafe(RawMap, Id);
        LOG_INFO("[DEATH] the guest lit bonfire %d (map %u) in my world -- it is my respawn too: %s", Id,
                 RawMapToArea(RawMap), Done ? "done" : "threw");
        return;
    }
    if (ReadJoinState() != kJoinInWorld) return;
    float P[3] = {};
    if (!FindBonfireByIdSafe(Id, P)) {
        LOG_INFO("[DEATH] the host lit bonfire %d (map %u) -- not loaded here, my way back stays as it was", Id,
                 RawMapToArea(RawMap));
        return;
    }
    g_restSpot = Spot{ RawMapToArea(RawMap), P[0], P[1], P[2], true };
    LOG_INFO("[DEATH] the host lit bonfire %d (map %u) -- after a death I come back there (%.2f, %.2f, %.2f)", Id,
             RawMapToArea(RawMap), P[0], P[1], P[2]);
}

// Game thread: a guest whose return is held for exactly this battle (boss_down.cpp).
bool GuestHeldForBattle(int32_t Battle) {
    return g_hold.Active && Battle > 0 && g_guestKilled.load() == Battle;
}

// The boss reward item for a guest (MpActiveHook asks): only while the fight
// this guest's copy was driven to the end of is in phase 3 here -- the one pass
// in which exe+0x181850 hands out the reward.
bool GuestBossRewardDue() {
    const int32_t   Killed = g_guestKilled.load();
    const ULONGLONG At     = g_guestKilledAt.load();
    if (Killed <= 0 || !At || GetTickCount64() - At > 2 * 60 * 1000) return false;
    const uintptr_t Boss = BossManagerPtr();
    int32_t Active = 0, Phase = 0;
    return Boss && ReadI32(Boss + 0x14, &Active) && ReadI32(Boss + 0x204, &Phase) && Active == Killed && Phase == 3;
}

void CancelDeathRejoin() {
    g_cancelRejoin.store(true);
    g_forgetSpots.store(true);
}

} // namespace DS2Coop::Sync
