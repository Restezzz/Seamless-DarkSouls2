// A boss fight that ends while one of the two players is down (docs §3.47; 17.09, second
// report, point 9: "the host dies on the boss, I kill it, the fight does not end and nobody
// gets a reward").
//
// Two gates freeze a dead player's side of the fight. Both answer the same question -- is the
// local character dead ([GMImp+0xD0] exists and its vt[0x1B0], the alive test IsHostDead
// uses, says no):
//
//   exe+0x248890, asked by the event area update exe+0x1959C0 (CALL at exe+0x1959EA). While
//     it says yes, only the tasks whose EventParam row has bit 0x02 at +0xD run. A boss's end
//     is a script: the IsBossKill condition (0x224EA) and command 0x2046A -> exe+0x1810E0 --
//     and the boss rows have that bit clear (m10_16 row 16000, the Belfry's 1016020: +0xD is
//     0). So a dead host never evaluates the kill: on 17.09 the last gargoyle fell at the
//     guest's at 17:42:24 and the host's manager stayed at "1016020 running, phase 1" until
//     its own respawn load, and the same again at 17:48:42.
//
//   exe+0x248940, asked by the boss update exe+0x181490 (CALL at exe+0x1814DA, JNZ past the
//     work). Phases 0, 2 and 3 are skipped while it says yes -- only phase 1 is exempt (table
//     exe+0x10C2E70). Phase 3 is where the souls (exe+0x181950) and the reward item
//     (exe+0x181850) are handed out, so a player who was down when the boss died got neither.
//
// While a boss fight is on and the other player is still up, the host's event tasks keep
// running although the host is down, and phases 2 and 3 finish for a player who is down --
// the host, or a guest whose copy of the host's fight was driven to its end (death_sync.cpp).
// Both gates are only ever answered differently while this player's own HP is at or below
// zero, which is read separately (PlayerCtrl+0x168): if the gates meant the opposite of what
// the disassembly says, nothing here would ever fire.

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

constexpr uint32_t kGameManagerImp = 0x16148F0;
constexpr uint32_t kNetRoot        = 0x1616CF8;
constexpr uint32_t kJoinCtrlVtable = 0x10D7BD8;
constexpr uint32_t kDeadForEvents  = 0x248890;   // () -> the local character is dead
constexpr uint32_t kDeadForBoss    = 0x248940;   // () -> the same, for the boss update
constexpr uint32_t kEventsReturn   = 0x1959EF;   // exe+0x1959C0: the event area update
constexpr uint32_t kBossReturn     = 0x1814DF;   // exe+0x181490: the boss update
constexpr int32_t  kJoinInWorld    = 7;

using GateFn = uint64_t(__fastcall*)();

GateFn g_deadForEvents = nullptr;
GateFn g_deadForBoss   = nullptr;

std::atomic<bool>     g_enabled{ true };     // ini boss_while_down
std::atomic<uint32_t> g_eventPasses{ 0 };
std::atomic<uint32_t> g_bossPasses{ 0 };

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

// This player's own HP, PlayerCtrl+0x168; false while there is no PlayerCtrl.
bool LocalDown() {
    uintptr_t Gm = 0, Player = 0;
    int32_t Hp = 0;
    if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0xD0, &Player)) return false;
    return ReadI32(Player + 0x168, &Hp) && Hp <= 0;
}

struct BossRead {
    int32_t   Battle;
    int32_t   Phase;
    uintptr_t Row;
};

bool ReadBossSafe(BossRead* Out) {
    uintptr_t Gm = 0, Events = 0, Boss = 0;
    if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0x70, &Events) ||
        !ReadPtr(Events + 0x88, &Boss)) {
        return false;
    }
    __try {
        Out->Battle = *reinterpret_cast<const int32_t*>(Boss + 0x14);
        Out->Phase = *reinterpret_cast<const int32_t*>(Boss + 0x204);
        Out->Row = *reinterpret_cast<const uintptr_t*>(Boss + 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// How many parts of the boss are still alive (bm+0xBC, +0x15C, +0x1FC): for the log.
void ReadPartsSafe(uint32_t* A, uint32_t* B, uint32_t* C) {
    *A = *B = *C = 0;
    uintptr_t Gm = 0, Events = 0, Boss = 0;
    if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0x70, &Events) ||
        !ReadPtr(Events + 0x88, &Boss)) {
        return;
    }
    __try {
        *A = *reinterpret_cast<const uint32_t*>(Boss + 0xBC);
        *B = *reinterpret_cast<const uint32_t*>(Boss + 0x15C);
        *C = *reinterpret_cast<const uint32_t*>(Boss + 0x1FC);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int JoinState() {
    uintptr_t Root = 0, Mp = 0, Ctrl = 0, Vtbl = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp) || !ReadPtr(Mp + 0x40, &Ctrl)) return -1;
    if (!ReadPtr(Ctrl, &Vtbl) || Vtbl != ExeBase() + kJoinCtrlVtable) return -1;
    int32_t State = -1;
    return ReadI32(Ctrl + 0xF8, &State) ? State : -1;
}

// The host's side of "a guest is in my world": an accept controller at state 0x10.
bool GuestFullyInSafe() {
    uintptr_t Root = 0, Mp = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp)) return false;
    __try {
        uintptr_t It = *reinterpret_cast<const uintptr_t*>(Mp + 0x48);
        const uintptr_t End = *reinterpret_cast<const uintptr_t*>(Mp + 0x50);
        for (int Guard = 0; It && It < End && Guard < 16; It += 8, ++Guard) {
            const uintptr_t Ctrl = *reinterpret_cast<const uintptr_t*>(It);
            if (Ctrl && *reinterpret_cast<const int32_t*>(Ctrl + 0x150) == 0x10) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// The host is down, a guest is in its world and still up, and a fight is on: its event
// scripts carry on, so the guest's kill is seen and the fight ends.
bool HostCarriesOn(BossRead* B) {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive() || !Lobby.IsHost() || !PartnerAliveReported()) return false;
    if (!ReadBossSafe(B) || B->Battle <= 0 || !B->Row || B->Phase < 1 || B->Phase > 3) return false;
    if (!LocalDown()) return false;
    return GuestFullyInSafe();
}

// A fight already decided pays out although this player is down: the host with a guest in
// its world, or a guest holding its return while its copy of the host's fight ends.
bool FinishWhileDown(BossRead* B) {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    if (!ReadBossSafe(B) || B->Battle <= 0 || !B->Row || (B->Phase != 2 && B->Phase != 3)) return false;
    if (!LocalDown()) return false;
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive()) return false;
    if (Lobby.IsHost()) return GuestFullyInSafe();
    return JoinState() == kJoinInWorld && GuestHeldForBattle(B->Battle);
}

uint64_t __fastcall DeadForEventsDetour() {
    const uint64_t Stock = g_deadForEvents();
    if (!(Stock & 0xFF)) return Stock;
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != ExeBase() + kEventsReturn) return Stock;
    BossRead B{};
    if (!HostCarriesOn(&B)) return Stock;
    const uint32_t N = g_eventPasses.fetch_add(1) + 1;
    static int32_t s_told = 0;
    if (s_told != B.Battle) {
        s_told = B.Battle;
        uint32_t P1 = 0, P2 = 0, P3 = 0;
        ReadPartsSafe(&P1, &P2, &P3);
        LOG_INFO("[BOSS] down in battle %d (phase %d) while the guest fights on -- the event scripts keep running "
                 "here, so the kill is seen (parts left %u/%u/%u, %u passes)", B.Battle, B.Phase, P1, P2, P3, N);
    }
    return Stock & ~static_cast<uint64_t>(0xFF);
}

uint64_t __fastcall DeadForBossDetour() {
    const uint64_t Stock = g_deadForBoss();
    if (!(Stock & 0xFF)) return Stock;
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != ExeBase() + kBossReturn) return Stock;
    BossRead B{};
    if (!FinishWhileDown(&B)) return Stock;
    g_bossPasses.fetch_add(1);
    static int32_t s_battle = 0, s_phase = 0;
    if (s_battle != B.Battle || s_phase != B.Phase) {
        s_battle = B.Battle;
        s_phase = B.Phase;
        LOG_INFO("[BOSS] down, but battle %d is decided: its phase %d runs here and hands out souls and the reward",
                 B.Battle, B.Phase);
    }
    return Stock & ~static_cast<uint64_t>(0xFF);
}

} // namespace

bool InstallBossDown(bool Enabled) {
    static bool Installed = false;
    g_enabled.store(Enabled);
    if (!Installed) {
        Installed = true;
        auto& Hooks = Hooks::HookManager::GetInstance();
        const bool Events = Hooks.InstallHook(reinterpret_cast<void*>(ExeBase() + kDeadForEvents),
                                              reinterpret_cast<void*>(&DeadForEventsDetour),
                                              reinterpret_cast<void**>(&g_deadForEvents));
        const bool Boss = Hooks.InstallHook(reinterpret_cast<void*>(ExeBase() + kDeadForBoss),
                                            reinterpret_cast<void*>(&DeadForBossDetour),
                                            reinterpret_cast<void**>(&g_deadForBoss));
        if (!Events) g_deadForEvents = nullptr;
        if (!Boss) g_deadForBoss = nullptr;
        LOG_INFO("[BOSS] a fight that ends while a player is down: event scripts exe+0x%X %s, the fight's own "
                 "phases exe+0x%X %s", kDeadForEvents, Events ? "hooked" : "NOT hooked", kDeadForBoss,
                 Boss ? "hooked" : "NOT hooked");
    }
    LOG_INFO("[BOSS] the boss fight carries on while a player is down: %s", Enabled ? "on" : "off");
    return g_deadForEvents != nullptr && g_deadForBoss != nullptr;
}

} // namespace DS2Coop::Sync
