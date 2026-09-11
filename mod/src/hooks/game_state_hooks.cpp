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
    auto* localPlayer = sessionMgr.GetLocalPlayer();
    if (localPlayer) {
        sessionMgr.NotifyPlayerDeath(localPlayer->playerId);
    }

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

    status = MH_EnableHook(targetFunc);
    if (status != MH_OK) {
        LOG_ERROR("MH_EnableHook failed: %s (target: %p)", MH_StatusToString(status), targetFunc);
        return false;
    }

    LOG_DEBUG("Hook installed at %p", targetFunc);
    return true;
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
