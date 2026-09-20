// Map objects a session keeps local, and the probe that found why (20.09-21.09, the lever gate between
// Majula and the Forest: "the host opened the doors -- they opened for me, but only once, and after the
// first pull I cannot pull either lever again").
//
// **The fix.** A state a map object enters is held on a guest until the host confirms it: exe+0x240270
// leaves the current state (+0x1C) alone and writes only the one it is going to (+0x1D) when the caller
// asks for that (its fourth argument, which the ctrl's own vt[0x20] sets), the new state is of the kind
// that gets confirmed, and exe+0x247CD0 says this game is a session's client. The confirmation is packet
// '&' (exe+0x23FD10) -- and an object whose event called 131651(obj, 0) drops every such packet, flag
// 0x80. Majula's gate event does exactly that (each player keeps its own gate), and the guest's own copy
// of that event closes the gate again about 14 s after it opened: the gate then sat at "state 30, going
// to 80" for good, the script waited for a state 10 that could never come, and while it waits it holds
// both levers' prompts off (flag 0x40 on them). Walking off into the far regions ended the script and
// reset the gate, which is why it worked once. So for an object that takes no state from the network the
// fourth argument is cleared here: it changes state at once, as it does for a host (ini
// map_objects_local).
//
// The gate is Majula's event 8000: 0x7FFFFFDC(gate 10043010, lever 10041015 on the Majula side,
// lever 10041020 on the Forest side, regions 800001, 800002, 800010, 800011). Each game runs its own
// copy (a guest takes the owner's branch, the mod answers ESD 130602 "no"), the gate is set to take
// no state from the network (131651(gate, 0): StateActCtrl flag 0x80), the levers are not: a lever's
// state changes go to the partner as map object packets '$' '%' '&' ''' (MapStateActPacketReceiver,
// exe+0x1F48C0), and a pull by the partner's copy runs here through the action executor. The script
// waits for a lever in state 74 or 84, asks the gate for 70 (opening), waits for 30 (open) and closes it
// (80, then 10) about 14 s later; in the regions 800010/800011 it starts over. The probe stays: it
// watches the three objects' state machines (StateActCtrl: +0x1C state, +0x1D the one it is going to,
// +0x1E asked, +0x20 flags -- 0x1 change asked, 0x40 prompts off, 0x80 no network, 0x100/0x200 an action
// taken and let go, bits 12-25 a sequence number; +0x18 the character using it) and every map object
// packet from the partner that names one of them (docs §3.52, §3.53).

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
constexpr uint32_t  kEnterState    = 0x240270;   // (ctrl, state, notify, confirmed, flag)
constexpr uint32_t  kSessionClient = 0x247CB0;   // () -> AL: a session client (its state changes wait)
constexpr uint32_t  kNoNetworkFlag = 0x80;       // StateActCtrl flag: takes no state from the network
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
using EnterFn = void(__fastcall*)(uintptr_t, uint8_t, uint8_t, uint8_t, uint8_t);
RecvFn  g_recvOriginal = nullptr;
EnterFn g_enterOriginal = nullptr;
std::atomic<bool> g_local{ true };   // ini map_objects_local

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

// The ctrl's flags, 0 when this is not a StateActCtrl.
uint32_t FlagsOfSafe(uintptr_t Ctrl) {
    __try {
        if (!Ctrl || *reinterpret_cast<const uintptr_t*>(Ctrl) != ExeBase() + kStateActCtrlVt) return 0;
        return *reinterpret_cast<const uint32_t*>(Ctrl + 0x20);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool SessionClientSafe() {
    __try {
        return (reinterpret_cast<uint64_t(__fastcall*)()>(ExeBase() + kSessionClient)() & 0xFF) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Every map object a guest touches changes state here and now, as it does for a host (21.09 morning,
// report 3 and 9, checklist 1 and 5).
//
// The game holds a guest's state change until the host confirms it: exe+0x240270 leaves the current
// state (+0x1C) alone and writes only the one it is going to (+0x1D) when the caller asks for a
// confirmation (its fourth argument, set by the ctrl's own vt[0x20]), the new state is of a kind that
// gets confirmed (its descriptor's +9, kinds 2 and 3), and exe+0x247CB0 -> exe+0x5135F0 says this game
// is a session's client. The confirmation is packet '&' (exe+0x23FD10), and it costs a round trip --
// when it comes at all: an object whose event called 131651(obj, 0) drops every such packet (flag
// 0x80), and the whole test of 21.09 morning has not one '&' in the host's log.
//
// Which player the game counts as a client is not asked here on purpose. The report of 21.09 morning
// has the doors and the lift from the host's own machine as well, and a state the game would not have
// held is not changed by clearing the fourth argument -- so it is cleared for both sides, and the fix
// does not hang on reading that one branch right.
//
// That hold is what a guest saw as: a door that stayed shut while it was pushed and was simply open at
// the end (the state was held, then the host's '$' put the last state in at once), a lift that starts
// late and looks as if the button were pressed twice, a lever that could not be pulled a second time
// (the script waits for the lever's own state, which is still the old one). Nothing of it makes sense
// in a seamless session, where every player runs its own world and the partner's world hears about the
// change through the very same packets. So the fourth argument is cleared for every object: the state
// is entered at once. A '&' that does arrive later finds the state already there and simply sets it
// again (exe+0x23FD10 takes the "+0x1C equals +0x1D" branch), so the two worlds still agree.
void __fastcall EnterStateDetour(uintptr_t Ctrl, uint8_t State, uint8_t Notify, uint8_t Confirmed, uint8_t Flag) {
    if (Confirmed && g_local.load(std::memory_order_relaxed) &&
        Session::SessionManager::GetInstance().IsActive()) {
        static std::atomic<uint32_t> s_told{ 0 };
        if (s_told.fetch_add(1) < 20) {
            const uint32_t Flags = FlagsOfSafe(Ctrl);
            LOG_INFO("[MAPOBJ] a map object goes to state %u at once -- the change would have waited for a "
                     "confirmation (ctrl %p, flags 0x%03X, this game is %s%s)", State,
                     reinterpret_cast<void*>(Ctrl), Flags, SessionClientSafe() ? "a session's client" : "not a client",
                     (Flags & kNoNetworkFlag) ? ", and the object takes no state from the network" : "");
        }
        Confirmed = 0;
    }
    g_enterOriginal(Ctrl, State, Notify, Confirmed, Flag);
}

} // namespace

void InstallMapStateAct(bool Local) {
    static bool Installed = false;
    g_local.store(Local);
    if (Installed) return;
    Installed = true;
    if (!Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + kStateActRecv),
                                                       reinterpret_cast<void*>(&StateActRecvDetour),
                                                       reinterpret_cast<void**>(&g_recvOriginal))) {
        LOG_WARNING("[LEVER] could not hook exe+0x%X (map object packets) -- the probe watches states only",
                    kStateActRecv);
    }
    if (!Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + kEnterState),
                                                       reinterpret_cast<void*>(&EnterStateDetour),
                                                       reinterpret_cast<void**>(&g_enterOriginal))) {
        LOG_WARNING("[MAPOBJ] could not hook exe+0x%X (a map object entering a state)", kEnterState);
    }
    LOG_INFO("[MAPOBJ] a guest's map objects change state at once instead of waiting for the host: %s",
             Local ? "on" : "off (map_objects_local=false)");
}

// Game thread, from the enemy generator update.
void MapStateActTick() {
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
