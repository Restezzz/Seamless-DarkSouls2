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

constexpr uint32_t  kMpPlayersWarp  = 0x513440;   // (session) -> bool
constexpr uint32_t  kEsdMpReturn    = 0x45DD67;   // ESD function 0x1FE2A
constexpr uint32_t  kNpcKindReturn  = 0x356398;   // NPC factory, kind 8/11 vs 7/10
constexpr uint32_t  kGenAreaCreate  = 0x41A5F0;   // (generator manager, area index)
constexpr uint32_t  kNetRoot        = 0x1616CF8;
constexpr uint32_t  kJoinCtrlVtable = 0x10D7BD8;
constexpr ULONGLONG kDeferMaxMs     = 20000;

using PredFn      = uint64_t(__fastcall*)(void*);
using GenCreateFn = void(__fastcall*)(void*, int32_t);

PredFn      g_mpPlayersWarp = nullptr;
GenCreateFn g_genCreate     = nullptr;

std::atomic<bool>      g_talkScripts{ true };      // ini guest_npc_talk_scripts
std::atomic<bool>      g_npcLocal{ true };         // ini guest_npc_local
std::atomic<bool>      g_waitSnapshot{ true };     // ini guest_wait_for_snapshot
std::atomic<uint32_t>  g_esdAnswers{ 0 };
std::atomic<uint32_t>  g_kindAnswers{ 0 };
std::atomic<uintptr_t> g_deferCtrl{ 0 };
std::atomic<ULONGLONG> g_deferSince{ 0 };
std::atomic<uint32_t>  g_deferredCalls{ 0 };

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

uint64_t __fastcall MpPlayersWarpDetour(void* Session) {
    const uint64_t Stock = g_mpPlayersWarp(Session);
    if (!(Stock & 0xFF)) return Stock;
    const uintptr_t Ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase();
    const bool Esd  = Ret == kEsdMpReturn && g_talkScripts.load();
    const bool Kind = Ret == kNpcKindReturn && g_npcLocal.load();
    if ((!Esd && !Kind) || !GuestInALobby()) return Stock;
    const uint32_t N = (Esd ? g_esdAnswers : g_kindAnswers).fetch_add(1) + 1;
    if (N <= 3 || N % 1000 == 0) {
        LOG_INFO("[NPC] %s asked whether this is someone else's multiplayer world -- answered no, as for the "
                 "world's owner (%u so far)", Esd ? "a character script (ESD 0x1FE2A)" : "the NPC factory", N);
    }
    return Stock & ~static_cast<uint64_t>(0xFF);
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
    g_genCreate(Mgr, AreaIndex);
}

bool HookAt(uint32_t Rva, void* Detour, void** Original, const char* What) {
    if (Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + Rva), Detour, Original)) {
        return true;
    }
    LOG_WARNING("[WORLD] could not hook exe+0x%X (%s)", Rva, What);
    return false;
}

} // namespace

bool InstallGuestWorld(bool TalkScripts, bool NpcLocal, bool WaitSnapshot) {
    static bool Installed = false;
    g_talkScripts.store(TalkScripts);
    g_npcLocal.store(NpcLocal);
    g_waitSnapshot.store(WaitSnapshot);
    if (!Installed) {
        Installed = true;
        HookAt(kMpPlayersWarp, reinterpret_cast<void*>(&MpPlayersWarpDetour),
               reinterpret_cast<void**>(&g_mpPlayersWarp), "the multiplayer-world predicate");
        HookAt(kGenAreaCreate, reinterpret_cast<void*>(&GenAreaCreateDetour),
               reinterpret_cast<void**>(&g_genCreate), "area generator creation");
    }
    LOG_INFO("[WORLD] a guest's world: NPC scripts %s, NPCs %s, characters %s",
             TalkScripts ? "as the owner's (talk)" : "the game's way",
             NpcLocal ? "run here (can be hit and talked to)" : "the game's way",
             WaitSnapshot ? "put in once the host's world has arrived" : "put in as the area loads");
    return g_mpPlayersWarp != nullptr && g_genCreate != nullptr;
}

} // namespace DS2Coop::Sync
