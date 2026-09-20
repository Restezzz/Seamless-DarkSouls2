// The guest walks into a boss's arena first (docs §3.47; 0.2.2 report, point 1: "the guest can
// hit the boss, but there is no health bar and the boss stands idle -- the boss should wake for
// the guest too").
//
// A boss fight starts from the host's map event script, and the condition it waits for tests the
// local player only: EventConditionChr_IsPlayerPointInside (eval exe+0x46F2C0, vtable slot 8)
// takes [GMImp+0xD0]'s position (vt[0x148]) and asks exe+0x461E10(regions [+0x10], count [+0x18],
// mode [+0x1C], position) -- mode 1 "inside any". So the host's game never sees the guest step in,
// and the guest's own copy of the script takes the other branch. The boss starts are recognised by
// their shape: in one state, region(S1, R, inside) with IsHost(S1, 1, 0) and region(S2, R, inside)
// with IsHost(S2, 0, 0) -- 37 of the 40 boss starts in the game's map scripts, and nothing else
// has it. Every condition is registered through exe+0x470A40(env, slot, condition), so the pairs
// are seen there, and the event they belong to (the task this thread is updating) is marked. From
// then on, on the host, while no fight is running and a guest is in the world and in the same map,
// that event's region condition also answers "inside" when the guest stands in the regions --
// and the host's own script starts the fight, as it would for the host.

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

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t kGameManagerImp = 0x16148F0;
constexpr uint32_t kNetRoot        = 0x1616CF8;
constexpr uint32_t kRegionEval     = 0x46F2C0;   // IsPlayerPointInside: (condition) -> AL
constexpr uint32_t kIsHostEval     = 0x46FDC0;   // IsHost: (condition) -> AL
constexpr uint32_t kCondRegister   = 0x470A40;   // (esd env, slot, condition)
constexpr uint32_t kRegionsHold    = 0x461E10;   // (regions, count, mode, float* position) -> AL
constexpr uint32_t kEvalSlot       = 0x40;       // vtable slot 8: the condition's eval
constexpr int      kRing           = 48;
constexpr uint32_t kMaxMarks       = 64;
constexpr uint32_t kMaxRegions     = 64;

using RegisterFn = void(__fastcall*)(void*, uint8_t, void*);
using EvalFn     = uint64_t(__fastcall*)(void*);
using HoldFn     = uint64_t(__fastcall*)(const void*, uint32_t, uint32_t, const float*);
using ChrPosFn   = const float*(__fastcall*)(void*, float*);

RegisterFn g_register   = nullptr;
EvalFn     g_regionEval = nullptr;

std::atomic<bool>      g_enabled{ true };            // ini boss_guest_starts
std::atomic<uint64_t>  g_marks[kMaxMarks] = {};      // (raw map << 32 | event) of boss starts
std::atomic<uint32_t>  g_markCount{ 0 };
std::atomic<uintptr_t> g_condMarks[kMaxMarks] = {};  // region conditions registered outside a task
std::atomic<uint32_t>  g_condMarkCount{ 0 };
std::atomic<uint32_t>  g_answered{ 0 };

// One registration: what kind of condition, and the fields that make up the pattern.
struct Reg {
    uintptr_t Env;
    uintptr_t Cond;
    uintptr_t Regions;
    uint32_t  Count;
    uint8_t   Slot;
    uint8_t   Kind;   // 1 region, 2 IsHost
    uint8_t   Mode;
    uint8_t   A10;
    uint8_t   A11;
};
thread_local Reg      t_ring[kRing] = {};
thread_local uint32_t t_ringNext = 0;

uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

bool InGameImage(uintptr_t P) {
    static const uintptr_t Base = ExeBase();
    static const uintptr_t End = [] {
        const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
        const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base + Dos->e_lfanew);
        return Base + Nt->OptionalHeader.SizeOfImage;
    }();
    return P >= Base && P < End;
}

bool ReadRegSafe(void* Env, uint8_t Slot, void* Cond, Reg* Out) {
    __try {
        const uintptr_t C = reinterpret_cast<uintptr_t>(Cond);
        const uintptr_t Vtbl = C ? *reinterpret_cast<const uintptr_t*>(C) : 0;
        if (!InGameImage(Vtbl)) return false;
        const uintptr_t Eval = *reinterpret_cast<const uintptr_t*>(Vtbl + kEvalSlot);
        Out->Env = reinterpret_cast<uintptr_t>(Env);
        Out->Cond = C;
        Out->Slot = Slot;
        if (Eval == ExeBase() + kRegionEval) {
            Out->Kind = 1;
            Out->Regions = *reinterpret_cast<const uintptr_t*>(C + 0x10);
            Out->Count = *reinterpret_cast<const uint32_t*>(C + 0x18);
            Out->Mode = *reinterpret_cast<const uint8_t*>(C + 0x1C);
        } else if (Eval == ExeBase() + kIsHostEval) {
            Out->Kind = 2;
            Out->A10 = *reinterpret_cast<const uint8_t*>(C + 0x10);
            Out->A11 = *reinterpret_cast<const uint8_t*>(C + 0x11);
        } else {
            Out->Kind = 0;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SameRegionsSafe(const Reg& A, const Reg& B) {
    if (A.Count != B.Count || !A.Count || A.Count > kMaxRegions || !A.Regions || !B.Regions) return false;
    if (A.Regions == B.Regions) return true;
    __try {
        return std::memcmp(reinterpret_cast<const void*>(A.Regions), reinterpret_cast<const void*>(B.Regions),
                           A.Count * sizeof(uintptr_t)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Marked(uint64_t Key) {
    const uint32_t N = g_markCount.load(std::memory_order_acquire);
    for (uint32_t I = 0; I < N && I < kMaxMarks; ++I) {
        if (g_marks[I].load(std::memory_order_relaxed) == Key) return true;
    }
    return false;
}

bool CondMarked(uintptr_t Cond) {
    const uint32_t N = g_condMarkCount.load(std::memory_order_acquire);
    for (uint32_t I = 0; I < N && I < kMaxMarks; ++I) {
        if (g_condMarks[I].load(std::memory_order_relaxed) == Cond) return true;
    }
    return false;
}

void Mark(uint64_t Key, const char* How, int32_t Event, uint32_t Map, uint32_t Regions) {
    if (Marked(Key)) return;
    const uint32_t N = g_markCount.load();
    if (N >= kMaxMarks) return;
    g_marks[N].store(Key);
    g_markCount.store(N + 1, std::memory_order_release);
    LOG_INFO("[BOSS] event %d of map %08X is a boss start (%s%u region(s)) -- a guest standing in them wakes the "
             "boss here too", Event, Map, How, Regions);
}

// The whole pattern was four conditions deep -- "the host is inside" and "someone who is not the host
// is inside", both spelled out -- and in the test of 21.09 morning not one boss start was recognised by
// it: no line of it in the host's log, so the guest walked into the Last Giant's arena and nothing
// woke. Two things are asked for now instead of four, and the thread's ring of registrations holds six
// times as many.
//
// The short pattern is the host's own branch: an IsHost(slot, 1, 0) with a region condition "inside" in
// the very same slot -- "I am the host and the player is standing in these regions", which is how every
// boss start asks. It is a marking only: it costs nothing until a guest is actually in those regions
// with no fight running (RegionEvalDetour).
void TryMarkHostBranch(const Reg& Host1) {
    const Reg* Region = nullptr;
    for (const Reg& R : t_ring) {
        if (R.Env != Host1.Env || !R.Cond || R.Slot != Host1.Slot) continue;
        if (R.Kind == 1 && R.Mode == 1 && R.Count && R.Count <= kMaxRegions) Region = &R;
    }
    if (!Region) return;
    const uintptr_t Task = CurrentEventTask();
    uint32_t Map = 0;
    int32_t Event = 0;
    if (Task && ReadEventTaskKey(Task, &Map, &Event)) {
        Mark((static_cast<uint64_t>(Map) << 32) | static_cast<uint32_t>(Event), "\"the host is inside\", ",
             Event, Map, Region->Count);
        return;
    }
    if (CondMarked(Region->Cond)) return;
    const uint32_t N = g_condMarkCount.load();
    if (N >= kMaxMarks) return;
    g_condMarks[N].store(Region->Cond);
    g_condMarkCount.store(N + 1, std::memory_order_release);
    LOG_INFO("[BOSS] a boss start outside an event task: \"the host is inside\" region condition %p marked",
             reinterpret_cast<void*>(Region->Cond));
}

// An IsHost(S2, 0, 0) just registered: the rest of the pattern among this thread's last
// registrations of the same state.
void TryMark(const Reg& Host0) {
    const Reg* RegionS2 = nullptr;
    const Reg* RegionS1 = nullptr;
    const Reg* HostS1 = nullptr;
    for (const Reg& R : t_ring) {
        if (R.Env != Host0.Env || !R.Cond) continue;
        if (R.Kind == 1 && R.Mode == 1 && R.Slot == Host0.Slot) RegionS2 = &R;
    }
    if (!RegionS2) return;
    for (const Reg& R : t_ring) {
        if (R.Env != Host0.Env || !R.Cond || R.Slot == Host0.Slot) continue;
        if (R.Kind == 2 && R.A10 == 1 && R.A11 == 0) HostS1 = &R;
    }
    if (!HostS1) return;
    for (const Reg& R : t_ring) {
        if (R.Env != Host0.Env || !R.Cond || R.Slot != HostS1->Slot) continue;
        if (R.Kind == 1 && R.Mode == 1 && SameRegionsSafe(R, *RegionS2)) RegionS1 = &R;
    }
    if (!RegionS1) return;

    const uintptr_t Task = CurrentEventTask();
    uint32_t Map = 0;
    int32_t Event = 0;
    if (Task && ReadEventTaskKey(Task, &Map, &Event)) {
        Mark((static_cast<uint64_t>(Map) << 32) | static_cast<uint32_t>(Event), "both branches, ", Event, Map,
             RegionS1->Count);
        return;
    }
    if (CondMarked(RegionS1->Cond)) return;
    const uint32_t N = g_condMarkCount.load();
    if (N >= kMaxMarks) return;
    g_condMarks[N].store(RegionS1->Cond);
    g_condMarkCount.store(N + 1, std::memory_order_release);
    LOG_INFO("[BOSS] a boss start registered outside an event task: region condition %p marked",
             reinterpret_cast<void*>(RegionS1->Cond));
}

void __fastcall RegisterDetour(void* Env, uint8_t Slot, void* Cond) {
    g_register(Env, Slot, Cond);
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    Reg R{};
    if (!ReadRegSafe(Env, Slot, Cond, &R) || R.Kind == 0) return;
    t_ring[t_ringNext++ % kRing] = R;
    if (R.Kind == 2 && R.A10 == 0 && R.A11 == 0) TryMark(R);
    if (R.Kind == 2 && R.A10 == 1 && R.A11 == 0) TryMarkHostBranch(R);
}

bool NoFightRunningSafe() {
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        const uintptr_t Events = Gm ? *reinterpret_cast<const uintptr_t*>(Gm + 0x70) : 0;
        const uintptr_t Boss = Events ? *reinterpret_cast<const uintptr_t*>(Events + 0x88) : 0;
        return Boss && *reinterpret_cast<const int32_t*>(Boss + 0x14) <= 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GuestFullyInSafe() {
    __try {
        const uintptr_t Root = *reinterpret_cast<const uintptr_t*>(ExeBase() + kNetRoot);
        const uintptr_t Mp = Root ? *reinterpret_cast<const uintptr_t*>(Root + 0x18) : 0;
        if (!Mp) return false;
        uintptr_t It = *reinterpret_cast<const uintptr_t*>(Mp + 0x48);
        const uintptr_t End = *reinterpret_cast<const uintptr_t*>(Mp + 0x50);
        for (int Guard = 0; It && It < End && Guard < 16; It += 8, ++Guard) {
            const uintptr_t Ctrl = *reinterpret_cast<const uintptr_t*>(It);
            if (Ctrl && *reinterpret_cast<const int32_t*>(Ctrl + 0x150) == 0x10) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// Whether the partner's character stands in this condition's regions; its position out.
bool PartnerInsideSafe(void* Cond, uintptr_t Partner, float* Pos) {
    __try {
        const uintptr_t C = reinterpret_cast<uintptr_t>(Cond);
        const uintptr_t Regions = *reinterpret_cast<const uintptr_t*>(C + 0x10);
        const uint32_t Count = *reinterpret_cast<const uint32_t*>(C + 0x18);
        const uint8_t Mode = *reinterpret_cast<const uint8_t*>(C + 0x1C);
        if (!Regions || !Count || Mode != 1) return false;
        const uintptr_t Vtbl = *reinterpret_cast<const uintptr_t*>(Partner);
        if (!InGameImage(Vtbl)) return false;
        const uintptr_t PosAt = *reinterpret_cast<const uintptr_t*>(Vtbl + 0x148);
        if (!InGameImage(PosAt)) return false;
        alignas(16) float Buffer[4] = {};
        const float* P = reinterpret_cast<ChrPosFn>(PosAt)(reinterpret_cast<void*>(Partner), Buffer);
        if (!P) return false;
        Pos[0] = P[0];
        Pos[1] = P[1];
        Pos[2] = P[2];
        return (reinterpret_cast<HoldFn>(ExeBase() + kRegionsHold)(reinterpret_cast<const void*>(Regions), Count, 1, P)
                & 0xFF) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t __fastcall RegionEvalDetour(void* Cond) {
    const uint64_t Stock = g_regionEval(Cond);
    if ((Stock & 0xFF) || !g_enabled.load(std::memory_order_relaxed)) return Stock;
    if (!g_markCount.load(std::memory_order_relaxed) && !g_condMarkCount.load(std::memory_order_relaxed)) return Stock;
    uint64_t Key = 0;
    const uintptr_t Task = CurrentEventTask();
    uint32_t Map = 0;
    int32_t Event = 0;
    const bool HaveKey = Task && ReadEventTaskKey(Task, &Map, &Event);
    if (HaveKey) Key = (static_cast<uint64_t>(Map) << 32) | static_cast<uint32_t>(Event);
    if (!(HaveKey && Marked(Key)) && !CondMarked(reinterpret_cast<uintptr_t>(Cond))) return Stock;
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive() || !Lobby.IsHost() || !NoFightRunningSafe() || !GuestFullyInSafe()) return Stock;
    const uintptr_t Partner = GetPartnerCharacter(1000);
    if (!Partner || !PlayersShareMap()) return Stock;
    float Pos[3] = {};
    if (!PartnerInsideSafe(Cond, Partner, Pos)) return Stock;
    static uint64_t s_told = 0;
    if (s_told != Key || !HaveKey) {
        s_told = Key;
        LOG_INFO("[BOSS] the guest stands in the arena of event %d (%.1f, %.1f, %.1f) -- its start condition "
                 "answered \"inside\" here, so the boss wakes (%u so far)", Event, Pos[0], Pos[1], Pos[2],
                 g_answered.load() + 1);
    }
    g_answered.fetch_add(1);
    return (Stock & ~static_cast<uint64_t>(0xFF)) | 1;
}

} // namespace

// The last way in: the host's own script has just asked for a battle to start, so the event task it
// asked from is a boss start, whatever its conditions looked like. From then on the guest can wake that
// boss by itself -- which is what a second try at a boss looks like, and a boss is rarely killed the
// first time (21.09 morning, report 8: "I walk in and the boss is not woken; the host walks in and it
// is").
void NoteBossStartTask(int32_t Battle) {
    if (!g_enabled.load()) return;
    const uintptr_t Task = CurrentEventTask();
    uint32_t Map = 0;
    int32_t Event = 0;
    if (!Task || !ReadEventTaskKey(Task, &Map, &Event)) return;
    const uint64_t Key = (static_cast<uint64_t>(Map) << 32) | static_cast<uint32_t>(Event);
    if (Marked(Key)) return;
    const uint32_t N = g_markCount.load();
    if (N >= kMaxMarks) return;
    g_marks[N].store(Key);
    g_markCount.store(N + 1, std::memory_order_release);
    LOG_INFO("[BOSS] event %d of map %08X started battle %d -- it is a boss start, so a guest standing in its "
             "regions wakes it here too", Event, Map, Battle);
}

bool InstallBossArena(bool Enabled) {
    static bool Installed = false;
    g_enabled.store(Enabled);
    if (!Installed) {
        Installed = true;
        auto& Hooks = Hooks::HookManager::GetInstance();
        if (!Hooks.InstallHook(reinterpret_cast<void*>(ExeBase() + kCondRegister),
                               reinterpret_cast<void*>(&RegisterDetour), reinterpret_cast<void**>(&g_register))) {
            g_register = nullptr;
        }
        if (!Hooks.InstallHook(reinterpret_cast<void*>(ExeBase() + kRegionEval),
                               reinterpret_cast<void*>(&RegionEvalDetour), reinterpret_cast<void**>(&g_regionEval))) {
            g_regionEval = nullptr;
        }
        LOG_INFO("[BOSS] a guest in the arena wakes the boss: condition registration exe+0x%X %s, region test "
                 "exe+0x%X %s", kCondRegister, g_register ? "hooked" : "NOT hooked", kRegionEval,
                 g_regionEval ? "hooked" : "NOT hooked");
    }
    return g_register && g_regionEval;
}

} // namespace DS2Coop::Sync
