#pragma once
// While VR is active the desktop game window is usually not focused (the player is in the headset, and
// the XR compositor may own a window). The engine ignores input and throttles while inactive, so keep
// it believing its window is active: answer focus queries with the game window and swallow deactivation.
#include <windows.h>

namespace focus {
bool install();            // at attach: hooks GetForegroundWindow/GetActiveWindow/GetFocus (MinHook)
void enable(HWND gameWnd);  // when the XR session starts: subclass the window and start overriding
void disable();             // no XR session: normal focus again (the window procedure stays, passing through)
}
