#pragma once
// Engine-agnostic OpenXR controller input: one action set, per-hand poses (with velocities for melee and
// throwing), trigger/squeeze/stick and buttons, and haptics. Game code maps HandState onto native commands.
#include <openxr/openxr.h>
#include <cstdint>

namespace xr {

enum Button : uint32_t {
    BtnPrimary = 1u << 0,    // A (right) / X (left); Index A; Vive/WMR menu
    BtnSecondary = 1u << 1,  // B / Y; Index B
    BtnStickClick = 1u << 2,
    BtnMenu = 1u << 3,
    BtnTriggerTouch = 1u << 4,
    BtnStickTouch = 1u << 5,
    BtnTrackpadPress = 1u << 6,  // Index trackpad force / Vive+WMR trackpad click
};

struct HandState {
    bool active = false;          // controller present and pose valid
    XrPosef aim{}, grip{};        // in the runtime's LOCAL space
    XrVector3f gripLinearVelocity{}, gripAngularVelocity{};
    bool velocityValid = false;
    float trigger = 0, squeeze = 0;  // 0..1 (Index: squeeze force, the deliberate grab)
    XrVector2f stick{};
    uint32_t buttons = 0;
    uint32_t pressed = 0, released = 0;  // edges since the previous sync
    XrPath profile = XR_NULL_PATH;
};

class Input {
public:
    // Create the action set and suggest bindings (before attaching); attach to the session.
    bool create(XrInstance instance, XrSession session);
    // Sync once per frame (after xrWaitFrame) and locate poses at `time` in `space`.
    void sync(XrSession session, XrSpace space, XrTime time);
    const HandState& hand(int h) const { return hands_[h]; }
    // amplitude 0..1, duration in seconds, frequency in Hz (0 = runtime default)
    void vibrate(XrSession session, int h, float amplitude, float seconds, float frequency = 0);
    const char* profileName(XrInstance instance, int h) const;
    bool ready() const { return set_ != XR_NULL_HANDLE; }

private:
    XrActionSet set_ = XR_NULL_HANDLE;
    XrPath handPath_[2]{};
    XrAction aim_{}, grip_{}, haptic_{}, trigger_{}, squeeze_{}, stick_{}, primary_{}, secondary_{}, stickClick_{}, menu_{},
        triggerTouch_{}, stickTouch_{}, trackpadForce_{}, trackpadClick_{};
    XrSpace aimSpace_[2]{}, gripSpace_[2]{};
    HandState hands_[2];
    XrInstance instance_ = XR_NULL_HANDLE;
};

Input& input();

}  // namespace xr
