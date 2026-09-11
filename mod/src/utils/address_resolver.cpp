#include "address_resolver.h"
#include "addresses.h"
#include "pattern_scanner.h"
#include "utils.h"

namespace DS2Coop {

bool AddressResolver::Initialize() {
    using namespace Utils;
    using namespace Addresses;

    Logger::GetInstance().LogInfo("=== Address Resolution Starting ===");

    // AOB patterns are in the static .text section — valid once the exe is mapped.
    // The global pointers they point to may be null until the game's managers
    // are constructed (Steam overlay, anti-cheat, engine init all happen async).
    // Retry reading the values for up to 30 seconds.

    // --- Locate static pointer addresses via AOB (one-time scan) ---
    uintptr_t gm_ptr_addr = 0;
    Logger::GetInstance().LogInfo("Scanning for GameManagerImp...");
    {
        uintptr_t match = PatternScanner::FindPattern(
            GAME_MANAGER_IMP.pattern, GAME_MANAGER_IMP.mask, nullptr);
        if (match)
            gm_ptr_addr = PatternScanner::ResolveRIP(
                match, GAME_MANAGER_IMP.offset_from_match, GAME_MANAGER_IMP.pointer_offset);
        else
            Logger::GetInstance().LogError("GameManagerImp pattern not found!");
    }

    uintptr_t ns_ptr_addr = 0;
    Logger::GetInstance().LogInfo("Scanning for NetSessionManager...");
    {
        uintptr_t match = PatternScanner::FindPattern(
            NET_SESSION_MANAGER.pattern, NET_SESSION_MANAGER.mask, nullptr);
        if (match)
            ns_ptr_addr = PatternScanner::ResolveRIP(
                match, NET_SESSION_MANAGER.offset_from_match, NET_SESSION_MANAGER.pointer_offset);
        else
            Logger::GetInstance().LogError("NetSessionManager pattern not found!");
    }

    uintptr_t ka_ptr_addr = 0;
    {
        uintptr_t match = PatternScanner::FindPattern(
            KATANA_MAIN_APP.pattern, KATANA_MAIN_APP.mask, nullptr);
        if (match)
            ka_ptr_addr = PatternScanner::ResolveRIP(
                match, KATANA_MAIN_APP.offset_from_match, KATANA_MAIN_APP.pointer_offset);
    }

    // --- Retry reading pointer values until non-null ---
    const int maxRetries = 60; // up to 30 seconds
    for (int attempt = 0; attempt < maxRetries; attempt++) {
        if (gm_ptr_addr && !m_game_manager_imp) {
            uintptr_t val = 0;
            if (PatternScanner::ReadPointer(gm_ptr_addr, val) && val) {
                m_game_manager_imp = val;
                Logger::GetInstance().LogInfo("GameManagerImp = 0x%p (after %d retries)",
                    reinterpret_cast<void*>(m_game_manager_imp), attempt);
            }
        }
        if (ns_ptr_addr && !m_net_session_manager) {
            uintptr_t val = 0;
            if (PatternScanner::ReadPointer(ns_ptr_addr, val) && val) {
                m_net_session_manager = val;
                Logger::GetInstance().LogInfo("NetSessionManager = 0x%p (after %d retries)",
                    reinterpret_cast<void*>(m_net_session_manager), attempt);
            }
        }
        if (ka_ptr_addr && !m_katana_main_app) {
            uintptr_t val = 0;
            if (PatternScanner::ReadPointer(ka_ptr_addr, val) && val)
                m_katana_main_app = val;
        }

        if (m_game_manager_imp && m_net_session_manager) break;
        if (attempt < maxRetries - 1) Sleep(500);
    }

    Logger::GetInstance().LogInfo("=== Address Resolution Complete ===");
    Logger::GetInstance().LogInfo("  GameManagerImp:    0x%p %s",
        reinterpret_cast<void*>(m_game_manager_imp),
        m_game_manager_imp ? "[OK]" : "[FAILED]");
    Logger::GetInstance().LogInfo("  NetSessionManager: 0x%p %s",
        reinterpret_cast<void*>(m_net_session_manager),
        m_net_session_manager ? "[OK]" : "[FAILED]");
    Logger::GetInstance().LogInfo("  KatanaMainApp:     0x%p %s",
        reinterpret_cast<void*>(m_katana_main_app),
        m_katana_main_app ? "[OK]" : "[OPTIONAL]");

    return IsValid();
}

} // namespace DS2Coop
