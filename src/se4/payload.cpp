// ss4smvr.dll entry: verifies the build, then intercepts the game's Vulkan entry points.
//
// Attach runs inside the proxy's DllMain (loader lock, process start). Keep it to the build check and
// one MinHook hook. Never create threads here: on Wine that stalls game startup (bisected 2026-10-23).
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/procaddr.hpp"
#include "se4/focus.hpp"
#include "se4/vr_device.hpp"
#include "se4/player_vr.hpp"
#include "se4/crosshair.hpp"
#include "se4/vehicles.hpp"
#include "se4/body.hpp"
#include "se4/melee.hpp"
#include "se4/scope.hpp"
#include "se4/hands.hpp"
#include "se4/arms.hpp"
#include "se4/gadgets.hpp"
#include "se4/wheel_panel.hpp"
#include "se4/hud_vr.hpp"
#include "se4/artifact.hpp"
#include "up/integrate.hpp"
#include "se4/options.hpp"
#include "vk/vk_layer.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cstdlib>
#include <cstring>

namespace {
HMODULE g_self;

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

unsigned readSkipMask(const wchar_t* logPath) {
    // Lab bisect switch: ss4smvr/debug_skip.txt holds a bitmask of attach steps to skip.
    wchar_t cfg[MAX_PATH];
    wcscpy(cfg, logPath);
    wcscpy(wcsrchr(cfg, L'\\') + 1, L"debug_skip.txt");
    HANDLE f = CreateFileW(cfg, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    char b[16] = {};
    DWORD r;
    ReadFile(f, b, 15, &r, nullptr);
    CloseHandle(f);
    return unsigned(strtoul(b, nullptr, 0));
}
}  // namespace

namespace vk_layer {
// First game Vulkan call, on a game thread outside the loader lock.
void onGameVulkan() {
    static LONG started;
    if (!InterlockedExchange(&started, 1)) CloseHandle(CreateThread(nullptr, 0, observer, nullptr, 0, nullptr));
}
}  // namespace vk_layer

extern "C" __declspec(dllexport) void ss4smvr_attach(HMODULE) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (!n || n >= MAX_PATH - 16) return;
    wcscpy(wcsrchr(path, L'\\') + 1, L"ss4smvr.log");
    se4::logOpen(path);
    VRLOG("ss4smvr payload attached");
    unsigned skip = readSkipMask(path);
    if (skip) VRLOG("attach: debug skip mask 0x%x", skip);
    se4::g_debugSkip = skip;
    se4::options::load(path);
    {
        wchar_t flag[MAX_PATH];
        wcscpy(flag, path);
        wcscpy(wcsrchr(flag, L'\\') + 1, L"enable_vr");
        char env[8] = {};
        se4::g_vrEnabled = GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES ||
                           (GetEnvironmentVariableA("SS4SMVR_VR", env, sizeof env) && env[0] == '1');
        VRLOG("attach: VR %s (opt-in: SS4SMVR_VR=1 or ss4smvr/enable_vr)", se4::g_vrEnabled ? "enabled" : "disabled");
    }
    se4::Build b;
    if (!se4::identifyBuild(b)) return;
    if (MH_Initialize() != MH_OK) {
        VRLOG("MinHook init failed");
        return;
    }
    if (!se4::g_vrEnabled) {
        // Flat play: no hooks, unless this desktop player opts into guards for VR peers in multiplayer.
        if (se4::options::get("mp_guards", 0)) player_vr::install(false);
        return;
    }
    if (!(skip & 2)) se4::installProcAddressHook();
    if (!(skip & 4)) focus::install();
    if (!(skip & 8)) {
        vrdev::hookShutdown();
        vrdev::hookViewport();
        vrdev::hookRender();
        upint::install();
    }
    if (!(skip & 0x10) && player_vr::install(true)) {
        if (!(skip & 0x20)) crosshair::install();
        if (!(skip & 0x40)) vehicles::install();
        if (!(skip & 0x80)) body::install();
        if (!(skip & 0x100)) melee::install();
        if (!(skip & 0x200)) scope::install();
        if (!(skip & 0x800)) arms::install();
        if (!(skip & 0x400)) hands::install();
        if (!(skip & 0x1000)) gadgets::install();
        if (!(skip & 0x2000)) wheel_panel::install();
        if (!(skip & 0x4000)) hud_vr::install();
        if (!(skip & 0x8000)) artifact::install();
    }
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = self;
        DisableThreadLibraryCalls(self);
    }
    return TRUE;
}
