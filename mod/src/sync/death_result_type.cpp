// A guest's death taken for the host's (0.2.2 point 3: "I was not respawned and just
// watched through my friend's camera").
//
// Each map load builds the death result the next death runs through:
// exe+0x44EF30 -> exe+0x191BB0 reads the local player's phantom id
// ([[GMImp+0xD0]+0xB0]+0x3C, 0 without a player) and the constructor exe+0x18F3A0
// keeps it at +0xE0. At the end of a death exe+0x18FFD0 picks the branch by that
// type: 0 is the host's (exe+0x190920), which waits in exe+0x191210 for as long as
// any session exists -- so a guest whose result was built with 0 stays dead, with
// the camera on the partner, until it leaves the lobby. On 17.09 at 14:45:49 the
// guest's result had type 0 ("phantom type 0" in the result sequence); after every
// join it is 1. The mod's own once-a-second zeroing of that id (EnableSummoning,
// for a solid look) could land between the game setting it and the read on a
// guest's own travel, which keeps join state 7 -- it is held back during loads now.
//
// This is the net under that: a guest in the host's world (join state 7) whose
// result is being built with type 0 gets the type its last result had in that
// state, 1 if none. Every result a guest's load builds is logged (probe P1).
// Ini guest_result_type_fix; off, the numbers are still logged.

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

constexpr uint32_t kResultCtor     = 0x18F3A0;    // EventResult::EventResult(this, phantom type) -> this
constexpr uint32_t kNetRoot        = 0x1616CF8;
constexpr uint32_t kGameManagerImp = 0x16148F0;
constexpr uint32_t kJoinCtrlVtable = 0x10D7BD8;
constexpr int      kJoinInWorld    = 7;

using CtorFn = void*(__fastcall*)(void*, uint8_t);

CtorFn               g_ctorOriginal = nullptr;
std::atomic<bool>    g_fixOn{ true };
std::atomic<uint8_t> g_lastGuestType{ 0 };

uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

struct GameNumbers {
    int     Join;        // join controller state, -1 none
    int32_t GameState;   // [GMImp+0x24AC]
    uint8_t Flags;       // [GMImp+0x24B1]
    int     PhantomId;   // [[GMImp+0xD0]+0xB0]+0x3C, -1 no player
};

GameNumbers ReadNumbersSafe() {
    GameNumbers N{ -1, -1, 0, -1 };
    __try {
        const uintptr_t Root = *reinterpret_cast<const uintptr_t*>(ExeBase() + kNetRoot);
        const uintptr_t Mp = Root ? *reinterpret_cast<const uintptr_t*>(Root + 0x18) : 0;
        const uintptr_t Ctrl = Mp ? *reinterpret_cast<const uintptr_t*>(Mp + 0x40) : 0;
        if (Ctrl && *reinterpret_cast<const uintptr_t*>(Ctrl) == ExeBase() + kJoinCtrlVtable) {
            N.Join = *reinterpret_cast<const int32_t*>(Ctrl + 0xF8);
        }
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        if (Gm) {
            N.GameState = *reinterpret_cast<const int32_t*>(Gm + 0x24AC);
            N.Flags = *reinterpret_cast<const uint8_t*>(Gm + 0x24B1);
            const uintptr_t Player = *reinterpret_cast<const uintptr_t*>(Gm + 0xD0);
            const uintptr_t Type = Player ? *reinterpret_cast<const uintptr_t*>(Player + 0xB0) : 0;
            if (Type) N.PhantomId = *reinterpret_cast<const uint8_t*>(Type + 0x3C);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return N;
}

void* __fastcall ResultCtorDetour(void* Result, uint8_t Type) {
    auto& Lobby = Session::SessionManager::GetInstance();
    if (Lobby.IsActive() && !Lobby.IsHost()) {
        const GameNumbers N = ReadNumbersSafe();
        uint8_t Use = Type;
        if (N.Join == kJoinInWorld) {
            if (Type != 0) {
                g_lastGuestType.store(Type);
            } else if (g_fixOn.load()) {
                const uint8_t Seen = g_lastGuestType.load();
                Use = Seen ? Seen : 1;
            }
        }
        const bool LeftAsHost = N.Join == kJoinInWorld && Use == 0;
        LOG_INFO("[DEATH] death result built with phantom type %u (join state %d, game state 0x%X, warp bit %s, "
                 "phantom id now %d)%s", static_cast<unsigned>(Type), N.Join, static_cast<unsigned>(N.GameState),
                 (N.Flags & 0x02) ? "on" : "off", N.PhantomId,
                 LeftAsHost ? " -- the host's branch, left as it is (guest_result_type_fix=false)" : "");
        if (Use != Type) {
            LOG_WARNING("[DEATH] a guest in the host's world would die as the host (type 0, which waits for the "
                        "session to end) -- built with type %u instead", static_cast<unsigned>(Use));
        }
        return g_ctorOriginal(Result, Use);
    }
    return g_ctorOriginal(Result, Type);
}

} // namespace

bool InstallDeathResultType(bool Enabled) {
    static bool Installed = false;
    g_fixOn.store(Enabled);
    if (!Installed) {
        Installed = true;
        if (!Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + kResultCtor),
                                                          reinterpret_cast<void*>(&ResultCtorDetour),
                                                          reinterpret_cast<void**>(&g_ctorOriginal))) {
            g_ctorOriginal = nullptr;
            LOG_WARNING("[DEATH] could not hook exe+0x%X (death result constructor)", kResultCtor);
        }
    }
    LOG_INFO("[DEATH] a guest's death result: %s", !g_ctorOriginal ? "NOT hooked"
             : Enabled ? "never built as the host's while in the host's world (guest_result_type_fix)"
                       : "logged only (guest_result_type_fix=false)");
    return g_ctorOriginal != nullptr;
}

} // namespace DS2Coop::Sync
