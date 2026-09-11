#pragma once

#include <cstdint>

namespace DS2Coop {

/**
 * Game address resolver
 * Finds and caches important game memory addresses using pattern scanning
 */
class AddressResolver {
public:
    static AddressResolver& GetInstance() {
        static AddressResolver instance;
        return instance;
    }
    
    /**
     * Initialize and find all required addresses
     * @return true if all critical addresses were found
     */
    bool Initialize();
    
    /**
     * Get resolved addresses
     */
    uintptr_t GetGameManagerImp() const { return m_game_manager_imp; }
    uintptr_t GetNetSessionManager() const { return m_net_session_manager; }
    uintptr_t GetKatanaMainApp() const { return m_katana_main_app; }
    
    /**
     * Check if all addresses are valid
     */
    bool IsValid() const {
        return m_game_manager_imp != 0 && m_net_session_manager != 0;
    }

private:
    AddressResolver() = default;
    ~AddressResolver() = default;
    AddressResolver(const AddressResolver&) = delete;
    AddressResolver& operator=(const AddressResolver&) = delete;
    
    uintptr_t m_game_manager_imp = 0;
    uintptr_t m_net_session_manager = 0;
    uintptr_t m_katana_main_app = 0;
};

} // namespace DS2Coop

