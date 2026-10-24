#pragma once
// First-person arms in VR (docs/re/R15_hands.md s1, s8). Every first-person weapon model is an X_Arms.mdl whose
// root the engine places at the controller; both arms keep their flat-screen pose around the gun. A mod motion
// handler on the local player's weapon instance parks the arm that does not hold that weapon behind the eyes
// (it cannot be hidden: one mesh, no per-bone scale), unless it grips a two-handed weapon. Option hands (default 1).
#include <cstdint>

namespace arms {
bool install();
// Simulation thread, from the weapon step of the local player's weapons.
void onWeaponStep(uint8_t* weapon, bool rightHand, bool gripped);
}  // namespace arms
