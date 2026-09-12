// Resting at a bonfire resets the world -- for everyone in the session.
//
// Each client keeps its own enemies: they are spawned locally by enemy
// generators, and the host's updates drive them only where the same generator
// is alive on both sides. The rest reset ran on the resting player's machine
// alone, so after a guest rested only the guest saw the respawned mobs, and the
// host had to rest as well before they appeared there.
//
// The reset is exe+0x17FD70. The bonfire menu's state machine (exe+0x17ED90)
// tail-jumps to it once per rest (exe+0x17F062). It takes nothing and reads
// everything from the game manager:
//
//   exe+0x417210([GMImp+0x40])     EnemyGeneratorManager: despawn, reset all 42
//                                  areas, queue every generator to spawn again,
//                                  pause spawning for 6 frames (+0x331)
//   exe+0x3C1B50 -> 0x3C27F0(0,0)  map objects back to their initial state
//   exe+0x44F880([GMImp+0x70])     the event manager's per-rest reset
//
// Hooked, it tells the other players. A player told replays the original --
// through the trampoline, so the replay is not reported back -- at the start of
// the enemy generator manager's own update (exe+0x417810, main loop): nothing is
// iterating the generator lists at that point, and the update only runs while a
// world is loaded, so a reset that arrives during a loading screen simply waits.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../../include/sync.h"
#include "../../include/hooks.h"
#include "../../include/network.h"
#include "../../include/session.h"
#include "../../include/ui.h"
#include "../../include/ui_settings.h"
#include "../../include/utils.h"

#include <atomic>
#include <mutex>
#include <string>

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t kRestResetRva = 0x17FD70;   // world reset on rest
constexpr uint32_t kGenUpdateRva = 0x417810;   // EnemyGeneratorManager update

// The reset takes no arguments; the detour forwards the four argument registers
// untouched anyway, in case the caller left something the game relies on.
using RestResetFn = void(__fastcall*)(void*, void*, void*, void*);
using GenUpdateFn = void(__fastcall*)(void*, float*);

RestResetFn g_restReset = nullptr;
GenUpdateFn g_genUpdate = nullptr;

std::atomic<bool> g_installed{ false };
std::atomic<bool> g_enabled{ true };
std::atomic<bool> g_pending{ false };
std::atomic<bool> g_broken{ false };   // a replay threw once: stop replaying this run
std::mutex        g_fromMutex;
std::string       g_pendingFrom;

bool HavePartner() {
    return Session::SessionManager::GetInstance().GetPlayers().size() > 1;
}

void BroadcastReset() {
    Network::PacketHeader Header{};
    Header.magic = 0x44533243;
    Header.type = Network::PacketType::WorldReset;
    Header.size = sizeof(Header);
    Header.timestamp = GetTickCount64();
    Network::PeerManager::GetInstance().BroadcastPacket(&Header);
}

void __fastcall RestResetDetour(void* A, void* B, void* C, void* D) {
    g_restReset(A, B, C, D);
    if (!g_enabled.load() || !HavePartner()) return;
    LOG_INFO("[WORLD] rested here -- the world was reset; telling the other players");
    BroadcastReset();
    UI::Overlay::GetInstance().ShowNotification(
        UI::Tr("Enemies respawned for your partner too", "Враги возродились и у напарника"),
        3.5f, UI::NotifyKind::Player);
}

// No C++ objects in here: the replay runs under SEH.
bool ReplayResetSafely() {
    __try {
        g_restReset(nullptr, nullptr, nullptr, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall GenUpdateDetour(void* Manager, float* Dt) {
    // The same safe spot on the game thread serves the world item toggle and
    // the free-travel code bytes (never written while the game runs them).
    LootSyncGameTick();
    FreeTravelGameTick();
    DeathSyncGameTick();
    SummonAcceptGameTick();
    PvpModesGameTick();
    if (g_pending.exchange(false) && g_enabled.load() && !g_broken.load()) {
        std::string From;
        {
            std::lock_guard<std::mutex> Lock(g_fromMutex);
            From = g_pendingFrom;
        }
        if (ReplayResetSafely()) {
            LOG_INFO("[WORLD] %s rested -- the world was reset here too", From.c_str());
            UI::Overlay::GetInstance().ShowNotification(
                UI::Format(UI::Tr("%s rested at a bonfire \xE2\x80\x94 enemies are back",
                                  "Игрок %s отдохнул у костра \xE2\x80\x94 враги возродились"),
                           From.c_str()),
                4.0f, UI::NotifyKind::Player);
        } else {
            g_broken.store(true);
            LOG_ERROR("[WORLD] replaying the rest reset threw -- rest sync is off for this run");
        }
    }
    g_genUpdate(Manager, Dt);
}

bool Hook(uint32_t Rva, void* Detour, void** Original, const char* What) {
    void* Target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)) + Rva);
    if (Hooks::HookManager::GetInstance().InstallHook(Target, Detour, Original)) return true;
    LOG_WARNING("[WORLD] could not hook %s at exe+0x%X", What, Rva);
    return false;
}

} // namespace

bool InstallWorldSync() {
    if (g_installed.exchange(true)) return g_restReset && g_genUpdate;
    const bool Reset = Hook(kRestResetRva, reinterpret_cast<void*>(&RestResetDetour),
                            reinterpret_cast<void**>(&g_restReset), "the rest reset");
    const bool Update = Reset && Hook(kGenUpdateRva, reinterpret_cast<void*>(&GenUpdateDetour),
                                      reinterpret_cast<void**>(&g_genUpdate), "the enemy generator update");
    LOG_INFO("[WORLD] rest sync %s", Reset && Update ? "hooked (exe+0x17FD70, exe+0x417810)" : "unavailable");
    return Reset && Update;
}

void SetWorldSyncEnabled(bool enabled) {
    g_enabled.store(enabled);
    LOG_INFO("[WORLD] rest sync %s", enabled ? "ON: one player's rest respawns everyone's enemies" : "OFF");
}

void RequestWorldReset(const std::string& fromName) {
    if (!g_enabled.load() || !g_restReset || !g_genUpdate) return;
    {
        std::lock_guard<std::mutex> Lock(g_fromMutex);
        g_pendingFrom = fromName;
    }
    g_pending.store(true);
    LOG_INFO("[WORLD] %s rested -- resetting the world here on the next enemy update", fromName.c_str());
}

} // namespace DS2Coop::Sync
