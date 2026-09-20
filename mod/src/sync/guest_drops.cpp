// Enemy drops for a guest, for every enemy (docs §3.33, §3.40).
//
// The two owner checks the last build opened (exe+0x16F010 from exe+0x13D465,
// exe+0x5135F0 from exe+0x1E25E0) fired twice in a whole evening as a guest -- for
// the boss and once more -- and never for the dozens of ordinary enemies, the
// dragon with its certain drop among them. Nothing in the code on the guest's side
// stops an ordinary enemy: the stop is at runtime, somewhere between the host's
// kill packet and the guest's own death handler for its copy of the enemy.
//
// So the guest does not wait for that handler. The host's kill arrives as packet
// '7' (ChrDeadPacketReceiver, exe+0x161A10); the packet names the enemy and its
// death animation, and the game's own decode (exe+0x161D60) finds the enemy's
// generator record. A moment later -- after the corpse exists, and only if the
// host's own timing table for that death drops anything at all -- the guest rolls
// that record's drop itself with the game's roll, exe+0x16DD70, which rolls the
// normal lot here (through the lot check opened before) with the guest's own dice
// and lays a drop only this player sees and can pick up.
//
// Every roll goes through one place, a hook on exe+0x16DD70: a record that has
// rolled in the last minute does not roll again, whichever path asks -- the
// guest's own handler when it does run, the "enemy not loaded" path inside the
// receiver, or this queue. A rest forgets them all.
//
// Probe (17.09, point 7: rarely an enemy dead at the host's stays up for the guest --
// it cannot be hit and still hits -- until the guest picks up its drop). The kill
// packet only posts a death request through the copy's [chr+0xE8], and the copy's
// death handler (exe+0x13C720) ignores it while some state is pending. So each kill
// whose copy still has HP when its drop is rolled is looked at again five seconds
// later, and a copy still standing then is written down with the fields that decide.
//
// Probe (17.09, points 9 and 16: a crystal lizard and the shade by the fire dropped
// for the host only). Every host kill this queue does NOT roll is written down with
// the reason and the numbers behind it, and every roll with whether a drop came of it.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../../include/sync.h"
#include "../../include/hooks.h"
#include "../../include/session.h"
#include "../../include/utils.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t  kDeadReceive    = 0x161A10;   // (receiver, id, data, length, arg5)
constexpr uint32_t  kDeadDecode     = 0x161D60;   // (out 0x50: character, record, ...; packet 0x18)
constexpr uint32_t  kDropRoll       = 0x16DD70;   // (generator record, flag)
constexpr uint32_t  kGenCtrlOf      = 0x17B7E0;   // (object reference*) -> generator controller
constexpr uint32_t  kDeathTiming    = 0x16DA90;   // (death animation) -> timing table
constexpr uint32_t  kNetRoot        = 0x1616CF8;
constexpr uint32_t  kJoinCtrlVtable = 0x10D7BD8;
constexpr ULONGLONG kRollDelayMs    = 1500;
// Long enough for the three paths that can roll one death to race each other (the receiver, the
// queue five seconds later, and this player's own death handler), and no longer: the same enemy
// standing again after a rest and killed a second time is a new kill and rolls again (21.09
// morning, report 4: "killed the snake and nothing dropped, and I had not killed it at home").
constexpr ULONGLONG kRolledMemoryMs = 20000;
constexpr int       kQueueSize      = 32;
constexpr int       kRolledSize     = 64;
constexpr ULONGLONG kStuckCheckMs   = 5000;
constexpr int       kStuckWatches   = 8;
constexpr uint32_t  kDropMgrGet     = 0x1E6550;   // -> drop manager
constexpr uint32_t  kDropMgrHas     = 0x1E41F0;   // (manager, handle*) -> the drop is live
constexpr uint32_t  kNoRollsLogged  = 60;

using ReceiveFn = uint64_t(__fastcall*)(void*, char, void*, uint32_t, void*);
using DecodeFn  = void(__fastcall*)(void* out, void* packet);
using RollFn    = uint64_t(__fastcall*)(void* record, char flag);
using GenFn     = void*(__fastcall*)(void* ref);
using TimingFn  = void*(__fastcall*)(int32_t anim);
using DropMgrFn = uintptr_t(__fastcall*)();
using DropHasFn = bool(__fastcall*)(uintptr_t manager, uint64_t* handle);

// Why a host's kill rolled nothing here.
enum NoRoll : int { kRolled = 0, kNoRecord, kNoTiming, kTimingNoDrop, kNoGenerator, kNoLot, kNoGenLink, kAlreadyRolled };
const char* NoRollName(int Why) {
    switch (Why) {
        case kNoRecord:      return "the packet names no generator record here";
        case kNoTiming:      return "no death timing row for its animation";
        case kTimingNoDrop:  return "its death timing row says no drop";
        case kNoGenerator:   return "no generator controller for the record";
        case kNoLot:         return "the record has no lot pointer (+0x60)";
        case kNoGenLink:     return "the controller has nothing at +0x58";
        case kAlreadyRolled: return "its drop was rolled in the last minute";
        default:             return "rolled";
    }
}

struct RollFacts {
    uintptr_t Chr;
    uintptr_t Record;
    int32_t   Anim;
    int32_t   Why;
    int32_t   RecordId;      // [rec+0x68]
    int32_t   LotValue;      // *[rec+0x60]
    int32_t   TimingDrop;    // [timing+8]
    bool      DropLive;      // after the roll: [rec+0x88] is a live drop
};
std::atomic<uint32_t> g_noRollsLogged{ 0 };

ReceiveFn g_receive = nullptr;
RollFn    g_roll    = nullptr;
std::atomic<bool> g_enabled{ true };

struct QueuedKill {
    uint8_t   Packet[0x18];
    ULONGLONG Due;
    bool      Used;
};
std::mutex g_queueMutex;
QueuedKill g_queue[kQueueSize] = {};

struct RolledRecord {
    uintptr_t Record;
    ULONGLONG At;
};
std::mutex   g_rolledMutex;
RolledRecord g_rolled[kRolledSize] = {};

// Game thread only: copies that still stood when their drop was rolled.
struct StuckWatch {
    uintptr_t Chr;
    uintptr_t Record;
    int32_t   Anim;
    ULONGLONG Due;
};
StuckWatch g_stuck[kStuckWatches] = {};
std::atomic<uint32_t> g_queuedKills{ 0 }, g_ownRolls{ 0 }, g_secondRolls{ 0 };

uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

bool GuestInHostWorld() {
    __try {
        const uintptr_t Root = *reinterpret_cast<const uintptr_t*>(ExeBase() + kNetRoot);
        if (!Root) return false;
        const uintptr_t Mp = *reinterpret_cast<const uintptr_t*>(Root + 0x18);
        if (!Mp) return false;
        const uintptr_t Ctrl = *reinterpret_cast<const uintptr_t*>(Mp + 0x40);
        if (!Ctrl || *reinterpret_cast<const uintptr_t*>(Ctrl) != ExeBase() + kJoinCtrlVtable) return false;
        return *reinterpret_cast<const int32_t*>(Ctrl + 0xF8) == 7;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RolledRecently(uintptr_t Record, ULONGLONG Now) {
    std::lock_guard<std::mutex> Lock(g_rolledMutex);
    for (const RolledRecord& R : g_rolled) {
        if (R.Record == Record && R.At && Now - R.At < kRolledMemoryMs) return true;
    }
    return false;
}

void MarkRolled(uintptr_t Record, ULONGLONG Now) {
    std::lock_guard<std::mutex> Lock(g_rolledMutex);
    int Oldest = 0;
    for (int I = 0; I < kRolledSize; ++I) {
        if (g_rolled[I].Record == Record || !g_rolled[I].At) {
            Oldest = I;
            break;
        }
        if (g_rolled[I].At < g_rolled[Oldest].At) Oldest = I;
    }
    g_rolled[Oldest] = RolledRecord{ Record, Now };
}

void ClearDropExpected(uintptr_t Record) {
    __try {
        *reinterpret_cast<uint8_t*>(Record + 0x76) &= static_cast<uint8_t>(~8u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

uint64_t __fastcall RollDetour(void* Record, char Flag) {
    if (!Record || Flag != 0 || !g_enabled.load() || !GuestInHostWorld()) return g_roll(Record, Flag);
    const uintptr_t Id = reinterpret_cast<uintptr_t>(Record);
    const ULONGLONG Now = GetTickCount64();
    if (RolledRecently(Id, Now)) {
        const uint32_t N = g_secondRolls.fetch_add(1) + 1;
        if (N <= 5 || N % 100 == 0) {
            LOG_INFO("[LOOT] a second roll for the same enemy's death -- skipped (%u so far)", N);
        }
        return 0;
    }
    MarkRolled(Id, Now);
    const uint64_t R = g_roll(Record, Flag);
    ClearDropExpected(Id);
    return R;
}

bool CopyPacketSafe(uint8_t* Dst, const void* Src) {
    __try {
        std::memcpy(Dst, Src, 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The receiver may run on the game's network side: nothing here walks game
// structures. The packet is only queued; the tick, on the game thread, decides
// whether this player is a guest in the host's world at all.
uint64_t __fastcall DeadReceiveDetour(void* Receiver, char Id, void* Data, uint32_t Length, void* Arg5) {
    const uint64_t R = g_receive(Receiver, Id, Data, Length, Arg5);
    if (Id != '7' || Length != 0x18 || !Data || !g_enabled.load() ||
        !Session::SessionManager::GetInstance().IsActive()) {
        return R;
    }
    uint8_t Packet[0x18];
    if (!CopyPacketSafe(Packet, Data)) return R;
    const ULONGLONG Due = GetTickCount64() + kRollDelayMs;
    std::lock_guard<std::mutex> Lock(g_queueMutex);
    for (QueuedKill& Q : g_queue) {
        if (Q.Used) continue;
        std::memcpy(Q.Packet, Packet, sizeof(Q.Packet));
        Q.Due = Due;
        Q.Used = true;
        g_queuedKills.fetch_add(1);
        break;
    }
    return R;
}

// The roll for one queued kill: decode, check the host dropped anything for that
// death, check the record is whole, roll through the filter. 1 rolled, 0 nothing
// to roll, -1 threw.
int RollQueuedSafe(uint8_t* Packet, RollFacts* F) {
    __try {
        uint8_t Rec[0x50] = {};
        uint8_t Copy[0x18];
        std::memcpy(Copy, Packet, sizeof(Copy));
        reinterpret_cast<DecodeFn>(ExeBase() + kDeadDecode)(Rec, Copy);
        const uintptr_t Record = *reinterpret_cast<const uintptr_t*>(Rec + 8);
        F->Anim = *reinterpret_cast<const int32_t*>(Copy + 0x10);
        F->Chr = *reinterpret_cast<const uintptr_t*>(Rec);
        F->Record = Record;
        F->Why = kNoRecord;
        if (!Record) return 0;
        F->RecordId = *reinterpret_cast<const int32_t*>(Record + 0x68);
        const uintptr_t LotPtr = *reinterpret_cast<const uintptr_t*>(Record + 0x60);
        F->LotValue = LotPtr ? *reinterpret_cast<const int32_t*>(LotPtr) : 0;
        const void* Timing = reinterpret_cast<TimingFn>(ExeBase() + kDeathTiming)(F->Anim);
        F->Why = kNoTiming;
        if (!Timing) return 0;
        F->TimingDrop = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(Timing) + 8);
        F->Why = kTimingNoDrop;
        if (F->TimingDrop < 0) return 0;
        const void* GenCtrl = reinterpret_cast<GenFn>(ExeBase() + kGenCtrlOf)(reinterpret_cast<void*>(Record + 0x10));
        F->Why = kNoGenerator;
        if (!GenCtrl) return 0;
        F->Why = kNoLot;
        if (!LotPtr) return 0;
        F->Why = kNoGenLink;
        if (!*reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(GenCtrl) + 0x58)) return 0;
        F->Why = kAlreadyRolled;
        if (RolledRecently(Record, GetTickCount64())) return 0;
        RollDetour(reinterpret_cast<void*>(Record), 0);
        F->Why = kRolled;
        const uintptr_t Mgr = reinterpret_cast<DropMgrFn>(ExeBase() + kDropMgrGet)();
        F->DropLive = Mgr && reinterpret_cast<DropHasFn>(ExeBase() + kDropMgrHas)(Mgr, reinterpret_cast<uint64_t*>(Record + 0x88));
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

struct CopyState {
    bool     Ok;
    int32_t  Hp;
    uintptr_t Controller;   // [chr+0xE8]: the death request goes through it
    int32_t  Pending5FC;
    int32_t  Wait5D0;
    uint32_t Bits4C8;
    uint8_t  Request759;
};

CopyState ReadCopySafe(uintptr_t Chr) {
    CopyState S{};
    if (!Chr) return S;
    __try {
        S.Hp = *reinterpret_cast<const int32_t*>(Chr + 0x168);
        S.Controller = *reinterpret_cast<const uintptr_t*>(Chr + 0xE8);
        const uintptr_t Status = *reinterpret_cast<const uintptr_t*>(Chr + 0xB8);
        if (Status) {
            S.Pending5FC = *reinterpret_cast<const int32_t*>(Status + 0x5FC);
            S.Wait5D0 = *reinterpret_cast<const int32_t*>(Status + 0x5D0);
            S.Bits4C8 = *reinterpret_cast<const uint32_t*>(Status + 0x4C8);
            S.Request759 = *reinterpret_cast<const uint8_t*>(Status + 0x759);
        }
        S.Ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        S.Ok = false;
    }
    return S;
}

void WatchIfStanding(uintptr_t Chr, uintptr_t Record, int32_t Anim, ULONGLONG Now) {
    const CopyState S = ReadCopySafe(Chr);
    if (!S.Ok || S.Hp <= 0) return;
    for (StuckWatch& W : g_stuck) {
        if (W.Chr) continue;
        W = StuckWatch{ Chr, Record, Anim, Now + kStuckCheckMs };
        return;
    }
}

void CheckStuckCopies(ULONGLONG Now) {
    for (StuckWatch& W : g_stuck) {
        if (!W.Chr || Now < W.Due) continue;
        const StuckWatch Was = W;
        W = StuckWatch{};
        const CopyState S = ReadCopySafe(Was.Chr);
        if (!S.Ok || S.Hp <= 0) continue;
        LOG_WARNING("[LOOT] probe: an enemy the host killed (death animation %d) still stands here %llu ms later -- "
                    "HP %d, controller %s, status +0x5FC %d, +0x5D0 %d, +0x4C8 0x%08X, death request +0x759 %u "
                    "(character 0x%llX, record 0x%llX)", Was.Anim,
                    static_cast<unsigned long long>(kRollDelayMs + kStuckCheckMs), S.Hp,
                    S.Controller ? "present" : "NONE", S.Pending5FC, S.Wait5D0, S.Bits4C8, S.Request759,
                    static_cast<unsigned long long>(Was.Chr), static_cast<unsigned long long>(Was.Record));
    }
}

bool HookAt(uint32_t Rva, void* Detour, void** Original, const char* What) {
    if (Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + Rva), Detour, Original)) {
        return true;
    }
    LOG_WARNING("[LOOT] could not hook exe+0x%X (%s)", Rva, What);
    return false;
}

} // namespace

bool InstallGuestDrops(bool Enabled) {
    static bool Installed = false;
    g_enabled.store(Enabled);
    if (!Installed) {
        Installed = true;
        HookAt(kDropRoll, reinterpret_cast<void*>(&RollDetour), reinterpret_cast<void**>(&g_roll), "the enemy drop roll");
        if (g_roll) {
            HookAt(kDeadReceive, reinterpret_cast<void*>(&DeadReceiveDetour), reinterpret_cast<void**>(&g_receive),
                   "the enemy death packet");
        }
    }
    LOG_INFO("[LOOT] a guest rolls every enemy's drop the host kills or it kills: %s",
             Enabled && g_receive ? "on" : "off");
    return g_receive != nullptr;
}

void GuestDropsTick() {
    if (!g_receive) return;
    const bool Active = g_enabled.load() && GuestInHostWorld();
    CheckStuckCopies(GetTickCount64());
    uint8_t Packet[0x18];
    for (int Pass = 0; Pass < 8; ++Pass) {
        bool Have = false;
        {
            std::lock_guard<std::mutex> Lock(g_queueMutex);
            const ULONGLONG Now = GetTickCount64();
            for (QueuedKill& Q : g_queue) {
                if (!Q.Used) continue;
                if (!Active) {
                    Q.Used = false;
                    continue;
                }
                if (Now < Q.Due) continue;
                std::memcpy(Packet, Q.Packet, sizeof(Packet));
                Q.Used = false;
                Have = true;
                break;
            }
        }
        if (!Have) return;
        RollFacts F{};
        const int Rolled = RollQueuedSafe(Packet, &F);
        if (Rolled >= 0 && F.Chr) WatchIfStanding(F.Chr, F.Record, F.Anim, GetTickCount64());
        if (Rolled == 1) {
            const uint32_t N = g_ownRolls.fetch_add(1) + 1;
            if (N <= 10 || N % 50 == 0 || !F.DropLive) {
                LOG_INFO("[LOOT] the host's kill (death animation %d, record id %d, lot %d): the enemy's drop rolled "
                         "here too -- %s (%u so far)", F.Anim, F.RecordId, F.LotValue,
                         F.DropLive ? "a drop lies there" : "no drop came of it", N);
            }
        } else if (Rolled == 0) {
            if (g_noRollsLogged.fetch_add(1) < kNoRollsLogged) {
                LOG_INFO("[LOOT] probe: the host's kill (death animation %d, record id %d, lot %d, timing drop %d) "
                         "rolled nothing here: %s", F.Anim, F.RecordId, F.LotValue, F.TimingDrop, NoRollName(F.Why));
            }
        } else if (Rolled < 0) {
            LOG_WARNING("[LOOT] rolling the drop of a host's kill threw -- skipped");
        }
    }
}

void ForgetGuestDropRolls() {
    std::lock_guard<std::mutex> Lock(g_rolledMutex);
    for (RolledRecord& R : g_rolled) R = RolledRecord{};
}

} // namespace DS2Coop::Sync
