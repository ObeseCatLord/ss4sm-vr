#pragma once
// The native weapon wheel (and its gadget ring) on a panel in front of the hand that opened it, with the world in
// stereo behind it, like the SS2 VR mod and eduke32 VR (docs/re/R16_wheel_panel.md).
//   - The wheel pushes an invisible menu screen (CMSDummyWeaponSelectionController) whose "show on the cinema
//     screen" default made the headset view go flat. While it is the top screen the render forces
//     vr_bForceImmersiveMode for that frame only (restored after), and the menu pointer leaves it alone.
//   - The panel is latched when the wheel opens: in front of the opening hand (both hands: between them), facing
//     the head. src/se4/hud_vr.cpp draws the wheel element there.
//   - The wheel's world blur/tint post-process is skipped, so the world stays sharp (option wheel_blur=1: native).
// Option wheel_panel (default 1).
namespace wheel_panel {
bool install();
// Render thread, before RenderVR: true while the wheel's menu screen is on top of `pi`'s menu manager (the panel
// is latched on the first such frame).
bool wheelOpen(void* pi);
// The latched panel (quaternion xyzw + position, device play space in engine units) while the wheel is open.
bool panel(float out[7]);
}  // namespace wheel_panel
