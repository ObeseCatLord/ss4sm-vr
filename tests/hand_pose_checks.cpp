// Finger poses (core/hand_pose.hpp).
#undef NDEBUG
#include "core/hand_pose.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace core;

static Quat axisAngle(Vec3 a, float t) { return {a.x * std::sin(t / 2), a.y * std::sin(t / 2), a.z * std::sin(t / 2), std::cos(t / 2)}; }

int main() {
    // Controller curls.
    float curl[FingerCount];
    curlsFromController({0.0f, true, 0.0f, true}, curl);
    assert(std::fabs(curl[Index] - 0.25f) < 1e-6f && curl[Middle] == 0.0f && std::fabs(curl[Thumb] - 0.7f) < 1e-6f);
    curlsFromController({1.0f, true, 1.0f, false}, curl);
    assert(std::fabs(curl[Index] - 1.0f) < 1e-6f && curl[Pinky] == 1.0f && std::fabs(curl[Thumb] - 0.1f) < 1e-6f);
    FingerAngles fist = anglesFromCurls(curl);
    assert(std::fabs(fist.a[Middle][1] - kMaxFlex[Middle][1]) < 1e-6f);

    // Joints: a straight hand has zero flexion; bending each joint by 0.4 rad about +X reads back 0.4.
    Quat j[26];
    bool v[26];
    for (int i = 0; i < 26; ++i) j[i] = Quat{}, v[i] = true;
    FingerAngles a;
    assert(anglesFromJoints(j, v, a));
    for (int f = 0; f < FingerCount; ++f)
        for (int k = 0; k < 3; ++k) assert(std::fabs(a.a[f][k]) < 1e-5f);
    // index finger: proximal 7, intermediate 8, distal 9, tip 10; each joint rotated 0.4 more than its parent
    // flexing: each bone turns its tip (-Z) toward the palm (-Y), a rotation about -X
    for (int i = 7; i <= 10; ++i) j[i] = axisAngle({-1, 0, 0}, 0.4f * float(i - 6));
    j[6] = Quat{};
    anglesFromJoints(j, v, a);
    for (int k = 0; k < 3; ++k) assert(std::fabs(a.a[Index][k] - 0.4f) < 1e-4f);
    const Vec3 tip = rotate(j[8], {0, 0, -1});
    assert(tip.y < 0);  // the tip really moved toward the palm side
    // invalid joints are reported
    v[8] = false;
    assert(!anglesFromJoints(j, v, a));

    // Smoothing converges; packing round-trips.
    FingerAngles cur;
    for (int i = 0; i < 100; ++i) smooth(cur, fist, 0.011f);
    assert(std::fabs(cur.a[Ring][2] - fist.a[Ring][2]) < 1e-3f);
    assert(std::fabs(unpackCurl(packCurl(fist, Pinky)) - 1.0f) < 1e-6f);
    std::puts("hand_pose_checks: ok");
}
