// Two-hand grip reach and swing gating (core/grip_swing.hpp), the logic the local and authority paths share.
#undef NDEBUG
#include "core/grip_swing.hpp"
#include <cassert>
#include <cmath>

using namespace core;

int main() {
    // grip: the foregrip is 25 cm ahead (-Z) of the main hand, in engine units at the world scale
    const Pose main{{0, 0, 0, 1}, {0, 1, 0}};
    const Vec3 fg = grip::foregrip(main, 1.0f);
    assert(std::fabs(fg.z + 0.25f) < 1e-6f && std::fabs(fg.y - 1.0f) < 1e-6f);
    const Vec3 fg2 = grip::foregrip(main, 2.0f);  // 2 units per metre: twice as far in units
    assert(std::fabs(fg2.z + 0.5f) < 1e-6f);
    // a yawed hand: ahead follows its -Z
    const float s = std::sin(0.785398f), c = std::cos(0.785398f);  // 90 deg about +Y
    const Vec3 side = grip::foregrip({{0, s, 0, c}, {0, 0, 0}}, 1.0f);
    assert(std::fabs(side.x + 0.25f) < 1e-5f && std::fabs(side.z) < 1e-5f);
    // reach: at the foregrip yes, 10 cm off yes (within 15 cm), 30 cm off no; a big snap distance widens it
    assert(grip::atForegrip(main, fg, 0.0f, 0.2f, 1.0f));
    assert(grip::atForegrip(main, fg + Vec3{0.10f, 0, 0}, 0.0f, 0.2f, 1.0f));
    assert(!grip::atForegrip(main, fg + Vec3{0.30f, 0, 0}, 0.0f, 0.2f, 1.0f));
    assert(grip::atForegrip(main, fg + Vec3{0.30f, 0, 0}, 0.5f, 0.2f, 1.0f));
    assert(grip::atForegrip(main, fg + Vec3{0.18f, 0, 0}, -1.0f, 0.2f, 1.0f));  // invalid snap: the default 0.2
    assert(!grip::atForegrip(main, fg + Vec3{0.18f, 0, 0}, 5.0f, 0.1f, 1.0f));  // out of range: default 0.1, reach 0.15

    // swing: head-relative speed ignores walking (head and hand move together)
    const Vec3 head0{0, 1.6f, 0}, head1{1.0f, 1.6f, 0}, hand0{0.3f, 1.3f, -0.3f}, hand1{1.3f, 1.3f, -0.3f};
    assert(swing::relativeSpeed(hand0 - head0, hand1 - head1, 0.1, 1.0f) < 1e-5f);
    // a 30 cm swing in 0.1 s: 3 m/s; in engine units at 2 units per metre the same motion is 60 units
    assert(std::fabs(swing::relativeSpeed({0, 0, 0}, {0.3f, 0, 0}, 0.1, 1.0f) - 3.0f) < 1e-4f);
    assert(std::fabs(swing::relativeSpeed({0, 0, 0}, {0.6f, 0, 0}, 0.1, 2.0f) - 3.0f) < 1e-4f);
    assert(swing::relativeSpeed({0, 0, 0}, {1, 0, 0}, 0.0, 1.0f) == 0.0f);  // no interval, no speed
    assert(swing::counts(3.0f, 2.0f, 0.02) && !swing::counts(1.0f, 2.0f, 0.02));
    assert(swing::counts(35.0f, 2.0f, 0.02));   // a fast flick of a knife tip
    assert(!swing::counts(60.0f, 2.0f, 0.02));  // a tracking jump
    assert(!swing::counts(3.0f, 2.0f, 0.3));    // too long an interval to be one motion
    return 0;
}
