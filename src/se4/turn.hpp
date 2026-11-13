#pragma once
// Native VR turning (docs/re/R26_vr_turning.md). The engine's VR rotation (0x14044e880 -> anchor turn) reads the stick
// from vrd's per-hand input table, which is only refreshed at spawn, so it never turned. Three in-memory read
// redirects make it work without writing any cvar (they are saved with the user's settings):
//   P1 the rotation method read -> the mod's value (1 smooth, 2 snap: VR Controls > Rotation Method, option snap_turn);
//   P2 the rotation controller read -> 2 (the engine's right-hand slot, which P3 feeds);
//   P3 the right stick load -> the mod's axis from the chosen stick (VR Controls > Rotation Controller), fed every frame
//      while native turning is on (else 0, so nothing turns);
//   P4 the comfort-vignette mode read -> VR Controls > Comfort mode / option turn_vignette (default 0).
// Smooth speed: VR Controls > Rotation Sensitivity / option turn_speed (deg/s, default 150), mapped onto the engine's own
// speed curve.
namespace turn {
bool install();
bool installed();  // every read redirect in place: the engine's VR rotation is driven by feed()
// Per frame: both sticks' x (the Rotation Controller picks one); `on` = native turning applies now (native mode, no wheel
// or menu holding the stick).
void feed(float leftX, float rightX, bool on);
// Settings (the VR Controls menu, src/se4/gfx_menu.cpp; options.txt snap_turn / turn_speed / turn_vignette win at start).
void setStart(int method, int hand, int speed, int vignette);  // before install(); -1 = unset
void setSnap(bool snap);
void setHand(int hand);  // 0 none, 1 left stick, 2 right stick, 3 either
void setSpeed(float degPerSec);
void setVignette(bool on);
bool snap();
int hand();
float speed();
bool vignette();
// The local VR player's accumulated anchor yaw (radians, vrd+0x674), 0 without vrd.
float yaw(const void* vrd);
}  // namespace turn
