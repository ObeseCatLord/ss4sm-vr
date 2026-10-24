#pragma once
// COpenXRDevice: replacement for the engine's stripped VR device (CVRDevice), installed in the engine's
// device global so its surviving VR consumers (camera, CPlayerVRData, controllers, haptics, stereo
// renderer) run on OpenXR data. Slot table and object layout: docs/re/R1_vtable.md.
namespace vrdev {
// Install before the graphics backend initialises (it queries the device during init). Requires an
// initialised XR runtime. Returns false and leaves the global untouched otherwise.
bool install();
bool installed();
// Once per frame after xr::input().sync(): refresh the engine button table (slot 0x0d0).
void updateFromInput();
// Hooks the engine's device shutdown so it never frees our statically allocated object.
bool hookShutdown();
// Makes sure engine objects the device presence implies exist before they are used (viewport VR viewer).
bool hookViewport();
// Device-local head placement (quaternion xyzw + position in engine units), as slot 0x080 returns it.
void headPlacement(float out[7]);
// The eye the engine is rendering now (device +0x18: 1 left, 2 right, 3 scope, 0 none).
int currentEye();
// Eye-in-head pose of eye 1/2 (quaternion xyzw + position in engine units), as slot 0x078 inverts it. False when
// the views are not valid.
bool eyeInHead(int eye, float out[7]);
// Sends the main loop's desktop Render to RenderVR while the device is installed (R10).
bool hookRender();
// Periodic diagnostics: per-slot call counts since the last report.
void logStats();
}  // namespace vrdev
