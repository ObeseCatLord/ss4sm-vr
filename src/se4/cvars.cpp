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
    setIndex(gen::gfx_iMaxFPSActive, 0);      // 0 = unlimited for the active cap
    setIndex(gen::gfx_iMaxFPSInactive, 1000); // 0 would mean 1 fps here, not unlimited
    setIndex(gen::men_iMaxAllowedFPS, 0);
    setIndex(gen::gfx_iWaitVSyncs, 0);
}
}  // namespace se4
