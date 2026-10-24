#pragma once
// Picture-in-picture scopes (docs/re/R14_scopes.md s6). The scoped weapons (SS4 Sniper and Devastator, SM Crossbow:
// secondary mode CSnipingWeaponMode) show a magnified view through the scope instead of the native full-screen
// mask, which the engine already skips for VR owners. The native zoom state, damage and sounds stay untouched.
//   - The engine renders the scope camera as its third view ("eye 3", the mixed-reality camera of Fusion): the
//     device raises vr_iMixedRealityOutput in slot 0x030 and lowers it in slot 0x038, and answers slots
//     0x028/0x070/0x078 for eye 3 with the scope target, projection and camera. One extra view per frame, only
//     while a scope is live.
//   - Camera: the weapon's barrel placement (the shot origin; the reticle centre is the point of impact), with the
//     desktop scoped FOV (player base FOV / swmp_fZoomFactor). Weapons are not drawn in eye 3.
//   - Presentation: the image goes to an OpenXR quad layer at the eyepiece, circular, in the weapon's hand.
// Options (ss4smvr/options.txt): scope (0 off, 1 when the eyepiece is near an eye or natively zoomed [default],
// 2 whenever a scoped weapon is held), scope_res (pixels, default 768), scope_size_cm (default 4),
// scope_eye_up_cm / scope_eye_back_cm (eyepiece above / behind the grip, default 9 / 3), scope_reticle (default 1).
#include <openxr/openxr.h>

namespace scope {
bool install();

// Device slots (main thread).
void beginFrame();                                  // slot 0x030
void endFrame();                                    // slot 0x038
void* target(void* gfx);                            // slot 0x028, eye 3
void projection(float* m, float nearZ, float farZ); // slot 0x070, eye 3
void eyeTransform(float* m);                        // slot 0x078, eye 3

// For the composition layer (present thread): pose in the XR play space and side length in metres.
struct Layer {
    bool visible = false;
    XrPosef pose{};
    float size = 0.04f;
    bool reticle = true;
};
// handGrip: the scope hand's grip pose in the play space.
Layer layer(const XrPosef& rightGrip, const XrPosef& leftGrip);
}  // namespace scope
