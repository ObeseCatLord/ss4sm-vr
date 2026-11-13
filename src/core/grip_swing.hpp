#pragma once
// Two-hand grip reach and melee swing gating, shared by the local and the authority paths (src/se4/hands.cpp,
// src/se4/melee.cpp) and host-tested (tests/grip_swing_checks.cpp). Engine-agnostic: poses in engine units, `unitsPerMetre`
// converts (engine units = metres / vr_fWorldScale).
#include "core/math.hpp"
#include <cmath>

namespace core::grip {
constexpr float kForegripAhead = 0.25f;  // m: the support hand's place ahead of the main hand along the barrel (-Z)
constexpr float kForegripReach = 0.15f;  // m: how near it the off hand must be to grip

// The foregrip point: kForegripAhead metres ahead of the main hand along its -Z.
inline Vec3 foregrip(const Pose& mainHand, float unitsPerMetre) {
    return mainHand.p + rotate(normalize(mainHand.q), Vec3{0, 0, -kForegripAhead * unitsPerMetre});
}
// Whether the off hand is at the foregrip: within the weapon's snap distance (engine units; outside 0..2 the default
// is used), but never nearer than kForegripReach metres.
inline bool atForegrip(const Pose& mainHand, const Vec3& offHand, float snap, float defaultSnap, float unitsPerMetre) {
    if (!(snap >= 0.0f && snap < 2.0f)) snap = defaultSnap;
    snap = std::fmax(snap, kForegripReach * unitsPerMetre);
    const Vec3 d = foregrip(mainHand, unitsPerMetre) - offHand;
    return dot(d, d) <= snap * snap;
}
}  // namespace core::grip

namespace core::swing {
constexpr float kMaxSpeed = 20.0f;  // m/s: faster is a tracking jump or a network step, not a swing

// Head-relative speed of a point (m/s): `rel` = point minus head now, `prevRel` the same `dt` seconds ago (engine units).
// Walking, riding or a snap turn moves the head with the hand and does not count.
inline float relativeSpeed(const Vec3& prevRel, const Vec3& rel, double dt, float unitsPerMetre) {
    if (!(dt > 1e-4) || !(unitsPerMetre > 0.0f)) return 0.0f;
    const Vec3 d = rel - prevRel;
    return std::sqrt(dot(d, d)) / float(dt) / unitsPerMetre;
}
// A swing: fast enough, not implausibly fast, measured over a short enough interval to be one motion.
inline bool counts(float speed, float minSpeed, double dt) { return speed >= minSpeed && speed <= kMaxSpeed && dt < 0.25; }
}  // namespace core::swing
