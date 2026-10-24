#include "se4/cvars.hpp"
#include "se4/log.hpp"

namespace se4 {
bool setIndex(gen::Sym s, int32_t value) {
    auto* p = build().at<int32_t>(s);
    if (!p) return false;
    int32_t old = *p;
    *p = value;
    VRLOG("cvar %s: %d -> %d", gen::kSymNames[s], old, value);
    return true;
}

void applyVrFramePacing() {
    // Raise caps instead of zeroing them: 0 is not "unlimited" for every cap (gfx_iMaxFPSInactive 0
    // gives 1 fps, and zeroed caps left the post-load chapter screen unable to continue).
    setIndex(gen::gfx_iMaxFPSInactive, 1000);
    setIndex(gen::gfx_iWaitVSyncs, 0);
}
}  // namespace se4
