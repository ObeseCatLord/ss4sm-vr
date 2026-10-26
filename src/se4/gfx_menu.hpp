#pragma once
// "DLSS" and "FSR 3.1" in the game's own Options > Graphics "Rescaling Filter" list, driving the upscaler
// (docs/re/R28_rescaling_filter_menu.md; menu mode = desktop with option desktop_upscaler, or VR, src/up/upscaler.hpp),
// plus (docs/re/R29_upscaler_menu_vr_sharpening.md):
//   - an "Upscaler Sharpening" slider (0..100 %, default 25) for the mod's RCAS pass after either backend, built like
//     the engine's XML menu builder builds a slider and bound to a renamed link the mod answers;
//   - in VR, the "Max 3D rendering Mpix" row as "VR Render Resolution": percentages of the eye resolution.
//   - The list is the CMSGraphicsOptions widget +0x1f0, bound to ren_iRescalingFilter through a cvar link whose option
//     collection holds the six stock filters ("0".."5"). The mod appends its entries (plain-text labels, mod value
//     strings) after them, one per available backend.
//   - wlink_set: a mod value selects the mod's upscaler and is swallowed, so the cvar is never written; any other
//     value (a stock filter, KeepSettings' revert) turns it off and goes through. wlink_get reports the mod value while
//     it is on, so the row shows DLSS / FSR 3.1. wlink_reset (Default) turns it off. The step keeps the row visible.
//   - The choices are saved in ss4smvr/upscaler_menu.txt (upscaler / vr_upscaler = off|dlss|fsr, vr_render, sharpness),
//     never in a game cvar or .ini. Options upscaler and upscale_sharpness in options.txt, when set, override them at
//     startup; the menu still changes them at runtime.
//   - The switch itself happens at the next frame boundary (upint::requestKind).
namespace gfxmenu {
// At attach in menu mode: resolves and verifies the symbols and call sites, reads the saved state and hands it to
// up::setMenuStart, up::setSharpness and (VR) upint::setVrPercent. Installs nothing.
bool prepare();
// After up::init on the Vulkan device: installs the menu hooks (all or none) when prepare passed, (desktop) the flat
// upscaler path is ready and at least one backend is available. Otherwise the menu stays stock.
void onUpscalerReady();
}  // namespace gfxmenu
