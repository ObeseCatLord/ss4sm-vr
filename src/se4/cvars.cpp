#include "se4/cvars.hpp"
#include "se4/log.hpp"
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
}  // namespace

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
