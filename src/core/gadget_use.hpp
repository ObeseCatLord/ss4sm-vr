#pragma once
// How a gadget in the hand goes off (src/se4/gadgets.cpp). Not an immersive physics throw: as with the SS2 VR
// grenades, the hand holding the gadget sets it off with a gesture that fits the gadget, and for a throwable
// the other hand aims (the native throw arc and target marker follow that hand).
//   Throw    aimed throwables (bot usage FIRE_AT_FOE_*: BlackHole, NerveGas, Decoy, MiniNuke, SM ArtilleryCall,
//            Hoverboard): a throwing motion of the gadget hand; the other hand aims.
//   Toss     CompanionDrone, SeriousTime (their animations throw): a throwing motion, no aim.
//   Inject   MedKit (its animation is "UseNeedle"): the needle onto the other forearm, or into the body (torso
//            down to the upper thighs) while it points into it, so a hand resting at the side does not inject.
//   Drink    SeriousRage ("Drink"): the gadget is brought to the mouth.
//   Chest    ExtraLife ("Spasm"): the gadget is pressed to the chest.
//   Trigger  anything else (unknown USE_IMMEDIATELY gadgets): a trigger pull.
// Poses are in the XR play space (metres, +Y up, a head's -Z is forward), times in milliseconds.
#include "core/grenade_throw.hpp"
#include "core/math.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>

namespace core::gadget {
enum class Use : uint8_t { Throw, Toss, Inject, Drink, Chest, Trigger };

inline const char* name(Use u) {
    switch (u) {
    case Use::Throw: return "aimed throw";
    case Use::Toss: return "toss";
    case Use::Inject: return "inject (arm or body)";
    case Use::Drink: return "drink";
    case Use::Chest: return "press to the chest";
    default: return "trigger";
    }
}

// `key`: the item's name key (gip_tsName, e.g. "MedKitGadgetItem"), may be null. `botUsage`:
// gip_bgutBotUsageType (0 not usable, 1 use immediately, 2..4 fire at foes).
inline Use classify(const char* key, int botUsage) {
    if (key) {
        if (std::strstr(key, "MedKit")) return Use::Inject;
        if (std::strstr(key, "SeriousRage")) return Use::Drink;
        if (std::strstr(key, "ExtraLife")) return Use::Chest;
        if (std::strstr(key, "CompanionDrone") || std::strstr(key, "SeriousTime")) return Use::Toss;
    }
    return botUsage >= 2 && botUsage <= 4 ? Use::Throw : Use::Trigger;
}

// Body points from the head pose.
inline Vec3 mouth(const Pose& head) { return head.p + rotate(head.q, {0, -0.09f, -0.07f}); }
inline Vec3 chest(const Pose& head) {
    const Vec3 f = rotate(head.q, {0, 0, -1});
    const float n = std::sqrt(f.x * f.x + f.z * f.z);
    const Vec3 flat = n > 1e-3f ? Vec3{f.x / n, 0, f.z / n} : Vec3{0, 0, -1};
    return Vec3{head.p.x, head.p.y - 0.32f, head.p.z} + flat * 0.10f;
}
// The point of the held gadget that touches (a needle, a bottle neck): just ahead of the fist.
inline Vec3 tip(const Pose& grip) { return grip.p + rotate(grip.q, {0, 0, -0.06f}); }
// Closest point to p on the segment a..b.
inline Vec3 closestOnSegment(Vec3 a, Vec3 b, Vec3 p) {
    const Vec3 ab = b - a;
    float t = dot(p - a, ab) / dot(ab, ab);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return a + ab * t;
}
// Distance from p to the other arm's forearm: from the wrist (behind the grip) towards the elbow.
inline float forearmDistance(const Pose& otherGrip, Vec3 p) {
    const Vec3 d = p - closestOnSegment(otherGrip.p + rotate(otherGrip.q, {0, 0, 0.05f}), otherGrip.p + rotate(otherGrip.q, {0, 0, 0.30f}), p);
    return std::sqrt(dot(d, d));
}
// The body's axis from the head pose (no body tracking): below the neck to the upper thighs, a little behind
// the eyes, upright.
inline void bodyAxis(const Pose& head, Vec3& top, Vec3& bottom) {
    const Vec3 f = rotate(head.q, {0, 0, -1});
    const float n = std::sqrt(f.x * f.x + f.z * f.z);
    const Vec3 back = n > 1e-3f ? Vec3{-f.x / n, 0, -f.z / n} * 0.05f : Vec3{0, 0, 0.05f};
    top = Vec3{head.p.x, head.p.y - 0.22f, head.p.z} + back;
    bottom = Vec3{head.p.x, head.p.y - 1.05f, head.p.z} + back;
}
// Distance from the needle tip to the body axis, and whether the needle (grip -Z) points into the body.
inline float bodyDistance(const Pose& head, const Pose& grip, bool& pointsIn) {
    Vec3 a, b;
    bodyAxis(head, a, b);
    const Vec3 p = tip(grip), in = closestOnSegment(a, b, p) - p;
    const float d = std::sqrt(dot(in, in));
    pointsIn = d > 1e-4f && dot(rotate(grip.q, {0, 0, -1}), in) > 0.5f * d;  // within 60 degrees of the axis
    return d;
}

constexpr float kForearmRadius = 0.09f, kBodyRadius = 0.17f, kMouthRadius = 0.13f, kChestRadius = 0.17f;
constexpr uint64_t kDwell = 100;  // ms in the zone before it goes off (a hand passing through does not)
constexpr float kTriggerOn = 0.6f, kTriggerOff = 0.3f;

struct Frame {
    bool handValid = false, otherValid = false, headValid = false;
    Pose hand, other, head;  // gadget hand grip, other hand grip, head
    float trigger = 0;       // gadget hand trigger
};

// One gadget in the hand, from bring-up until it goes off. A zone must be left once before it can fire (the
// gadget may come up with the hand already there), the trigger must be released once.
class Activation {
public:
    void begin(Use u) {
        use_ = u, fired_ = false, armed_ = false, since_ = 0;
        throw_.clear();
        if (u == Use::Throw || u == Use::Toss) throw_.press(0);
    }
    Use use() const { return use_; }
    bool fired() const { return fired_; }
    bool armed() const { return armed_; }
    float distance() const { return dist_; }  // last distance to the zone (m), for logs
    // True on the update the gadget should go off.
    bool update(const Frame& f, uint64_t now) {
        if (fired_) return false;
        switch (use_) {
        case Use::Throw:
        case Use::Toss: {
            if (!f.headValid) return false;
            const Vec3 fwd = rotate(f.head.q, {0, 0, -1});
            if (throw_.update(f.handValid, f.hand.p, now, fwd)) return fired_ = true;
            if (throw_.state == GrenadeThrow::State::Idle) throw_.press(0);  // a launch timed out: hold again
            return false;
        }
        case Use::Trigger:
            if (f.trigger < kTriggerOff) armed_ = true;
            return armed_ && f.trigger > kTriggerOn ? (fired_ = true) : false;
        default: break;
        }
        // in: in a zone now; clear: well away from every zone (a zone must be left once after bring-up).
        bool in = false, clear = f.handValid;
        float d = 1e9f;
        if (f.handValid) {
            const Vec3 p = tip(f.hand);
            if (use_ == Use::Inject) {
                if (f.otherValid) {
                    const float da = forearmDistance(f.other, p);
                    in |= da <= kForearmRadius, clear &= da > kForearmRadius * 1.5f, d = da;
                }
                if (f.headValid) {
                    bool pointsIn = false;
                    const float db = bodyDistance(f.head, f.hand, pointsIn);
                    in |= db <= kBodyRadius && pointsIn, clear &= db > kBodyRadius * 1.5f || !pointsIn;
                    d = std::fmin(d, db - kBodyRadius + kForearmRadius);  // log: as if both had the forearm's radius
                }
            } else if (f.headValid) {
                const Vec3 dz = p - (use_ == Use::Drink ? mouth(f.head) : chest(f.head));
                const float r = use_ == Use::Drink ? kMouthRadius : kChestRadius;
                d = std::sqrt(dot(dz, dz)), in = d <= r, clear = d > r * 1.5f;
            }
        }
        dist_ = d;
        if (clear) armed_ = true;
        if (!in) since_ = 0;
        if (!armed_ || !in) return false;
        if (!since_) since_ = now ? now : 1;
        return now - since_ >= kDwell ? (fired_ = true) : false;
    }

private:
    Use use_ = Use::Trigger;
    bool fired_ = false, armed_ = false;
    float dist_ = 0;
    uint64_t since_ = 0;
    GrenadeThrow throw_;
};
}  // namespace core::gadget
