#pragma once
// Virtual XInput gamepad (user index 0) fed from the VR controllers. Movement, turning, triggers and
// buttons reach the game through its own gamepad bindings (DefaultGamepadBindings.lua), so every
// command stays native and multiplayer-safe. Game rumble (XInputSetState) becomes hand haptics.
namespace gamepad {
void publishFromXr();  // after xr::input().sync(), on the frame thread
bool active();
void logStats();
// The hand whose wheel is held: 0 left, 1 right, -1 both (or none).
int wheelHand();
}
