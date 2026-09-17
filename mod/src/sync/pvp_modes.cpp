// How the two players stand towards each other: no damage, friendly fire, or a
// real fight.
//
// Asked for on 12.09, three positions: (a) hits between the players do nothing,
// which is how it works today; (b) hits land, but neither player can lock the
// camera on to the other; (c) the partner counts as a hostile character, the way
// an invader does.
//
// All hostility in this game comes out of one 28x28 byte matrix at
// exe+0x1583E40 (row stride 0x1C). A character's effective team is
//
//   pt = [chr+0xB0]                                  // PlayerType
//   if   [pt+0x4D]   team = *(u32*)(exe+0x1584150 + 4*pt[0x3D])
//   elif [pt+0x4E]   team = *(u32*)(exe+0x15841C0 + 4*pt[0x3D])
//   else             team =  pt[0x3D]
//
// and three predicates read matrix[a*28 + b]: exe+0x44A6E0 "hostile" (the value
// is 1 or 3), exe+0x44A690 "same team" (0), exe+0x44A6B0 "hostile, strict" (1).
// The damage filter -- exe+0x13A6A0, vtable slot [40] of all four
// DamageActionCtrl classes -- reads nothing else about the two characters, so
// the team byte decides damage by itself. Lock-on is a separate test
// (exe+0x382630 slot [18]) with its own gate: status flag 0x37, asked through the
// getter exe+0x100DD0 (a 5-byte jump into obfuscated code that reads bit idx of the
// qword array at [chr+0xB8]+0x4B8). Only the lock-on filter (exe+0x3829DA) and its
// second pass (exe+0x3895A1) ask for 0x37 -- the damage code asks for 0xC-0xE and
// 0x1E. That is the only reason mode (b) is possible at all -- damage and lock-on
// otherwise consult the same matrix cell, so no pair of team numbers can separate
// them. (Until 17.09 the flag was written at [[chr+0xB8]+0x18]+0x0B, a different
// getter's input bitset: +0x18 is an int that stays 0, so nothing was ever written
// and friendly fire let both players lock on. The character update also clears the
// whole +0x4B8 qword every frame, so writing the right bit would not hold either --
// the getter answers for the partner instead.)
//
// Measured 16.09 with the probe below, on both machines: the host is team 0, the
// guest -- once EnableSummoning has run -- team 0 in its own copy, and the host
// sees the guest as team 2 (matrix[0][2] = 0: no damage). The team column of the
// phantom type table (exe+0x10C0050, byte 6) names the rest: invaders (types
// 7-10) are team 7, and team 7 is ALLIED with the enemies (matrix[7][0x17..0x19]
// and back = 2) while hostile to team 0 -- literally "an evil spirit". Team 0x10
// is hostile to team 0 and to the enemies, both ways.
//
// So the modes are a team for the GUEST, the same on both machines -- the guest
// writes it into its own PlayerType, the host into its copy of the guest -- and
// the host keeps team 0 everywhere. Every machine then sees both players with
// the same teams, so it does not matter on which side a hit is judged:
//   none            guest team 0 (its own copy) / left as the game made it (host's copy)
//   friendly fire   guest team 0x10, plus flag 0x37 answered "set" for the partner's
//                   copy on each machine, so neither can lock on; the enemies still
//                   attack both
//   PvP             guest team 7, no flag: the guest is an evil spirit, the host's
//                   enemies leave it alone, and lock-on works both ways
// Expected side effect of both fight modes: a bonfire refuses rest while a
// hostile character is near (exe+0x1CB3D0), and that now includes the partner.
// Everything written is put back when the mode returns to none or the session
// ends.

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
#include "../../include/session.h"
#include "../../include/network.h"
#include "../../include/ui.h"
#include "../../include/ui_settings.h"
#include "../../include/utils.h"

#include <atomic>
#include <cstdint>

#pragma intrinsic(_ReturnAddress)

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t  kGameManagerImp = 0x16148F0;  // *(exe+...) = GameManagerImp; +0xD0 local player
constexpr uint32_t  kTeamMatrix     = 0x1583E40;  // 28 x 28 bytes, row stride 0x1C
constexpr uint32_t  kTeamMapA       = 0x1584150;  // used when PlayerType+0x4D is set
constexpr uint32_t  kTeamMapB       = 0x15841C0;  // used when PlayerType+0x4E is set
constexpr uint32_t  kTeamCount      = 28;
constexpr ptrdiff_t kTypeInChr      = 0xB0;       // chr+0xB0 -> PlayerType
constexpr ptrdiff_t kPhantomId      = 0x3C;       // PlayerType+0x3C ChrNetworkPhantomId
constexpr ptrdiff_t kTeamByte       = 0x3D;       // PlayerType+0x3D TeamType, 0..27
constexpr ptrdiff_t kTeamMapFlagA   = 0x4D;
constexpr ptrdiff_t kTeamMapFlagB   = 0x4E;

// The modes (docs §3.25).
constexpr uint8_t   kGuestTeamFriendlyFire = 0x10;   // hostile to team 0 and to the enemies, both ways
constexpr uint8_t   kGuestTeamPvp          = 0x07;   // the invaders' team: hostile to 0, allied with enemies
constexpr uint32_t  kNetRoot        = 0x1616CF8;  // *(exe+...) = network root; +0x18 multiplayer manager
constexpr uint32_t  kJoinCtrlVtable = 0x10D7BD8;  // NetSummonJoinMultiplayCtrl
constexpr int32_t   kJoinInWorld    = 7;
constexpr ptrdiff_t kStatusInChr    = 0xB8;       // chr+0xB8 -> status
constexpr uint32_t  kStatusFlagGet  = 0x100DD0;   // (status, flag index) -> AL: the flag is set
constexpr int       kFlagNoLockOn   = 0x37;       // "cannot be locked on to"
constexpr uint32_t  kLockFilterCall = 0x3829DF;   // return address in the lock-on filter exe+0x382630
constexpr uint32_t  kLockPassCall   = 0x3895A6;   // return address in its second pass exe+0x3893B0
constexpr ptrdiff_t kGeneratorInChr = 0x110;      // -1 for a player, a generator record otherwise
constexpr ULONGLONG kHostModeFreshMs = 10000;     // a guest trusts the host's last word this long
constexpr ULONGLONG kModeResendMs    = 3000;      // the host repeats its choice this often
constexpr ULONGLONG kPartnerFreshMs  = 500;       // a partner object seen drawn this recently

uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

bool ReadPtr(uintptr_t Addr, uintptr_t* Out) {
    __try {
        *Out = *reinterpret_cast<const uintptr_t*>(Addr);
        return *Out != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// One character's four bytes and the team the game would actually use for it.
struct Side {
    bool     Ok;
    uint8_t  PhantomId;
    uint8_t  TeamByte;
    uint8_t  MapFlagA;
    uint8_t  MapFlagB;
    uint32_t Effective;
};

Side ReadSide(uintptr_t Chr) {
    Side S{};
    if (!Chr) return S;
    __try {
        uintptr_t Type = 0;
        if (!ReadPtr(Chr + kTypeInChr, &Type)) return S;
        S.PhantomId = *reinterpret_cast<const uint8_t*>(Type + kPhantomId);
        S.TeamByte  = *reinterpret_cast<const uint8_t*>(Type + kTeamByte);
        S.MapFlagA  = *reinterpret_cast<const uint8_t*>(Type + kTeamMapFlagA);
        S.MapFlagB  = *reinterpret_cast<const uint8_t*>(Type + kTeamMapFlagB);
        if (S.MapFlagA) {
            S.Effective = *reinterpret_cast<const uint32_t*>(ExeBase() + kTeamMapA + 4u * S.TeamByte);
        } else if (S.MapFlagB) {
            S.Effective = *reinterpret_cast<const uint32_t*>(ExeBase() + kTeamMapB + 4u * S.TeamByte);
        } else {
            S.Effective = S.TeamByte;
        }
        S.Ok = true;
        return S;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        S.Ok = false;
        return S;
    }
}

// The raw relation byte, or -1 when either team is outside the table.
int MatrixCell(uint32_t A, uint32_t B) {
    if (A >= kTeamCount || B >= kTeamCount) return -1;
    __try {
        return *reinterpret_cast<const uint8_t*>(ExeBase() + kTeamMatrix + A * kTeamCount + B);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

const char* Meaning(int Cell) {
    switch (Cell) {
        case 0:  return "same team, no damage";
        case 1:  return "hostile";
        case 2:  return "neither (allied other team?)";
        case 3:  return "hostile, second kind";
        default: return "unknown";
    }
}

// --- the modes ------------------------------------------------------------------
std::atomic<uint8_t>   g_chosenMode{ kDamageNone };   // this player's own choice; a host's is the one in force
std::atomic<uint8_t>   g_hostMode{ kDamageNone };     // the host's, as last sent to this guest
std::atomic<ULONGLONG> g_hostModeAt{ 0 };
std::atomic<ULONGLONG> g_lastSendAt{ 0 };

// What this machine wrote into the partner's copy, so it can be put back. Game
// thread only.
struct PartnerWrite {
    uintptr_t Chr          = 0;
    bool      TeamWritten  = false;
    uint8_t   OriginalTeam = 0;
};
PartnerWrite g_partnerWrite;
uint8_t      g_appliedMode = kDamageNone;

// The partner's status object while friendly fire is on, else 0. The getter hook
// only compares pointers against it, so a value left over from a freed character
// can never be dereferenced.
using StatusFlagFn = bool(__fastcall*)(void* status, int index);
StatusFlagFn             g_statusFlagOriginal = nullptr;
std::atomic<uintptr_t>   g_noLockStatus{ 0 };
std::atomic<uint32_t>    g_noLockAnswers{ 0 };     // times the getter answered "set" for the partner
std::atomic<uintptr_t>   g_noLockLastCaller{ 0 };

bool __fastcall StatusFlagDetour(void* Status, int Index) {
    if (Index == kFlagNoLockOn && Status) {
        const uintptr_t NoLock = g_noLockStatus.load(std::memory_order_relaxed);
        if (NoLock && reinterpret_cast<uintptr_t>(Status) == NoLock) {
            g_noLockAnswers.fetch_add(1, std::memory_order_relaxed);
            g_noLockLastCaller.store(reinterpret_cast<uintptr_t>(_ReturnAddress()), std::memory_order_relaxed);
            return true;
        }
    }
    return g_statusFlagOriginal(Status, Index);
}

uintptr_t StatusOf(uintptr_t Chr) {
    uintptr_t Status = 0;
    return ReadPtr(Chr + kStatusInChr, &Status) ? Status : 0;
}

const char* ModeName(uint8_t Mode) {
    switch (Mode) {
        case kDamageFriendlyFire: return "friendly fire without lock-on";
        case kDamagePvp:          return "PvP (the guest an evil spirit)";
        default:                  return "no damage";
    }
}

uint8_t TeamForGuest(uint8_t Mode) {
    return Mode == kDamageFriendlyFire ? kGuestTeamFriendlyFire : Mode == kDamagePvp ? kGuestTeamPvp : 0;
}

// Join controller state: 7 in the host's world; -1 none, -2 another kind.
int ReadJoinState() {
    uintptr_t Root = 0, Mp = 0, Ctrl = 0, Vtbl = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp)) return -1;
    if (!ReadPtr(Mp + 0x40, &Ctrl)) return -1;
    if (!ReadPtr(Ctrl, &Vtbl) || Vtbl != ExeBase() + kJoinCtrlVtable) return -2;
    __try {
        return *reinterpret_cast<const int32_t*>(Ctrl + 0xF8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// A player's character, not an NPC and not this player: the same vtable as the
// local player and no generator record (docs §3.31). Checked before every write,
// because the partner object is only known from the code that draws it and does
// not survive a map load.
bool IsOtherPlayer(uintptr_t Chr, uintptr_t Local) {
    if (!Chr || !Local || Chr == Local) return false;
    __try {
        const uintptr_t Mine = *reinterpret_cast<const uintptr_t*>(Local);
        const uintptr_t Its  = *reinterpret_cast<const uintptr_t*>(Chr);
        const int32_t   Gen  = *reinterpret_cast<const int32_t*>(Chr + kGeneratorInChr);
        return Mine != 0 && Mine == Its && Gen == -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteTeam(uintptr_t Chr, uint8_t Team, uint8_t* Was) {
    __try {
        const uintptr_t Type = *reinterpret_cast<const uintptr_t*>(Chr + kTypeInChr);
        if (!Type) return false;
        uint8_t* Byte = reinterpret_cast<uint8_t*>(Type + kTeamByte);
        if (Was) *Was = *Byte;
        if (*Byte != Team) *Byte = Team;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


// Puts back whatever was written into the partner's copy -- if that object still
// is a player's character; one freed by a map load is left alone.
void RestorePartner(uintptr_t Local) {
    const PartnerWrite W = g_partnerWrite;
    g_partnerWrite = PartnerWrite{};
    if (!W.Chr || !W.TeamWritten || !IsOtherPlayer(W.Chr, Local)) return;
    WriteTeam(W.Chr, W.OriginalTeam, nullptr);
    LOG_INFO("[PVP] the partner's copy put back: team %u", W.OriginalTeam);
}

void SendMode(uint8_t Mode) {
    Network::DamageModePacket Packet{};
    Packet.header.magic = 0x44533243;
    Packet.header.type = Network::PacketType::DamageMode;
    Packet.header.size = sizeof(Network::DamageModePacket);
    Packet.mode = Mode;
    Network::PeerManager::GetInstance().BroadcastPacket(&Packet.header);
}

// Game thread. Logs each change of the partner the getter says "no lock-on" for.
void SetNoLockTarget(uintptr_t Status, const char* WhyOff) {
    const uintptr_t Was = g_noLockStatus.exchange(Status);
    if (Was == Status) return;
    if (!g_statusFlagOriginal) {
        if (Status) LOG_WARNING("[PVP] friendly fire: the status flag getter is not hooked -- lock-on stays allowed");
        return;
    }
    if (Status) {
        LOG_INFO("[PVP] lock-on onto the partner switched off here (flag 0x37 answered for its status %p)",
                 reinterpret_cast<void*>(Status));
    } else {
        LOG_INFO("[PVP] lock-on onto the partner allowed again (%s)", WhyOff);
    }
}

// Once every ten seconds while friendly fire holds a partner: how often the game
// asked, and from where -- the lock-on filter should be the caller.
void NoLockProbeTick() {
    static ULONGLONG s_at = 0;
    const ULONGLONG Now = GetTickCount64();
    if (Now - s_at < 10000) return;
    s_at = Now;
    const uint32_t Answers = g_noLockAnswers.exchange(0);
    if (!g_noLockStatus.load() || !Answers) return;
    const uintptr_t Caller = g_noLockLastCaller.load() - ExeBase();
    LOG_INFO("[PVP] friendly fire: \"cannot be locked on to\" answered %u times for the partner in 10 s, last from "
             "exe+0x%llX (%s)", Answers, static_cast<unsigned long long>(Caller),
             Caller == kLockFilterCall ? "the lock-on filter" : Caller == kLockPassCall ? "its second pass" : "elsewhere");
}

void ApplyModes() {
    auto& Lobby = Session::SessionManager::GetInstance();
    const bool Active = Lobby.IsActive();
    const bool Host   = Active && Lobby.IsHost();
    const bool Guest  = Active && !Host && ReadJoinState() == kJoinInWorld;
    const uint8_t Mode = (Host || Guest) ? GetDamageMode() : kDamageNone;

    if (Host) {
        const ULONGLONG Now = GetTickCount64();
        if (Now - g_lastSendAt.load() >= kModeResendMs) {
            g_lastSendAt.store(Now);
            SendMode(g_chosenMode.load());
        }
    }
    if (Mode != g_appliedMode) {
        LOG_INFO("[PVP] damage between the players: %s -> %s (%s)", ModeName(g_appliedMode), ModeName(Mode),
                 Host ? "my choice as the host" : Guest ? "the host's choice" : "not in co-op");
        g_appliedMode = Mode;
    }

    // The host sees its own choice when it makes it (overlay.cpp); a guest was
    // never told at all (17.09). Only a choice that really came from the host is
    // announced: a host in a long loading screen stops sending, the guest falls
    // back to no damage for a while, and that is not the host changing its mind.
    static uint8_t s_announced = kDamageNone;
    if (!Guest) {
        s_announced = kDamageNone;
    } else {
        const ULONGLONG At = g_hostModeAt.load();
        const bool Fresh = At && GetTickCount64() - At < kHostModeFreshMs;
        if (Fresh && Mode != s_announced) {
            s_announced = Mode;
            const char* Names[3] = { UI::Tr("None", "Нет"), UI::Tr("Friendly fire", "Огонь по своим"), UI::Tr("PvP", "PvP") };
            UI::Overlay::GetInstance().ShowNotification(
                UI::Format(UI::Tr("Damage between players (the host's choice): %s", "Урон между игроками (выбор хоста): %s"),
                           Names[Mode <= kDamagePvp ? Mode : kDamageNone]),
                3.5f, UI::NotifyKind::Info);
        }
    }

    uintptr_t Gm = 0, Local = 0;
    if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0xD0, &Local)) {   // loading
        SetNoLockTarget(0, "loading");
        return;
    }

    const uintptr_t Partner = GetPartnerCharacter(kPartnerFreshMs);
    const bool PartnerOk = IsOtherPlayer(Partner, Local);
    if (g_partnerWrite.Chr && (!PartnerOk || Partner != g_partnerWrite.Chr)) RestorePartner(Local);

    // No lock-on on the partner in friendly fire, on both machines: the getter
    // answers "set" for flag 0x37 on the partner's status and asks the game about
    // everything else.
    const uintptr_t NoLock = (Mode == kDamageFriendlyFire && PartnerOk) ? StatusOf(Partner) : 0;
    SetNoLockTarget(NoLock, Mode != kDamageFriendlyFire ? "friendly fire is off" : "no partner character here");

    // A guest's own team -- EnableSummoning writes the same value once a second.
    if (Guest) WriteTeam(Local, TeamForGuest(Mode), nullptr);
    if (Mode == kDamageNone) {
        RestorePartner(Local);
        return;
    }
    if (!PartnerOk) return;
    g_partnerWrite.Chr = Partner;

    // The host's copy of the guest gets the guest's team.
    if (Host) {
        uint8_t Was = 0;
        if (WriteTeam(Partner, TeamForGuest(Mode), &Was) && !g_partnerWrite.TeamWritten) {
            g_partnerWrite.TeamWritten  = true;
            g_partnerWrite.OriginalTeam = Was;
            LOG_INFO("[PVP] the guest's copy here: team %u -> %u", Was, TeamForGuest(Mode));
        }
    }
}

void ProbeTick();

} // namespace

void SetDamageMode(uint8_t Mode) {
    if (Mode > kDamagePvp) Mode = kDamageNone;
    if (g_chosenMode.exchange(Mode) != Mode) {
        LOG_INFO("[PVP] chosen: %s", ModeName(Mode));
        g_lastSendAt.store(0);   // the host tells its guests on the next frame
    }
}

uint8_t GetChosenDamageMode() {
    return g_chosenMode.load();
}

uint8_t GetDamageMode() {
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive()) return kDamageNone;
    if (Lobby.IsHost()) return g_chosenMode.load();
    const ULONGLONG At = g_hostModeAt.load();
    return (At && GetTickCount64() - At < kHostModeFreshMs) ? g_hostMode.load() : kDamageNone;
}

void NoteHostDamageMode(uint8_t Mode) {
    if (Mode > kDamagePvp) return;
    g_hostMode.store(Mode);
    g_hostModeAt.store(GetTickCount64());
}

uint8_t GuestOwnTeam() {
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive() || Lobby.IsHost() || ReadJoinState() != kJoinInWorld) return 0;
    return TeamForGuest(GetDamageMode());
}

bool InstallPvpModes() {
    static bool Installed = false;
    if (Installed) return g_statusFlagOriginal != nullptr;
    Installed = true;
    const bool Ok = Hooks::HookManager::GetInstance().InstallHook(
        reinterpret_cast<void*>(ExeBase() + kStatusFlagGet), reinterpret_cast<void*>(&StatusFlagDetour),
        reinterpret_cast<void**>(&g_statusFlagOriginal));
    if (!Ok) g_statusFlagOriginal = nullptr;
    LOG_INFO("[PVP] status flag getter exe+0x%X %s", kStatusFlagGet,
             Ok ? "hooked: friendly fire keeps lock-on off the partner" : "NOT hooked: friendly fire allows lock-on");
    return Ok;
}

void PvpModesGameTick() {
    ApplyModes();
    NoLockProbeTick();
    ProbeTick();
}

namespace {

// Once every five seconds while a session is running and the partner's character
// is on hand, and again whenever any of the numbers change. Reads only.
void ProbeTick() {
    if (!Session::SessionManager::GetInstance().IsActive()) return;

    const uintptr_t Partner = GetPartnerCharacter(5000);
    if (!Partner) return;

    uintptr_t Gm = 0, Local = 0;
    if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0xD0, &Local)) return;

    const Side Mine   = ReadSide(Local);
    const Side Theirs = ReadSide(Partner);
    if (!Mine.Ok || !Theirs.Ok) return;

    const int Out = MatrixCell(Mine.Effective, Theirs.Effective);
    const int In  = MatrixCell(Theirs.Effective, Mine.Effective);

    static ULONGLONG s_at = 0;
    static uint64_t  s_last = 0;
    const uint64_t Now = (static_cast<uint64_t>(Mine.PhantomId) << 56) |
                         (static_cast<uint64_t>(Mine.TeamByte) << 48) |
                         (static_cast<uint64_t>(Theirs.PhantomId) << 40) |
                         (static_cast<uint64_t>(Theirs.TeamByte) << 32) |
                         (static_cast<uint64_t>(static_cast<uint8_t>(Out)) << 8) |
                          static_cast<uint64_t>(static_cast<uint8_t>(In));
    const ULONGLONG Tick = GetTickCount64();
    if (Now == s_last && Tick - s_at < 5000) return;
    s_last = Now;
    s_at = Tick;

    LOG_INFO("[PVP] me: phantom id %u, team byte %u (maps %u/%u) -> team %u | partner: phantom id %u, "
             "team byte %u (maps %u/%u) -> team %u | matrix me->them %d (%s), them->me %d (%s)",
             Mine.PhantomId, Mine.TeamByte, Mine.MapFlagA, Mine.MapFlagB, Mine.Effective,
             Theirs.PhantomId, Theirs.TeamByte, Theirs.MapFlagA, Theirs.MapFlagB, Theirs.Effective,
             Out, Meaning(Out), In, Meaning(In));
}

} // namespace

} // namespace DS2Coop::Sync
