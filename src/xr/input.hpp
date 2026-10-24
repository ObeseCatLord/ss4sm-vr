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
    // Steam Frame controllers (XR_VALVE_frame_controller_interaction): ABXY on the right, D-pad and View on the
    // left, a bumper on each, and the grip's second stage (squeeze/click).
    BtnX = 1u << 7,
    BtnY = 1u << 8,
    BtnBumper = 1u << 9,
    BtnDpadUp = 1u << 10,
    BtnDpadDown = 1u << 11,
    BtnDpadLeft = 1u << 12,
    BtnDpadRight = 1u << 13,
    BtnView = 1u << 14,
    BtnSqueezeClick = 1u << 15,
};

enum class ControllerKind { Touch, Index, Vive, WMR, Simple, Frame, Unknown };

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
    ControllerKind kind = ControllerKind::Unknown;  // from the current interaction profile
};

class Input {
public:
    // Create the action set and suggest bindings (before attaching); attach to the session.
    bool create(XrInstance instance, XrSession session);
    // Sync once per frame (after xrWaitFrame) and locate poses at `time` in `space`.
    void sync(XrSession session, XrSpace space, XrTime time);
    // Locate the gaze in `viewSpace` (call after sync, with the same time).
    void locateGaze(XrSpace viewSpace, XrTime time);
    const HandState& hand(int h) const { return hands_[h]; }
    // amplitude 0..1, duration in seconds, frequency in Hz (0 = runtime default)
    void vibrate(XrSession session, int h, float amplitude, float seconds, float frequency = 0);
    const char* profileName(XrInstance instance, int h) const;
    bool ready() const { return set_ != XR_NULL_HANDLE; }
    // Eye gaze (XR_EXT_eye_gaze_interaction), in VIEW space: valid only when the runtime tracks the eyes.
    bool gaze(XrPosef& inView) const {
        if (!gazeValid_) return false;
        inView = gaze_;
        return true;
    }

private:
    XrActionSet set_ = XR_NULL_HANDLE;
    XrPath handPath_[2]{};
    XrAction aim_{}, grip_{}, haptic_{}, trigger_{}, squeeze_{}, stick_{}, primary_{}, secondary_{}, stickClick_{}, menu_{},
        triggerTouch_{}, stickTouch_{}, trackpadForce_{}, trackpadClick_{}, x_{}, y_{}, bumper_{}, dpad_[4]{}, view_{}, squeezeClick_{};
    XrSpace aimSpace_[2]{}, gripSpace_[2]{};
    XrAction gazeAction_{};
    XrSpace gazeSpace_{};
    XrPosef gaze_{};
    bool gazeValid_ = false;
    HandState hands_[2];
    XrInstance instance_ = XR_NULL_HANDLE;
};

Input& input();

}  // namespace xr
