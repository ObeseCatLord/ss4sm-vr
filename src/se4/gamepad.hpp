#pragma once
// Virtual XInput gamepad (user index 0) fed from the VR controllers. Movement, turning, triggers and
// buttons reach the game through its own gamepad bindings (DefaultGamepadBindings.lua), so every
// command stays native and multiplayer-safe. Game rumble (XInputSetState) becomes hand haptics.
namespace gamepad {
void publishFromXr();  // after xr::input().sync(), on the frame thread
bool active();
void logStats();
// The wheel spot (core::controls::Spot) pointed at while a wheel is held, else -1. Any thread.
int wheelSpot();
// Simulation thread: the spot the wheel was released on, once: spot | hand << 4 (hand 0 left, 1 right), else -1.
int takeSpotAction();
// A weapon wheel is held (either hand).
bool wheelHeld();
// The hand whose wheel is held: 0 left, 1 right, -1 both (or none).
int wheelHand();
// The use/melee (kick) button state from the controller mapping (src/se4/gadgets.cpp runs the native VR use).
bool kick();
}
