#pragma once
// "DLSS" and "FSR 3.1" in the game's own Options > Graphics "Rescaling Filter" list, driving the desktop upscaler
// (docs/re/R28_rescaling_filter_menu.md; desktop menu mode = VR off and option desktop_upscaler, src/up/upscaler.hpp).
//   - The list is the CMSGraphicsOptions widget +0x1f0, bound to ren_iRescalingFilter through a cvar link whose option
//     collection holds the six stock filters ("0".."5"). The mod appends its entries (plain-text labels, mod value
//     strings) after them, one per available backend.
//   - wlink_set: a mod value selects the mod's upscaler and is swallowed, so the cvar is never written; any other
//     value (a stock filter, KeepSettings' revert) turns it off and goes through. wlink_get reports the mod value while
//     it is on, so the row shows DLSS / FSR 3.1. wlink_reset (Default) turns it off. The step keeps the row visible.
//   - The choice is saved in ss4smvr/upscaler_menu.txt (upscaler=off|dlss|fsr), never in a game cvar or .ini. Option
//     upscaler in options.txt, when set, overrides it at startup; the menu still switches at runtime.
//   - The switch itself happens at the next frame boundary (upint::requestKind).
namespace gfxmenu {
// At attach in desktop menu mode: resolves and verifies the symbols and call sites, reads the startup choice and hands
// it to up::setMenuStart. Installs nothing.
bool prepare();
// After up::init on the desktop Vulkan device: installs the menu hooks (all or none) when prepare passed, the flat
// upscaler path is ready and at least one backend is available. Otherwise the menu stays stock.
void onUpscalerReady();
}  // namespace gfxmenu
