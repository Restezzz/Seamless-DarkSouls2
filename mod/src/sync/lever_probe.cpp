// Probe (20.09, the lever gate between Majula and the Forest: "the host opened the doors -- they
// opened for me, but only once; the second time they stayed as they were, and after he opened them
// I cannot open them myself without joining again"). Nothing here changes the game.
//
// The gate is Majula's event 8000: 0x7FFFFFDC(gate 10043010, lever 10041015 on the Majula side,
// lever 10041020 on the Forest side, regions 800001, 800002, 800010, 800011). Each game runs its own
// copy (a guest takes the owner's branch, the mod answers ESD 130602 "no"), the gate is set to take
// no state from the network (131651(gate, 0): StateActCtrl flag 0x80), the levers are not: a lever's
// state changes go to the partner as map object packets '$' '%' '&' ''' (MapStateActPacketReceiver,
// exe+0x1F48C0), and a pull by the partner's copy runs here through the action executor. The script
// waits for a lever in state 74 or 84, asks the gate for 70 (opening), waits for 30 (open), and
// closes it (80, then 10) when the local player stands in 800001/800002; in 800010/800011 it starts
// over. Which of these steps stops on the guest the log does not say yet, so this watches the
// three objects' state machines (StateActCtrl: +0x1C state, +0x1D the one before, +0x1E asked,
// +0x20 flags -- 0x1 change asked, 0x40 prompts off, 0x80 no network, 0x100/0x200 an action taken
// and let go, bits 12-25 a sequence number; +0x18 the character using it) and every map object
// packet from the partner that names one of them (docs §3.52).

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

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t  kGameManagerImp = 0x16148F0;
constexpr uint32_t  kFindObject     = 0x3C1B90;   // (raw map, entity id) -> map object or 0
constexpr uint32_t  kStateComponent = 0x1CA790;   // (object + 0xB8, object) -> the object's state-act component
constexpr uint32_t  kObjectByHandle = 0x17BD90;   // (u32* handle) -> map object or 0
constexpr uint32_t  kStateActCtrlVt = 0x10CF668;  // StateActCtrl::vftable
constexpr uint32_t  kStateActRecv   = 0x1F48C0;   // (receiver, type, data, size, sender)
constexpr uint32_t  kMajula         = 0x0A040000;
constexpr ULONGLONG kPollMs         = 250;
constexpr ULONGLONG kCountEveryMs   = 30000;

struct Watched {
    int32_t     Id;
    const char* Name;
};
constexpr Watched kWatched[] = {
    { 10043010, "gate" },
    { 10041015, "lever on the Majula side" },
    { 10041020, "lever on the Forest side" },
};
constexpr size_t kWatchedCount = sizeof(kWatched) / sizeof(kWatched[0]);

struct CtrlView {
    uint8_t  Cur;
    uint8_t  Before;
    uint8_t  Asked;
    uint32_t Flags;
    uint64_t User;
};

std::atomic<uintptr_t> g_objects[kWatchedCount] = {};   // the objects as last found (game thread writes)
std::atomic<uint32_t>  g_packets[4] = {};               // '$' '%' '&' ''' from the partner, any object

using RecvFn = void(__fastcall*)(void*, char, const uint8_t*, uint32_t, void*);
RecvFn g_recvOriginal = nullptr;

uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

uintptr_t FindObjectSafe(int32_t Id) {
    __try {
        if (!*reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp)) return 0;
        return reinterpret_cast<uintptr_t(__fastcall*)(uint32_t, int32_t)>(ExeBase() + kFindObject)(kMajula, Id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool ReadCtrlSafe(uintptr_t Obj, CtrlView* View) {
    __try {
        if (!Obj) return false;
        const uintptr_t Comp = reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t, uintptr_t)>(
            ExeBase() + kStateComponent)(Obj + 0xB8, Obj);
        if (!Comp) return false;
        const uintptr_t Ctrl = *reinterpret_cast<const uintptr_t*>(Comp + 0x48);
        if (!Ctrl || *reinterpret_cast<const uintptr_t*>(Ctrl) != ExeBase() + kStateActCtrlVt) return false;
        View->Cur = *reinterpret_cast<const uint8_t*>(Ctrl + 0x1C);
        View->Before = *reinterpret_cast<const uint8_t*>(Ctrl + 0x1D);
        View->Asked = *reinterpret_cast<const uint8_t*>(Ctrl + 0x1E);
        View->Flags = *reinterpret_cast<const uint32_t*>(Ctrl + 0x20);
        View->User = *reinterpret_cast<const uint64_t*>(Ctrl + 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t ObjectOfPacketSafe(const uint8_t* Data) {
    __try {
        return reinterpret_cast<uintptr_t(__fastcall*)(const uint8_t*)>(ExeBase() + kObjectByHandle)(Data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool ReadMeSafe(float* Me) {
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        const uintptr_t Player = Gm ? *reinterpret_cast<const uintptr_t*>(Gm + 0xD0) : 0;
        if (!Player) return false;
        for (int I = 0; I < 3; ++I) Me[I] = *reinterpret_cast<const float*>(Player + 0x90 + I * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The flags that tell a step, without the sequence number (logged on its own).
uint32_t StepFlags(uint32_t Flags) {
    return Flags & 0xFFFu;
}

uint32_t SequenceOf(uint32_t Flags) {
    return (Flags >> 12) & 0x3FFFu;
}

int WatchedIndex(uintptr_t Obj) {
    if (!Obj) return -1;
    for (size_t I = 0; I < kWatchedCount; ++I) {
        if (g_objects[I].load(std::memory_order_relaxed) == Obj) return static_cast<int>(I);
    }
    return -1;
}

void __fastcall StateActRecvDetour(void* Receiver, char Type, const uint8_t* Data, uint32_t Size, void* Sender) {
    const int Kind = Type - '$';
    int Index = -1;
    CtrlView Before{};
    bool HaveBefore = false;
    if (Kind >= 0 && Kind < 4 && Data && Size >= 12) {
        g_packets[Kind].fetch_add(1, std::memory_order_relaxed);
        Index = WatchedIndex(ObjectOfPacketSafe(Data));
        if (Index >= 0) HaveBefore = ReadCtrlSafe(g_objects[Index].load(), &Before);
    }
    g_recvOriginal(Receiver, Type, Data, Size, Sender);
    if (Index < 0) return;
    CtrlView After{};
    const bool HaveAfter = ReadCtrlSafe(g_objects[Index].load(), &After);
    LOG_INFO("[LEVER] from the partner: '%c' for the %s -- state %u, number %u; mine %u -> %u (asked %u, flags 0x%03X, "
             "seq %u)", Type, kWatched[Index].Name, Data[4], *reinterpret_cast<const uint32_t*>(Data + 8),
             HaveBefore ? Before.Cur : 0xFFu, HaveAfter ? After.Cur : 0xFFu, HaveAfter ? After.Asked : 0xFFu,
             HaveAfter ? StepFlags(After.Flags) : 0u, HaveAfter ? SequenceOf(After.Flags) : 0u);
}

} // namespace

void InstallLeverProbe() {
    static bool Installed = false;
    if (Installed) return;
    Installed = true;
    if (!Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + kStateActRecv),
                                                       reinterpret_cast<void*>(&StateActRecvDetour),
                                                       reinterpret_cast<void**>(&g_recvOriginal))) {
        LOG_WARNING("[LEVER] could not hook exe+0x%X (map object packets) -- the lever probe watches states only",
                    kStateActRecv);
    }
}

// Game thread, from the enemy generator update.
void LeverProbeTick() {
    static ULONGLONG s_at = 0, s_countAt = 0;
    static CtrlView s_last[kWatchedCount] = {};
    static bool s_have[kWatchedCount] = {};
    const ULONGLONG Now = GetTickCount64();
    if (Now - s_at < kPollMs) return;
    s_at = Now;
    if (!Session::SessionManager::GetInstance().IsActive()) {
        for (size_t I = 0; I < kWatchedCount; ++I) {
            g_objects[I].store(0);
            s_have[I] = false;
        }
        return;
    }
    if (Now - s_countAt >= kCountEveryMs) {
        s_countAt = Now;
        const uint32_t A = g_packets[0].exchange(0), B = g_packets[1].exchange(0);
        const uint32_t C = g_packets[2].exchange(0), D = g_packets[3].exchange(0);
        if (A + B + C + D) {
            LOG_INFO("[LEVER] map object packets from the partner in 30 s: '$' %u, '%%' %u, '&' %u, ''' %u", A, B, C, D);
        }
    }
    for (size_t I = 0; I < kWatchedCount; ++I) {
        const uintptr_t Obj = FindObjectSafe(kWatched[I].Id);
        if (Obj != g_objects[I].exchange(Obj)) {
            s_have[I] = false;
            if (Obj) LOG_INFO("[LEVER] the %s (%d) is map object %p here", kWatched[I].Name, kWatched[I].Id,
                              reinterpret_cast<void*>(Obj));
        }
        CtrlView View{};
        if (!Obj || !ReadCtrlSafe(Obj, &View)) continue;
        const CtrlView& Last = s_last[I];
        const bool Same = s_have[I] && Last.Cur == View.Cur && Last.Asked == View.Asked &&
                          StepFlags(Last.Flags) == StepFlags(View.Flags) && Last.User == View.User;
        s_last[I] = View;
        s_have[I] = true;
        if (Same) continue;
        float Me[3] = {};
        const bool Where = ReadMeSafe(Me);
        LOG_INFO("[LEVER] the %s: state %u (before %u, asked %u), flags 0x%03X, seq %u, user 0x%llX; I stand at "
                 "(%.1f, %.1f, %.1f)", kWatched[I].Name, View.Cur, View.Before, View.Asked, StepFlags(View.Flags),
                 SequenceOf(View.Flags), static_cast<unsigned long long>(View.User), Where ? Me[0] : 0.0f,
                 Where ? Me[1] : 0.0f, Where ? Me[2] : 0.0f);
    }
}

} // namespace DS2Coop::Sync
