#include "se4/focus.hpp"
#include "se4/log.hpp"
#include <MinHook.h>

namespace focus {
namespace {
using HwndFn = HWND(WINAPI*)();
HwndFn realForeground, realActive, realFocus;
HWND g_wnd;
WNDPROC g_prevProc;
volatile bool g_on;

HWND WINAPI hookForeground() { return g_on ? g_wnd : realForeground(); }
HWND WINAPI hookActive() {
    if (g_on && GetWindowThreadProcessId(g_wnd, nullptr) == GetCurrentThreadId()) return g_wnd;
    return realActive();
}
HWND WINAPI hookFocus() {
    if (g_on && GetWindowThreadProcessId(g_wnd, nullptr) == GetCurrentThreadId()) return g_wnd;
    return realFocus();
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_on) {
        switch (msg) {
        case WM_ACTIVATEAPP:
            if (!wp) return 0;  // swallow deactivation
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) return 0;
            break;
        case WM_NCACTIVATE:
            if (!wp) return TRUE;
            break;
        case WM_KILLFOCUS:
            return 0;
        }
    }
    return CallWindowProcW(g_prevProc, h, msg, wp, lp);
}

template <class T> bool hook(const char* name, void* detour, T* orig) {
    void* t = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), name));
    return t && MH_CreateHook(t, detour, reinterpret_cast<void**>(orig)) == MH_OK && MH_EnableHook(t) == MH_OK;
}
}  // namespace

bool install() {
    if (!GetModuleHandleW(L"user32.dll")) LoadLibraryW(L"user32.dll");
    bool ok = hook("GetForegroundWindow", reinterpret_cast<void*>(hookForeground), &realForeground) &&
              hook("GetActiveWindow", reinterpret_cast<void*>(hookActive), &realActive) &&
              hook("GetFocus", reinterpret_cast<void*>(hookFocus), &realFocus);
    VRLOG("focus: hooks %s", ok ? "installed" : "FAILED");
    return ok;
}

void enable(HWND wnd) {
    if (!wnd || g_on) return;
    g_wnd = wnd;
    g_prevProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(proc)));
    g_on = true;
    // Tell the engine it is active now, in case it was deactivated before VR started.
    PostMessageW(wnd, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageW(wnd, WM_ACTIVATE, WA_ACTIVE, 0);
    PostMessageW(wnd, WM_SETFOCUS, 0, 0);
    VRLOG("focus: game window %p kept active while VR runs", static_cast<void*>(wnd));
}
}  // namespace focus
