#include "se4/cvars.hpp"
#include "se4/log.hpp"
#include <MinHook.h>
#include <windows.h>

namespace se4 {
namespace {
struct Saved {
    gen::Sym sym;
    int32_t value;
};
Saved g_saved[16];
int g_savedCount;
SRWLOCK g_lock = SRWLOCK_INIT;

using SaveFn = void(__cdecl*)(void* file, int mode);
SaveFn realSave;
void __cdecl hookedSave(void* file, int mode) {
    int32_t now[sizeof g_saved / sizeof g_saved[0]];
    AcquireSRWLockExclusive(&g_lock);
    const int n = g_savedCount;
    for (int i = 0; i < n; ++i)
        if (auto* p = build().at<int32_t>(g_saved[i].sym)) now[i] = *p, *p = g_saved[i].value;
    realSave(file, mode);
    for (int i = 0; i < n; ++i)
        if (auto* p = build().at<int32_t>(g_saved[i].sym)) *p = now[i];
    ReleaseSRWLockExclusive(&g_lock);
    static int logged;
    if (n && logged++ < 4) VRLOG("cvar: settings saved with the user's own values for %d overridden cvar(s)", n);
}
}  // namespace

bool installSaveGuard() {
    if (realSave) return true;
    void* target = build().at<void>(gen::cvar_save);
    if (!target || MH_CreateHook(target, reinterpret_cast<void*>(hookedSave), reinterpret_cast<void**>(&realSave)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        VRLOG("cvar: save guard FAILED - overridden cvars may reach the user's settings");
        return false;
    }
    return true;
}

bool setIndex(gen::Sym s, int32_t value) {
    auto* p = build().at<int32_t>(s);
    if (!p) return false;
    AcquireSRWLockExclusive(&g_lock);
    bool known = false;
    for (int i = 0; i < g_savedCount; ++i) known |= g_saved[i].sym == s;
    if (!known && g_savedCount < int(sizeof g_saved / sizeof g_saved[0])) g_saved[g_savedCount++] = {s, *p};
    int32_t old = *p;
    *p = value;
    ReleaseSRWLockExclusive(&g_lock);
    VRLOG("cvar %s: %d -> %d", gen::kSymNames[s], old, value);
    return true;
}

void restoreOverrides() {
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_savedCount; ++i)
        if (auto* p = build().at<int32_t>(g_saved[i].sym)) *p = g_saved[i].value;
    int n = g_savedCount;
    g_savedCount = 0;
    ReleaseSRWLockExclusive(&g_lock);
    if (n) VRLOG("cvar: restored %d overridden values", n);
}

void applyVrFramePacing() {
    // Raise caps instead of zeroing them: 0 is not "unlimited" for every cap (gfx_iMaxFPSInactive 0
    // gives 1 fps, and zeroed caps left the post-load chapter screen unable to continue).
    setIndex(gen::gfx_iMaxFPSInactive, 1000);
    setIndex(gen::gfx_iWaitVSyncs, 0);
}
}  // namespace se4
