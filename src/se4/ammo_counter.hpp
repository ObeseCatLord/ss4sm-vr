#pragma once
// On-weapon ammo counters in VR (docs/re/R32_ammo_counters.md): the engine's own counter values (weapon vt+0xa90 ammo,
// vt+0xa98 maximum, the calls its VR counter 0x1402d4b70 makes every weapon step) drawn on the top-rear of each gun
// in the local player's hands as an OpenXR quad layer (src/vk/ammo_layer.cpp). Weapons whose params set a VR model
// (wp_pmcVRModel: the minigun and grenade launcher) carry the engine's own AmmoCounter surface and are skipped.
// Options: ammo_counter (default 1), ammo_counter_native (1: skip weapons with the native counter; 0: draw on them
// too), ammo_counter_size_cm, ammo_counter_up_cm / _back_cm (offsets for every weapon, on top of the per-weapon spot).
#include "core/math.hpp"
#include <cstdint>

namespace ammo {
bool install();
// Simulation thread, from the weapon step hook (src/se4/hands.cpp), for each weapon of the local player.
void onWeaponStep(uint8_t* weapon, uint8_t* player, bool rightHand);

struct Hand {
    bool visible = false;
    int count = 0, max = 0;
    core::Quat gripToSpot{0, 0, 0, 1};  // the counter's frame relative to the hand's grip pose (OpenXR)
    core::Vec3 gripOffset{};            // its centre in the grip frame, metres
    float width = 0.05f;                // quad width in metres (height = width / 2)
};
// Present thread: the counter for OpenXR hand 0 (left) / 1 (right), fresh within the last 250 ms.
Hand hand(int xrHand);
}  // namespace ammo
