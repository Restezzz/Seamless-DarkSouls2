// Main mod initialization
//
// Hook installation order:
// 1. MinHook initialization
// 2. Address resolution (GameManagerImp, NetSessionManager via AOB scan)
// 3. Protobuf interception hooks (the core mechanism - blocks disconnect messages)
// 4. Winsock hooks (connection monitoring)
// 5. Game state hooks (optional - local event detection)
// 6. Network/session/UI subsystems

#include "../../include/mod.h"
#include "../../include/hooks.h"
#include "../../include/session.h"
#include "../../include/network.h"
#include "../../include/sync.h"
#include "../../include/ui.h"
#include "../../include/utils.h"
#include "../../include/address_resolver.h"
#include "../../include/ui_settings.h"
#include <fstream>
#include <chrono>
#include <utility>

using namespace DS2Coop;
using namespace DS2Coop::Utils;

SeamlessCoopMod& SeamlessCoopMod::GetInstance() {
    static SeamlessCoopMod instance;
    return instance;
}

bool SeamlessCoopMod::Initialize() {
    if (m_initialized) {
        LOG_WARNING("Mod already initialized");
        return true;
    }

    LOG_INFO("==========================================");
    LOG_INFO("Initializing Seamless Co-op Mod...");
    LOG_INFO("==========================================");

    // Load configuration
    LoadConfig();
    UI::InitUiSettings(m_config.language, m_config.menu_key);

    if (!m_config.enabled) {
        LOG_INFO("Mod is disabled in configuration");
        return false;
    }

    // Detect game version
    DetectGameVersion();

    // ================================================================
    // STEP 1: Initialize MinHook
    // ================================================================
    LOG_INFO("[1/6] Initializing MinHook...");
    if (!Hooks::HookManager::GetInstance().Initialize()) {
        LOG_ERROR("FATAL: MinHook initialization failed");
        return false;
    }
    LOG_INFO("  MinHook ready");

    // ================================================================
    // STEP 2: Resolve game memory addresses via AOB pattern scanning
    // ================================================================
    LOG_INFO("[2/6] Scanning for game addresses...");
    bool addressesFound = AddressResolver::GetInstance().Initialize();
    if (addressesFound) {
        LOG_INFO("  GameManagerImp:    0x%p [OK]",
                 reinterpret_cast<void*>(AddressResolver::GetInstance().GetGameManagerImp()));
        LOG_INFO("  NetSessionManager: 0x%p [OK]",
                 reinterpret_cast<void*>(AddressResolver::GetInstance().GetNetSessionManager()));
    } else {
        LOG_WARNING("  Address resolution failed - player data reads will be unavailable");
        LOG_WARNING("  Protobuf hooks may still work for disconnect prevention");
    }

    // ================================================================
    // STEP 3: Install protobuf interception hooks (THE CRITICAL HOOKS)
    // These hook SerializeWithCachedSizesToArray and ParseFromArray
    // to intercept and block disconnect messages at the network layer.
    // ================================================================
    LOG_INFO("[3/6] Installing protobuf interception hooks...");
    bool protobufHooked = Hooks::ProtobufHooks::InstallHooks();
    if (protobufHooked) {
        LOG_INFO("  Protobuf hooks ACTIVE - disconnect blocking available");
        // Enable seamless mode immediately
        Hooks::ProtobufHooks::SetSeamlessActive(true);
    } else {
        LOG_ERROR("  Protobuf hooks FAILED - mod running in passive mode");
        LOG_ERROR("  Session disconnect prevention will NOT work");
    }

    // ================================================================
    // STEP 4: Install Winsock hooks + server redirect
    // ================================================================
    LOG_INFO("[4/7] Installing Winsock hooks...");
    Hooks::WinsockHooks::InstallHooks();

    if (m_config.use_custom_server) {
        LOG_INFO("[4/7] Setting up server redirect to %s:%u...",
                 m_config.server_ip.c_str(), m_config.server_port);

        // Configure the Winsock hook to redirect port 50031
        Hooks::WinsockHooks::SetServerRedirect(m_config.server_ip, m_config.server_port);

        // The server's public key: the copy in the game folder (the file a friend
        // is sent), else the host's own server folder next to the game (the
        // package's SeamlessServer; older setups used Server_upstream or Server).
        std::string keyPath;
        {
            static const char* const kKeyCandidates[] = {
                "ds2_server_public.key",
                "SeamlessServer/Saved/default/public.key",
                "Server_upstream/Saved/default/public.key",
                "Server/Saved/default/public.key",
                "Saved/default/public.key",
            };
            std::ifstream testKey;
            for (const char* candidate : kKeyCandidates) {
                testKey.open(candidate);
                if (testKey.good()) {
                    keyPath = candidate;
                    LOG_INFO("[4/7] Server public key: %s", candidate);
                    break;
                }
                testKey.close();
                testKey.clear();
            }
            if (keyPath.empty()) {
                LOG_WARNING("[4/7] No public key file found — RSA patching will be skipped");
                LOG_WARNING("[4/7] Place ds2_server_public.key in game folder for server auth");
                // Still patch hostname in background thread (needs SteamStub wait)
                std::string ip = m_config.server_ip;
                CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
                    auto* ipStr = static_cast<std::string*>(param);
                    Hooks::ServerRedirect::PatchHostname(*ipStr);
                    delete ipStr;
                    return 0;
                }, new std::string(ip), 0, nullptr);
            } else {
                testKey.close();
                // Run hostname + RSA patching in a background thread
                // (needs to wait for SteamStub to unpack)
                std::string ip = m_config.server_ip;
                std::string kp = keyPath;
                CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
                    auto* args = static_cast<std::pair<std::string, std::string>*>(param);
                    Hooks::ServerRedirect::Install(args->first, args->second);
                    delete args;
                    return 0;
                }, new std::pair<std::string, std::string>(ip, kp), 0, nullptr);
            }
        }
    }

    // ================================================================
    // STEP 5: Install game state hooks (optional local event detection)
    // ================================================================
    LOG_INFO("[5/7] Installing game state hooks...");
    Hooks::GameState::InstallHooks();

    // ================================================================
    // STEP 6: Initialize subsystems
    // ================================================================
    LOG_INFO("[6/7] Initializing subsystems...");

    // Network manager (our P2P layer)
    if (!Network::PeerManager::GetInstance().Initialize(m_config.port)) {
        LOG_WARNING("  Network manager failed to initialize (can retry from menu)");
    } else {
        LOG_INFO("  Network manager ready (port %u)", m_config.port);
    }

    // Session manager
    if (!Session::SessionManager::GetInstance().Initialize()) {
        LOG_WARNING("  Session manager failed to initialize");
    } else {
        LOG_INFO("  Session manager ready");
    }

    // UI overlay + DX11 renderer (hooks IDXGISwapChain::Present)
    if (!UI::OverlayRenderer::GetInstance().Initialize()) {
        LOG_WARNING("  DX11 Present hook failed - in-game overlay unavailable");
    } else {
        LOG_INFO("  DX11 Present hook installed");
    }
    UI::Overlay::GetInstance().Initialize();
    LOG_INFO("  UI overlay ready (menu key: %s)", UI::KeyName(UI::GetMenuKey()).c_str());

    // Title notifier
    UI::TitleScreenNotifier::GetInstance().Start();

    // Start the main update loop in a background thread
    // This drives networking, sync, and session management
    m_updateThread = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
        auto* mod = static_cast<SeamlessCoopMod*>(param);
        LOG_INFO("Update thread started");

        auto lastTime = std::chrono::steady_clock::now();

        while (mod->IsInitialized()) {
            auto now = std::chrono::steady_clock::now();
            float deltaTime = std::chrono::duration<float>(now - lastTime).count();
            lastTime = now;

            // Update session (which updates networking + player sync)
            auto& sessionMgr = Session::SessionManager::GetInstance();
            sessionMgr.Update(deltaTime);

            // ~20Hz update rate
            Sleep(50);
        }

        LOG_INFO("Update thread exiting");
        return 0;
    }, this, 0, nullptr);

    m_initialized = true;

    // Final status report
    LOG_INFO("==========================================");
    LOG_INFO("SEAMLESS CO-OP INITIALIZATION COMPLETE");
    LOG_INFO("==========================================");
    LOG_INFO("  Addresses resolved: %s", addressesFound ? "YES" : "NO");
    LOG_INFO("  Protobuf hooks:     %s", protobufHooked ? "ACTIVE" : "FAILED");
    LOG_INFO("  Disconnect blocking: %s",
             Hooks::ProtobufHooks::IsSeamlessActive() ? "ENABLED" : "DISABLED");
    LOG_INFO("");
    if (protobufHooked) {
        LOG_INFO("  Press %s to open co-op menu", UI::KeyName(UI::GetMenuKey()).c_str());
        LOG_INFO("  Host a session or join via IP");
        LOG_INFO("  Sessions persist through boss kills and deaths");
    } else {
        LOG_INFO("  Running in PASSIVE MODE (title bar indicator only)");
        LOG_INFO("  Protobuf patterns may not match your game version");
    }
    LOG_INFO("==========================================");

    return true;
}

void SeamlessCoopMod::Shutdown() {
    if (!m_initialized) return;

    LOG_INFO("Shutting down mod...");

    // Signal update thread to stop, then wait
    m_initialized = false;
    if (m_updateThread) {
        WaitForSingleObject(m_updateThread, 3000);
        CloseHandle(m_updateThread);
        m_updateThread = nullptr;
    }

    // Disable seamless before unhooking
    Hooks::ProtobufHooks::SetSeamlessActive(false);

    LOG_INFO("Blocked %u disconnect messages during this session",
             Hooks::ProtobufHooks::GetBlockedMessageCount());
    LOG_INFO("Total protobuf messages processed: %u",
             Hooks::ProtobufHooks::GetTotalMessageCount());

    // Stop UI
    UI::TitleScreenNotifier::GetInstance().Stop();
    UI::Overlay::GetInstance().Shutdown();

    // Shutdown subsystems
    Sync::PlayerSync::GetInstance().Shutdown();
    Sync::ProgressSync::GetInstance().Shutdown();
    Session::SessionManager::GetInstance().Shutdown();
    Network::PeerManager::GetInstance().Shutdown();

    // Unhook
    Hooks::ProtobufHooks::UninstallHooks();
    Hooks::WinsockHooks::UninstallHooks();
    Hooks::GameState::UninstallHooks();
    Hooks::HookManager::GetInstance().Shutdown();

    LOG_INFO("Mod shutdown complete");
}

bool SeamlessCoopMod::DetectGameVersion() {
    LOG_INFO("Detecting game version...");

    uintptr_t baseAddress = Memory::GetModuleBase();
    if (!baseAddress) {
        LOG_ERROR("Failed to get module base address");
        return false;
    }

    LOG_INFO("  Module base: 0x%p", reinterpret_cast<void*>(baseAddress));
    m_gameVersion = GameVersion::SteamLatest;
    LOG_INFO("  Assuming Steam latest version");

    return true;
}

bool SeamlessCoopMod::InstallHooks() {
    // Hooks are now installed directly in Initialize() in the correct order
    return true;
}

void SeamlessCoopMod::UninstallHooks() {
    // Handled in Shutdown()
}

void SeamlessCoopMod::LoadConfig() {
    LOG_INFO("Loading configuration...");

    m_config = ModConfig{};

    std::ifstream configFile("ds2_seamless_coop.ini");
    if (configFile.is_open()) {
        std::string line;
        while (std::getline(configFile, line)) {
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;

            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);

                // Trim
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);

                if (key == "enabled") {
                    m_config.enabled = (value == "true" || value == "1");
                } else if (key == "debug_logging") {
                    m_config.debug_logging = (value == "true" || value == "1");
                } else if (key == "max_players") {
                    m_config.max_players = static_cast<uint16_t>(std::stoi(value));
                } else if (key == "port") {
                    m_config.port = static_cast<uint16_t>(std::stoi(value));
                } else if (key == "allow_invasions") {
                    m_config.allow_invasions = (value == "true" || value == "1");
                } else if (key == "language") {
                    m_config.language = value;
                } else if (key == "menu_key") {
                    m_config.menu_key = value;
                } else if (key == "rest_sync") {
                    m_config.rest_sync = (value == "true" || value == "1");
                } else if (key == "loot_sync") {
                    m_config.loot_sync = (value == "true" || value == "1");
                } else if (key == "free_travel") {
                    m_config.free_travel = (value == "true" || value == "1");
                } else if (key == "death_respawn") {
                    m_config.death_respawn = (value == "true" || value == "1");
                } else if (key == "npc_spawn") {
                    m_config.npc_spawn = (value == "true" || value == "1");
                } else if (key == "boss_fog_wait") {
                    m_config.boss_fog_wait = (value == "true" || value == "1");
                } else if (key == "npc_talk") {
                    m_config.npc_talk = (value == "true" || value == "1");
                } else if (key == "npc_solid") {
                    m_config.npc_solid = (value == "true" || value == "1");
                } else if (key == "debug_hotkeys") {
                    m_config.debug_hotkeys = (value == "true" || value == "1");
                } else if (key == "auto_summon") {
                    m_config.auto_summon = (value == "true" || value == "1");
                } else if (key == "sign_under_feet") {
                    m_config.sign_under_feet = (value == "true" || value == "1");
                } else if (key == "flag_sync") {
                    m_config.flag_sync = value;
                } else if (key == "sync_bonfires") {
                    m_config.sync_bonfires = (value == "true" || value == "1");
                } else if (key == "sync_items") {
                    m_config.sync_items = (value == "true" || value == "1");
                } else if (key == "sync_enemies") {
                    m_config.sync_enemies = (value == "true" || value == "1");
                } else if (key == "server_ip") {
                    m_config.server_ip = value;
                } else if (key == "server_port") {
                    m_config.server_port = static_cast<uint16_t>(std::stoi(value));
                } else if (key == "use_custom_server") {
                    m_config.use_custom_server = (value == "true" || value == "1");
                }
            }
        }
        configFile.close();
        LOG_INFO("Configuration loaded from file");
    } else {
        LOG_INFO("No configuration file found, using defaults");
        SaveConfig();
    }

    if (m_config.debug_logging) {
        Logger::GetInstance().SetMinLevel(LogLevel::Debug);
    }
}

void SeamlessCoopMod::SaveConfig() {
    std::ofstream configFile("ds2_seamless_coop.ini");
    if (configFile.is_open()) {
        configFile << "# Dark Souls 2 Seamless Co-op Configuration\n\n";
        configFile << "enabled=true\n";
        configFile << "debug_logging=" << (m_config.debug_logging ? "true" : "false") << "\n";
        configFile << "max_players=" << m_config.max_players << "\n";
        configFile << "port=" << m_config.port << "\n";
        configFile << "\n# Sync settings\n";
        configFile << "allow_invasions=" << (m_config.allow_invasions ? "true" : "false") << "\n";
        configFile << "auto_summon=" << (m_config.auto_summon ? "true" : "false") << "\n";
        configFile << "rest_sync=" << (m_config.rest_sync ? "true" : "false") << "\n";
        configFile << "loot_sync=" << (m_config.loot_sync ? "true" : "false") << "\n";
        configFile << "free_travel=" << (m_config.free_travel ? "true" : "false") << "\n";
        configFile << "death_respawn=" << (m_config.death_respawn ? "true" : "false") << "\n";
        configFile << "npc_spawn=" << (m_config.npc_spawn ? "true" : "false") << "\n";
        configFile << "boss_fog_wait=" << (m_config.boss_fog_wait ? "true" : "false") << "\n";
        configFile << "npc_talk=" << (m_config.npc_talk ? "true" : "false") << "\n";
        configFile << "npc_solid=" << (m_config.npc_solid ? "true" : "false") << "\n";
        configFile << "debug_hotkeys=" << (m_config.debug_hotkeys ? "true" : "false") << "\n";
        configFile << "sign_under_feet=" << (m_config.sign_under_feet ? "true" : "false") << "\n";
        configFile << "flag_sync=" << m_config.flag_sync << "\n";
        configFile << "sync_bonfires=" << (m_config.sync_bonfires ? "true" : "false") << "\n";
        configFile << "sync_items=" << (m_config.sync_items ? "true" : "false") << "\n";
        configFile << "sync_enemies=" << (m_config.sync_enemies ? "true" : "false") << "\n";
        configFile << "\n# Custom server settings\n";
        configFile << "use_custom_server=" << (m_config.use_custom_server ? "true" : "false") << "\n";
        configFile << "server_ip=" << m_config.server_ip << "\n";
        configFile << "server_port=" << m_config.server_port << "\n";
        configFile << "\n# Interface (changed from the in-game menu)\n";
        configFile << "language=" << m_config.language << "\n";
        configFile << "menu_key=" << m_config.menu_key << "\n";
        configFile.close();
    }
}

void SeamlessCoopMod::SetUiPreferences(const std::string& language, const std::string& menuKey) {
    m_config.language = language;
    m_config.menu_key = menuKey;
    SaveConfig();
}
