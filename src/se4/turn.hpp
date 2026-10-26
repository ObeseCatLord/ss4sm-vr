#pragma once
// Native VR turning (docs/re/R26_vr_turning.md). The engine's VR rotation (0x14044e880 -> anchor turn) reads the stick
// from vrd's per-hand input table, which is only refreshed at spawn, so it never turned. Three in-memory read
// redirects make it work without writing any cvar (they are saved with the user's settings):
//   P1 the rotation method read -> the mod's value (1 smooth, 2 snap: option snap_turn);
//   P2 the rotation controller read -> 2 (right hand);
//   P3 the right stick load -> the mod's axis, fed every frame while native turning is on (else 0, so nothing turns);
//   P4 the comfort-vignette mode read -> option turn_vignette (default 0: that pass never ran under the mod).
// Smooth speed: option turn_speed (deg/s, default 150), mapped onto the engine's own speed curve.
namespace turn {
bool install();
// Per frame: the right stick x; `on` = native turning applies now (native mode, no wheel or menu holding the stick).
void feed(float stickX, bool on);
// The local VR player's accumulated anchor yaw (radians, vrd+0x674), 0 without vrd.
float yaw(const void* vrd);
}  // namespace turn
