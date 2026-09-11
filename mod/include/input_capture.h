#pragma once

// Taking the game's input away while the co-op menu is open.
//
// Drawing the menu over the game was not enough: the character kept walking,
// the camera kept turning, and there was no cursor until the game's own pause
// menu was opened. The game reads the keyboard and mouse through DirectInput
// (it imports DirectInput8Create, which this DLL proxies), pads through XInput,
// and keeps its hidden cursor centred with SetCursorPos. While blocked:
//
//   - DirectInput keyboard and mouse report nothing, and the mouse is released
//     so the Windows cursor moves freely (re-acquired when the menu closes);
//   - XInput pads report a neutral state;
//   - the game's SetCursorPos calls are ignored.
//
// Window messages are withheld from the game by the WndProc hook in renderer.cpp.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace DS2Coop::UI {

void InstallInputCapture();          // once, after MinHook is up; safe to repeat
void SetGameInputBlocked(bool blocked);
bool IsGameInputBlocked();

// The menu uses the real Windows cursor: the system draws it at the monitor's
// rate rather than once per game frame, and it is there even when the game
// window never sees the mouse move. The game hides, clips and recentres the
// cursor; while the menu wants it, those calls are held back. ShowCursor only
// takes effect on the thread that owns the window, so the switch itself is made
// from the window procedure (ApplyMenuCursor), not from the render thread.
void SetMenuCursorWanted(bool wanted);   // any thread
void ApplyMenuCursor(HWND hwnd);         // window thread, on every message
HWND GetGameWindow();                    // renderer.cpp

} // namespace DS2Coop::UI
