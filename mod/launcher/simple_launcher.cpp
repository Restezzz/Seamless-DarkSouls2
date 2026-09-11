#include <Windows.h>
#include <iostream>
#include <string>
#include <filesystem>
#include <TlHelp32.h>

namespace fs = std::filesystem;

bool InjectDLL(DWORD processId, const std::wstring& dllPath);
std::wstring FindGameExecutable();
DWORD FindGameProcess();

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleA("DS2 Seamless Co-op - Auto Launcher");

    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);

    std::cout << "\n";
    std::cout << "  ===============================================\n";
    std::cout << "  DARK SOULS 2: SEAMLESS CO-OP - AUTO LAUNCH\n";
    std::cout << "  ===============================================\n\n";

    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    // Check if DLL exists - first check current dir, then game folder
    wchar_t currentDir[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, currentDir);
    std::wstring dllPath = std::wstring(currentDir) + L"\\ds2_seamless_coop.dll";
    
    // Also check next to the exe itself
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = std::wstring(exePath);
    exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/"));
    std::wstring dllPathExeDir = exeDir + L"\\ds2_seamless_coop.dll";
    
    // Check game folder too
    std::wstring gameDllPath = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game\\ds2_seamless_coop.dll";
    
    // Priority: current dir > exe dir > game folder
    if (!fs::exists(dllPath)) {
        if (fs::exists(dllPathExeDir)) {
            dllPath = dllPathExeDir;
        } else if (fs::exists(gameDllPath)) {
            dllPath = gameDllPath;
        }
    }

    if (!fs::exists(dllPath)) {
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cout << "  ERROR: ds2_seamless_coop.dll not found!\n";
        std::cout << "  Expected: " << fs::path(dllPath).string() << "\n\n";
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        std::cout << "  Press Enter to exit...";
        std::cin.get();
        return 1;
    }

    SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::cout << "  [OK] Mod DLL found\n\n";
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    // Check if game is already running — inject immediately, no prompt
    DWORD existingPid = FindGameProcess();
    if (existingPid) {
        SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        std::cout << "  [OK] Dark Souls 2 is running (PID: " << existingPid << ")\n\n";
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

        std::cout << "  [..] Injecting seamless co-op mod...\n\n";

        if (InjectDLL(existingPid, dllPath)) {
            SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            std::cout << "  ===============================================\n";
            std::cout << "         MOD INJECTED SUCCESSFULLY!\n";
            std::cout << "  ===============================================\n\n";
            SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            std::cout << "  Co-op menu in game: F1 by default (rebindable in the menu)\n\n";
        } else {
            SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
            std::cout << "  FAILED! Try running as Administrator.\n\n";
            SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        }

        std::cout << "  Press Enter to exit...";
        std::cin.get();
        return 0;
    }

    // Find game executable
    std::cout << "  [..] Searching for Dark Souls 2 installation...\n";
    std::wstring gameExe = FindGameExecutable();

    if (gameExe.empty()) {
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cout << "  ERROR: Could not find DarkSoulsII.exe!\n\n";
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        std::cout << "  Please install Dark Souls 2 via Steam.\n\n";
        std::cout << "  Press Enter to exit...";
        std::cin.get();
        return 1;
    }

    SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::wcout << L"  [OK] Found game: " << gameExe << L"\n\n";
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    // Launch game in OFFLINE mode
    std::cout << "  [..] Launching Dark Souls 2 in OFFLINE mode...\n";
    std::cout << "      (Seamless Co-op requires offline mode for safety)\n\n";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    // Add -offline parameter to force offline mode
    std::wstring cmdLine = L"\"" + gameExe + L"\" -offline";
    std::wstring workingDir = gameExe.substr(0, gameExe.find_last_of(L"\\/"));

    if (!CreateProcessW(
        gameExe.c_str(),
        const_cast<LPWSTR>(cmdLine.c_str()),
        nullptr, nullptr, FALSE, 0,
        nullptr, workingDir.c_str(),
        &si, &pi)) {

        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cout << "  ERROR: Failed to launch game!\n";
        std::cout << "  Error code: " << GetLastError() << "\n\n";
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        std::cout << "  Press Enter to exit...";
        std::cin.get();
        return 1;
    }

    SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::cout << "  [OK] Game launched!\n\n";
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    std::cout << "  [..] Waiting for game to initialize...\n";
    std::cout << "      (This takes about 10 seconds)\n\n";

    // Wait for game window to appear
    int attempts = 0;
    DWORD gamePid = 0;
    while (attempts < 30 && !gamePid) {
        Sleep(1000);
        gamePid = FindGameProcess();
        if (!gamePid) {
            std::cout << "  .";
            attempts++;
        }
    }

    std::cout << "\n\n";

    if (!gamePid) {
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cout << "  ERROR: Game started but couldn't detect it!\n\n";
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        std::cout << "  The game is running. You can try the regular launcher\n";
        std::cout << "  to inject the mod manually.\n\n";
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::cout << "  Press Enter to exit...";
        std::cin.get();
        return 1;
    }

    SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::cout << "  [OK] Game detected! (PID: " << gamePid << ")\n\n";
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    // Inject DLL
    std::cout << "  [..] Injecting seamless co-op mod...\n\n";

    if (InjectDLL(gamePid, dllPath)) {
        SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        std::cout << "  ===============================================\n";
        std::cout << "           MOD INJECTED SUCCESSFULLY!\n";
        std::cout << "  ===============================================\n\n";
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

        std::cout << "  [OK] Dark Souls 2 is running with seamless co-op!\n";
        std::cout << "  [OK] Game is in OFFLINE mode (auto-configured)\n\n";

        std::cout << "  HOW TO USE:\n";
        std::cout << "  • Co-op menu: F1 by default, rebindable in the menu\n";
        std::cout << "  • Host or join a session\n";
        std::cout << "  • Play through the game with friends!\n\n";

        std::cout << "  TIPS:\n";
        std::cout << "  • Port 27015 must be open for hosting\n";
        std::cout << "  • Share your session code via Discord/Steam\n";
        std::cout << "  • Check ds2_seamless_coop.log if issues occur\n\n";

        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        std::cout << "  Enjoy seamless co-op! Praise the sun! \\[T]/\n\n";
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    } else {
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cout << "  ===============================================\n";
        std::cout << "              INJECTION FAILED!\n";
        std::cout << "  ===============================================\n\n";
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

        std::cout << "  The game is running, but mod injection failed.\n\n";
        std::cout << "  SOLUTIONS:\n";
        std::cout << "  • Run this launcher as Administrator\n";
        std::cout << "  • Disable antivirus temporarily\n";
        std::cout << "  • Check if DLL is blocked (Properties > Unblock)\n\n";
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::cout << "  Press Enter to exit...";
    std::cin.get();
    return 0;
}

std::wstring FindGameExecutable() {
    // Common Steam library paths
    std::vector<std::wstring> paths = {
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game\\DarkSoulsII.exe",
        L"C:\\Program Files\\Steam\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game\\DarkSoulsII.exe",
        L"D:\\SteamLibrary\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game\\DarkSoulsII.exe",
        L"D:\\Steam\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game\\DarkSoulsII.exe",
        L"E:\\SteamLibrary\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game\\DarkSoulsII.exe",
        L"F:\\SteamLibrary\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game\\DarkSoulsII.exe",
    };

    for (const auto& path : paths) {
        if (fs::exists(path)) {
            return path;
        }
    }

    return L"";
}

DWORD FindGameProcess() {
    // Try window title first
    HWND hwnd = FindWindowW(nullptr, L"DARK SOULS II");
    if (!hwnd) {
        hwnd = FindWindowW(nullptr, L"DARK SOULS II: Scholar of the First Sin");
    }

    if (hwnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        return pid;
    }

    // Try process snapshot
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };

        if (Process32FirstW(hSnapshot, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"DarkSoulsII.exe") == 0) {
                    DWORD pid = pe.th32ProcessID;
                    CloseHandle(hSnapshot);
                    return pid;
                }
            } while (Process32NextW(hSnapshot, &pe));
        }
        CloseHandle(hSnapshot);
    }

    return 0;
}

bool InjectDLL(DWORD processId, const std::wstring& dllPath) {
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, processId);

    if (!hProcess) {
        return false;
    }

    size_t pathSize = (dllPath.length() + 1) * sizeof(wchar_t);
    LPVOID pRemoteBuf = VirtualAllocEx(hProcess, nullptr, pathSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!pRemoteBuf) {
        CloseHandle(hProcess);
        return false;
    }

    if (!WriteProcessMemory(hProcess, pRemoteBuf, dllPath.c_str(), pathSize, nullptr)) {
        VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) {
        VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    LPTHREAD_START_ROUTINE pLoadLibrary =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(hKernel32, "LoadLibraryW"));

    if (!pLoadLibrary) {
        VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, pLoadLibrary, pRemoteBuf, 0, nullptr);

    if (!hThread) {
        VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, INFINITE);

    DWORD exitCode;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    if (exitCode == 0) {
        return false;
    }

    // Give DLL time to initialize
    Sleep(2000);

    return true;
}
