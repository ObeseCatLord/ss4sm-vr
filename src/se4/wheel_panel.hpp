#pragma once
// The native weapon wheel (and its gadget ring) on a panel in front of the hand that opened it, with the world in
// stereo behind it, like the SS2 VR mod and eduke32 VR (docs/re/R16_wheel_panel.md).
//   - The wheel pushes an invisible menu screen (CMSDummyWeaponSelectionController) whose "show on the cinema
//     screen" default made the headset view go flat. While it is the top screen the render forces
//     vr_bForceImmersiveMode for that frame only (restored after), and the menu pointer leaves it alone.
//   - In immersive frames the engine draws each HUD element on a 3D panel: model = anchor (head) * the element's
//     hep_qvVRPlacement (params +0x18 quaternion, +0x28 position) * hep_fVRStretch (+0x34). For the wheel the
//     anchor is replaced, for its draw only, so that the model is a panel latched when the wheel opened: in front
//     of the opening hand (both hands: between them), facing the head.
// Option wheel_panel (default 1), wheel_panel_scale (percent of the native stretch, default 100).
namespace wheel_panel {
bool install();
// Render thread, before RenderVR: true while the wheel's menu screen is on top of `pi`'s menu manager (the panel
// is latched on the first such frame).
bool wheelOpen(void* pi);
}  // namespace wheel_panel
