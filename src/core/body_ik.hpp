#pragma once
// Full-body IK from head + two hands (local player body and remote players). Original implementation;
// the approach (yaw deadzone body, pelvis under head, analytic two-bone limbs with pole vectors, limited
// arm stretch) follows the eduke32_vr design notes. Units: metres, play space (y up, -Z forward).
#include "core/math.hpp"

namespace core {

struct BodyDims {
    float height = 1.80f;          // standing eye height ~ 0.94 * height
    float neck = 0.20f;            // eye -> neck base (C7), down/back
    float spine = 0.50f;           // neck base -> hip joints
    float shoulderHalf = 0.19f;    // half shoulder width
    float upperArm = 0.30f, foreArm = 0.28f;
    float hipHalf = 0.10f, thigh = 0.48f, shin = 0.47f;
    float maxArmStretch = 1.6f;    // limbs may stretch to reach tracked hands (eduke32_vr: 1.6x)
};

struct BodyState {
    float yaw = 0;          // body heading (radians, 0 = facing -Z)
    bool seeded = false;
};

struct Limb {
    Vec3 root, joint, end;  // shoulder/hip, elbow/knee, wrist/ankle
    bool stretched = false;
};

struct BodyPose {
    Vec3 head, neck, pelvis;
    Quat torso;             // pelvis orientation (yaw only)
    Limb arm[2];            // 0 left, 1 right
    Limb leg[2];
};

// Analytic two-bone IK: root fixed, end as close to `target` as limb lengths allow (optionally stretched
// up to `maxStretch`), bending towards `pole`.
inline Limb twoBone(Vec3 root, Vec3 target, Vec3 pole, float a, float b, float maxStretch = 1.0f) {
    Limb l{root, root, target, false};
    Vec3 d = target - root;
    float len = std::sqrt(dot(d, d));
    if (!(len > 1e-5f)) {
        l.joint = root + Vec3{0, -a, 0};
        l.end = l.joint + Vec3{0, -b, 0};
        return l;
    }
    Vec3 dir = d * (1 / len);
    float reach = a + b;
    float scale = 1;
    if (len > reach) {
        scale = std::fmin(len / reach, maxStretch);
        l.stretched = scale > 1.0001f;
        a *= scale, b *= scale;
        reach = a + b;
        if (len > reach) {  // still out of reach: straight limb pointing at the target
            l.joint = root + dir * a;
            l.end = root + dir * reach;
            return l;
        }
    }
    float minLen = std::fabs(a - b) + 1e-4f;
    if (len < minLen) len = minLen;
    // law of cosines: distance from root to the joint's projection on the root->target line
    float x = (a * a - b * b + len * len) / (2 * len);
    float h = std::sqrt(std::fmax(a * a - x * x, 0.0f));
    Vec3 pv = pole - root;
    Vec3 bendDir = pv - dir * dot(pv, dir);
    float bl = std::sqrt(dot(bendDir, bendDir));
    if (bl < 1e-5f) {  // pole on the line: pick any perpendicular
        bendDir = std::fabs(dir.y) < 0.9f ? cross(dir, Vec3{0, 1, 0}) : cross(dir, Vec3{1, 0, 0});
        bl = std::sqrt(dot(bendDir, bendDir));
    }
    bendDir = bendDir * (1 / bl);
    l.joint = root + dir * x + bendDir * h;
    l.end = root + dir * len;
    return l;
}

// Body heading follows the head with a deadzone; while moving it catches up fully.
inline float updateBodyYaw(BodyState& s, Quat head, bool moving, float dt, float deadzone = 0.785f /*45 deg*/,
                           float catchUp = 4.0f) {
    Vec3 f = rotate(head, {0, 0, -1});
    float headYaw = (f.x * f.x + f.z * f.z > 1e-4f) ? std::atan2(-f.x, -f.z) : s.yaw;
    if (!s.seeded) {
        s.yaw = headYaw;
        s.seeded = true;
        return s.yaw;
    }
    float diff = std::remainder(headYaw - s.yaw, 2 * Pi);
    float limit = moving ? 0.0f : deadzone;
    if (std::fabs(diff) > limit) {
        float excess = diff - std::copysign(limit, diff);
        float stepMax = catchUp * dt * (moving ? 2.0f : 1.0f);
        float step = std::fabs(excess) < stepMax || !moving ? excess : std::copysign(stepMax, excess);
        if (!moving) step = excess;  // outside the deadzone the torso turns with the head
        s.yaw = std::remainder(s.yaw + step, 2 * Pi);
    }
    return s.yaw;
}

// Solve the body. `hands[i]` are grip positions (0 left, 1 right) and `handValid[i]` their tracking state.
// Feet are planted under the hips on the floor (y = floor); stride animation is left to the caller.
inline BodyPose solveBody(const BodyDims& d, BodyState& s, Pose head, const Vec3 hands[2], const bool handValid[2],
                          bool moving, float dt, float floorY = 0) {
    BodyPose b{};
    float yawAngle = updateBodyYaw(s, head.q, moving, dt);
    Quat torso = yaw(yawAngle);
    b.torso = torso;
    b.head = head.p;
    Vec3 back = rotate(torso, {0, 0, 1});
    b.neck = head.p + Vec3{0, -d.neck, 0} + back * 0.06f;
    // Pelvis straight below the neck (crouching shortens the leg chain, the spine stays rigid).
    b.pelvis = b.neck + Vec3{0, -d.spine, 0};
    // Feet stay on the floor: the pelvis never rises above full leg length (the spine absorbs the rest).
    float maxPelvis = floorY + (d.thigh + d.shin) * 0.99f;
    if (b.pelvis.y > maxPelvis) b.pelvis.y = maxPelvis;
    Vec3 right = rotate(torso, {1, 0, 0});
    for (int i = 0; i < 2; ++i) {
        float side = i ? 1.0f : -1.0f;
        Vec3 shoulder = b.neck + right * (side * d.shoulderHalf) + Vec3{0, -0.04f, 0};
        Vec3 target = handValid[i] ? hands[i] : shoulder + Vec3{0, -(d.upperArm + d.foreArm) * 0.95f, 0} + right * (side * 0.05f);
        // elbows point down and outward, slightly back
        Vec3 pole = shoulder + Vec3{0, -0.5f, 0} + right * (side * 0.35f) + back * 0.25f;
        b.arm[i] = twoBone(shoulder, target, pole, d.upperArm, d.foreArm, d.maxArmStretch);
        Vec3 hip = b.pelvis + right * (side * d.hipHalf);
        Vec3 foot{hip.x, floorY, hip.z};
        Vec3 knee = hip + rotate(torso, {0, -0.3f, -0.6f});  // knees forward
        b.leg[i] = twoBone(hip, foot, knee, d.thigh, d.shin, 1.0f);
    }
    return b;
}

// ---- Retargeting onto an animated skeleton (engine motion handler, parent-relative bone transforms) ----

// Shortest rotation taking direction a to direction b.
inline Quat fromTo(Vec3 a, Vec3 b) {
    float la = std::sqrt(dot(a, a)), lb = std::sqrt(dot(b, b));
    if (la < 1e-6f || lb < 1e-6f) return {};
    a = a * (1 / la), b = b * (1 / lb);
    float d = dot(a, b);
    if (d < -0.99999f) {  // opposite: any perpendicular axis
        Vec3 ax = std::fabs(a.x) < 0.9f ? cross(a, Vec3{1, 0, 0}) : cross(a, Vec3{0, 1, 0});
        float l = std::sqrt(dot(ax, ax));
        return {ax.x / l, ax.y / l, ax.z / l, 0};
    }
    Vec3 c = cross(a, b);
    return normalize(Quat{c.x, c.y, c.z, 1 + d});
}

// One arm of an animated skeleton: local (parent-relative) transforms of the upper arm, forearm, hand and the
// weapon bone under the hand, plus the model-space transform of the upper arm's parent (the shoulder).
struct ArmLocals {
    Pose parent;  // model space
    Pose arm, fore, hand, weapon;  // local
};

// Re-aims the arm so the weapon bone lands on `weaponTarget` (model space, position and orientation): two-bone
// IK for the shoulder->elbow->wrist chain bending towards `pole`, the arm and forearm swung from their
// animated directions (twist kept), the hand oriented so the weapon bone matches the target orientation.
// Only rotations change (bone lengths stay the skeleton's). `weight` blends from the animation (0) to IK (1).
inline void retargetArm(ArmLocals& a, Pose weaponTarget, Vec3 pole, float weight = 1, float maxStretch = 1.0f) {
    Pose armG = compose(a.parent, a.arm);
    Pose foreG = compose(armG, a.fore);
    Pose handG = compose(foreG, a.hand);
    // Wrist target: the hand pose that puts the weapon bone on the target.
    Quat handRot = multiply(weaponTarget.q, inverse(a.weapon.q));
    Vec3 wrist = weaponTarget.p - rotate(handRot, a.weapon.p);
    float upper = std::sqrt(dot(a.fore.p, a.fore.p)), lower = std::sqrt(dot(a.hand.p, a.hand.p));
    Limb l = twoBone(armG.p, wrist, pole, upper, lower, maxStretch);
    // Upper arm: swing the animated direction onto shoulder->elbow.
    Quat swing = fromTo(foreG.p - armG.p, l.joint - armG.p);
    Quat armNew = multiply(swing, armG.q);
    armG.q = armNew;
    foreG = compose(armG, a.fore);
    handG = compose(foreG, a.hand);
    // Forearm: swing onto elbow->wrist.
    swing = fromTo(handG.p - foreG.p, l.end - foreG.p);
    foreG.q = multiply(swing, foreG.q);
    Quat armLocal = multiply(inverse(a.parent.q), armNew);
    Quat foreLocal = multiply(inverse(armG.q), foreG.q);
    Quat handLocal = multiply(inverse(foreG.q), handRot);
    auto blend = [weight](Quat from, Quat to) {
        if (from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w < 0) to = {-to.x, -to.y, -to.z, -to.w};
        return normalize(Quat{from.x + (to.x - from.x) * weight, from.y + (to.y - from.y) * weight,
                              from.z + (to.z - from.z) * weight, from.w + (to.w - from.w) * weight});
    };
    a.arm.q = blend(a.arm.q, armLocal);
    a.fore.q = blend(a.fore.q, foreLocal);
    a.hand.q = blend(a.hand.q, handLocal);
}

}  // namespace core
