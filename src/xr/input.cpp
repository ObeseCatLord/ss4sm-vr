#include "xr/input.hpp"
#include "se4/log.hpp"
#include "xr/runtime.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace xr {
namespace {
Input g_input;

XrPath toPath(XrInstance inst, const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(inst, s, &p);
    return p;
}
}  // namespace

Input& input() { return g_input; }

bool Input::create(XrInstance inst, XrSession session) {
    instance_ = inst;
    handPath_[0] = toPath(inst, "/user/hand/left");
    handPath_[1] = toPath(inst, "/user/hand/right");
    XrActionSetCreateInfo sc{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(sc.actionSetName, "gameplay");
    strcpy(sc.localizedActionSetName, "Serious Sam VR gameplay");
    if (XR_FAILED(xrCreateActionSet(inst, &sc, &set_))) {
        VRLOG("input: xrCreateActionSet failed");
        return false;
    }
    auto make = [&](XrAction& a, const char* name, const char* label, XrActionType type) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        snprintf(ci.actionName, sizeof ci.actionName, "%s", name);
        snprintf(ci.localizedActionName, sizeof ci.localizedActionName, "%s", label);
        ci.actionType = type;
        ci.countSubactionPaths = 2;
        ci.subactionPaths = handPath_;
        if (XR_FAILED(xrCreateAction(set_, &ci, &a))) VRLOG("input: action %s failed", name);
    };
    make(aim_, "aim", "Aim pose", XR_ACTION_TYPE_POSE_INPUT);
    make(grip_, "grip", "Hand pose", XR_ACTION_TYPE_POSE_INPUT);
    make(haptic_, "haptic", "Haptic feedback", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    make(trigger_, "trigger", "Fire", XR_ACTION_TYPE_FLOAT_INPUT);
    make(squeeze_, "squeeze", "Grab", XR_ACTION_TYPE_FLOAT_INPUT);
    make(stick_, "stick", "Move / turn", XR_ACTION_TYPE_VECTOR2F_INPUT);
    make(primary_, "primary", "Primary button", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(secondary_, "secondary", "Secondary button", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(stickClick_, "stick_click", "Stick click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(menu_, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(triggerTouch_, "trigger_touch", "Trigger touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(stickTouch_, "stick_touch", "Stick touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(trackpadForce_, "trackpad_force", "Trackpad press (force)", XR_ACTION_TYPE_FLOAT_INPUT);
    make(trackpadClick_, "trackpad_click", "Trackpad press", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(x_, "button_x", "X button", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(y_, "button_y", "Y button", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(bumper_, "bumper", "Bumper", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(dpad_[0], "dpad_up", "D-pad up", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(dpad_[1], "dpad_down", "D-pad down", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(dpad_[2], "dpad_left", "D-pad left", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(dpad_[3], "dpad_right", "D-pad right", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(view_, "view", "View button", XR_ACTION_TYPE_BOOLEAN_INPUT);
    make(squeezeClick_, "squeeze_click", "Grip second stage", XR_ACTION_TYPE_BOOLEAN_INPUT);

    struct Profile {
        const char* name;
        const char* axis;      // thumbstick or trackpad
        const char* squeeze;   // component bound to the float squeeze action
        int kind;              // 0 touch, 1 index, 2 vive, 3 wmr, 4 simple, 5 steam frame
    };
    // Steam Frame: only with its extension. SteamVR before 2.15.1 used the path without the _valve suffix; both are
    // suggested and the one the runtime does not know is rejected.
    const bool frame = runtime().hasExtension(kFrameExtension);
    const Profile profiles[] = {
        {"/interaction_profiles/valve/frame_controller_valve", "thumbstick", "squeeze/value", 5},
        {"/interaction_profiles/valve/frame_controller", "thumbstick", "squeeze/value", 5},
        {"/interaction_profiles/valve/index_controller", "thumbstick", "squeeze/force", 1},
        {"/interaction_profiles/oculus/touch_controller", "thumbstick", "squeeze/value", 0},
        {"/interaction_profiles/htc/vive_controller", "trackpad", "squeeze/click", 2},
        {"/interaction_profiles/microsoft/motion_controller", "thumbstick", "squeeze/click", 3},
        {"/interaction_profiles/khr/simple_controller", nullptr, nullptr, 4},
    };
    int accepted = 0;
    for (const auto& p : profiles) {
        if (p.kind == 5 && !frame) continue;
        std::vector<XrActionSuggestedBinding> b;
        auto bind = [&](XrAction a, int h, const char* comp) {
            std::string s = std::string(h ? "/user/hand/right/" : "/user/hand/left/") + comp;
            b.push_back({a, toPath(inst, s.c_str())});
        };
        for (int h = 0; h < 2; ++h) {
            bind(aim_, h, "input/aim/pose");
            bind(grip_, h, "input/grip/pose");
            bind(haptic_, h, "output/haptic");
            if (p.kind == 4) {
                bind(trigger_, h, "input/select/click");
                bind(menu_, h, "input/menu/click");
                continue;
            }
            bind(trigger_, h, "input/trigger/value");
            bind(squeeze_, h, (std::string("input/") + p.squeeze).c_str());
            bind(stick_, h, (std::string("input/") + p.axis).c_str());
            bind(stickClick_, h, (std::string("input/") + p.axis + "/click").c_str());
            switch (p.kind) {
            case 0:
                bind(primary_, h, h ? "input/a/click" : "input/x/click");
                bind(secondary_, h, h ? "input/b/click" : "input/y/click");
                bind(triggerTouch_, h, "input/trigger/touch");
                bind(stickTouch_, h, "input/thumbstick/touch");
                if (!h) bind(menu_, h, "input/menu/click");
                break;
            case 1:
                bind(primary_, h, "input/a/click");
                bind(secondary_, h, "input/b/click");
                bind(triggerTouch_, h, "input/trigger/touch");
                bind(stickTouch_, h, "input/thumbstick/touch");
                bind(trackpadForce_, h, "input/trackpad/force");
                break;
            case 2:
                bind(menu_, h, "input/menu/click");
                bind(trackpadClick_, h, "input/trackpad/click");
                break;
            case 3:
                bind(menu_, h, "input/menu/click");
                bind(trackpadClick_, h, "input/trackpad/click");
                break;
            case 5:
                if (h) {
                    bind(primary_, h, "input/a/click");
                    bind(secondary_, h, "input/b/click");
                    bind(x_, h, "input/x/click");
                    bind(y_, h, "input/y/click");
                    bind(menu_, h, "input/menu/click");
                } else {
                    bind(dpad_[0], h, "input/dpad_up/click");
                    bind(dpad_[1], h, "input/dpad_down/click");
                    bind(dpad_[2], h, "input/dpad_left/click");
                    bind(dpad_[3], h, "input/dpad_right/click");
                    bind(view_, h, "input/view/click");
                }
                bind(bumper_, h, "input/bumper/click");
                bind(squeezeClick_, h, "input/squeeze/click");
                bind(triggerTouch_, h, "input/trigger/touch");
                bind(stickTouch_, h, "input/thumbstick/touch");
                break;
            }
        }
        XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        s.interactionProfile = toPath(inst, p.name);
        s.countSuggestedBindings = uint32_t(b.size());
        s.suggestedBindings = b.data();
        XrResult r = xrSuggestInteractionProfileBindings(inst, &s);
        if (XR_SUCCEEDED(r)) ++accepted;
        else VRLOG("input: profile %s rejected (%d)", p.name, int(r));
    }
    // Eye gaze: its own profile and top-level path, no subaction paths.
    if (runtime().eyeGazeSupported()) {
        XrActionCreateInfo gc{XR_TYPE_ACTION_CREATE_INFO};
        strcpy(gc.actionName, "eye_gaze");
        strcpy(gc.localizedActionName, "Eye gaze");
        gc.actionType = XR_ACTION_TYPE_POSE_INPUT;
        if (XR_SUCCEEDED(xrCreateAction(set_, &gc, &gazeAction_))) {
            XrActionSuggestedBinding b{gazeAction_, toPath(inst, "/user/eyes_ext/input/gaze_ext/pose")};
            XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            s.interactionProfile = toPath(inst, "/interaction_profiles/ext/eye_gaze_interaction");
            s.countSuggestedBindings = 1;
            s.suggestedBindings = &b;
            XrResult r = xrSuggestInteractionProfileBindings(inst, &s);
            VRLOG("input: eye gaze binding %s (%d)", XR_SUCCEEDED(r) ? "accepted" : "rejected", int(r));
            if (XR_FAILED(r)) gazeAction_ = XR_NULL_HANDLE;
        }
    }
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &set_;
    if (!accepted || XR_FAILED(xrAttachSessionActionSets(session, &at))) {
        VRLOG("input: attach failed (%d profiles accepted)", accepted);
        return false;
    }
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo si{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        si.subactionPath = handPath_[h];
        si.poseInActionSpace.orientation.w = 1;
        si.action = aim_;
        xrCreateActionSpace(session, &si, &aimSpace_[h]);
        si.action = grip_;
        xrCreateActionSpace(session, &si, &gripSpace_[h]);
    }
    if (gazeAction_) {
        XrActionSpaceCreateInfo si{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        si.action = gazeAction_;
        si.poseInActionSpace.orientation.w = 1;
        xrCreateActionSpace(session, &si, &gazeSpace_);
    }
    VRLOG("input: action set attached, %d controller profiles suggested", accepted);
    return true;
}

void Input::sync(XrSession session, XrSpace space, XrTime time) {
    if (!set_) return;
    XrActiveActionSet active{set_, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session, &si))) return;
    for (int h = 0; h < 2; ++h) {
        HandState& s = hands_[h];
        uint32_t before = s.buttons;
        auto info = [&](XrAction a) {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = a;
            gi.subactionPath = handPath_[h];
            return gi;
        };
        auto f = [&](XrAction a) {
            XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
            auto gi = info(a);
            return XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
        };
        auto bl = [&](XrAction a) {
            XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
            auto gi = info(a);
            return XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &st)) && st.isActive && st.currentState;
        };
        s.trigger = f(trigger_);
        s.squeeze = f(squeeze_);
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        auto gi = info(stick_);
        s.stick = XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &v)) && v.isActive ? v.currentState : XrVector2f{0, 0};
        uint32_t b = 0;
        if (bl(primary_)) b |= BtnPrimary;
        if (bl(secondary_)) b |= BtnSecondary;
        if (bl(stickClick_)) b |= BtnStickClick;
        if (bl(menu_)) b |= BtnMenu;
        if (bl(triggerTouch_)) b |= BtnTriggerTouch;
        if (bl(stickTouch_)) b |= BtnStickTouch;
        if (f(trackpadForce_) > 0.6f || bl(trackpadClick_)) b |= BtnTrackpadPress;
        if (s.kind == ControllerKind::Frame) {
            if (bl(x_)) b |= BtnX;
            if (bl(y_)) b |= BtnY;
            if (bl(bumper_)) b |= BtnBumper;
            for (int d = 0; d < 4; ++d)
                if (bl(dpad_[d])) b |= BtnDpadUp << d;
            if (bl(view_)) b |= BtnView;
            if (bl(squeezeClick_)) b |= BtnSqueezeClick;
        }
        s.buttons = b;
        s.pressed = b & ~before;
        s.released = before & ~b;

        XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        loc.next = &vel;
        constexpr XrSpaceLocationFlags valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        bool gripOk = XR_SUCCEEDED(xrLocateSpace(gripSpace_[h], space, time, &loc)) && (loc.locationFlags & valid) == valid;
        if (gripOk) {
            s.grip = loc.pose;
            s.velocityValid = (vel.velocityFlags & (XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)) ==
                              (XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT);
            s.gripLinearVelocity = vel.linearVelocity;
            s.gripAngularVelocity = vel.angularVelocity;
        }
        XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
        bool aimOk = XR_SUCCEEDED(xrLocateSpace(aimSpace_[h], space, time, &aim)) && (aim.locationFlags & valid) == valid;
        if (aimOk) s.aim = aim.pose;
        s.active = gripOk && aimOk;

        XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session, handPath_[h], &ps)) && ps.interactionProfile != s.profile) {
            s.profile = ps.interactionProfile;
            const char* name = profileName(instance_, h);
            s.kind = strstr(name, "oculus/touch") ? ControllerKind::Touch
                     : strstr(name, "valve/index") ? ControllerKind::Index
                     : strstr(name, "htc/vive")    ? ControllerKind::Vive
                     : strstr(name, "microsoft/motion") ? ControllerKind::WMR
                     : strstr(name, "khr/simple")  ? ControllerKind::Simple
                     : strstr(name, "valve/frame_controller") ? ControllerKind::Frame
                                                   : ControllerKind::Unknown;
            VRLOG("input: %s hand profile %s", h ? "right" : "left", name);
        }
    }
}

void Input::vibrate(XrSession session, int h, float amplitude, float seconds, float frequency) {
    if (!set_) return;
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amplitude;
    v.duration = XrDuration(seconds * 1e9f);
    v.frequency = frequency > 0 ? frequency : XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = haptic_;
    hi.subactionPath = handPath_[h];
    xrApplyHapticFeedback(session, &hi, reinterpret_cast<XrHapticBaseHeader*>(&v));
}

void Input::locateGaze(XrSpace viewSpace, XrTime time) {
    gazeValid_ = false;
    if (!gazeSpace_) return;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    XrEyeGazeSampleTimeEXT st{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
    loc.next = &st;
    if (XR_FAILED(xrLocateSpace(gazeSpace_, viewSpace, time, &loc))) return;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT))
        return;
    gaze_ = loc.pose;
    gazeValid_ = true;
}

const char* Input::profileName(XrInstance inst, int h) const {
    static char buf[2][XR_MAX_PATH_LENGTH];
    uint32_t n = 0;
    if (hands_[h].profile == XR_NULL_PATH || XR_FAILED(xrPathToString(inst, hands_[h].profile, XR_MAX_PATH_LENGTH, &n, buf[h])))
        return "none";
    return buf[h];
}

}  // namespace xr
