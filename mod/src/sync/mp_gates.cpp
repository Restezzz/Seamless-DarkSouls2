// What a host with a guest -- or a guest -- is kept from doing that a player alone
// can do (docs §3.39).
//
// Covenants. A host with the partner in its world could not join a covenant
// (16.09 evening, Targray): the talk opened, the yes/no came, then a message box
// from the talk script (exe+0x198920 from exe+0x46314C) -- the covenant write
// itself, exe+0x38BD80, has no session check at all. The refusal is a branch of
// the talk script on "is this multiplayer". While a talk with an NPC is open
// ([[[GMImp+0x70]+0x48]+0x40] != 0) the two questions a script can ask about it
// get the answer a player alone gets: exe+0x513580 "in multiplayer" from
// EventConditionNet_IsMultiPlay (return exe+0x46FE99) and from the character-
// script dispatcher (exe+0x45DDB4), and exe+0x45C520 "players in the session"
// from CompareMultiPlayNum (exe+0x46FCDE) and the dispatcher (exe+0x45E0EB).
//
// Summoning from Majula. The host's summon went out and the guest waited for an
// arrival that never came; the accept controller's constructor (exe+0x2BC3F0) had
// refused it. Of its three checks the one Majula fails is, by elimination, the
// area slot check exe+0x2C0700 at exe+0x2BC680 -- the same check the mod already
// passes for its own sign placement. For the lobby's host it lets the partner in;
// the other two checks are logged when they say no, in case it is one of them.
//
// Probes. The ship's map table at No-man's Wharf never offered its prompt while a
// guest was in the host's world, most likely an event script asking "multiplayer?"
// -- each event that asks is logged once with its id, so the next session names
// it. And for the Pharros contraption a guest can press but that never does
// anything: every "object searched" hit is logged, and the IsHost answers of the
// next two seconds are logged by the IsHost detour (player_sync.cpp).

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

#pragma intrinsic(_ReturnAddress)

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t kGameManagerImp     = 0x16148F0;
constexpr uint32_t kInMultiplayer      = 0x513580;   // (session) -> AL
constexpr uint32_t kIsMultiPlayRet     = 0x46FE99;
constexpr uint32_t kInMpEsdRet         = 0x45DDB4;
constexpr uint32_t kPlayerCount        = 0x45C520;   // (kind, flag) -> count
constexpr uint32_t kCompareCountRet    = 0x46FCDE;
constexpr uint32_t kCountEsdRet        = 0x45E0EB;
constexpr uint32_t kSlotAreaCheck      = 0x2C0700;   // (slot area manager, sign type) -> AL
constexpr uint32_t kSlotAreaAcceptRet  = 0x2BC685;
constexpr uint32_t kSummonCheck1       = 0x291C30;   // (a, b, c, d) -> AL
constexpr uint32_t kSummonCheck1Ret    = 0x2BC594;
constexpr uint32_t kSummonCheck2       = 0x2C6460;   // (mp, desc) -> AL
constexpr uint32_t kSummonCheck2Ret    = 0x2BC5D6;
constexpr uint32_t kTaskUpdate         = 0x196B80;   // (event task, arg)
constexpr uint32_t kIsSearch           = 0x4705E0;   // (condition) -> AL

using Fn1  = uint64_t(__fastcall*)(void*);
using Fn2  = uint64_t(__fastcall*)(void*, void*);
using Fn4  = uint64_t(__fastcall*)(void*, void*, void*, void*);
using CountFn = uint64_t(__fastcall*)(int32_t, char);
using SlotFn  = uint64_t(__fastcall*)(void*, uint8_t);

Fn1     g_inMultiplayer = nullptr;
CountFn g_playerCount   = nullptr;
SlotFn  g_slotArea      = nullptr;
Fn4     g_check1        = nullptr;
Fn2     g_check2        = nullptr;
Fn2     g_taskUpdate    = nullptr;
Fn1     g_isSearch      = nullptr;

std::atomic<bool>      g_enabled{ true };
std::atomic<uint32_t>  g_talkAnswers{ 0 };
std::atomic<ULONGLONG> g_searchHitAt{ 0 };
thread_local uintptr_t t_task = 0;

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

// A talk with an NPC is open: EventTalkManager holds the NPC's handle.
bool TalkOpen() {
    uintptr_t Gm = 0, Events = 0, Talk = 0, Npc = 0;
    return ReadPtr(ExeBase() + kGameManagerImp, &Gm) && ReadPtr(Gm + 0x70, &Events) &&
           ReadPtr(Events + 0x48, &Talk) && ReadPtr(Talk + 0x40, &Npc);
}

bool InLobby() {
    return Session::SessionManager::GetInstance().IsActive();
}

// Each event that asks about multiplayer, logged once (the first 64 of them).
void NoteTaskAsking(const char* What, uintptr_t Task, uint64_t Answer) {
    static std::atomic<int32_t> s_seen[64];
    static std::atomic<uint32_t> s_count{ 0 };
    int32_t Id = 0;
    uint8_t Byte = 0;
    __try {
        Id = *reinterpret_cast<const int32_t*>(Task + 0x24);
        Byte = *reinterpret_cast<const uint8_t*>(Task + 0x2A);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    const uint32_t N = s_count.load();
    for (uint32_t I = 0; I < N && I < 64; ++I) {
        if (s_seen[I].load() == Id) return;
    }
    if (N >= 64) return;
    s_seen[N].store(Id);
    s_count.store(N + 1);
    LOG_INFO("[GATES] event %d (+0x2A %u) asks %s -> %llu", Id, static_cast<unsigned>(Byte), What,
             static_cast<unsigned long long>(Answer & 0xFF));
}

uint64_t __fastcall InMultiplayerDetour(void* Session) {
    const uint64_t Stock = g_inMultiplayer(Session);
    if (!(Stock & 0xFF) || !g_enabled.load()) return Stock;
    const uintptr_t Ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase();
    if (Ret != kIsMultiPlayRet && Ret != kInMpEsdRet) return Stock;
    if (!InLobby()) return Stock;
    if (TalkOpen()) {
        const uint32_t N = g_talkAnswers.fetch_add(1) + 1;
        if (N <= 5 || N % 200 == 0) {
            LOG_INFO("[GATES] a talk script asked whether this is multiplayer -- answered no while talking (%u so far)", N);
        }
        return Stock & ~static_cast<uint64_t>(0xFF);
    }
    if (Ret == kIsMultiPlayRet && t_task) NoteTaskAsking("IsMultiPlay", t_task, Stock);
    return Stock;
}

uint64_t __fastcall PlayerCountDetour(int32_t Kind, char Flag) {
    const uint64_t Stock = g_playerCount(Kind, Flag);
    if (!g_enabled.load() || !(Stock & 0xFFFFFFFFull)) return Stock;
    const uintptr_t Ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase();
    if (Ret != kCompareCountRet && Ret != kCountEsdRet) return Stock;
    if (!InLobby()) return Stock;
    if (TalkOpen()) {
        const uint32_t N = g_talkAnswers.fetch_add(1) + 1;
        if (N <= 5 || N % 200 == 0) {
            LOG_INFO("[GATES] a talk script asked how many players are here (%llu) -- answered 0 while talking (%u so far)",
                     static_cast<unsigned long long>(Stock & 0xFFFFFFFFull), N);
        }
        return 0;
    }
    if (Ret == kCompareCountRet && t_task) NoteTaskAsking("CompareMultiPlayNum", t_task, Stock);
    return Stock;
}

uint64_t __fastcall SlotAreaCheckDetour(void* Mgr, uint8_t Type) {
    const uint64_t Stock = g_slotArea(Mgr, Type);
    if (Stock & 0xFF) return Stock;
    const uintptr_t Ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase();
    auto& Lobby = Session::SessionManager::GetInstance();
    if (Ret != kSlotAreaAcceptRet || !g_enabled.load() || !Lobby.IsActive() || !Lobby.IsHost()) return Stock;
    LOG_INFO("[GATES] summoning the partner: this area's slot check said no (sign type %u) -- let through for the lobby",
             static_cast<unsigned>(Type));
    return (Stock & ~static_cast<uint64_t>(0xFF)) | 1;
}

uint64_t __fastcall SummonCheck1Detour(void* A, void* B, void* C, void* D) {
    const uint64_t Stock = g_check1(A, B, C, D);
    if (!(Stock & 0xFF) && reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase() == kSummonCheck1Ret && InLobby()) {
        LOG_INFO("[GATES] summoning the partner: the sign-type check (exe+0x291C30) said no");
    }
    return Stock;
}

uint64_t __fastcall SummonCheck2Detour(void* Mp, void* Desc) {
    const uint64_t Stock = g_check2(Mp, Desc);
    if (!(Stock & 0xFF) && reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase() == kSummonCheck2Ret && InLobby()) {
        LOG_INFO("[GATES] summoning the partner: the session check (exe+0x2C6460) said no");
    }
    return Stock;
}

uint64_t __fastcall TaskUpdateDetour(void* Task, void* Arg) {
    const uintptr_t Outer = t_task;
    t_task = reinterpret_cast<uintptr_t>(Task);
    const uint64_t R = g_taskUpdate(Task, Arg);
    t_task = Outer;
    return R;
}

uint64_t __fastcall IsSearchDetour(void* Condition) {
    const uint64_t Stock = g_isSearch(Condition);
    if (!(Stock & 0xFF) || !InLobby()) return Stock;
    g_searchHitAt.store(GetTickCount64());
    static std::atomic<int32_t> s_lastObject{ 0 };
    int32_t Object = 0;
    __try {
        Object = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(Condition) + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (s_lastObject.exchange(Object) != Object) {
        int32_t Event = 0;
        if (t_task) {
            __try {
                Event = *reinterpret_cast<const int32_t*>(t_task + 0x24);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        LOG_INFO("[GATES] object %d searched (event %d) -- the IsHost answers of the next two seconds follow", Object, Event);
    }
    return Stock;
}

bool HookAt(uint32_t Rva, void* Detour, void** Original, const char* What) {
    if (Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + Rva), Detour, Original)) {
        return true;
    }
    LOG_WARNING("[GATES] could not hook exe+0x%X (%s)", Rva, What);
    return false;
}

} // namespace

bool InstallMpGates(bool Enabled) {
    static bool Installed = false;
    g_enabled.store(Enabled);
    if (!Installed) {
        Installed = true;
        HookAt(kInMultiplayer, reinterpret_cast<void*>(&InMultiplayerDetour), reinterpret_cast<void**>(&g_inMultiplayer),
               "in multiplayer");
        HookAt(kPlayerCount, reinterpret_cast<void*>(&PlayerCountDetour), reinterpret_cast<void**>(&g_playerCount),
               "players in the session");
        HookAt(kSlotAreaCheck, reinterpret_cast<void*>(&SlotAreaCheckDetour), reinterpret_cast<void**>(&g_slotArea),
               "the summon area slot check");
        HookAt(kSummonCheck1, reinterpret_cast<void*>(&SummonCheck1Detour), reinterpret_cast<void**>(&g_check1),
               "summon check 1");
        HookAt(kSummonCheck2, reinterpret_cast<void*>(&SummonCheck2Detour), reinterpret_cast<void**>(&g_check2),
               "summon check 2");
        HookAt(kTaskUpdate, reinterpret_cast<void*>(&TaskUpdateDetour), reinterpret_cast<void**>(&g_taskUpdate),
               "event task update");
        HookAt(kIsSearch, reinterpret_cast<void*>(&IsSearchDetour), reinterpret_cast<void**>(&g_isSearch),
               "object searched");
    }
    LOG_INFO("[GATES] %s", Enabled
        ? "while talking, NPC scripts see a player alone (covenants); the lobby's host can summon from Majula"
        : "off (mp_gates=false): the game's own answers, probes only");
    return g_inMultiplayer != nullptr;
}

bool RecentSearchHit() {
    const ULONGLONG At = g_searchHitAt.load();
    return At && GetTickCount64() - At < 2000;
}

} // namespace DS2Coop::Sync
