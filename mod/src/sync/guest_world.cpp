// A guest's copy of the host's world, as the game builds it: the NPCs, their talk,
// and the characters put in before the host's world has arrived (docs §3.37).
//
// Talk. Nothing on the prompt path refuses a guest whose phantom id is 0 -- the
// prompt is never made: on 16.09 evening a guest saw no "Talk" at all in ten
// minutes (1751 action samples), while in log (4) the same guest, standing in a
// copy of Majula it had loaded with a warp of its own (no multiplayer bit), talked
// to everyone. The difference is one predicate, exe+0x513440: "a session with
// players, in a world entered by a multiplayer warp". Two callers decide the NPCs:
//
//   exe+0x45DD62 (return exe+0x45DD67)  ESD function 0x1FE2A in the character ESD
//                                       dispatcher -- what an NPC's talk script
//                                       asks before it makes its prompt
//   exe+0x356393 (return exe+0x356398)  the NPC factory: kind 8 / 11 ("not run
//                                       here": no hits, no local handling) instead
//                                       of 7 / 10
//
// For a guest in a lobby both get "no", which is exactly the state that worked in
// log (4). Each has its own switch in the ini.
//
// The map a guest walked or travelled to on its own (17.09, point 1: NPCs usable
// only while the host is in the same area). A map's event scripts -- the NPCs' talk
// among them -- run only through exe+0x453280 -> exe+0x1959C0(event area), and that
// first asks exe+0x195CD0: in a session, in a world entered by a multiplayer warp,
// "does the join's map (exe+0x2C6DE0 -> [joinCtrl+0x19C]) equal this area's map
// ([area+0x18])?" The game writes +0x19C only on a guest's own map (exe+0x2C18CC)
// and at the arrival (exe+0x2C2BB2), never on a travel, so every task of a map the
// guest reached by itself stayed frozen: no "Talk" (the prompt is script command
// 0x1FD68) and no NPC whose generator waits for its task (exe+0x451A50). In the log,
// the first talk prompt of a guest alone in Majula came 15 s after the mod's enemy
// sync pinned +0x19C to that map. So for a guest in the host's world the answer is
// "yes" for the map it stands in; +0x19C itself is left alone, because incoming
// packets are routed by it. Same switch as the talk scripts.
//
// Characters before the snapshot. A guest's arrival loads the host's map in join
// state 3 and gets the host's world -- event flags, the enemies' dead-state store
// -- in state 4 (exe+0x2C2FA0). The area's generators are made when the area loads
// (exe+0x417810 -> exe+0x41A5F0), before that: NPCs and enemies were decided on the
// guest's empty map flags and an empty dead-state store. Hence enemies the host had
// killed standing again for the guest, a petrified statue that was a normal enemy
// for it, and an NPC in the Wharf cave only it could see. exe+0x41A5F0 clears its
// own queue slot ([mgr+0x332..0x335]) and the manager's update asks again every
// frame, so leaving the call out while the join is in states 3-4 simply waits;
// capped at 20 s, well inside the 30 s the join gives the snapshot.
//
// The enemies' live states (0.2.2 points 3 and 11: enemies the host had killed alive
// again for the guest after every join). The snapshot has two parts for enemies: the
// store of kill counters and drop words, and one 0x34-byte live state per generator
// record -- "killed since the last rest" among them ([+0x2A] & 3, rec+0x76 & 3 once
// applied). exe+0x2C2FA0 applies the second only to a generator block that exists:
// exe+0x40E1D0(exe+0x419A70(genMgr, [joinCtrl+0x19C]), states). With the area's
// generators held back above there is no block yet, so the states were dropped
// without a word and only the counters arrived (all 17 joins of 17.09). So when there
// is no block they are copied, and applied with the same call right after the block
// is made -- before anything in it spawns.

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
#include "../../include/utils.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#pragma intrinsic(_ReturnAddress)

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t  kMpPlayersWarp  = 0x513440;   // (session) -> bool
constexpr uint32_t  kEsdMpReturn    = 0x45DD67;   // ESD function 0x1FE2A
constexpr uint32_t  kNpcKindReturn  = 0x356398;   // NPC factory, kind 8/11 vs 7/10
constexpr uint32_t  kGenAreaCreate  = 0x41A5F0;   // (generator manager, area index)
constexpr uint32_t  kAreaEventsRun  = 0x195CD0;   // (event area) -> AL: this area's event tasks run here
constexpr uint32_t  kNetRoot        = 0x1616CF8;
constexpr uint32_t  kGameManagerImp = 0x16148F0;
constexpr uint32_t  kJoinCtrlVtable = 0x10D7BD8;
constexpr uint32_t  kJoinSnapshot   = 0x2C2FA0;   // (join ctrl, snapshot, players, p4, event states, live states*, p7)
constexpr uint32_t  kGenBlockFind   = 0x419A70;   // (generator manager, raw map) -> that map's generator block, or 0
constexpr uint32_t  kApplyLive      = 0x40E1D0;   // (generator block, vector of 0x34-byte live states*)
constexpr ULONGLONG kDeferMaxMs     = 20000;
constexpr size_t    kLiveStateSize  = 0x34;
constexpr uint32_t  kLiveStatesMax  = 512;
constexpr uint32_t  kGenRecordSize  = 0xA0;       // [block+0x18] + i * 0xA0, [block+0x20] of them
constexpr int32_t   kGenAreas       = 0x2A;       // [genMgr+0x20 + index*8]
constexpr ULONGLONG kStashKeepMs    = 30000;
constexpr ULONGLONG kRecheckMs      = 3000;

using PredFn      = uint64_t(__fastcall*)(void*);
using GenCreateFn = void(__fastcall*)(void*, int32_t);
using SnapshotFn  = void(__fastcall*)(void*, void*, void*, void*, void*, void*, void*);
using BlockFindFn = uintptr_t(__fastcall*)(uintptr_t, int32_t);
using ApplyLiveFn = void(__fastcall*)(uintptr_t, const uintptr_t*);

PredFn      g_mpPlayersWarp = nullptr;
GenCreateFn g_genCreate     = nullptr;
PredFn      g_areaEventsRun = nullptr;
SnapshotFn  g_snapshot      = nullptr;

std::atomic<bool>      g_talkScripts{ true };      // ini guest_npc_talk_scripts
std::atomic<bool>      g_npcLocal{ true };         // ini guest_npc_local
std::atomic<bool>      g_waitSnapshot{ true };     // ini guest_wait_for_snapshot
std::atomic<bool>      g_liveStates{ true };       // ini enemy_states_at_join
std::atomic<bool>      g_liftFix{ true };              // ini guest_lift_fix

// The host's live states, kept until the join map's block exists.
struct LiveStash {
    bool      Set;
    int32_t   Map;
    uintptr_t Ctrl;
    ULONGLONG At;
    uint32_t  Count;
    uint32_t  Dead;
    uint8_t   Data[kLiveStatesMax * kLiveStateSize];
};
std::mutex g_stashMutex;
LiveStash  g_stash{};
// A block the states were applied to, counted again a little later (probe).
uintptr_t  g_recheckBlock = 0;
int32_t    g_recheckMap = 0;
ULONGLONG  g_recheckAt = 0;
std::atomic<uint32_t>  g_esdAnswers{ 0 };
std::atomic<uint32_t>  g_kindAnswers{ 0 };
std::atomic<uintptr_t> g_deferCtrl{ 0 };
std::atomic<ULONGLONG> g_deferSince{ 0 };
std::atomic<uint32_t>  g_deferredCalls{ 0 };
std::atomic<int32_t>   g_eventsFreedMap{ 0 };     // the last map whose events were let run here (log once)
std::atomic<uint32_t>  g_eventsFreedCalls{ 0 };

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

// The join controller and its state; -1 and 0 with none.
int JoinState(uintptr_t* CtrlOut) {
    *CtrlOut = 0;
    uintptr_t Root = 0, Mp = 0, Ctrl = 0, Vtbl = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp) || !ReadPtr(Mp + 0x40, &Ctrl)) return -1;
    if (!ReadPtr(Ctrl, &Vtbl) || Vtbl != ExeBase() + kJoinCtrlVtable) return -1;
    *CtrlOut = Ctrl;
    __try {
        return *reinterpret_cast<const int32_t*>(Ctrl + 0xF8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool GuestInALobby() {
    auto& Lobby = Session::SessionManager::GetInstance();
    return Lobby.IsActive() && !Lobby.IsHost();
}

bool ReadI32(uintptr_t Addr, int32_t* Out) {
    __try {
        *Out = *reinterpret_cast<const int32_t*>(Addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The map this player stands in, the game's own value ([[[netRoot+0x20]+0x5B8]+0xC]).
bool ReadLocalMap(int32_t* Out) {
    uintptr_t Root = 0, List = 0, Local = 0;
    return ReadPtr(ExeBase() + kNetRoot, &Root) && ReadPtr(Root + 0x20, &List) && ReadPtr(List + 0x5B8, &Local) &&
           ReadI32(Local + 0xC, Out) && *Out != 0;
}

uint32_t MapNumber(int32_t Raw) {
    const uint32_t R = static_cast<uint32_t>(Raw);
    return ((R >> 24) & 0xFF) * 1000000u + ((R >> 16) & 0xFF) * 10000u + ((R >> 8) & 0xFF) * 100u + (R & 0xFF);
}

uint64_t __fastcall AreaEventsRunDetour(void* Area) {
    const uint64_t Stock = g_areaEventsRun(Area);
    if ((Stock & 0xFF) || !Area || !g_talkScripts.load(std::memory_order_relaxed) || !GuestInALobby()) return Stock;
    uintptr_t Ctrl = 0;
    if (JoinState(&Ctrl) != 7) return Stock;
    int32_t AreaMap = 0, MyMap = 0;
    if (!ReadI32(reinterpret_cast<uintptr_t>(Area) + 0x18, &AreaMap) || !ReadLocalMap(&MyMap) || AreaMap != MyMap) {
        return Stock;
    }
    g_eventsFreedCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_eventsFreedMap.exchange(AreaMap) != AreaMap) {
        int32_t Pinned = 0;
        ReadI32(Ctrl + 0x19C, &Pinned);
        LOG_INFO("[NPC] the events of map %u, where I stand, were held back because the join names map %u -- "
                 "run here (NPC talk and NPCs that wait for their event)", MapNumber(AreaMap), MapNumber(Pinned));
    }
    return Stock | 1;
}

// An event script, not a talk, asked ESD function 130602 (0.2.2, the lift, report point 2). The
// dispatcher exe+0x45C6A0 serves the map event scripts as well as the talk scripts, so answering
// "no" at exe+0x45DD67 for a guest also sent every map event that asks it down the world owner's
// branch -- the lift's init event 1030 (m10_30) among them: `IF (f130602() != 1)` -> sub-machine
// 0x7FFFFFF3, "set the cabin (10302000) to state 40", then flag 130000001. On 17.09 the guest's log
// shows that flag at 14:31:29, the second the mod let the guest's events of that map run.
//
// The first 0.2.2 test (18.09) kept the game's answer for every event script -- and the NPC events
// ask it too: 111040-111343 in Majula, 111100-111153 at the crones' in Things Betwixt, 111243-111294
// in the Forest ("the game's answer 1 kept"), and the guest had no "Talk" in Majula nor the
// hatchlings in the nest. So only the lift's own event gets the game's answer; every other event
// script gets "no", as up to 0.2.1 (ini guest_lift_fix).
//
// Except Things Betwixt, all of it (the second 0.2.2 test, 19.09). A new game started together: both
// players took the crones' offer in the same second, as on 18.09 -- but this time the crones' own
// events (111100-111150, 4000000) ran for the guest down the owner's branch, and the host, whose
// character was made in the very second the guest's was (flags 102000012/102000015 at 18:35:09 on
// both sides), was left on a black screen with only the HUD. On 18.09 those events took the game's
// answer ("the game's answer 1 kept" at 22:52:53) and both made their characters. There the crones
// only tell lore once the characters exist, and the guest still makes its own character: the
// crones' offer is event 16000, which never asks 130602.
struct OwnAnswerEvent {
    uint32_t Map;
    int32_t  Event;
};
constexpr int32_t kAnyEvent = -1;
constexpr OwnAnswerEvent kOwnAnswerEvents[] = {
    { 0x0A1E0000u, 1030 },        // m10_30: the lift's init (cabin 10302000 to state 40, flag 130000001)
    { 0x0A020000u, kAnyEvent },   // m10_02: Things Betwixt, where the characters are made
};

bool OwnAnswerEventFor(uintptr_t Task, uint32_t* MapOut, int32_t* EventOut) {
    if (!ReadEventTaskKey(Task, MapOut, EventOut)) return false;
    for (const OwnAnswerEvent& E : kOwnAnswerEvents) {
        if (E.Map == *MapOut && (E.Event == kAnyEvent || E.Event == *EventOut)) return true;
    }
    return false;
}

void NoteEventAsked130602(uintptr_t Task, uint64_t Stock) {
    static uint64_t s_seen[256];
    static uint32_t s_count = 0;
    uint32_t Map = 0;
    int32_t Event = 0;
    if (!ReadEventTaskKey(Task, &Map, &Event)) return;
    const uint64_t Key = (static_cast<uint64_t>(Map) << 32) | static_cast<uint32_t>(Event);
    for (uint32_t I = 0; I < s_count; ++I) {
        if (s_seen[I] == Key) return;
    }
    if (s_count < 256) s_seen[s_count++] = Key;
    LOG_INFO("[NPC] event %d of map %u asks whether this is someone else's multiplayer world (ESD 130602) -- "
             "the game's answer %u kept: an event script, not a talk", Event, MapNumber(static_cast<int32_t>(Map)),
             static_cast<unsigned>(Stock & 0xFF));
}

uint64_t __fastcall MpPlayersWarpDetour(void* Session) {
    const uint64_t Stock = g_mpPlayersWarp(Session);
    if (!(Stock & 0xFF)) return Stock;
    const uintptr_t Ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase();
    const bool Esd  = Ret == kEsdMpReturn && g_talkScripts.load();
    const bool Kind = Ret == kNpcKindReturn && g_npcLocal.load();
    if ((!Esd && !Kind) || !GuestInALobby()) return Stock;
    if (Esd && g_liftFix.load()) {
        const uintptr_t Task = CurrentEventTask();
        uint32_t Map = 0;
        int32_t Event = 0;
        if (Task && OwnAnswerEventFor(Task, &Map, &Event)) {
            NoteEventAsked130602(Task, Stock);
            return Stock;
        }
    }
    const uint32_t N = (Esd ? g_esdAnswers : g_kindAnswers).fetch_add(1) + 1;
    if (N <= 3 || N % 1000 == 0) {
        LOG_INFO("[NPC] %s asked whether this is someone else's multiplayer world -- answered no, as for the "
                 "world's owner (%u so far)", Esd ? "a character script (ESD 0x1FE2A)" : "the NPC factory", N);
    }
    return Stock & ~static_cast<uint64_t>(0xFF);
}

// --- the enemies' live states at a join -------------------------------------------
uintptr_t GeneratorManager() {
    uintptr_t Gm = 0, GenMgr = 0;
    return ReadPtr(ExeBase() + kGameManagerImp, &Gm) && ReadPtr(Gm + 0x40, &GenMgr) ? GenMgr : 0;
}

bool FindBlockSafe(uintptr_t GenMgr, int32_t Map, uintptr_t* Block) {
    __try {
        *Block = reinterpret_cast<BlockFindFn>(ExeBase() + kGenBlockFind)(GenMgr, Map);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *Block = 0;
        return false;
    }
}

// How many of a block's generator records say "killed since the last rest".
int CountDeadRecordsSafe(uintptr_t Block, int* Total) {
    *Total = 0;
    __try {
        const uintptr_t First = *reinterpret_cast<const uintptr_t*>(Block + 0x18);
        const uint32_t Count = *reinterpret_cast<const uint32_t*>(Block + 0x20);
        if (!First || Count > 4096) return -1;
        int Dead = 0;
        for (uint32_t I = 0; I < Count; ++I) {
            if (*reinterpret_cast<const uint8_t*>(First + I * kGenRecordSize + 0x76) & 3) ++Dead;
        }
        *Total = static_cast<int>(Count);
        return Dead;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Copies the live states out of the snapshot's vector {begin, end}. False if unreadable.
bool CopyLiveStatesSafe(void* States, LiveStash* Out, uint32_t* Offered) {
    *Offered = 0;
    __try {
        const uintptr_t Begin = *reinterpret_cast<const uintptr_t*>(States);
        const uintptr_t End = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(States) + 8);
        if (!Begin || End < Begin) return false;
        *Offered = static_cast<uint32_t>((End - Begin) / kLiveStateSize);
        const uint32_t Count = *Offered < kLiveStatesMax ? *Offered : kLiveStatesMax;
        std::memcpy(Out->Data, reinterpret_cast<const void*>(Begin), Count * kLiveStateSize);
        uint32_t Dead = 0;
        for (uint32_t I = 0; I < Count; ++I) {
            if (Out->Data[I * kLiveStateSize + 0x2A] & 3) ++Dead;
        }
        Out->Count = Count;
        Out->Dead = Dead;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ApplyLiveStatesSafe(uintptr_t Block, const uint8_t* Data, uint32_t Count) {
    __try {
        const uintptr_t Vector[3] = { reinterpret_cast<uintptr_t>(Data),
                                      reinterpret_cast<uintptr_t>(Data) + Count * kLiveStateSize,
                                      reinterpret_cast<uintptr_t>(Data) + Count * kLiveStateSize };
        reinterpret_cast<ApplyLiveFn>(ExeBase() + kApplyLive)(Block, Vector);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall JoinSnapshotDetour(void* Ctrl, void* Snap, void* Players, void* P4, void* Events, void* States,
                                   void* P7) {
    int32_t State = -1, Map = 0;
    const uintptr_t C = reinterpret_cast<uintptr_t>(Ctrl);
    if (Ctrl && States && g_liveStates.load() && GuestInALobby() && ReadI32(C + 0xF8, &State) && State == 4 &&
        ReadI32(C + 0x19C, &Map)) {
        uintptr_t Block = 0;
        const uintptr_t GenMgr = GeneratorManager();
        if (GenMgr) FindBlockSafe(GenMgr, Map, &Block);
        std::lock_guard<std::mutex> Lock(g_stashMutex);
        uint32_t Offered = 0;
        const bool Copied = CopyLiveStatesSafe(States, &g_stash, &Offered);
        LOG_INFO("[WORLD] the host's world for map %u arrived: %u enemy live states, %u of them killed since the "
                 "host's last rest -- %s", MapNumber(Map), Offered, Copied ? g_stash.Dead : 0,
                 !Copied ? "unreadable, left to the game"
                 : Block ? "this map's generators exist, the game applies them"
                         : "this map's generators are not made yet, so they are kept and applied once they are");
        g_stash.Set = Copied && !Block && GenMgr;
        g_stash.Map = Map;
        g_stash.Ctrl = C;
        g_stash.At = GetTickCount64();
        if (Offered > kLiveStatesMax) {
            LOG_WARNING("[WORLD] only the first %u of %u live states are kept", kLiveStatesMax, Offered);
        }
    }
    g_snapshot(Ctrl, Snap, Players, P4, Events, States, P7);
}

// Right after the generators of an area are made: the kept states, if they are this area's.
void ApplyKeptLiveStates(void* Mgr, int32_t AreaIndex) {
    std::lock_guard<std::mutex> Lock(g_stashMutex);
    if (!g_stash.Set) return;
    const ULONGLONG Now = GetTickCount64();
    uintptr_t Ctrl = 0;
    JoinState(&Ctrl);
    if (Now - g_stash.At > kStashKeepMs || Ctrl != g_stash.Ctrl) {
        LOG_INFO("[WORLD] the kept live states of map %u were never applied (%s)", MapNumber(g_stash.Map),
                 Ctrl != g_stash.Ctrl ? "another join" : "no generators for that map in 30 s");
        g_stash.Set = false;
        return;
    }
    if (AreaIndex < 0 || AreaIndex >= kGenAreas) return;
    uintptr_t Made = 0, Block = 0;
    if (!ReadPtr(reinterpret_cast<uintptr_t>(Mgr) + 0x20 + static_cast<uintptr_t>(AreaIndex) * 8, &Made)) return;
    if (!FindBlockSafe(reinterpret_cast<uintptr_t>(Mgr), g_stash.Map, &Block) || Block != Made) return;   // another area
    int Total = 0;
    const int DeadBefore = CountDeadRecordsSafe(Block, &Total);
    const bool Done = ApplyLiveStatesSafe(Block, g_stash.Data, g_stash.Count);
    const int DeadAfter = CountDeadRecordsSafe(Block, &Total);
    LOG_INFO("[WORLD] the host's %u enemy live states applied to map %u as its generators were made: %s -- "
             "%d of %d records killed since the host's last rest (%d before)", g_stash.Count, MapNumber(g_stash.Map),
             Done ? "done" : "threw", DeadAfter, Total, DeadBefore);
    g_stash.Set = false;
    if (Done) {
        g_recheckBlock = Block;
        g_recheckMap = g_stash.Map;
        g_recheckAt = Now + kRecheckMs;
    }
}

void __fastcall GenAreaCreateDetour(void* Mgr, int32_t AreaIndex) {
    if (g_waitSnapshot.load() && GuestInALobby()) {
        uintptr_t Ctrl = 0;
        const int State = JoinState(&Ctrl);
        const ULONGLONG Now = GetTickCount64();
        if (Ctrl != g_deferCtrl.load()) {          // another join: start over
            g_deferCtrl.store(Ctrl);
            g_deferSince.store(0);
        }
        if (Ctrl && (State == 3 || State == 4)) {
            ULONGLONG Since = g_deferSince.load();
            if (!Since) {
                Since = Now;
                g_deferSince.store(Now);
                LOG_INFO("[WORLD] joining (state %d): the characters of area index %d wait for the host's world "
                         "(flags and the enemies' dead-state) before they are put in", State, AreaIndex);
            }
            if (Now - Since < kDeferMaxMs) {
                g_deferredCalls.fetch_add(1);
                return;   // stays queued; the manager asks again next frame
            }
        } else if (g_deferSince.load() && State >= 5) {
            LOG_INFO("[WORLD] join state %d: characters put in now, with the host's world applied (waited %llu ms, "
                     "%u deferred calls)", State, static_cast<unsigned long long>(Now - g_deferSince.load()),
                     g_deferredCalls.exchange(0));
            g_deferSince.store(0);
        }
    }
    uintptr_t CarryCtrl = 0;
    if (!GuestInALobby() || JoinState(&CarryCtrl) != 7) {
        CarryHostWorldFlagsHomeNow("a map of my own world is being made");   // player_sync.cpp
    }
    EnemyReconcileBeforeArea(Mgr, AreaIndex);
    ApplyHomeKillsBeforeArea(Mgr, AreaIndex);   // loot_sync.cpp: once-only enemies killed elsewhere
    g_genCreate(Mgr, AreaIndex);
    ApplyKeptLiveStates(Mgr, AreaIndex);
    EnemyReconcileAfterArea(Mgr, AreaIndex);
}

bool HookAt(uint32_t Rva, void* Detour, void** Original, const char* What) {
    if (Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + Rva), Detour, Original)) {
        return true;
    }
    LOG_WARNING("[WORLD] could not hook exe+0x%X (%s)", Rva, What);
    return false;
}

} // namespace

bool InstallGuestWorld(bool TalkScripts, bool NpcLocal, bool WaitSnapshot, bool LiveStates) {
    static bool Installed = false;
    g_talkScripts.store(TalkScripts);
    g_npcLocal.store(NpcLocal);
    g_waitSnapshot.store(WaitSnapshot);
    g_liveStates.store(LiveStates);
    if (!Installed) {
        Installed = true;
        HookAt(kMpPlayersWarp, reinterpret_cast<void*>(&MpPlayersWarpDetour),
               reinterpret_cast<void**>(&g_mpPlayersWarp), "the multiplayer-world predicate");
        HookAt(kGenAreaCreate, reinterpret_cast<void*>(&GenAreaCreateDetour),
               reinterpret_cast<void**>(&g_genCreate), "area generator creation");
        HookAt(kAreaEventsRun, reinterpret_cast<void*>(&AreaEventsRunDetour),
               reinterpret_cast<void**>(&g_areaEventsRun), "the event-area gate");
        HookAt(kJoinSnapshot, reinterpret_cast<void*>(&JoinSnapshotDetour),
               reinterpret_cast<void**>(&g_snapshot), "the join snapshot");
    }
    LOG_INFO("[WORLD] a guest's world: NPC scripts %s, NPCs %s, characters %s, enemies %s",
             TalkScripts ? "as the owner's (talk)" : "the game's way",
             NpcLocal ? "run here (can be hit and talked to)" : "the game's way",
             WaitSnapshot ? "put in once the host's world has arrived" : "put in as the area loads",
             LiveStates && g_snapshot ? "as the host left them (killed ones stay dead)" : "the game's way");
    return g_mpPlayersWarp != nullptr && g_genCreate != nullptr;
}

void SetGuestLiftFix(bool On) {
    g_liftFix.store(On);
}

// Game thread: the records the host's states were applied to, counted again a few
// seconds on -- whether something brought the killed ones back in between.
void GuestWorldTick() {
    std::lock_guard<std::mutex> Lock(g_stashMutex);
    if (!g_recheckAt || GetTickCount64() < g_recheckAt) return;
    g_recheckAt = 0;
    uintptr_t Block = 0;
    const uintptr_t GenMgr = GeneratorManager();
    if (!GenMgr || !FindBlockSafe(GenMgr, g_recheckMap, &Block) || Block != g_recheckBlock) {
        LOG_INFO("[WORLD] map %u's generators are gone again %llu s after the host's states were applied",
                 MapNumber(g_recheckMap), kRecheckMs / 1000);
        return;
    }
    int Total = 0;
    const int Dead = CountDeadRecordsSafe(Block, &Total);
    LOG_INFO("[WORLD] %llu s later: %d of %d records of map %u killed since the host's last rest",
             kRecheckMs / 1000, Dead, Total, MapNumber(g_recheckMap));
}

} // namespace DS2Coop::Sync
