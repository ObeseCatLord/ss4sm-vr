#pragma once
// Foliage sway while an upscaler runs (docs/re/R29_upscaler_menu_vr_sharpening.md s5). Tree bend, branch and leaf-edge
// sway are vertex animations (VegetationShader.vsh) with no motion vectors, so DLSS / FSR cannot follow them and swaying
// leaves shimmer. sha_iAllowTreeSwaying (0 none, 1 limited, 2 all; a saved cvar, never written) gates all of it: its
// reads (6 in Sam4, 7 in SamSM, found by scanning .text for rip-relative cmp/mov of it) are pointed at a mod copy that
// mirrors the cvar, or holds min(cvar, mode) while an upscaler runs.
namespace foliage {
bool install();
// Per frame (flat: player-views entry; VR: device slot 0x030).
void update(bool upscaling);
// -1 the game's own setting, 0 still, 1 limited, 2 full (any thread).
void setMode(int mode);
int mode();
}  // namespace foliage
