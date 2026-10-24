// Arm retargeting onto an animated skeleton (core::retargetArm).
#undef NDEBUG
#include "core/body_ik.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace core;

static float dist(Vec3 a, Vec3 b) { Vec3 d = a - b; return std::sqrt(dot(d, d)); }
static float qdot(Quat a, Quat b) { return std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w); }

int main() {
    // fromTo
    Quat q = fromTo({1, 0, 0}, {0, 1, 0});
    Vec3 r = rotate(q, {1, 0, 0});
    assert(dist(r, {0, 1, 0}) < 1e-5f);
    q = fromTo({1, 0, 0}, {-1, 0, 0});
    assert(dist(rotate(q, {1, 0, 0}), {-1, 0, 0}) < 1e-4f);

    // A T-pose right arm along +X from a shoulder at (0.2, 1.4, 0); bones along local +X with a twisted
    // forearm frame so the solver must not rely on bone axes.
    ArmLocals a;
    a.parent = {yaw(0.3f), {0.2f, 1.4f, 0}};
    a.arm = {multiply(inverse(yaw(0.3f)), Quat{}), {0.05f, 0, 0}};
    a.fore = {Quat{0.2588f, 0, 0, 0.9659f}, {0.30f, 0, 0}};  // 30 deg roll about the bone
    a.hand = {Quat{}, {0.28f, 0, 0}};
    a.weapon = {Quat{0, 0, 0.3827f, 0.9239f}, {0.08f, 0.02f, 0}};
    // target in reach, in front and below
    Pose target{normalize(Quat{0.1f, 0.2f, 0.0f, 0.97f}), {0.35f, 1.15f, -0.35f}};
    retargetArm(a, target, {0.6f, 0.9f, 0.3f});
    Pose armG = compose(a.parent, a.arm), foreG = compose(armG, a.fore), handG = compose(foreG, a.hand),
         weaponG = compose(handG, a.weapon);
    std::printf("weapon error %.4f m, rot %.4f\n", dist(weaponG.p, target.p), 1 - qdot(weaponG.q, target.q));
    assert(dist(weaponG.p, target.p) < 0.01f);
    assert(1 - qdot(weaponG.q, target.q) < 1e-4f);
    // elbow bends towards the pole side (+X/down)
    assert(foreG.p.y < armG.p.y);
    // bone lengths unchanged
    assert(std::fabs(dist(foreG.p, armG.p) - 0.30f) < 1e-4f && std::fabs(dist(handG.p, foreG.p) - 0.28f) < 1e-4f);

    // out of reach: arm points at the target, weapon falls short but on the line
    ArmLocals b = a;
    Pose far{target.q, {2.0f, 1.4f, 0}};
    retargetArm(b, far, {0.6f, 0.9f, 0.3f});
    Pose bArm = compose(b.parent, b.arm), bFore = compose(bArm, b.fore);
    Vec3 d1 = bFore.p - bArm.p, d2 = far.p - bArm.p;
    float c = dot(d1, d2) / std::sqrt(dot(d1, d1) * dot(d2, d2));
    assert(c > 0.99f);

    // weight 0 leaves the animation untouched
    ArmLocals w = a, w0 = a;
    retargetArm(w, target, {0.6f, 0.9f, 0.3f}, 0.0f);
    assert(qdot(w.arm.q, w0.arm.q) > 0.99999f && qdot(w.hand.q, w0.hand.q) > 0.99999f);
    return 0;
}
