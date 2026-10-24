#pragma once
// Vehicles (docs/re/R7_vehicles.md). Thumbstick driving needs nothing here: the virtual pad's left stick is the
// native analog steer/throttle. This module adds the optional grab mode on the vehicle model's real steering
// wheel or handlebars: while a hand squeezes near the wheel, the hands' angle about the steering bone becomes
// the native steer (virtual pad LX, so physics and multiplayer stay native) and the bone is posed 1:1 with
// the hands through the vehicle's own bone follower after the native animate step (visual only).
#include <cstdint>

namespace vehicles {
bool install();
// Simulation thread, once per local VR player action (before it is sent).
void step(uint8_t* player, void* vrd);
// Frame thread: the steer the hands are holding (pad LX, -1..1), when grab steering is active.
bool steerOverride(float& lx);
// The local VR player is driving a vehicle (grips grab instead of using gadgets).
bool driving();
}  // namespace vehicles
