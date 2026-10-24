#pragma once
// Finger poses for the VR hands (docs/re/R15_hands.md s6). Engine-agnostic: both sources, OpenXR hand joints and
// controller state, reduce to per-joint flexion angles (thumb, index, middle, ring, pinky; joints _01.._03), which
// the hands module applies to the arms skeleton about axes derived from its rest pose.
#include "core/math.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace core {
enum Finger { Thumb, Index, Middle, Ring, Pinky, FingerCount };

struct FingerAngles {
    float a[FingerCount][3] = {};  // radians, positive = flex toward the palm
    float thumbAbduction = 0;      // radians, positive = away from the index finger
};

// Controller state (s6.2). touch* are capacitive touches; squeeze is the grip curl (Index: squeeze/value).
struct ControllerFingers {
    float trigger = 0;       // 0..1
    bool triggerTouch = false;
    float squeeze = 0;       // 0..1
    bool thumbRest = false;  // stick, trackpad or face-button touch
};

// Curls 0..1 per finger from controller state: index from the trigger (resting on it = 0.25), the other three from
// the squeeze, the thumb resting on the controller top (0.7) or lifted (0.1).
inline void curlsFromController(const ControllerFingers& c, float curl[FingerCount]) {
    const float trig = std::clamp(c.trigger, 0.0f, 1.0f);
    curl[Index] = trig > 0.01f ? 0.25f + 0.75f * trig : (c.triggerTouch ? 0.25f : 0.0f);
    const float sq = std::clamp(c.squeeze, 0.0f, 1.0f);
    curl[Middle] = curl[Ring] = curl[Pinky] = sq;
    curl[Thumb] = c.thumbRest ? 0.7f : 0.1f;
}

// Maximum flexion per joint for curl 1 (a loose fist).
constexpr float kMaxFlex[FingerCount][3] = {
    {0.52f, 0.70f, 0.79f},  // thumb 30, 40, 45 degrees
    {1.05f, 1.57f, 1.05f},  // 60, 90, 60
    {1.05f, 1.57f, 1.05f},
    {1.05f, 1.57f, 1.05f},
    {1.05f, 1.57f, 1.05f},
};

inline FingerAngles anglesFromCurls(const float curl[FingerCount]) {
    FingerAngles r;
    for (int f = 0; f < FingerCount; ++f)
        for (int j = 0; j < 3; ++j) r.a[f][j] = std::clamp(curl[f], 0.0f, 1.0f) * kMaxFlex[f][j];
    r.thumbAbduction = 0.35f * (1.0f - std::clamp(curl[Thumb], 0.0f, 1.0f));
    return r;
}

// OpenXR hand joints (XR_EXT_hand_tracking, 26 joints) in any common space: per joint orientation; +Z points back
// along the bone (away from the tip), +Y out of the back of the finger, +X across. valid[i] says the orientation is
// usable. Flexion of a joint = signed angle between consecutive bone directions; positive when the child bone turns
// toward the palm (-Y), i.e. about the parent's -X.
inline bool anglesFromJoints(const Quat joint[26], const bool valid[26], FingerAngles& out) {
    // first joint index per finger: thumb metacarpal 2, index metacarpal 6, middle 11, ring 16, little 21
    static const int first[FingerCount] = {2, 6, 11, 16, 21};
    bool ok = true;
    for (int f = 0; f < FingerCount; ++f) {
        // thumb: metacarpal->proximal->distal->tip (2..5); others skip the metacarpal: proximal (7) onwards
        const int base = f == Thumb ? first[f] : first[f] + 1;
        for (int j = 0; j < 3; ++j) {
            const int a = base + j - (f == Thumb ? 0 : 1), b = a + 1;
            if (!valid[a] || !valid[b]) {
                ok = false;
                continue;
            }
            const Vec3 da = rotate(joint[a], {0, 0, -1}), db = rotate(joint[b], {0, 0, -1});
            const Vec3 axis = rotate(joint[a], {-1, 0, 0});
            const float s = dot(cross(da, db), axis), c = dot(da, db);
            out.a[f][j] = std::atan2(s, c);
        }
    }
    if (valid[2] && valid[6]) {
        const Vec3 thumb = rotate(joint[2], {0, 0, -1}), index = rotate(joint[6], {0, 0, -1});
        out.thumbAbduction = std::acos(std::clamp(dot(thumb, index), -1.0f, 1.0f));
    }
    return ok;
}

// Critically damped smoothing toward the target (time constant tau seconds).
inline void smooth(FingerAngles& cur, const FingerAngles& target, float dt, float tau = 0.06f) {
    const float k = 1.0f - std::exp(-std::max(dt, 0.0f) / tau);
    for (int f = 0; f < FingerCount; ++f)
        for (int j = 0; j < 3; ++j) cur.a[f][j] += (target.a[f][j] - cur.a[f][j]) * k;
    cur.thumbAbduction += (target.thumbAbduction - cur.thumbAbduction) * k;
}

// Network form (wire v2): 0..255 fractions of kMaxFlex for joint _02 (the others follow the curl model).
inline uint8_t packCurl(const FingerAngles& a, int f) {
    return uint8_t(std::lround(std::clamp(a.a[f][1] / kMaxFlex[f][1], 0.0f, 1.0f) * 255.0f));
}
inline float unpackCurl(uint8_t v) { return float(v) / 255.0f; }
}  // namespace core
