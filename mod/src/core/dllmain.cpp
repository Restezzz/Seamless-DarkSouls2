// DLL entry point + dinput8.dll proxy
//
// When this DLL is named "dinput8.dll" and placed in the game folder,
// the game loads it automatically (Windows DLL search order).
// We forward all real DirectInput calls to the system dinput8.dll
// so controller/keyboard input keeps working.

#include <Windows.h>
#include "../include/mod.h"
#include "../include/utils.h"

using namespace DS2Coop;
using namespace DS2Coop::Utils;

// ============================================================================
// dinput8.dll proxy — forward DirectInput8Create to the real system DLL
// ============================================================================
static HMODULE g_realDinput8 = nullptr;

typedef HRESULT(WINAPI* DirectInput8Create_t)(
    HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DirectInput8Create_t g_realDirectInput8Create = nullptr;

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf,
    LPVOID* ppvOut, LPUNKNOWN punkOuter)
{
    if (!g_realDinput8) {
        // Load the real dinput8.dll from system32
        wchar_t sysDir[MAX_PATH];
        GetSystemDirectoryW(sysDir, MAX_PATH);
        wcscat_s(sysDir, L"\\dinput8.dll");
        g_realDinput8 = LoadLibraryW(sysDir);
    }

    if (!g_realDirectInput8Create && g_realDinput8) {
        g_realDirectInput8Create = reinterpret_cast<DirectInput8Create_t>(
            GetProcAddress(g_realDinput8, "DirectInput8Create"));
    }

    if (g_realDirectInput8Create) {
        return g_realDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    }

    return E_FAIL;
}

// ============================================================================
// DllMain
// ============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hModule);

        Logger::GetInstance().Initialize(L"ds2_seamless_coop.log");

        LOG_INFO("========================================");
        LOG_INFO("DLL_PROCESS_ATTACH - DLL IS LOADING!");
        LOG_INFO("DS2 Seamless Co-op Mod v%s", MOD_VERSION);
        LOG_INFO("DLL Module Handle: 0x%p", hModule);
        LOG_INFO("Process ID: %lu", GetCurrentProcessId());
        LOG_INFO("========================================");

        HANDLE hThread = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            LOG_INFO("Mod initialization thread started...");
            InstallCrashLog();
            Sleep(3000);
            LOG_INFO("Calling SeamlessCoopMod::Initialize()...");
            auto& mod = SeamlessCoopMod::GetInstance();
            if (mod.Initialize()) {
                LOG_INFO("========================================");
                LOG_INFO("MOD INITIALIZED SUCCESSFULLY!");
                LOG_INFO("Co-op menu: F1 by default, rebindable in the menu");
                LOG_INFO("========================================");
            } else {
                LOG_ERROR("========================================");
                LOG_ERROR("MOD INITIALIZATION FAILED!");
                LOG_ERROR("Check ds2_seamless_coop.log for details");
                LOG_ERROR("========================================");
            }
            GuardCrashLog();
            return 0;
        }, nullptr, 0, nullptr);
        if (hThread) CloseHandle(hThread);
        else LOG_ERROR("FATAL: CreateThread failed — mod will not initialize");

        LOG_INFO("DllMain returning TRUE (success)");
        break;
    }
        
    case DLL_PROCESS_DETACH:
        LOG_INFO("Shutting down mod...");
        SeamlessCoopMod::GetInstance().Shutdown();
        Logger::GetInstance().Shutdown();
        break;
    }
    return TRUE;
}

