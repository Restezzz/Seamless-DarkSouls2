#include <Windows.h>
#include <io.h>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <filesystem>
#include <vector>
#include <Shlwapi.h>
#include <ShlObj.h>
#include <TlHelp32.h>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Shell32.lib")

namespace fs = std::filesystem;

// Forward declarations
bool InjectDLL(DWORD processId, const std::wstring& dllPath);
std::wstring FindGameDirectory();
DWORD FindGameProcess();
void ShowMenu();
bool IsElevated();
bool RelaunchAsAdmin();

int main(int argc, char* argv[]) {
    // Enable UTF-8 console output so box-drawing chars render correctly
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // Check if running as admin, if not, relaunch
    if (!IsElevated()) {
        std::cout << "\n  [!] Launcher needs Administrator privileges to inject DLL.\n";
        std::cout << "  [!] Relaunching as Administrator...\n\n";
        
        if (RelaunchAsAdmin()) {
            return 0; // Successfully relaunched, this instance can exit
        } else {
            std::cerr << "\n  [ERROR] Failed to relaunch as Administrator.\n";
            std::cerr << "  Please right-click the launcher and select 'Run as Administrator'\n\n";
            std::cout << "  Press Enter to exit...";
            std::cin.get();
            return 1;
        }
    }
    // Set console colors
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    
    std::cout << "\n";
    std::cout << "  ╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "  ║                                                          ║\n";
    std::cout << "  ║     DARK SOULS 2: SEAMLESS CO-OP MOD LAUNCHER           ║\n";
    std::cout << "  ║     Version 1.0.0                                        ║\n";
    std::cout << "  ║                                                          ║\n";
    std::cout << "  ╚══════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
    
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    
    // Get current directory
    wchar_t currentDir[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, currentDir);
    
    // Check if DLL exists
    std::wstring dllPath = std::wstring(currentDir) + L"\\ds2_seamless_coop.dll";
    if (!fs::exists(dllPath)) {
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "\n  [ERROR] ds2_seamless_coop.dll not found!\n";
        std::cerr << "  Expected location: " << fs::path(dllPath).string() << "\n\n";
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        std::cout << "  Please make sure the DLL is in the same directory as this launcher.\n\n";
        std::cout << "  Press Enter to exit...";
        std::cin.get();
        return 1;
    }
    
    SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::cout << "  [✓] Mod DLL found\n\n";
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    
    // Try to find game
    std::cout << "  [→] Looking for Dark Souls 2...\n";
    
    DWORD processId = FindGameProcess();
    std::wstring gameDir;
    
    if (!processId) {
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        std::cout << "  [!] Game not running\n\n";
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        
        // Try to find game directory
        std::cout << "  [→] Searching for game installation...\n";
        gameDir = FindGameDirectory();
        
        if (!gameDir.empty()) {
            SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            std::wcout << L"  [✓] Found game at: " << gameDir << L"\n\n";
            SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            
            std::cout << "  Would you like to:\n";
            std::cout << "  1) Launch game now and inject mod\n";
            std::cout << "  2) Just inject mod when I start the game manually\n";
            std::cout << "  3) Exit\n\n";
            std::cout << "  Choice: ";
            
            char choice;
            std::cin >> choice;
            std::cin.ignore();
            
            if (choice == '1') {
                std::wstring gameExe = gameDir + L"\\DarkSoulsII.exe";
                std::cout << "\n  [→] Launching Dark Souls 2 (with admin privileges for injection)...\n";
                
                // Launch game as admin so we can inject from admin launcher
                SHELLEXECUTEINFOW sei = { sizeof(sei) };
                sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
                sei.lpVerb = L"runas";  // Launch as admin
                sei.lpFile = gameExe.c_str();
                sei.lpDirectory = gameDir.c_str();
                sei.nShow = SW_SHOWNORMAL;
                
                if (!ShellExecuteExW(&sei) || !sei.hProcess) {
                    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
                    std::cerr << "  [ERROR] Failed to launch game: " << GetLastError() << "\n\n";
                    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
                    std::cout << "  Press Enter to exit...";
                    std::cin.get();
                    return 1;
                }
                
                std::cout << "  [✓] Game launched! Waiting for initialization...\n";
                Sleep(8000); // Wait longer for game to initialize
                
                processId = GetProcessId(sei.hProcess);
                std::cout << "  [✓] Game PID: " << processId << "\n";
                
                CloseHandle(sei.hProcess);
            } else if (choice == '2') {
                std::cout << "\n  Waiting for game to start...\n";
                std::cout << "  (Start Dark Souls 2 now, then come back here)\n\n";
                
                while (!processId) {
                    processId = FindGameProcess();
                    if (!processId) {
                        Sleep(1000);
                        std::cout << ".";
                    }
                }
                std::cout << "\n\n  [✓] Game detected!\n";
            } else {
                return 0;
            }
        } else {
            SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            std::cout << "  [!] Couldn't auto-detect game installation\n\n";
            SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            std::cout << "  Please:\n";
            std::cout << "  1. Start Dark Souls 2\n";
            std::cout << "  2. Run this launcher again\n\n";
            std::cout << "  Press Enter to exit...";
            std::cin.get();
            return 1;
        }
    } else {
        SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        std::cout << "  [✓] Dark Souls 2 is running (PID: " << processId << ")\n\n";
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    }
    
    // Inject DLL
    std::cout << "  [→] Injecting seamless co-op mod...\n";
    std::cout << "  [DEBUG] DLL Path: " << fs::path(dllPath).string() << "\n";
    std::cout << "  [DEBUG] Target PID: " << processId << "\n\n";
    
    if (InjectDLL(processId, dllPath)) {
        SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        std::cout << "\n  ╔══════════════════════════════════════════════════════════╗\n";
        std::cout << "  ║                    SUCCESS!                              ║\n";
        std::cout << "  ║          Seamless Co-op Mod Injected!                    ║\n";
        std::cout << "  ╚══════════════════════════════════════════════════════════╝\n\n";
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        
        std::cout << "  The mod is now active!\n\n";
        std::cout << "  IMPORTANT: Check these locations:\n";
        std::cout << "  1. Game folder: ds2_seamless_coop.log\n";
        std::cout << "  2. Launcher folder: ds2_seamless_coop.log\n";
        std::cout << "  3. Look for window title change\n\n";
        
        std::cout << "  If log file exists, mod loaded successfully!\n";
        std::cout << "  If not, there may be missing dependencies.\n\n";
        
        std::cout << "  HOW TO USE:\n";
        std::cout << "  • Co-op menu in game: F1 by default, rebindable in the menu\n";
        std::cout << "  • Boss fights won't disconnect you!\n";
        std::cout << "  • Deaths won't disconnect you!\n\n";
        
        std::cout << "  Have fun, and praise the sun! \\[T]/\n\n";
    } else {
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cout << "\n  [ERROR] Failed to inject DLL!\n\n";
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        std::cout << "  Possible solutions:\n";
        std::cout << "  • Run this launcher as Administrator\n";
        std::cout << "  • Disable antivirus temporarily\n";
        std::cout << "  • Check if DLL is blocked by Windows (Right-click → Properties → Unblock)\n\n";
    }
    
    std::cout << "  Press Enter to exit...";
    std::cin.get();
    return 0;
}

std::wstring FindGameDirectory() {
    // Common Steam library paths
    std::vector<std::wstring> steamPaths = {
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game",
        L"C:\\Program Files\\Steam\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game",
        L"D:\\SteamLibrary\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game",
        L"D:\\Steam\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game",
        L"E:\\SteamLibrary\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game",
        L"F:\\SteamLibrary\\steamapps\\common\\Dark Souls II Scholar of the First Sin\\Game",
    };
    
    // Try common paths
    for (const auto& path : steamPaths) {
        std::wstring exePath = path + L"\\DarkSoulsII.exe";
        if (fs::exists(exePath)) {
            return path;
        }
    }
    
    // Try to read Steam config
    wchar_t programFilesX86[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, 0, programFilesX86);
    std::wstring steamConfig = std::wstring(programFilesX86) + L"\\Steam\\steamapps\\libraryfolders.vdf";
    
    // TODO: Parse VDF file for additional library folders
    // For now, just return empty if not found in common locations
    
    return L"";
}

DWORD FindGameProcess() {
    DWORD processId = 0;
    
    // Try window title first
    HWND hwnd = FindWindowW(nullptr, L"DARK SOULS II");
    if (!hwnd) {
        hwnd = FindWindowW(nullptr, L"DARK SOULS II: Scholar of the First Sin");
    }
    
    if (hwnd) {
        GetWindowThreadProcessId(hwnd, &processId);
        return processId;
    }
    
    // Try process name
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        
        if (Process32FirstW(hSnapshot, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"DarkSoulsII.exe") == 0) {
                    processId = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(hSnapshot, &pe));
        }
        
        CloseHandle(hSnapshot);
    }
    
    return processId;
}

bool InjectDLL(DWORD processId, const std::wstring& dllPath) {
    // Open target process
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, processId);
    
    if (!hProcess) {
        std::cerr << "  Failed to open process. Error: " << GetLastError() << "\n";
        std::cerr << "  Try running as Administrator!\n";
        return false;
    }
    
    // Allocate memory in target process
    size_t dllPathSize = (dllPath.length() + 1) * sizeof(wchar_t);
    LPVOID pRemoteBuf = VirtualAllocEx(hProcess, nullptr, dllPathSize,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    
    if (!pRemoteBuf) {
        std::cerr << "  Failed to allocate memory in target process. Error: " << GetLastError() << "\n";
        CloseHandle(hProcess);
        return false;
    }
    
    // Write DLL path to target process
    if (!WriteProcessMemory(hProcess, pRemoteBuf, dllPath.c_str(), dllPathSize, nullptr)) {
        std::cerr << "  Failed to write to target process memory. Error: " << GetLastError() << "\n";
        VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }
    
    // Get LoadLibraryW address
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) {
        std::cerr << "  Failed to get kernel32.dll handle.\n";
        VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }
    
    LPTHREAD_START_ROUTINE pLoadLibrary =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(hKernel32, "LoadLibraryW"));
    
    if (!pLoadLibrary) {
        std::cerr << "  Failed to get LoadLibraryW address.\n";
        VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }
    
    // Create remote thread to load DLL
    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, pLoadLibrary, pRemoteBuf, 0, nullptr);
    
    if (!hThread) {
        std::cerr << "  Failed to create remote thread. Error: " << GetLastError() << "\n";
        VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }
    
    // Wait for thread to finish (10s timeout — game anti-cheat can delay LoadLibrary)
    DWORD waitResult = WaitForSingleObject(hThread, 10000);
    if (waitResult == WAIT_TIMEOUT) {
        std::cout << "  [!] Remote thread timed out — DLL may still be loading in background\n";
    }
    
    // Get thread exit code (module handle)
    DWORD exitCode;
    GetExitCodeThread(hThread, &exitCode);
    
    // Cleanup
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
    CloseHandle(hProcess);
    
    if (exitCode == 0) {
        DWORD lastError = GetLastError();
        std::cerr << "  LoadLibrary returned NULL. DLL failed to load!\n";
        std::cerr << "  Last Error Code: " << lastError << "\n";
        std::cerr << "  Possible reasons:\n";
        
        if (lastError == 126) {
            std::cerr << "    - ERROR 126: Module not found or missing dependencies\n";
        } else if (lastError == 127) {
            std::cerr << "    - ERROR 127: Procedure not found\n";
        } else if (lastError == 998) {
            std::cerr << "    - ERROR 998: Invalid access to memory location\n";
        } else {
            std::cerr << "    - Check Windows Event Viewer for details\n";
        }
        
        std::cerr << "    - Missing dependencies (Visual C++ Redistributable)\n";
        std::cerr << "    - DLL is for wrong architecture (x64 vs x86)\n";
        std::cerr << "    - Antivirus blocked the DLL\n";
        std::cerr << "    - DllMain returned FALSE\n";
        
        // Try to get more info from the process
        std::wcerr << L"\n  DLL Path used: " << dllPath << L"\n";
        std::wcerr << L"  DLL Size: " << fs::file_size(dllPath) << L" bytes\n";
        
        return false;
    }
    
    std::cout << "  [✓] DLL module loaded at: 0x" << std::hex << exitCode << std::dec << "\n";
    std::wcout << L"  [✓] Module handle: 0x" << std::hex << exitCode << std::dec << L"\n";
    
    // Give DLL time to initialize
    std::cout << "  [→] Waiting for DLL to initialize (3 seconds)...\n";
    Sleep(3000);
    
    std::cout << "  [✓] DLL should be active now!\n";
    
    return true;
}

bool IsElevated() {
    BOOL elevated = FALSE;
    HANDLE hToken = nullptr;
    
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(TOKEN_ELEVATION);
        
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size)) {
            elevated = elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    
    return elevated != FALSE;
}

bool RelaunchAsAdmin() {
    wchar_t szPath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, szPath, MAX_PATH) == 0) {
        return false;
    }
    
    // Use ShellExecuteW to relaunch with "runas" (admin request)
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = szPath;
    sei.hwnd = nullptr;
    sei.nShow = SW_NORMAL;
    
    if (!ShellExecuteExW(&sei)) {
        DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) {
            // User cancelled the UAC prompt
            return false;
        }
        return false;
    }
    
    return true;
}
