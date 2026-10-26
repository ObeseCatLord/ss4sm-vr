#pragma once
#include <cstdint>
// The native weapon wheel (and its gadget ring) on a panel in front of the hand that opened it, with the world in
// stereo behind it, like the SS2 VR mod and eduke32 VR (docs/re/R16_wheel_panel.md).
//   - The wheel pushes an invisible menu screen (CMSDummyWeaponSelectionController) whose "show on the cinema
//     screen" default made the headset view go flat. While it is the top screen the render forces
//     vr_bForceImmersiveMode for that frame only (restored after), and the menu pointer leaves it alone.
//   - The panel is latched when the wheel opens: in front of the opening hand (both hands: between them), facing
//     the head. src/se4/hud_vr.cpp draws the wheel element there.
//   - The wheel's world blur/tint post-process is skipped, so the world stays sharp (option wheel_blur=1: native).
// Spots beside the wheel (R18, like Fusion's VR wheel buttons): moving the wheel hand past the wheel to the side
// points at Menu / Netricsa (left) or Quick load / Quick save (right); releasing the wheel there keeps the weapons
// and runs that action through the game's own VR wheel button functions (save/load only where the game allows it).
// Option wheel_panel (default 1), wheel_spots (default 1).
namespace wheel_panel {
bool install();
// Render thread, before RenderVR: true while the wheel's menu screen is on top of `pi`'s menu manager (the panel
// is latched on the first such frame).
bool wheelOpen(void* pi);
// The latched panel (quaternion xyzw + position, device play space in engine units) while the wheel is open.
bool panel(float out[7]);
// Simulation thread (local VR player): run a spot the wheel was released on.
void onTick(uint8_t* player);
// HUD render, before the wheel element draws: no weapon highlighted while a spot is pointed at.
void beforeWheelDraw(uint8_t* wheel);
bool spotsAvailable();
}  // namespace wheel_panel
