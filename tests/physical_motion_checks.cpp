// Ported from SS2 VR tests/physical_motion_checks.cpp (MIT, SS2VR contributors).
#undef NDEBUG
#include "core/physical_motion.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace core;
static void check(bool ok, const char *message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    PhysicalMotion left, right;
    Pose pose{};
    auto sample = left.sample(pose, 1, 1000, 1000, true);
    check(!sample.known && !sample.fresh && !sample.down, "First pose invented neutral");
    sample = left.sample(pose, 2, 1030, 1030, true);
    check(sample.known && sample.fresh && !sample.down, "Fresh quiet pair unavailable");
    right.sample(pose, 1, 1000, 1000, true);
    right.sample(pose, 2, 1030, 1030, true);
    for (unsigned axis = 0; axis != 3; ++axis) {
        PhysicalMotion motion;
        motion.sample({}, 1, 1000, 1000, true);
        Pose moved{};
        if (axis == 0) moved.p.x = .06f;
        if (axis == 1) moved.p.y = .06f;
        if (axis == 2) moved.p.z = .06f;
        sample = motion.sample(moved, 2, 1030, 1030, true);
        check(sample.known && sample.down, "XYZ gesture missing");
    }
    pose.p.x = .06f;
    sample = left.sample(pose, 3, 1060, 1060, true);
    check(sample.down && !right.down, "Hands coupled");
    sample = left.sample(pose, 3, 1060, 1070, true);
    check(sample.known && !sample.fresh && sample.down, "Duplicate changed gesture");
    pose.p.x += .025f;
    check(left.sample(pose, 4, 1090, 1090, true).down, "Hysteresis dropped held motion");
    check(!left.sample(pose, 5, 1120, 1120, true).down, "Quiet motion stayed down");
    PhysicalMotion rotation;
    rotation.sample({}, 1, 1000, 1000, true);
    Pose spun{yaw(.3f), {}};
    check(rotation.sample(spun, 2, 1030, 1030, true).down, "Rotation gesture missing");
    spun.q = {-spun.q.x, -spun.q.y, -spun.q.z, -spun.q.w};
    sample = rotation.sample(spun, 3, 1060, 1060, true);
    check(sample.known && !sample.down, "Quaternion sign invented motion");
    for (unsigned axis = 0; axis != 3; ++axis) {
        PhysicalMotion angular;
        angular.sample({}, 1, 1000, 1000, true);
        Pose moved{};
        const float sine = std::sin(.15f);
        moved.q.w = std::cos(.15f);
        if (axis == 0) moved.q.x = sine;
        if (axis == 1) moved.q.y = sine;
        if (axis == 2) moved.q.z = sine;
        check(angular.sample(moved, 2, 1030, 1030, true).down, "XYZ angular gesture missing");
    }
    // (SS2 weaponTracking/Input checks omitted: SS2-specific input type.)
    PhysicalMotion slow;
    slow.sample({}, 1, 1000, 1000, true);
    sample = slow.sample(Pose{{}, {.06f, 0, 0}}, 2, 1060, 1060, true);
    check(sample.known && !sample.down, "30ms normalization missing");
    PhysicalMotion replay;
    replay.sample({}, 10, 1000, 1000, true);
    replay.sample({}, 11, 1030, 1030, true);
    check(!replay.sample({}, 9, 970, 1030, true).known && replay.sequence == 11,
          "Replay became a baseline");
    check(!replay.sample({}, 10, 1000, 1030, true).known, "Replay invented quiet pair");
    check(replay.sample({}, 12, 1060, 1060, true).known, "New source failed after replay");
    sample = left.sample(pose, 6, 1300, 1300, true);
    check(!sample.known && !sample.fresh, "Gap invented release");
    sample = left.sample(pose, 6, 1300, 1501, true);
    check(!sample.known && !left.seeded, "Stale sample admitted");
    check(!left.sample(pose, 7, 1600, 1599, true).known, "Future sample admitted");
    left.sample(pose, 7, 1600, 1600, true);
    check(!left.sample(pose, 8, 1630, 1630, false).known && !left.seeded, "Loss invented release");
    check(!left.sample(pose, 9, 1660, 1660, true).known, "Recovery reused baseline");
    Pose jump = pose; jump.p.z += 1;
    check(!left.sample(jump, 10, 1690, 1690, true).known, "Discontinuity invented gesture or neutral");
    check(left.sample(jump, 11, 1720, 1720, true).known, "Quiet recovery failed");
    Pose bad = pose; bad.q.w = 0;
    check(!left.sample(bad, 12, 1750, 1750, true).known, "Nonunit pose accepted");
    bad = pose; bad.p.x = std::numeric_limits<float>::infinity();
    check(!left.sample(bad, 13, 1780, 1780, true).known, "Nonfinite pose accepted");
    // Arming now belongs to the independent PhysicalGestureInput, tested in
    // physical_gesture_checks. Motion only supplies truthful observation flags.
    PhysicalMotion context;
    sample = context.sample({}, 1, 1000, 1000, true);
    check(!sample.known && !sample.quiet, "Initial pose invented neutral");
    sample = context.sample({}, 2, 1030, 1030, true);
    check(sample.known && sample.fresh && sample.quiet && !sample.down, "Quiet pair was not observed");
    sample = context.sample(Pose{{}, {.06f, 0, 0}}, 3, 1060, 1060, true);
    check(sample.known && sample.down && !sample.quiet, "Moving sample was not observed");
    context.reset();
    sample = context.sample({}, 4, 1090, 1090, true);
    check(!sample.known && !sample.quiet, "Context reset invented neutral");
    sample = context.sample(Pose{{}, {.06f, 0, 0}}, 5, 1120, 1120, true);
    check(sample.down && !sample.quiet, "Movement after reset is not a neutral witness");
    std::cout << "LOCAL motion sample checks passed\n";
}
