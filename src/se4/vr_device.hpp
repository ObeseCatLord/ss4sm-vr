#pragma once
// COpenXRDevice: replacement for the engine's stripped VR device (CVRDevice), installed in the engine's
// device global so its surviving VR consumers (camera, CPlayerVRData, controllers, haptics, stereo
// renderer) run on OpenXR data. Slot table and object layout: docs/re/R1_vtable.md.
namespace vrdev {
// Install before the graphics backend initialises (it queries the device during init). Requires an
// initialised XR runtime. Returns false and leaves the global untouched otherwise.
bool install();
bool installed();
// Riding: the head and controllers given to the engine are shifted vertically by this many metres (play space).
void setSeatOffset(float metres);
float seatOffset();
// A menu screen (in-game menu, Netricsa, dialogs) is on top: the controllers use the menu layout.
bool menuActive();
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
// Size of the eye render targets slot 0x028 creates: slot 0x060 x vr_fRenderingPixelRatio.
void eyeTargetSize(int& w, int& h);
// The eye whose render target `dp` is (the drawport slot 0x028 returned), or 0. Single pass renders the HUD with
// the device's eye field at 0; this tells the eye from the drawport.
int eyeOfDrawport(const void* dp);
// HUD render (main thread): the eye being drawn - the device's eye field, else the eye of the drawport the HUD element
// draws into (set by hud_vr per element; single pass).
void setHudDrawport(const void* dp);
int hudEye();
// Eye-in-head pose of eye 1/2 (quaternion xyzw + position in engine units), as slot 0x078 inverts it. False when
// the views are not valid.
bool eyeInHead(int eye, float out[7]);
// Tangents of eye 1/2's projection as slot 0x070 builds it (left/right/up/down angles in radians, no jitter).
bool eyeFov(int eye, float& left, float& right, float& up, float& down);
// Sends the main loop's desktop Render to RenderVR while the device is installed (R10).
bool hookRender();
// Periodic diagnostics: per-slot call counts since the last report.
void logStats();
}  // namespace vrdev
