// Game State Hooks - Local event detection
//
// These hooks are SECONDARY to the protobuf interception approach.
// The protobuf hooks handle the critical disconnect prevention.
// These hooks detect local game events (death, boss kill) for logging
// and for triggering seamless mode state changes.
//
// If the AOB patterns for these functions can't be found, the mod still
// works - we just won't have local event logging.

#include "../../include/hooks.h"
#include "../../include/sync.h"
#include "../../include/session.h"
#include "../../include/network.h"
#include "../../include/utils.h"
#include "MinHook.h"

using namespace DS2Coop::Hooks;
using namespace DS2Coop::Utils;

// Original function pointers
static GameState::PlayerDeathFunc g_originalPlayerDeath = nullptr;
static GameState::BossDefeatedFunc g_originalBossDefeated = nullptr;

// ============================================================================
// Hook implementations
// ============================================================================

static void __fastcall PlayerDeathHook(void* playerPtr) {
    LOG_INFO("[GAME] Player death detected");

    // Notify session manager
    auto& sessionMgr = DS2Coop::Session::SessionManager::GetInstance();
    // By id, not through GetLocalPlayer(): that hands out a pointer into the
    // player list the network thread changes. NotifyPlayerDeath looks the id up
    // under the lock and does nothing outside a session.
    sessionMgr.NotifyPlayerDeath(DS2Coop::Network::PeerManager::GetInstance().GetLocalPlayerId());

    // Call original - let the death happen
    g_originalPlayerDeath(playerPtr);

    // The protobuf hooks will block the disconnect that follows
    LOG_INFO("[GAME] Player died - protobuf hooks will block disconnect");
}

static void __fastcall BossDefeatedHook(void* bossPtr) {
    LOG_INFO("[GAME] Boss defeated!");

    // Synchronize boss defeat
    auto& progressSync = DS2Coop::Sync::ProgressSync::GetInstance();
    progressSync.SyncBossDefeat(0); // TODO: extract boss ID from bossPtr

    // Call original
    g_originalBossDefeated(bossPtr);

    // The protobuf hooks will block the disconnect that follows
    LOG_INFO("[GAME] Boss killed - protobuf hooks will block disconnect");
}

// ============================================================================
// Installation
// ============================================================================
bool GameState::InstallHooks() {
    LOG_INFO("Installing game state hooks...");

    // These addresses need to be found via reverse engineering.
    // For now, they are null - the mod works without them via protobuf interception.
    void* playerDeathAddr = nullptr;
    void* bossDefeatedAddr = nullptr;

    // TODO: Add AOB patterns for these functions when found.
    // For now, the protobuf interception handles everything.

    if (!playerDeathAddr && !bossDefeatedAddr) {
        LOG_INFO("Game state hooks: no addresses available (non-critical)");
        LOG_INFO("Protobuf interception handles disconnect prevention.");
        return true;
    }

    int hooked = 0;

    if (playerDeathAddr) {
        if (HookManager::GetInstance().InstallHook(
            playerDeathAddr,
            reinterpret_cast<void*>(&PlayerDeathHook),
            reinterpret_cast<void**>(&g_originalPlayerDeath)
        )) {
            LOG_INFO("  HOOKED PlayerDeath");
            hooked++;
        }
    }

    if (bossDefeatedAddr) {
        if (HookManager::GetInstance().InstallHook(
            bossDefeatedAddr,
            reinterpret_cast<void*>(&BossDefeatedHook),
            reinterpret_cast<void**>(&g_originalBossDefeated)
        )) {
            LOG_INFO("  HOOKED BossDefeated");
            hooked++;
        }
    }

    LOG_INFO("Game state hooks: %d installed", hooked);
    return true;
}

void GameState::UninstallHooks() {
    LOG_INFO("Uninstalling game state hooks...");
}

// ============================================================================
// HookManager implementation (shared by all hook types)
// ============================================================================
HookManager& HookManager::GetInstance() {
    static HookManager instance;
    return instance;
}

bool HookManager::Initialize() {
    if (m_initialized) return true;

    LOG_INFO("Initializing MinHook...");

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        LOG_ERROR("Failed to initialize MinHook: %s", MH_StatusToString(status));
        return false;
    }

    m_initialized = true;
    LOG_INFO("MinHook initialized");
    return true;
}

void HookManager::Shutdown() {
    if (!m_initialized) return;

    LOG_INFO("Shutting down MinHook...");
    MH_Uninitialize();
    m_initialized = false;
}

// How deep this thread is in BeginBatch/EndBatch. Only the thread that opened a batch queues its
// hooks; one installed meanwhile on another thread goes live at once, as its caller expects.
static thread_local int t_batchDepth = 0;

bool HookManager::InstallHook(void* targetFunc, void* detourFunc, void** originalFunc) {
    if (!m_initialized) {
        LOG_ERROR("HookManager not initialized");
        return false;
    }

    MH_STATUS status = MH_CreateHook(targetFunc, detourFunc, originalFunc);
    if (status != MH_OK) {
        LOG_ERROR("MH_CreateHook failed: %s (target: %p)", MH_StatusToString(status), targetFunc);
        return false;
    }

    const bool Batched = t_batchDepth > 0;
    status = Batched ? MH_QueueEnableHook(targetFunc) : MH_EnableHook(targetFunc);
    if (status != MH_OK) {
        LOG_ERROR("%s failed: %s (target: %p)", Batched ? "MH_QueueEnableHook" : "MH_EnableHook",
                  MH_StatusToString(status), targetFunc);
        return false;
    }

    if (Batched) {
        m_batchQueued.fetch_add(1);
        LOG_DEBUG("Hook queued at %p", targetFunc);
    } else {
        LOG_DEBUG("Hook installed at %p", targetFunc);
    }
    return true;
}

void HookManager::BeginBatch() {
    ++t_batchDepth;
}

void HookManager::EndBatch() {
    if (t_batchDepth == 0 || --t_batchDepth > 0) return;
    const uint32_t Queued = m_batchQueued.exchange(0);
    if (!Queued || !m_initialized) return;
    const ULONGLONG Start = GetTickCount64();
    const MH_STATUS Status = MH_ApplyQueued();
    if (Status != MH_OK) {
        LOG_ERROR("MH_ApplyQueued failed: %s (%u hooks queued)", MH_StatusToString(Status), Queued);
        return;
    }
    LOG_INFO("%u hooks went live at once (%llu ms)", Queued,
             static_cast<unsigned long long>(GetTickCount64() - Start));
}

bool HookManager::RemoveHook(void* targetFunc) {
    if (!m_initialized) return false;

    MH_DisableHook(targetFunc);
    MH_RemoveHook(targetFunc);
    return true;
}

bool HookManager::EnableHooks() {
    if (!m_initialized) return false;
    return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
}

bool HookManager::DisableHooks() {
    if (!m_initialized) return false;
    return MH_DisableHook(MH_ALL_HOOKS) == MH_OK;
}
