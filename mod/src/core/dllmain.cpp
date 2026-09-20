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
// Another mod's dinput8.dll, loaded by this one
// ============================================================================
//
// A modpack for DS2 usually comes with ModEngine, and ModEngine is a dinput8.dll as well: only one
// file can carry that name, so one of the two has to load the other. ModEngine can load this mod
// through its own list, but its `blockNetworkAccess` then has to be turned off by hand, and people
// get that wrong. So this works the other way round too: put ModEngine's DLL in the game folder under
// any other name and write it in ds2_seamless_coop.ini
//
//     chain_dll=modengine.dll
//
// It is loaded before anything else this mod does, and DirectInput8Create is handed to it instead of
// to the system DLL, so the chain is game -> this mod -> ModEngine -> system dinput8. Empty by
// default, and a name that cannot be loaded is only a line in the log.
static HMODULE g_chain = nullptr;

static void ReadChainName(char* Out, size_t Size) {
    Out[0] = 0;
    FILE* F = nullptr;
    if (fopen_s(&F, "ds2_seamless_coop.ini", "r") != 0 || !F) return;
    char Line[256] = {};
    while (fgets(Line, sizeof(Line), F)) {
        char* At = Line;
        while (*At == ' ' || *At == '\t') ++At;
        if (*At == '#' || *At == ';') continue;
        if (_strnicmp(At, "chain_dll", 9) != 0) continue;
        char* Eq = strchr(At, '=');
        if (!Eq) continue;
        ++Eq;
        while (*Eq == ' ' || *Eq == '\t') ++Eq;
        size_t Len = strlen(Eq);
        while (Len && (Eq[Len - 1] == '\n' || Eq[Len - 1] == '\r' || Eq[Len - 1] == ' ')) Eq[--Len] = 0;
        if (Len) strncpy_s(Out, Size, Eq, _TRUNCATE);
        break;
    }
    fclose(F);
}

static void LoadChainDll() {
    char Name[128] = {};
    ReadChainName(Name, sizeof(Name));
    if (!Name[0]) return;
    g_chain = LoadLibraryA(Name);
    if (g_chain) {
        LOG_INFO("[CHAIN] %s loaded by this mod -- its DirectInput8Create is used instead of the system one", Name);
    } else {
        LOG_WARNING("[CHAIN] %s could not be loaded (error %lu) -- is it in the game folder?", Name, GetLastError());
    }
}

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
        // Another mod's DLL first, if the ini named one: it is the next link of the chain.
        if (g_chain && GetProcAddress(g_chain, "DirectInput8Create")) {
            g_realDinput8 = g_chain;
        } else {
            // Load the real dinput8.dll from system32
            wchar_t sysDir[MAX_PATH];
            GetSystemDirectoryW(sysDir, MAX_PATH);
            wcscat_s(sysDir, L"\\dinput8.dll");
            g_realDinput8 = LoadLibraryW(sysDir);
        }
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
            LoadChainDll();   // before anything else: another mod's DLL wants the game as it starts
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

