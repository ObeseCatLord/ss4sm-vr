// Physical melee simulation checks.
#undef NDEBUG
#include "core/melee_sim.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace core;
static float dist(Vec3 a, Vec3 b) { Vec3 d = a - b; return std::sqrt(dot(d, d)); }

int main() {
    MeleeParams light, heavy;
    light.mass = 1.0f;
    heavy.mass = 8.0f;
    // Converges to a static target without overshooting far.
    MeleeSim s;
    s.reset({Quat{}, {0, 0, 0}});
    Pose target{yaw(0.8f), {0.5f, 0.2f, -0.3f}};
    float maxD = 0;
    for (int i = 0; i < 400; ++i) {
        s.step(target, 1.0f / 90, light);
        maxD = std::fmax(maxD, dist(s.pose.p, {0, 0, 0}));
    }
    assert(dist(s.pose.p, target.p) < 0.01f);
    assert(maxD < dist(target.p, {0, 0, 0}) * 1.05f);  // critically damped: no large overshoot
    assert(std::fabs(s.pose.q.y - target.q.y) < 0.02f);
    // Heavier weapons lag more after the same 50 ms of a sudden hand move.
    MeleeSim a, b;
    a.reset({});
    b.reset({});
    Pose moved{Quat{}, {1, 0, 0}};
    for (int i = 0; i < 5; ++i) {
        a.step(moved, 0.01f, light);
        b.step(moved, 0.01f, heavy);
    }
    assert(a.pose.p.x > b.pose.p.x);
    // Tip speed includes rotation: spinning about +Y at 10 rad/s moves the 0.9 m tip at 9 m/s.
    MeleeSim spin;
    spin.reset({});
    spin.angularVelocity = {0, 10, 0};
    assert(std::fabs(spin.tipSpeed(light) - 9.0f) < 1e-3f);
    // Contact: stops at the fraction and removes velocity into the surface.
    MeleeSim c;
    c.reset({});
    Pose prev = c.pose;
    c.pose.p = {0, 0, -1};
    c.velocity = {0, 0, -5};
    c.applyContact(prev, {true, 0.5f, {0, 0, 1}});
    assert(std::fabs(c.pose.p.z + 0.5f) < 0.01f && c.velocity.z >= 0);
    // Blade segment runs along -Z from bladeStart to bladeEnd.
    Segment seg = c.blade({Quat{}, {0, 0, 0}}, light);
    assert(std::fabs(seg.a.z + light.bladeStart) < 1e-5f && std::fabs(seg.b.z + light.bladeEnd) < 1e-5f);
    std::puts("melee_checks: ok");
    return 0;
}
