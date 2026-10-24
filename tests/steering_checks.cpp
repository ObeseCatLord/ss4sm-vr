// Grab-to-steer checks.
#undef NDEBUG
#include "core/steering.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace core;

int main() {
    // wheel facing the driver: axis towards the driver (+Z), angle 0 at 12 o'clock (+Y)
    SteerAxis a{{0, 1, -0.5f}, {0, 0, 1}, {0, 1, 0}};
    const float r = 0.18f, maxA = 2.5f;
    SteeringGrab g;
    Vec3 hands[2] = {{-r, 1, -0.5f}, {r, 1, -0.5f}};  // 9 and 3 o'clock
    bool grip[2] = {true, true};
    assert(g.update(a, hands, grip, r, maxA) == 0 && g.holding());  // grabbing does not turn
    // rotate both hands +30 degrees around +Z (counter-clockwise seen from the driver = steering left)
    float t = 0.5236f;
    for (int i = 0; i < 2; ++i) {
        float x = hands[i].x, y = hands[i].y - 1;
        hands[i] = {x * std::cos(t) - y * std::sin(t), 1 + x * std::sin(t) + y * std::cos(t), -0.5f};
    }
    float ang = g.update(a, hands, grip, r, maxA);
    assert(std::fabs(ang - t) < 1e-3f && g.steer(maxA) > 0);
    // releasing one hand keeps the angle; the other continues to steer
    grip[0] = false;
    assert(std::fabs(g.update(a, hands, grip, r, maxA) - t) < 1e-3f);
    // hands far from the rim cannot grab
    SteeringGrab g2;
    Vec3 farHands[2] = {{0, 2, 0}, {0.5f, 1, 0.5f}};
    bool both[2] = {true, true};
    g2.update(a, farHands, both, r, maxA);
    assert(!g2.holding());
    // clamp to max angle
    SteeringGrab g3;
    g3.angle = 10;
    bool none[2] = {false, false};
    assert(g3.update(a, hands, none, r, maxA) == maxA);
    std::puts("steering_checks: ok");
    return 0;
}
