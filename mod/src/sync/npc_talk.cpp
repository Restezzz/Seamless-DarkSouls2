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
//
// 12.09, and this is what the first attempt had wrong. Both of the guest's NPC
// symptoms -- see-through characters and no prompt -- come from the same field,
// the phantom id at [[chr+0xB0]+0x3C] and its neighbours (docs §3.24):
//
//   * see-through: exe+0x16F6D0(chr) returns [[chr+0xB0]+0x38] if positive, else
//     +0x48, else 0, and that number IS the CHR_PHANTOM_PARAM row the character
//     is drawn with. Nothing global paints a guest's world -- it is per
//     character, so answering 0 for everything that is not the local player is
//     enough to have the game draw the world solid.
//   * no prompt: the refusal is on the GUEST, not the NPC. For phantom id 1 the
//     jump table at exe+0x4539A8 wants bit 2 of the prompt's flags, and a
//     script's flags are 01 FC 0F, so the answer is no before hollowing or
//     anything else is looked at. With id 0 that whole branch is skipped.
//
// Which also explains why overruling the answer did nothing: the result is
// cached in the character's bit in ctrl+0xA0 and only re-taken on entering or
// leaving the prompt's zone (exe+0x454400 clears it). Forcing a yes after the
// fact registered a prompt the bookkeeping had already written off -- no prompt
// in two sessions and a crash at exe+0x18B10E out of exe+0x4534A6. So the id is
// zeroed for the duration of the call instead, and the game answers on merit.

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
// The row of CHR_PHANTOM_PARAM a character is drawn with; 0 means solid.
constexpr uint32_t  kPhantomRow       = 0x16F6D0; // (chr) -> phantom param row id
constexpr ptrdiff_t kTypeInChr        = 0xB0;     // chr+0xB0 -> PlayerType
constexpr ptrdiff_t kPhantomIdInType  = 0x3C;     // PlayerType+0x3C ChrNetworkPhantomId

using PromptAllowedFn = uint64_t(__fastcall*)(void*, const uint8_t*);
using PhantomRowFn    = uint32_t(__fastcall*)(void*);

// Set by MH_CreateHook before the hook goes live, so the detour never sees null.
void* g_promptAllowedOriginal = nullptr;
std::atomic<uintptr_t> g_lastOpenedPrompt{ 0 };   // for the log: one line per prompt

// Off unless the ini asks for it. Two sessions with this forced open produced no
// prompt whatsoever -- the NPCs are not in the guest's world to begin with --
// and the game then crashed reading address 0 at exe+0x18B10E, called from
// exe+0x4534A6, which is inside this same prompt code. That is a suspicion and
// not a proof, and a suspicion is reason enough to leave it off.
std::atomic<bool> g_talkEnabled{ false };

void* g_phantomRowOriginal = nullptr;
std::atomic<bool> g_solidEnabled{ true };

// The partner's character object, noticed while the game asks how to draw it.
// This is the only place the mod ever sees it: the session only carries the
// partner's coordinates, and the phantom itself is made by the game's own
// netcode. Kept with the time it was last seen, because a character does not
// survive a map load. The camera needs it (docs §3.23).
std::atomic<uintptr_t> g_partnerChr{ 0 };
std::atomic<unsigned long long> g_partnerChrAt{ 0 };

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

// Every character the game asks how to draw, and two jobs done on the way.
//
// Answering 0 for anything that is not the local player means the row lookup
// finds no ghost row and the solid branch runs, so the world stops being
// see-through for a guest -- and the partner stops being a white phantom on the
// host's screen, which was on the list too. The NPCs' own fields are left alone,
// so everything else that reads them still sees the truth.
uint32_t __fastcall PhantomRowDetour(void* Chr) {
    const uint32_t Stock = reinterpret_cast<PhantomRowFn>(g_phantomRowOriginal)(Chr);
    if (!Chr) return Stock;

    const uintptr_t Here  = reinterpret_cast<uintptr_t>(Chr);
    const uintptr_t Local = LocalPlayer();
    if (!Local || Here == Local) return Stock;

    // A player's character carries the same vtable as this player's own; an NPC
    // does not. That tells the partner apart without calling into the game's
    // runtime type machinery from a per-frame detour.
    uintptr_t MyVtbl = 0, ItsVtbl = 0;
    const bool IsPlayer = ReadPtr(Local, &MyVtbl) && ReadPtr(Here, &ItsVtbl) && MyVtbl == ItsVtbl;
    if (IsPlayer) {
        g_partnerChr.store(Here);
        g_partnerChrAt.store(GetTickCount64());
    }

    const bool Solid = g_solidEnabled.load() && Session::SessionManager::GetInstance().IsActive();

    static std::atomic<uint32_t> s_logged{ 0 };
    if (Stock != 0 && s_logged.fetch_add(1) < 40) {
        uintptr_t Type = 0;
        int32_t PhantomId = -1;
        if (ReadPtr(Here + kTypeInChr, &Type)) ReadI32(Type + kPhantomIdInType, &PhantomId);
        LOG_INFO("[NPC] %p wants phantom row %u (network phantom id %d, %s)%s",
                 Chr, Stock, PhantomId, IsPlayer ? "a player" : "not a player",
                 Solid ? " -- answered 0, so it is drawn solid" : "");
    }

    return Solid ? 0u : Stock;
}

// Returns a bool in AL; the rest of RAX is passed through untouched.
uint64_t __fastcall PromptAllowedDetour(void* Chr, const uint8_t* Flags) {
    // The phantom id is zeroed for the duration of the call rather than the
    // answer being overruled afterwards -- see the top of this file for why the
    // second way cannot work. Only for this player, only from the zone-enter
    // handler, and only while actually a guest in someone else's world.
    bool      Mine  = false;
    uintptr_t Type  = 0;
    uint8_t   Saved = 0;
    if (g_talkEnabled.load() && Chr &&
        reinterpret_cast<uintptr_t>(Chr) == LocalPlayer() &&
        reinterpret_cast<uintptr_t>(_ReturnAddress()) == ExeBase() + kPromptEnterRet &&
        IsGuestInHostWorld() &&
        ReadPtr(reinterpret_cast<uintptr_t>(Chr) + kTypeInChr, &Type)) {
        __try {
            uint8_t* Id = reinterpret_cast<uint8_t*>(Type + kPhantomIdInType);
            Saved = *Id;
            if (Saved != 0) {
                *Id = 0;
                Mine = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Mine = false;
        }
    }

    const uint64_t Answer = reinterpret_cast<PromptAllowedFn>(g_promptAllowedOriginal)(Chr, Flags);

    if (Mine) {
        __try {
            *reinterpret_cast<uint8_t*>(Type + kPhantomIdInType) = Saved;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        const uintptr_t Prompt = reinterpret_cast<uintptr_t>(Flags) - kFlagsInPrompt;
        int32_t Action = -1;
        ReadI32(Prompt + kActionInPrompt, &Action);
        if (g_lastOpenedPrompt.exchange(Prompt) != Prompt) {
            LOG_INFO("[TALK] prompt %p (action %d, %s): asked as a host instead of phantom id %u -> %s",
                     reinterpret_cast<void*>(Prompt), Action,
                     Action == kActionTalk ? "talk" : "something else",
                     static_cast<unsigned>(Saved), (Answer & 0xFF) ? "yes" : "still no");
        }
    }
    return Answer;
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
    LOG_INFO("[TALK] a guest asking an NPC for a prompt: %s", on
             ? "asked as a host would be (npc_talk=true)"
             : "asked as a phantom, which the game turns down flat (npc_talk=false)");
}

void SetNpcSolidEnabled(bool on) {
    g_solidEnabled.store(on);
    LOG_INFO("[NPC] the world's other characters: %s", on
             ? "drawn solid (npc_solid=true)"
             : "as the game draws them for a phantom, see-through (npc_solid=false)");
}

uintptr_t GetPartnerCharacter(uint64_t maxAgeMs) {
    const unsigned long long Seen = g_partnerChrAt.load();
    if (!Seen) return 0;
    const unsigned long long Now = GetTickCount64();
    if (Now - Seen > maxAgeMs) return 0;   // stale: a map load throws characters away
    return g_partnerChr.load();
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
    LOG_INFO("[TALK] a guest is asked about NPC prompts as a host is (exe+0x453760)");
    if (Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + kPhantomRow),
                                                      reinterpret_cast<void*>(&PhantomRowDetour),
                                                      &g_phantomRowOriginal)) {
        LOG_INFO("[NPC] the world's characters are drawn solid for a guest (exe+0x%X)", kPhantomRow);
    } else {
        LOG_WARNING("[NPC] could not hook exe+0x%X -- the world stays see-through", kPhantomRow);
    }
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
