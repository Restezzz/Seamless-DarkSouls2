// Being summoned from anywhere -- also from where the game takes no signs.
//
// The host's summon reaches the sign's owner as PushRequestSummonSign, and the
// sign manager answers it in exe+0x2A0FF0 (NetSvrSummonSignManager, listener
// vtable exe+0x10D6270 slot 0; RCX = manager+8). Before it joins it asks, in
// this order:
//
//   1. is this my live sign (manager+0xF0, +0x18, id at +0x20)   -- else reject, code 2
//   2. exe+0x275E80(manager, 0xD): capability 0xD, "can be
//      summoned", on the capability object [netRoot+0x38]         -- else reject, code 1
//   3. exe+0x275E20(manager, &type): the multiplayer manager lets
//      this sign type be summoned here (exe+0x2C6570), and 0xD    -- else reject, code 2
//   4. the multiplayer manager is not busy ([[netRoot+0x18]+8],
//      a counter the bonfire menu, events and cutscenes raise with
//      exe+0x2C5C70 and lower with exe+0x2C93E0)                  -- else reject, code 0
//
// and then takes the summon: it blocks 0xD and exe+0x2C6330 builds the join
// controller. Where signs are not allowed the capability is off, so a sign the
// mod put down there anyway was turned down the moment the summon arrived.
//
// For a sign the mod put down, 2 and 3 are answered yes for that one call, on
// the thread making it; 1 and 4 stay the game's. Everything else reads the real
// values, so the soapstone and the game's own signs keep their rules.
//
// 4 is left alone on purpose: joining from inside the bonfire menu is not
// something the game was built to survive. Instead the mod does not put its
// sign down while the counter is up (player_sync.cpp), and a summon that still
// arrives at a busy moment -- the player sat down after the sign went out --
// is followed by a fresh sign the moment the player is free again. The Majula
// failure of 12.09 (23:52:29) was exactly that: sitting at the Majula bonfire.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../../include/sync.h"
#include "../../include/hooks.h"
#include "../../include/ui.h"
#include "../../include/ui_settings.h"
#include "../../include/utils.h"

#include <atomic>
#include <cstdint>

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

// --- game functions and data (RVA) -------------------------------------------
// All three start with whole instructions of 5+ bytes (checked in the
// disassembly): 0x275E80 with a RIP-relative MOV, the others with pushes.
constexpr uint32_t kSummonPush  = 0x2A0FF0;   // (manager+8, push): someone summons my sign
constexpr uint32_t kCanDo       = 0x275E80;   // (manager, capability) -> allowed now
constexpr uint32_t kTypeHere    = 0x275E20;   // (manager, &sign type) -> summonable here
constexpr uint32_t kNetRoot     = 0x1616CF8;  // *(exe+...) = network root; +0x18 multiplayer manager
constexpr int      kCapSummoned = 0xD;        // "can be summoned through a sign"

// The push (0x110 bytes): +0 the summoner's player id, +4 its name, +0x48 the
// sign id, +0x50 the session to join.
constexpr size_t kPushSummoner = 0x00;
constexpr size_t kPushSignId   = 0x48;

// A placed sign seldom waits longer; every placement arms it again.
constexpr ULONGLONG kArmedForMs = 10 * 60 * 1000;
// Free this long before a declined summon is followed by a fresh sign.
constexpr ULONGLONG kFreeSettleMs = 1500;

using SummonPushFn = void(__fastcall*)(void*, const uint8_t*);
using CanDoFn      = uint64_t(__fastcall*)(void*, int);
using TypeHereFn   = uint64_t(__fastcall*)(void*, const uint8_t*);

// Set by MH_CreateHook before the hook goes live, so a detour never sees null.
void* g_summonPushOriginal = nullptr;
void* g_canDoOriginal      = nullptr;
void* g_typeHereOriginal   = nullptr;

std::atomic<ULONGLONG> g_armedUntil{ 0 };
std::atomic<bool>      g_replaceWhenFree{ false };   // a summon was declined while busy
ULONGLONG              g_freeSince = 0;              // game thread only (the tick)

// What the checks do during one answer to a summon, on the thread giving it.
struct PushScope {
    bool InPush   = false;
    bool Force    = false;
    int  CanDo    = -1;   // the game's own answers (-1: not asked)
    int  TypeHere = -1;
};
thread_local PushScope t_scope;

const char* Answer(int Value) {
    return Value < 0 ? "not asked" : (Value ? "yes" : "no");
}

// Both return a bool in AL; the rest of RAX is passed through untouched.
uint64_t __fastcall CanDoDetour(void* Manager, int Capability) {
    uint64_t Result = reinterpret_cast<CanDoFn>(g_canDoOriginal)(Manager, Capability);
    PushScope& Scope = t_scope;
    if (Scope.InPush && Capability == kCapSummoned) {
        if (Scope.CanDo < 0) Scope.CanDo = (Result & 0xFF) != 0;
        if (Scope.Force) Result = 1;
    }
    return Result;
}

uint64_t __fastcall TypeHereDetour(void* Manager, const uint8_t* Type) {
    uint64_t Result = reinterpret_cast<TypeHereFn>(g_typeHereOriginal)(Manager, Type);
    PushScope& Scope = t_scope;
    if (Scope.InPush) {
        if (Scope.TypeHere < 0) Scope.TypeHere = (Result & 0xFF) != 0;
        if (Scope.Force) Result = 1;
    }
    return Result;
}

bool ReadPush(const uint8_t* Push, uint32_t* Summoner, uint32_t* SignId) {
    __try {
        *Summoner = *reinterpret_cast<const uint32_t*>(Push + kPushSummoner);
        *SignId   = *reinterpret_cast<const uint32_t*>(Push + kPushSignId);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The multiplayer manager's busy counter ([[netRoot+0x18]+8]); -1 unreadable.
int ReadBusyCount() {
    __try {
        const uintptr_t Root = *reinterpret_cast<const uintptr_t*>(
            reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)) + kNetRoot);
        if (!Root) return -1;
        const uintptr_t Mp = *reinterpret_cast<const uintptr_t*>(Root + 0x18);
        if (!Mp) return -1;
        return *reinterpret_cast<const uint8_t*>(Mp + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void __fastcall SummonPushDetour(void* Listener, const uint8_t* Push) {
    const bool Armed = GetTickCount64() < g_armedUntil.load();
    const int  Busy  = ReadBusyCount();
    uint32_t Summoner = 0, SignId = 0;
    ReadPush(Push, &Summoner, &SignId);

    // Pushes are handed out one after another, but should one ever arrive
    // inside another, the outer answer gets its own state back afterwards.
    const PushScope Outer = t_scope;
    t_scope = PushScope{ true, Armed, -1, -1 };
    reinterpret_cast<SummonPushFn>(g_summonPushOriginal)(Listener, Push);
    const PushScope Mine = t_scope;
    t_scope = Outer;

    // The game's own answers. Both "not asked": the sign was not ours or not
    // alive (rejected, code 2). A "no" while armed is what was answered yes.
    LOG_INFO("[JOIN] summon on sign %u by player %u: can be summoned %s, sign type allowed here %s, busy %d%s",
             SignId, Summoner, Answer(Mine.CanDo), Answer(Mine.TypeHere), Busy,
             Armed ? " -- the mod's sign, let through" : "");

    if (Armed && Busy > 0) {
        LOG_INFO("[JOIN] declined by the game: busy (bonfire, menu or event) -- a fresh sign goes down once free");
        g_replaceWhenFree.store(true);
        DS2Coop::UI::Overlay::GetInstance().ShowNotification(
            DS2Coop::UI::Tr("At a bonfire the summon cannot be taken -- get up and it is tried again by itself.",
                            "У костра призыв не принимается \xE2\x80\x94 встань, и он повторится сам."),
            6.0f, DS2Coop::UI::NotifyKind::Warning);
    }
}

bool HookAt(uint32_t Rva, void* Detour, void** Original, const char* What) {
    const uintptr_t ExeBase = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    if (DS2Coop::Hooks::HookManager::GetInstance().InstallHook(
            reinterpret_cast<void*>(ExeBase + Rva), Detour, Original)) {
        return true;
    }
    LOG_WARNING("[JOIN] could not hook exe+0x%X (%s)", Rva, What);
    return false;
}

} // namespace

bool InstallSummonAccept() {
    // Each hook at most once, so a second call never hooks a live target again.
    if (!g_canDoOriginal) {
        HookAt(kCanDo, reinterpret_cast<void*>(&CanDoDetour), &g_canDoOriginal, "capability check");
    }
    if (!g_typeHereOriginal) {
        HookAt(kTypeHere, reinterpret_cast<void*>(&TypeHereDetour), &g_typeHereOriginal, "sign type check");
    }
    // Without both checks the push hook could only log; leave the game alone.
    if (!g_summonPushOriginal && g_canDoOriginal && g_typeHereOriginal) {
        HookAt(kSummonPush, reinterpret_cast<void*>(&SummonPushDetour), &g_summonPushOriginal, "summon push");
    }
    const bool Ready = g_canDoOriginal && g_typeHereOriginal && g_summonPushOriginal;
    LOG_INFO("[JOIN] summons from anywhere: %s",
             Ready ? "ready (exe+0x2A0FF0, exe+0x275E80, exe+0x275E20)" : "NOT available");
    return Ready;
}

void ArmSummonAccept() {
    g_armedUntil.store(GetTickCount64() + kArmedForMs);
}

bool IsSummonBusy() {
    return ReadBusyCount() > 0;
}

void WarnBusyForSign() {
    DS2Coop::UI::Overlay::GetInstance().ShowNotification(
        DS2Coop::UI::Tr("Get up from the bonfire -- the sign goes down by itself.",
                        "Встань от костра \xE2\x80\x94 знак поставится сам."),
        5.0f, DS2Coop::UI::NotifyKind::Warning);
}

void SummonAcceptGameTick() {
    if (!g_replaceWhenFree.load()) return;
    const ULONGLONG Now = GetTickCount64();
    if (IsSummonBusy()) {
        g_freeSince = 0;
        return;
    }
    if (!g_freeSince) {
        g_freeSince = Now;
        return;
    }
    if (Now - g_freeSince < kFreeSettleMs) return;
    g_replaceWhenFree.store(false);
    g_freeSince = 0;
    LOG_INFO("[JOIN] free again -- putting the sign down once more");
    RequestRejoinSignPlacement();
}

} // namespace DS2Coop::Sync
