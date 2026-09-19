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
constexpr ptrdiff_t kGeneratorInChr   = 0x110;    // chr+0x110: generator record, -1 for a player

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

// Whether a character is another player of this session: one of the five network
// player slots of the player list [netRoot+0x20] (slot i at +0x1A8 + i*0xD0; exe+0x51D4B0
// walks them by peer id) holds it as its character at slot +0x40. A player-shaped NPC
// -- a red phantom, a summonable NPC phantom -- is in none of them, although it carries
// the player class and -1 at chr+0x110 just like the partner (17.09: red phantoms were
// drawn solid for the guest too, point 11 of the second report).
bool IsSessionPlayerCharacter(uintptr_t Chr) {
    if (!Chr) return false;
    __try {
        const uintptr_t Root = *reinterpret_cast<const uintptr_t*>(ExeBase() + kNetRoot);
        const uintptr_t List = Root ? *reinterpret_cast<const uintptr_t*>(Root + 0x20) : 0;
        if (!List) return false;
        for (int I = 0; I < 5; ++I) {
            if (*reinterpret_cast<const uintptr_t*>(List + 0x1E8 + static_cast<uintptr_t>(I) * 0xD0) == Chr) return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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

    // A player's character carries the same vtable as this player's own -- and so
    // does every human-shaped NPC: the character factory builds those as
    // PlayerCtrl too (exe+0x37EBE0, called at exe+0x35628E). On 16.09 that is how a
    // Majula NPC was taken for the partner: "partner team 17" turned up in both
    // players' logs, flipping at every world reset as the NPC was destroyed and
    // made again -- and the death camera was pointed at whatever this returned.
    // What tells the two apart is [chr+0x110]: a character made by a generator
    // keeps its generator record there, a player keeps -1 (exe+0x13CF00 tests for
    // exactly that). Still no call into the game from a per-frame detour.
    uintptr_t MyVtbl = 0, ItsVtbl = 0;
    int32_t   Generator = 0;
    const bool SameClass = ReadPtr(Local, &MyVtbl) && ReadPtr(Here, &ItsVtbl) && MyVtbl == ItsVtbl;
    const bool PlayerShaped = SameClass && ReadI32(Here + kGeneratorInChr, &Generator) && Generator == -1;
    // Player-shaped is not enough: red phantoms and other NPC phantoms are built the
    // same way. Only a character one of the network player slots holds is a player.
    const bool IsPlayer = PlayerShaped && IsSessionPlayerCharacter(Here);
    if (IsPlayer) {
        g_partnerChr.store(Here);
        g_partnerChrAt.store(GetTickCount64());
    }
    static std::atomic<uint32_t> s_classLines{ 0 };
    if (SameClass && s_classLines.fetch_add(1) < 12) {
        int32_t Mine = 0;
        ReadI32(Local + kGeneratorInChr, &Mine);
        LOG_INFO("[NPC] %p is built like a player; chr+0x110 = %d (this player's own: %d) -> %s",
                 Chr, Generator, Mine,
                 IsPlayer ? "the partner" : PlayerShaped ? "an NPC phantom (no player slot holds it)" : "a human-shaped NPC");
    }

    // Only the partner is drawn solid (0.2.2, second report point 11: red phantoms, NPCs
    // and enemies look the way the game draws them).
    const bool Solid = g_solidEnabled.load() && IsPlayer && Session::SessionManager::GetInstance().IsActive();

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
// The three flag bytes a prompt hands the predicate (01 FC 0F for a script's).
bool ReadPromptFlags(const uint8_t* Flags, uint8_t Out[3]) {
    __try {
        Out[0] = Flags[0];
        Out[1] = Flags[1];
        Out[2] = Flags[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t __fastcall PromptAllowedDetour(void* Chr, const uint8_t* Flags) {
    // The phantom id is zeroed for the duration of the call rather than the
    // answer being overruled afterwards -- see the top of this file for why the
    // second way cannot work. Only for this player and only from the zone-enter
    // handler -- but in ANY stage of a join, not just once it has settled.
    //
    // 16.09: talking worked with everyone in Majula and never with Melentia at
    // her bonfire, and this detour did not log a single swap all session. The
    // guest had arrived right beside her: the zone check ran at arrival, before
    // the join reached state 7 and before EnableSummoning zeroed the id, and the
    // "no" stayed cached for as long as the guest stood in her zone. Majula only
    // worked because those zones were entered later, with the id already 0.
    //
    // 16.09 evening a guest with npc_talk=true could talk to no one, and there was
    // nothing to read: the id had long been 0, so no swap happened and nothing was
    // logged. Every answer this player gets in a session is written down now, one
    // line per prompt, swapped or not -- which prompt, its action and flags, the
    // phantom id at that moment and the game's answer.
    const bool Ours = Chr &&
                      reinterpret_cast<uintptr_t>(Chr) == LocalPlayer() &&
                      reinterpret_cast<uintptr_t>(_ReturnAddress()) == ExeBase() + kPromptEnterRet &&
                      Session::SessionManager::GetInstance().IsActive();
    bool      Swapped = false;
    uintptr_t Type    = 0;
    uint8_t   Saved   = 0;
    if (Ours && ReadPtr(reinterpret_cast<uintptr_t>(Chr) + kTypeInChr, &Type)) {
        __try {
            uint8_t* Id = reinterpret_cast<uint8_t*>(Type + kPhantomIdInType);
            Saved = *Id;
            if (g_talkEnabled.load() && Saved != 0) {
                *Id = 0;
                Swapped = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Swapped = false;
        }
    }

    const uint64_t Answer = reinterpret_cast<PromptAllowedFn>(g_promptAllowedOriginal)(Chr, Flags);

    if (Swapped) {
        __try {
            *reinterpret_cast<uint8_t*>(Type + kPhantomIdInType) = Saved;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (Ours) {
        const uintptr_t Prompt = reinterpret_cast<uintptr_t>(Flags) - kFlagsInPrompt;
        if (g_lastOpenedPrompt.exchange(Prompt) != Prompt) {
            int32_t Action = -1;
            ReadI32(Prompt + kActionInPrompt, &Action);
            uint8_t Bytes[3] = { 0, 0, 0 };
            ReadPromptFlags(Flags, Bytes);
            LOG_INFO("[TALK] prompt %p (action %d, %s, flags %02X %02X %02X): phantom id %u%s, %s -> %s",
                     reinterpret_cast<void*>(Prompt), Action,
                     Action == kActionTalk ? "talk" : "something else",
                     Bytes[0], Bytes[1], Bytes[2], static_cast<unsigned>(Saved),
                     Swapped ? " (asked as a host instead)" : (g_talkEnabled.load() ? "" : " (npc_talk=false)"),
                     IsGuestInHostWorld() ? "in the host's world" : "not in a host's world yet",
                     (Answer & 0xFF) ? "yes" : "no");
        }
    }
    return Answer;
}

// --- probe: which prompts are offered at all (21.09, the nest in Things Betwixt) ---------------
// The hatchlings talk to the host and not to the guest, and the guest's log holds no prompt line for
// the action the host used (0x1B): the prompt was never even asked about, so it was never registered.
// exe+0x453CE0 is the registration: it refuses when the character's own bit is set in ctrl+0xA0 (the
// zone-enter answer, exe+0x453760 above), when the action is a bonfire's 0xD/0xE in a session as a
// guest (patched elsewhere), or for the two special ids 0x1C and 0x27. This writes down every
// registration attempt of an action this player has not seen yet: the action id, that bit, and what
// the session looks like. One line per action id, so a busy world costs nothing.
constexpr uint32_t kRegisterPrompt = 0x453CE0;   // (EventKeyGuideCtrl)
constexpr uint32_t kActionInGuide  = 0x8C;
constexpr uint32_t kMaskInGuide    = 0xA0;
using RegisterPromptFn = void(__fastcall*)(void*);
void* g_registerPromptOriginal = nullptr;

bool ReadGuideSafe(uintptr_t Guide, int32_t* Action, uint64_t* Mask) {
    __try {
        *Action = *reinterpret_cast<const int32_t*>(Guide + kActionInGuide);
        *Mask = *reinterpret_cast<const uint64_t*>(Guide + kMaskInGuide);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall RegisterPromptDetour(void* Guide) {
    static std::atomic<uint32_t> s_seen[64] = {};
    static std::atomic<uint32_t> s_count{ 0 };
    int32_t Action = -1;
    uint64_t Mask = 0;
    if (Session::SessionManager::GetInstance().IsActive() &&
        ReadGuideSafe(reinterpret_cast<uintptr_t>(Guide), &Action, &Mask)) {
        bool Told = false;
        const uint32_t N = s_count.load();
        for (uint32_t I = 0; I < N && I < 64 && !Told; ++I) {
            Told = s_seen[I].load() == static_cast<uint32_t>(Action);
        }
        if (!Told && N < 64) {
            s_seen[N].store(static_cast<uint32_t>(Action));
            s_count.store(N + 1);
            LOG_INFO("[TALK] a prompt for action %d is being offered here (refused-for mask 0x%llX, %s)", Action,
                     static_cast<unsigned long long>(Mask),
                     IsGuestInHostWorld() ? "I am a guest in the host's world" : "my own world");
        }
    }
    reinterpret_cast<RegisterPromptFn>(g_registerPromptOriginal)(Guide);
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
    LOG_INFO("[NPC] the partner's character: %s", on
             ? "drawn solid, as a player and not a phantom; everyone else as the game draws them (npc_solid=true)"
             : "as the game draws it (npc_solid=false)");
}

bool IsSessionPlayer(uintptr_t chr) {
    return IsSessionPlayerCharacter(chr);
}

// A talk with an NPC is open right now.
//
// [[GMImp+0x70]+0x48] is EventTalkManager; +0x40 is the int32 handle of the NPC the talk
// action (exe+0x452C80, MOV dword [rcx+0x40],eax) put there, 0 when there is none (the
// constructor exe+0x194080 writes 0 and, separately, +0x44). 0.2.1 read eight bytes there,
// taking in +0x44: on 17.09 the host's first "answered no while talking" came at 16:24:15,
// fifteen minutes before its first talk (16:39:27), and from then on every event script
// that asked "in multiplayer?" got "no" all session. Read as the int32 it is -- and because
// nothing seen clears it when a talk is over, only while the character it names exists and
// stands within kTalkRangeM of this player. Game thread (a virtual call on the characters).
namespace {
constexpr uint32_t kHandleToChr = 0x17B830;   // (&handle) -> the character, or 0
constexpr float    kTalkRangeM  = 6.0f;
using HandleToChrFn = uintptr_t(__fastcall*)(const int32_t*);
using ChrPosFn      = const float*(__fastcall*)(void*, float*);

// The handle can outlive the NPC it named, and the resolver (a switch on the handle's
// low nibble, exe+0x17B830) hands back whatever sits in that slot now. Before a virtual
// call on it: its vtable and the slot's target both lie inside the game's own image.
bool InGameImage(uintptr_t P) {
    static const uintptr_t Base = ExeBase();
    static const uintptr_t End = [] {
        const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
        const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base + Dos->e_lfanew);
        return Base + Nt->OptionalHeader.SizeOfImage;
    }();
    return P >= Base && P < End;
}

bool TalkOpenNearbySafe(float* Distance) {
    *Distance = -1.0f;
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        const uintptr_t Events = Gm ? *reinterpret_cast<const uintptr_t*>(Gm + 0x70) : 0;
        const uintptr_t TalkMgr = Events ? *reinterpret_cast<const uintptr_t*>(Events + 0x48) : 0;
        const uintptr_t Local = Gm ? *reinterpret_cast<const uintptr_t*>(Gm + 0xD0) : 0;
        if (!TalkMgr || !Local) return false;
        const int32_t Handle = *reinterpret_cast<const int32_t*>(TalkMgr + 0x40);
        if (!Handle) return false;
        const uintptr_t Npc = reinterpret_cast<HandleToChrFn>(ExeBase() + kHandleToChr)(&Handle);
        if (!Npc || Npc == Local) return false;
        const uintptr_t NpcVtbl = *reinterpret_cast<const uintptr_t*>(Npc);
        const uintptr_t LocalVtbl = *reinterpret_cast<const uintptr_t*>(Local);
        if (!NpcVtbl || !LocalVtbl) return false;
        if (!InGameImage(NpcVtbl) || !InGameImage(LocalVtbl)) return false;
        const uintptr_t NpcPosAt = *reinterpret_cast<const uintptr_t*>(NpcVtbl + 0x148);
        const uintptr_t LocalPosAt = *reinterpret_cast<const uintptr_t*>(LocalVtbl + 0x148);
        if (!InGameImage(NpcPosAt) || !InGameImage(LocalPosAt)) return false;
        const ChrPosFn NpcPos = reinterpret_cast<ChrPosFn>(NpcPosAt);
        const ChrPosFn LocalPos = reinterpret_cast<ChrPosFn>(LocalPosAt);
        alignas(16) float A[4] = {}, B[4] = {};
        const float* P = NpcPos(reinterpret_cast<void*>(Npc), A);
        const float* Q = LocalPos(reinterpret_cast<void*>(Local), B);
        if (!P || !Q) return false;
        const float Dx = P[0] - Q[0], Dy = P[1] - Q[1], Dz = P[2] - Q[2];
        const float Sq = Dx * Dx + Dy * Dy + Dz * Dz;
        if (!(Sq == Sq)) return false;
        *Distance = Sq;
        return Sq <= kTalkRangeM * kTalkRangeM;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool IsTalkOpenNearby() {
    float Distance = 0.0f;
    return TalkOpenNearbySafe(&Distance);
}

// The partner's copy is about to be destroyed and made again (travel_sync.cpp): the pointer kept here
// would outlive it.
void ForgetPartnerCharacter() {
    g_partnerChrAt.store(0);
    g_partnerChr.store(0);
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
    if (Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + kRegisterPrompt),
                                                      reinterpret_cast<void*>(&RegisterPromptDetour),
                                                      &g_registerPromptOriginal)) {
        LOG_INFO("[TALK] watching which prompts are offered at all (exe+0x%X)", kRegisterPrompt);
    }
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
