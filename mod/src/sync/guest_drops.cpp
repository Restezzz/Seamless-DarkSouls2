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
constexpr ULONGLONG kRolledMemoryMs = 60000;
constexpr int       kQueueSize      = 32;
constexpr int       kRolledSize     = 64;

using ReceiveFn = uint64_t(__fastcall*)(void*, char, void*, uint32_t, void*);
using DecodeFn  = void(__fastcall*)(void* out, void* packet);
using RollFn    = uint64_t(__fastcall*)(void* record, char flag);
using GenFn     = void*(__fastcall*)(void* ref);
using TimingFn  = void*(__fastcall*)(int32_t anim);

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
int RollQueuedSafe(uint8_t* Packet, int32_t* AnimOut) {
    __try {
        uint8_t Rec[0x50] = {};
        uint8_t Copy[0x18];
        std::memcpy(Copy, Packet, sizeof(Copy));
        reinterpret_cast<DecodeFn>(ExeBase() + kDeadDecode)(Rec, Copy);
        const uintptr_t Record = *reinterpret_cast<const uintptr_t*>(Rec + 8);
        const int32_t Anim = *reinterpret_cast<const int32_t*>(Copy + 0x10);
        *AnimOut = Anim;
        if (!Record) return 0;
        const void* Timing = reinterpret_cast<TimingFn>(ExeBase() + kDeathTiming)(Anim);
        if (!Timing) return 0;
        if (*reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(Timing) + 0xB) & 0x80) return 0;   // [t+8] < 0: no drop
        const void* GenCtrl = reinterpret_cast<GenFn>(ExeBase() + kGenCtrlOf)(reinterpret_cast<void*>(Record + 0x10));
        if (!GenCtrl) return 0;
        if (!*reinterpret_cast<const uintptr_t*>(Record + 0x60)) return 0;
        if (!*reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(GenCtrl) + 0x58)) return 0;
        const bool Already = RolledRecently(Record, GetTickCount64());
        if (Already) return 0;
        RollDetour(reinterpret_cast<void*>(Record), 0);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
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
        int32_t Anim = 0;
        const int Rolled = RollQueuedSafe(Packet, &Anim);
        if (Rolled == 1) {
            const uint32_t N = g_ownRolls.fetch_add(1) + 1;
            if (N <= 10 || N % 50 == 0) {
                LOG_INFO("[LOOT] the host's kill (death animation %d): the enemy's drop rolled here too (%u so far)", Anim, N);
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
