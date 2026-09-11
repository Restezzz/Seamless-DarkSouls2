#include "../../include/ui.h"
#include "../../include/utils.h"
#include "../../include/ui_settings.h"
#include <Windows.h>
#include <thread>
#include <string>

using namespace DS2Coop::Utils;

namespace DS2Coop::UI {

TitleScreenNotifier& TitleScreenNotifier::GetInstance() {
    static TitleScreenNotifier instance;
    return instance;
}

TitleScreenNotifier::~TitleScreenNotifier() {
    Stop();
}

void TitleScreenNotifier::Start() {
    if (m_running) return;
    
    m_running = true;
    m_thread = std::thread(&TitleScreenNotifier::UpdateThread, this);
}

void TitleScreenNotifier::Stop() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }

    // Restore original title
    HWND hwnd = FindGameWindow();
    if (hwnd) {
        SetWindowTextW(hwnd, L"DARK SOULS II");
    }
}

// Helper to find window by process name
static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);

    if (processId != 0) {
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (hProcess) {
            char processName[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameA(hProcess, 0, processName, &size)) {
                // Check if this is DarkSoulsII.exe
                if (strstr(processName, "DarkSoulsII.exe") != nullptr) {
                    // Found it! Check if it's a main window
                    if (IsWindowVisible(hwnd) && GetWindowTextLengthW(hwnd) > 0) {
                        *(HWND*)lParam = hwnd;
                        CloseHandle(hProcess);
                        return FALSE; // Stop enumeration
                    }
                }
            }
            CloseHandle(hProcess);
        }
    }
    return TRUE; // Continue enumeration
}

HWND TitleScreenNotifier::FindGameWindow() {
    // First try the simple approach (works for windowed mode)
    HWND hwnd = FindWindowW(nullptr, L"DARK SOULS II");
    if (!hwnd) {
        hwnd = FindWindowW(nullptr, L"DARK SOULS II: Scholar of the First Sin");
    }

    // If that failed, enumerate all windows and find by process name (works for fullscreen)
    if (!hwnd) {
        EnumWindows(EnumWindowsProc, (LPARAM)&hwnd);
    }

    return hwnd;
}

void TitleScreenNotifier::UpdateThread() {
    LOG_INFO("Title screen notifier thread started");

    int attempts = 0;
    bool firstUpdate = true;

    while (m_running) {
        HWND hwnd = FindGameWindow();
        if (hwnd) {
            // Update window title to show mod is active
            // The key and the language can change while the game runs.
            const std::string Title = Format(
                Tr("DARK SOULS II  \xE2\x80\xA2  Seamless Co-op  \xE2\x80\xA2  %s \xE2\x80\x94 menu",
                   "DARK SOULS II  \xE2\x80\xA2  Seamless Co-op  \xE2\x80\xA2  %s \xE2\x80\x94 меню"),
                KeyName(GetMenuKey()).c_str());
            wchar_t newTitle[256] = {};
            MultiByteToWideChar(CP_UTF8, 0, Title.c_str(), -1, newTitle, 256);

            if (SetWindowTextW(hwnd, newTitle)) {
                if (firstUpdate) {
                    LOG_INFO("========================================");
                    LOG_INFO("WINDOW TITLE UPDATED!");
                    LOG_INFO("========================================");
                    LOG_INFO("Title bar now shows: %s", Title.c_str());
                    LOG_INFO("");
                    LOG_INFO("This confirms the mod is loaded and active!");
                    LOG_INFO("Look at the top of your game window!");
                    LOG_INFO("========================================");
                    firstUpdate = false;
                }
                attempts++;
            } else {
                LOG_ERROR("Failed to set window title! Error: %lu", GetLastError());
            }
        } else {
            if (firstUpdate) {
                LOG_WARNING("Waiting for game window...");
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    LOG_INFO("Title screen notifier stopped (updated %d times)", attempts);
}

} // namespace DS2Coop::UI

