// Talking to NPCs as a guest in the host's world.
//
// An NPC's "Talk" prompt is an EventKeyGuideCtrl made by its talk script
// (EventEzStateChrCtrl command 0x1FD68 -> exe+0x470BC0, ctor exe+0x4533D0). The
// prompt keeps its action type at +0x8C -- 9 is talk: the executor exe+0x451E50
// runs case 9 as exe+0x452C80, which puts the NPC's handle into
// EventTalkManager+0x40 for the talk script to pick up -- and its flags at
// +0xAA..+0xAC (01 FC 0F for every prompt a script makes).
//
// When a character enters the prompt's area, slot +0x18 (exe+0x454310) asks
// exe+0x453760(chr, &flags) whether that character may use it. A "no" sets the
// character's bit in ctrl+0xA0, and the prompt is never registered for it
// (exe+0x453CE0, the test at exe+0x453D6D). For these flags exe+0x453760 turns
// down every network phantom id from 1 to 15; a guest in the host's world has
// one, so no NPC ever offered it "Talk" (Melentia and Majula, 12.09). Nothing in
// the talk code itself checks for a phantom.
//
// So exe+0x453760 answers "yes" for exactly this case: called from the prompt's
// enter handler (return address exe+0x45436C -- the door component
// exe+0x1CCDC0 calls it from elsewhere), for the local player, a guest in the
// host's world, on a talk prompt (action 9). Every other prompt, character and
// caller gets the game's answer. The answer is taken when the player walks into
// the prompt's area, so an area entered before the join settled is put right
// by walking out of it and back in.
//
// What a talk writes may not all stick for a guest: the event flag setter
// exe+0x474A60 drops a guest's writes (unless exe+0x25CDB0 lets them through).

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
#include "../../include/utils.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#pragma intrinsic(_ReturnAddress)

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

// --- game functions and data (RVA) -------------------------------------------
// exe+0x453760 starts with MOV [RSP+0x10],RBX (48 89 5C 24 10): 5 whole bytes.
constexpr uint32_t  kPromptAllowed  = 0x453760;   // (chr, &prompt flags) -> this character may use it
constexpr uint32_t  kPromptEnterRet = 0x45436C;   // return address of that call in the enter handler
constexpr uint32_t  kGameManagerImp = 0x16148F0;  // *(exe+...) = GameManagerImp; +0xD0 local player
constexpr uint32_t  kNetRoot        = 0x1616CF8;  // *(exe+...) = network root; +0x18 multiplayer manager
constexpr uint32_t  kJoinCtrlVtable = 0x10D7BD8;  // NetSummonJoinMultiplayCtrl
constexpr int32_t   kJoinInWorld    = 7;          // join controller state: in the host's world
constexpr int32_t   kActionTalk     = 9;
constexpr ptrdiff_t kFlagsInPrompt  = 0xAA;       // the flags the predicate gets sit at ctrl+0xAA
constexpr ptrdiff_t kActionInPrompt = 0x8C;       // the prompt's action type at ctrl+0x8C

using PromptAllowedFn = uint64_t(__fastcall*)(void*, const uint8_t*);

// Set by MH_CreateHook before the hook goes live, so the detour never sees null.
void* g_promptAllowedOriginal = nullptr;
std::atomic<uintptr_t> g_lastOpenedPrompt{ 0 };   // for the log: one line per prompt

// Off unless the ini asks for it. Two sessions with this forced open produced no
// prompt whatsoever -- the NPCs are not in the guest's world to begin with --
// and the game then crashed reading address 0 at exe+0x18B10E, called from
// exe+0x4534A6, which is inside this same prompt code. That is a suspicion and
// not a proof, and a suspicion is reason enough to leave it off.
std::atomic<bool> g_talkEnabled{ false };

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

bool ReadI32(uintptr_t Addr, int32_t* Out) {
    __try {
        *Out = *reinterpret_cast<const int32_t*>(Addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t LocalPlayer() {
    uintptr_t Gm = 0, Player = 0;
    if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0xD0, &Player)) return 0;
    return Player;
}

// In the host's world: the join controller ([[netRoot+0x18]+0x40]) is in state 7.
bool IsGuestInHostWorld() {
    uintptr_t Root = 0, Mp = 0, Ctrl = 0, Vtbl = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp) || !ReadPtr(Mp + 0x40, &Ctrl)) return false;
    if (!ReadPtr(Ctrl, &Vtbl) || Vtbl != ExeBase() + kJoinCtrlVtable) return false;
    int32_t State = -1;
    return ReadI32(Ctrl + 0xF8, &State) && State == kJoinInWorld;
}

// Returns a bool in AL; the rest of RAX is passed through untouched.
uint64_t __fastcall PromptAllowedDetour(void* Chr, const uint8_t* Flags) {
    const uint64_t Stock = reinterpret_cast<PromptAllowedFn>(g_promptAllowedOriginal)(Chr, Flags);
    if ((Stock & 0xFF) != 0 || !g_talkEnabled.load()) return Stock;
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != ExeBase() + kPromptEnterRet) return Stock;
    if (!Chr || reinterpret_cast<uintptr_t>(Chr) != LocalPlayer() || !IsGuestInHostWorld()) return Stock;
    const uintptr_t Prompt = reinterpret_cast<uintptr_t>(Flags) - kFlagsInPrompt;
    int32_t Action = 0;
    if (!ReadI32(Prompt + kActionInPrompt, &Action) || Action != kActionTalk) return Stock;
    if (g_lastOpenedPrompt.exchange(Prompt) != Prompt) {
        LOG_INFO("[TALK] talk prompt %p opened for me as a guest (the game turns phantoms down)",
                 reinterpret_cast<void*>(Prompt));
    }
    return (Stock & ~static_cast<uint64_t>(0xFF)) | 1;
}

// --- probe: a generated character being taken off the map ---------------------
// If the NPCs are not simply never put in but put in and then removed, that goes
// through exe+0x40F300 -> exe+0x415E70([GMImp+0x40], status, 0, 0) ->
// exe+0x419460 -> exe+0x40FDB0(status, 1) -- the tail of the generator's
// group-mask culling (exe+0x419D50, mask test exe+0x41EEE0). This only watches:
// it logs the first take-downs with the caller, so "never there" and "taken
// away again" can be told apart in a single session. exe+0x40F300 begins with
// MOV [RSP+0x18],RBX, five whole bytes, and the call is rare -- it fires on a
// removal, not every frame.
constexpr uint32_t kTakeDown = 0x40F300;
using TakeDownFn = void(__fastcall*)(void*, void*, void*, void*);
void* g_takeDownOriginal = nullptr;
std::atomic<uint32_t> g_takeDowns{ 0 };

// The appear/disappear state of a generated character's status (+0x76, 2 bits).
int ReadAppearState(const void* Status) {
    __try {
        return *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(Status) + 0x76) & 3;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void __fastcall TakeDownDetour(void* A, void* B, void* C, void* D) {
    const uint32_t Count = g_takeDowns.fetch_add(1) + 1;
    if (Count <= 20 || Count % 500 == 0) {
        LOG_INFO("[NPC] a generated character is being taken off the map (#%u): %p, %p, appear state %d (from exe+0x%llX)",
                 Count, A, B, ReadAppearState(B),
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase()));
    }
    reinterpret_cast<TakeDownFn>(g_takeDownOriginal)(A, B, C, D);
}

} // namespace

void SetNpcTalkEnabled(bool on) {
    g_talkEnabled.store(on);
    LOG_INFO("[TALK] the talk prompt for a guest: %s", on
             ? "forced open (npc_talk=true)"
             : "left as the game has it (npc_talk=false)");
}

bool InstallNpcTalk() {
    static bool Installed = false;
    if (Installed) return g_promptAllowedOriginal != nullptr;
    Installed = true;
    if (!Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + kPromptAllowed),
                                                       reinterpret_cast<void*>(&PromptAllowedDetour),
                                                       &g_promptAllowedOriginal)) {
        LOG_WARNING("[TALK] could not hook exe+0x%X -- a guest still cannot talk to NPCs", kPromptAllowed);
        return false;
    }
    LOG_INFO("[TALK] a guest can talk to NPCs in the host's world (exe+0x453760, talk prompts only)");
    if (Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + kTakeDown),
                                                      reinterpret_cast<void*>(&TakeDownDetour),
                                                      &g_takeDownOriginal)) {
        LOG_INFO("[NPC] watching generated characters being taken off the map (exe+0x%X)", kTakeDown);
    } else {
        LOG_WARNING("[NPC] could not watch exe+0x%X (characters taken off the map)", kTakeDown);
    }
    return true;
}

} // namespace DS2Coop::Sync
