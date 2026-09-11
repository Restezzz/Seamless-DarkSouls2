// See input_capture.h for what this is for.
//
// DirectInput devices of one kind share their method implementations inside
// the system dinput8.dll, so hooking the functions behind a throwaway device's
// vtable covers the game's devices too, whenever they were created. The ANSI
// and Unicode interfaces may point at different functions; both are hooked, and
// each detour knows which original to call.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <Xinput.h>

#include "../../include/input_capture.h"
#include "../../include/hooks.h"
#include "../../include/utils.h"

#include <atomic>
#include <cstring>
#include <mutex>

#pragma comment(lib, "dxguid.lib")

using namespace DS2Coop::Utils;

namespace DS2Coop::UI {

namespace {

std::atomic<bool> g_blocked{ false };
std::atomic<bool> g_installed{ false };

// ---- DirectInput -----------------------------------------------------------------

using AcquireFn  = HRESULT(WINAPI*)(void*);
using GetStateFn = HRESULT(WINAPI*)(void*, DWORD, LPVOID);
using GetDataFn  = HRESULT(WINAPI*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using CapsFn     = HRESULT(WINAPI*)(void*, LPDIDEVCAPS);

// vtable slots of IDirectInputDevice8 (A and W alike)
constexpr int kCaps = 3, kAcquire = 7, kUnacquire = 8, kGetState = 9, kGetData = 10;

struct HookSet {
    void*      AcquireTarget = nullptr;
    void*      GetStateTarget = nullptr;
    void*      GetDataTarget = nullptr;
    AcquireFn  Acquire = nullptr;
    GetStateFn GetState = nullptr;
    GetDataFn  GetData = nullptr;
};
HookSet g_sets[2];

enum DeviceType { Other = 0, Mouse = 1, Keyboard = 2 };
struct Device { void* Ptr; DeviceType Type; bool Released; };
std::mutex g_deviceMutex;
Device     g_devices[16] = {};
int        g_deviceCount = 0;

void** Vtable(void* Obj) { return *reinterpret_cast<void***>(Obj); }

// The device's entry, created on first sight from its capabilities.
Device* Lookup(void* Dev) {
    for (int i = 0; i < g_deviceCount; i++) {
        if (g_devices[i].Ptr == Dev) return &g_devices[i];
    }
    if (g_deviceCount >= 16) return nullptr;
    DIDEVCAPS Caps{};
    Caps.dwSize = sizeof(Caps);
    DeviceType Type = Other;
    if (SUCCEEDED(reinterpret_cast<CapsFn>(Vtable(Dev)[kCaps])(Dev, &Caps))) {
        const BYTE T = GET_DIDEVICE_TYPE(Caps.dwDevType);
        Type = T == DI8DEVTYPE_MOUSE ? Mouse : T == DI8DEVTYPE_KEYBOARD ? Keyboard : Other;
    }
    g_devices[g_deviceCount] = { Dev, Type, false };
    LOG_INFO("[INPUT] game device %p is a %s", Dev,
             Type == Mouse ? "mouse" : Type == Keyboard ? "keyboard" : "controller or other");
    return &g_devices[g_deviceCount++];
}

// Common part of the two read methods. Returns true when the call was answered
// here (blocked) and must not reach DirectInput. Keeps the mouse released while
// blocked, and acquires it back once the menu has closed.
template <int N>
bool Intercept(void* Dev, DeviceType& TypeOut) {
    bool Release = false, Reacquire = false;
    {
        std::lock_guard<std::mutex> Lock(g_deviceMutex);
        Device* D = Lookup(Dev);
        TypeOut = D ? D->Type : Other;
        if (!D || D->Type == Other) return false;
        if (g_blocked.load()) {
            if (D->Type == Mouse && !D->Released) { D->Released = true; Release = true; }
        } else if (D->Released) {
            D->Released = false;
            Reacquire = true;
        }
    }
    if (Release)   reinterpret_cast<AcquireFn>(Vtable(Dev)[kUnacquire])(Dev);
    if (Reacquire) g_sets[N].Acquire(Dev);
    return g_blocked.load();
}

template <int N>
HRESULT WINAPI AcquireDetour(void* Dev) {
    if (g_blocked.load()) {
        std::lock_guard<std::mutex> Lock(g_deviceMutex);
        Device* D = Lookup(Dev);
        if (D && D->Type == Mouse) return DI_OK;   // stays released while the menu is open
    }
    return g_sets[N].Acquire(Dev);
}

template <int N>
HRESULT WINAPI GetStateDetour(void* Dev, DWORD Size, LPVOID Data) {
    DeviceType Type = Other;
    if (Intercept<N>(Dev, Type)) {
        if (Data) memset(Data, 0, Size);
        return DI_OK;
    }
    return g_sets[N].GetState(Dev, Size, Data);
}

template <int N>
HRESULT WINAPI GetDataDetour(void* Dev, DWORD Size, LPDIDEVICEOBJECTDATA Items, LPDWORD InOut, DWORD Flags) {
    DeviceType Type = Other;
    if (Intercept<N>(Dev, Type)) {
        if (Type == Keyboard) {
            // Drain what was typed into the menu so it does not reach the game later.
            DWORD Flush = INFINITE;
            g_sets[N].GetData(Dev, Size, nullptr, &Flush, 0);
        }
        if (InOut) *InOut = 0;
        return DI_OK;
    }
    return g_sets[N].GetData(Dev, Size, Items, InOut, Flags);
}

bool HookOne(void* Target, void* Detour, void** Original, const char* What) {
    if (!Target) return false;
    if (DS2Coop::Hooks::HookManager::GetInstance().InstallHook(Target, Detour, Original)) return true;
    LOG_WARNING("[INPUT] could not hook %s at %p", What, Target);
    return false;
}

template <int N>
void HookSetFrom(void** Vt) {
    HookSet& H = g_sets[N];
    H.AcquireTarget = Vt[kAcquire];
    H.GetStateTarget = Vt[kGetState];
    H.GetDataTarget = Vt[kGetData];
    HookOne(H.AcquireTarget, reinterpret_cast<void*>(&AcquireDetour<N>), reinterpret_cast<void**>(&H.Acquire), "Acquire");
    HookOne(H.GetStateTarget, reinterpret_cast<void*>(&GetStateDetour<N>), reinterpret_cast<void**>(&H.GetState), "GetDeviceState");
    HookOne(H.GetDataTarget, reinterpret_cast<void*>(&GetDataDetour<N>), reinterpret_cast<void**>(&H.GetData), "GetDeviceData");
}

// A throwaway keyboard device through each interface flavour, for the vtables.
void HookDirectInput() {
    wchar_t Path[MAX_PATH];
    GetSystemDirectoryW(Path, MAX_PATH);
    wcscat_s(Path, L"\\dinput8.dll");
    HMODULE Real = LoadLibraryW(Path);
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto Create = Real ? reinterpret_cast<CreateFn>(GetProcAddress(Real, "DirectInput8Create")) : nullptr;
    if (!Create) {
        LOG_WARNING("[INPUT] system dinput8.dll not available — the game keeps its input");
        return;
    }

    void** Vt[2] = {};
    const IID* Iids[2] = { &IID_IDirectInput8W, &IID_IDirectInput8A };
    for (int i = 0; i < 2; i++) {
        IUnknown* Di = nullptr;
        if (FAILED(Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, *Iids[i],
                          reinterpret_cast<LPVOID*>(&Di), nullptr)) || !Di) continue;
        void* Dev = nullptr;
        using CreateDeviceFn = HRESULT(WINAPI*)(void*, REFGUID, void**, LPUNKNOWN);
        if (SUCCEEDED(reinterpret_cast<CreateDeviceFn>(Vtable(Di)[3])(Di, GUID_SysKeyboard, &Dev, nullptr)) && Dev) {
            Vt[i] = Vtable(Dev);
            static_cast<IUnknown*>(Dev)->Release();
        }
        Di->Release();
    }

    if (Vt[0]) HookSetFrom<0>(Vt[0]);
    if (Vt[1] && (!Vt[0] || Vt[1][kGetState] != Vt[0][kGetState])) HookSetFrom<1>(Vt[1]);
    LOG_INFO("[INPUT] DirectInput hooked (%s)", Vt[0] && Vt[1] && Vt[1][kGetState] != Vt[0][kGetState]
                                                  ? "Unicode and ANSI" : "shared");
}

// ---- XInput and the cursor ---------------------------------------------------------

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xinputGetState = nullptr;

DWORD WINAPI XInputGetStateDetour(DWORD User, XINPUT_STATE* State) {
    const DWORD Rc = g_xinputGetState(User, State);
    if (Rc == ERROR_SUCCESS && State && g_blocked.load()) {
        ZeroMemory(&State->Gamepad, sizeof(State->Gamepad));
    }
    return Rc;
}

using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ShowCursorFn   = int(WINAPI*)(BOOL);
using SetCursorFn    = HCURSOR(WINAPI*)(HCURSOR);
using ClipCursorFn   = BOOL(WINAPI*)(const RECT*);
SetCursorPosFn g_setCursorPos = nullptr;
ShowCursorFn   g_showCursor   = nullptr;
SetCursorFn    g_setCursor    = nullptr;
ClipCursorFn   g_clipCursor   = nullptr;

// The menu's Windows cursor: wanted from the render thread, switched on the
// window thread (ApplyMenuCursor).
std::atomic<bool> g_cursorWanted{ false };
bool g_cursorShown = false;   // window thread only
// ShowCursor(TRUE) calls the menu added on top of the game's own, taken back
// when the menu closes. The game's thread (ShowCursorDetour) and the window
// thread (ApplyMenuCursor) both move the real count and this one, so every real
// call and its bookkeeping happen together under g_cursorMutex -- done apart,
// the two could drift, and nothing would ever bring them back in line.
std::recursive_mutex g_cursorMutex;
int  g_showCalls = 0;         // under g_cursorMutex
RECT g_gameClip{};            // the clip the game last asked for, put back on close
bool g_gameClipSet = false;   // both under g_cursorMutex

// The mod's own calls go past the detours, to the originals.
int     RealShowCursor(BOOL Show)   { return g_showCursor ? g_showCursor(Show) : ShowCursor(Show); }
BOOL    RealClipCursor(const RECT* R) { return g_clipCursor ? g_clipCursor(R) : ClipCursor(R); }
BOOL    RealSetCursorPos(int X, int Y) { return g_setCursorPos ? g_setCursorPos(X, Y) : SetCursorPos(X, Y); }
HCURSOR RealSetCursor(HCURSOR C)    { return g_setCursor ? g_setCursor(C) : SetCursor(C); }

BOOL WINAPI SetCursorPosDetour(int X, int Y) {
    if (g_blocked.load()) return TRUE;   // the game keeps the hidden cursor centred
    return g_setCursorPos(X, Y);
}

// The game keeps its own copy of the display count (exe+0xAF42B0, input manager
// +0x668) and calls ShowCursor only while that copy is not what it wants: TRUE
// until it reads 0, FALSE once when it reads 0. So every call goes through, and
// the game is shown the count without the menu's additions. Swallowing its
// hides and letting it read our additions made it read 1, 2, 3... and call TRUE
// every frame -- the count reached 3151 and the cursor stayed up in play (the
// 12.09 test). While the menu is open, a hide from the game is answered by one
// more show of our own.
int WINAPI ShowCursorDetour(BOOL Show) {
    std::lock_guard<std::recursive_mutex> Lock(g_cursorMutex);
    int Real = g_showCursor(Show);
    if (!Show && g_cursorWanted.load() && Real < 0) {
        Real = g_showCursor(TRUE);
        ++g_showCalls;
    }
    return Real - g_showCalls;
}

HCURSOR WINAPI SetCursorDetour(HCURSOR Cursor) {
    if (!Cursor && g_cursorWanted.load()) return g_setCursor(LoadCursor(nullptr, IDC_ARROW));
    return g_setCursor(Cursor);
}

BOOL WINAPI ClipCursorDetour(const RECT* Rect) {
    {
        std::lock_guard<std::recursive_mutex> Lock(g_cursorMutex);
        if (Rect) {
            g_gameClip = *Rect;
            g_gameClipSet = true;
        } else {
            g_gameClipSet = false;
        }
    }
    if (g_cursorWanted.load()) return TRUE;   // free to move while the menu is open
    return g_clipCursor(Rect);
}

void HookXInputAndCursor() {
    for (const wchar_t* Name : { L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll" }) {
        if (HMODULE X = GetModuleHandleW(Name)) {
            if (HookOne(reinterpret_cast<void*>(GetProcAddress(X, "XInputGetState")),
                        reinterpret_cast<void*>(&XInputGetStateDetour),
                        reinterpret_cast<void**>(&g_xinputGetState), "XInputGetState")) {
                LOG_INFO("[INPUT] XInput hooked");
            }
            break;
        }
    }
    if (HMODULE U = GetModuleHandleW(L"user32.dll")) {
        HookOne(reinterpret_cast<void*>(GetProcAddress(U, "SetCursorPos")),
                reinterpret_cast<void*>(&SetCursorPosDetour),
                reinterpret_cast<void**>(&g_setCursorPos), "SetCursorPos");
        HookOne(reinterpret_cast<void*>(GetProcAddress(U, "ShowCursor")),
                reinterpret_cast<void*>(&ShowCursorDetour),
                reinterpret_cast<void**>(&g_showCursor), "ShowCursor");
        HookOne(reinterpret_cast<void*>(GetProcAddress(U, "SetCursor")),
                reinterpret_cast<void*>(&SetCursorDetour),
                reinterpret_cast<void**>(&g_setCursor), "SetCursor");
        HookOne(reinterpret_cast<void*>(GetProcAddress(U, "ClipCursor")),
                reinterpret_cast<void*>(&ClipCursorDetour),
                reinterpret_cast<void**>(&g_clipCursor), "ClipCursor");
    }
}

} // namespace

void InstallInputCapture() {
    if (g_installed.exchange(true)) return;
    HookDirectInput();
    HookXInputAndCursor();
}

void SetGameInputBlocked(bool blocked) {
    if (g_blocked.exchange(blocked) != blocked) {
        LOG_INFO("[INPUT] game input %s", blocked ? "held by the menu" : "returned to the game");
    }
}

bool IsGameInputBlocked() { return g_blocked.load(); }

void SetMenuCursorWanted(bool wanted) {
    if (g_cursorWanted.exchange(wanted) == wanted) return;
    // Wake the window thread so the switch lands now, not on the next stray message.
    if (HWND Hwnd = GetGameWindow()) PostMessageW(Hwnd, WM_NULL, 0, 0);
}

void ApplyMenuCursor(HWND hwnd) {
    const bool Wanted = g_cursorWanted.load();
    if (Wanted == g_cursorShown) return;
    g_cursorShown = Wanted;

    if (Wanted) {
        // Up the display count until the cursor shows, and remember by how much.
        int Count = -1, Game = 0;
        {
            std::lock_guard<std::recursive_mutex> Lock(g_cursorMutex);
            int Added = 0;
            do {
                Count = RealShowCursor(TRUE);
                ++Added;
            } while (Count < 0 && Added < 64);
            g_showCalls += Added;
            Game = Count - g_showCalls;
        }
        RealClipCursor(nullptr);

        // Where the game parked its hidden cursor may be off the window, or on
        // another monitor: bring it to the middle of the game.
        RECT Client{};
        POINT Pos{};
        if (hwnd && GetClientRect(hwnd, &Client) && GetCursorPos(&Pos)) {
            POINT Local = Pos;
            ScreenToClient(hwnd, &Local);
            if (!PtInRect(&Client, Local)) {
                POINT Mid{ (Client.left + Client.right) / 2, (Client.top + Client.bottom) / 2 };
                ClientToScreen(hwnd, &Mid);
                RealSetCursorPos(Mid.x, Mid.y);
            }
        }
        RealSetCursor(LoadCursor(nullptr, IDC_ARROW));
        LOG_INFO("[INPUT] menu cursor shown (display count %d, the game's own %d)", Count, Game);
    } else {
        RECT Clip{};
        bool ClipSet = false;
        {
            std::lock_guard<std::recursive_mutex> Lock(g_cursorMutex);
            for (; g_showCalls > 0; --g_showCalls) RealShowCursor(FALSE);
            Clip = g_gameClip;
            ClipSet = g_gameClipSet;
        }
        if (ClipSet) RealClipCursor(&Clip);
        LOG_INFO("[INPUT] cursor handed back to the game");
    }
}

} // namespace DS2Coop::UI
