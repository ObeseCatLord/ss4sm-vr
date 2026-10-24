#pragma once
// Direct access to engine console variables at addresses harvested from the registration tables.
#include "se4/build.hpp"
#include <cstdint>

namespace se4 {
// INDEX/BOOL cvars are 32-bit signed integers. The first override of a cvar records its value so
// restoreOverrides() can put it back (called when the engine shuts the VR device down, so values the mod
// forces for VR are not what the engine finds at exit). Returns false when the build lacks the symbol.
bool setIndex(gen::Sym s, int32_t value);
void restoreOverrides();
// VR frame pacing: the headset drives the frame rate, so engine FPS caps (including the
// unfocused-window cap, default 20) are lifted while an XR session exists.
void applyVrFramePacing();
}  // namespace se4
