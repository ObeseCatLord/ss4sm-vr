// msimg32.dll proxy: forwards the five msimg32 exports to the system copy and loads the VR payload.
// Sam4.exe/SamSM.exe import only AlphaBlend from MSIMG32.dll. Nothing else happens here; the payload
// decides (by build fingerprint) whether to do anything at all.
#include <windows.h>
#include <wchar.h>

namespace {
HMODULE g_system;
HMODULE g_payload;

FARPROC real(const char* name) {
    if (!g_system) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 16) return nullptr;
        wcscat(path, L"\\msimg32.dll");
        g_system = LoadLibraryW(path);
    }
    return g_system ? GetProcAddress(g_system, name) : nullptr;
}

void loadPayload(HMODULE self) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash || (slash - path) + 24 >= MAX_PATH) return;
    wcscpy(slash + 1, L"ss4smvr\\ss4smvr.dll");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;
    g_payload = LoadLibraryW(path);
    if (!g_payload) return;
    using Attach = void (*)(HMODULE);
    if (auto attach = reinterpret_cast<Attach>(GetProcAddress(g_payload, "ss4smvr_attach"))) attach(self);
}
}  // namespace

extern "C" {
BOOL WINAPI ProxyAlphaBlend(HDC a, int b, int c, int d, int e, HDC f, int g, int h, int i, int j, BLENDFUNCTION k) {
    using F = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
    static F f_ = reinterpret_cast<F>(real("AlphaBlend"));
    return f_ ? f_(a, b, c, d, e, f, g, h, i, j, k) : FALSE;
}
BOOL WINAPI ProxyGradientFill(HDC a, PTRIVERTEX b, ULONG c, PVOID d, ULONG e, ULONG f) {
    using F = BOOL(WINAPI*)(HDC, PTRIVERTEX, ULONG, PVOID, ULONG, ULONG);
    static F f_ = reinterpret_cast<F>(real("GradientFill"));
    return f_ ? f_(a, b, c, d, e, f) : FALSE;
}
BOOL WINAPI ProxyTransparentBlt(HDC a, int b, int c, int d, int e, HDC f, int g, int h, int i, int j, UINT k) {
    using F = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, int, int, UINT);
    static F f_ = reinterpret_cast<F>(real("TransparentBlt"));
    return f_ ? f_(a, b, c, d, e, f, g, h, i, j, k) : FALSE;
}
BOOL WINAPI ProxyDllInitialize(HINSTANCE a, DWORD b, LPVOID c) {
    using F = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
    static F f_ = reinterpret_cast<F>(real("DllInitialize"));
    return f_ ? f_(a, b, c) : TRUE;
}
VOID WINAPI ProxyvSetDdrawflag() {
    using F = VOID(WINAPI*)();
    static F f_ = reinterpret_cast<F>(real("vSetDdrawflag"));
    if (f_) f_();
}
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        real("AlphaBlend");
        loadPayload(self);
    }
    return TRUE;
}
