// ss4smvr.dll entry: verifies the build, then waits for the game's own modules to load before acting.
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "vk/vk_layer.hpp"
#include <windows.h>
#include <winternl.h>
#include <MinHook.h>
#include <cstdlib>
#include <cstring>

namespace {
HMODULE g_self;

// LdrRegisterDllNotification (ntdll, documented in MSDN for Windows Vista+; implemented by Wine).
struct DllNotificationData {
    ULONG Flags;
    const UNICODE_STRING* FullDllName;
    const UNICODE_STRING* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
using NotifyFn = VOID(CALLBACK*)(ULONG reason, const DllNotificationData* data, PVOID ctx);
using RegisterFn = NTSTATUS(NTAPI*)(ULONG, NotifyFn, PVOID, PVOID*);
PVOID g_cookie;

bool nameIs(const UNICODE_STRING* s, const wchar_t* want) {
    size_t n = wcslen(want);
    return s && s->Length / 2 == n && !_wcsnicmp(s->Buffer, want, n);
}

void startObserver();
VOID CALLBACK onDll(ULONG reason, const DllNotificationData* d, PVOID) {
    if (reason != 1) return;  // LDR_DLL_NOTIFICATION_REASON_LOADED
    if (nameIs(d->BaseDllName, L"vulkan-1.dll")) {
        vk_layer::onVulkanLoaded(d->DllBase);
        startObserver();
    }
}

DWORD WINAPI observer(LPVOID);
// Never create threads from DllMain/attach during process start: on Wine that stalls startup (bisected
// 2026-10-23). The observer starts once the game itself loads vulkan-1.dll.
void startObserver() {
    static LONG started;
    if (!InterlockedExchange(&started, 1)) CloseHandle(CreateThread(nullptr, 0, observer, nullptr, 0, nullptr));
}

DWORD WINAPI observer(LPVOID) {
    const auto& b = se4::build();
    for (int i = 0; i < 120; ++i) {
        Sleep(1000);
        void** dev = b.at<void*>(se4::gen::vr_device_global);
        int* sps = b.at<int>(se4::gen::vr_iUsingSinglePassStereo);
        if (i % 10 == 0 || (dev && *dev))
            VRLOG("observe t=%ds device=%p singlePass=%d", i + 1, dev ? *dev : nullptr, sps ? *sps : -1);
    }
    return 0;
}
}  // namespace

extern "C" __declspec(dllexport) void ss4smvr_attach(HMODULE) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (!n || n >= MAX_PATH - 16) return;
    wcscpy(wcsrchr(path, L'\\') + 1, L"ss4smvr.log");
    se4::logOpen(path);
    VRLOG("ss4smvr payload attached");
    unsigned skip = 0;  // lab bisect: ss4smvr/debug_skip.txt holds a bitmask of attach steps to skip
    {
        wchar_t cfg[MAX_PATH];
        wcscpy(cfg, path);
        wcscpy(wcsrchr(cfg, L'\\') + 1, L"debug_skip.txt");
        HANDLE f = CreateFileW(cfg, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char b[16] = {};
            DWORD r;
            ReadFile(f, b, 15, &r, nullptr);
            CloseHandle(f);
            skip = strtoul(b, nullptr, 0);
            VRLOG("attach: debug skip mask 0x%x", skip);
        }
    }
    se4::Build b;
    if (!se4::identifyBuild(b)) return;
    VRLOG("attach: MH_Initialize");
    if (!(skip & 1) && MH_Initialize() != MH_OK) { VRLOG("MinHook init failed"); return; }
    VRLOG("attach: register dll notification");
    auto reg = reinterpret_cast<RegisterFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
    if (!(skip & 2) && (!reg || reg(0, onDll, nullptr, &g_cookie) != 0)) VRLOG("LdrRegisterDllNotification unavailable");
    VRLOG("attach: notification registered");
    if (HMODULE vk = GetModuleHandleW(L"vulkan-1.dll")) vk_layer::onVulkanLoaded(vk);
    VRLOG("attach: done");
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = self;
        DisableThreadLibraryCalls(self);
    }
    return TRUE;
}
