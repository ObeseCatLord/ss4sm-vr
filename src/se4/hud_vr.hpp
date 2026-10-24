#pragma once
// The game's HUD in immersive VR (docs/re/R8_crosshair.md A5, docs/re/R16_wheel_panel.md s2.1). With the VR device
// in immersive mode the HUD render draws every element as a quad on a head-relative, lazily following panel:
// model = anchor * hep_qvVRPlacement (params +0x18 quaternion, +0x28 position) * hep_fVRStretch (+0x34, y flipped),
// so element pixels become metres. SS4/SM shipped every element with identity / 0 / 1 (they never authored a VR
// HUD), which puts the HUD hundreds of metres off-screen; and even with placements written, most elements do not
// follow them (simple elements set their own matrix, R8 A6). So by default (hud_vr_mode=0, flat):
//   - the HUD render takes its 2D branch in each eye image (its immersive-flag call is gated), as on desktop; world
//     markers and the crosshair project per eye (src/se4/crosshair.cpp), so they sit at their targets' depth.
//     (gfx_fSafeAreaX/Y around element draws did not move the layout in the lab; pulling the HUD in from the
//     image edges is open);
//   - while the weapon wheel is open the HUD keeps its 3D branch and the wheel is drawn on the hand panel.
// hud_vr_mode=1 (3D panel, experimental: only the wheel and canvas elements follow it) does, per element draw
// (restored right after, in memory only):
//   - ordinary elements get a placement that maps the drawport onto a panel hud_distance_cm away whose height
//     covers hud_fov degrees (defaults 120 cm, 40 deg), centred on the native lazy-follow anchor;
//   - the weapon wheel is drawn on the hand panel of src/se4/wheel_panel.cpp (anchor = inverse(head) * panel),
//     the drawport height mapped to wheel_panel_cm (default 45);
//   - the crosshair element is drawn in 2D (its VR flag off and batching off for its draw only), so each eye
//     gets its own projection of the barrel target (src/se4/crosshair.cpp) and the crosshair has the target's
//     depth.
// Elements whose params already carry a VR placement are left native. Option hud_vr (default 1).
namespace hud_vr {
bool install();
}
