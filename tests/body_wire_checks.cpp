// Body IK and pose wire-format checks.
#undef NDEBUG
#include "core/body_ik.hpp"
#include "core/wire.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace core;
static float len(Vec3 v) { return std::sqrt(dot(v, v)); }

int main() {
    // two-bone: bone lengths preserved within reach, end on target, joint bends towards the pole
    Limb l = twoBone({0, 0, 0}, {0.4f, -0.2f, 0}, {0, -1, 0}, 0.3f, 0.28f);
    assert(std::fabs(len(l.joint - l.root) - 0.3f) < 1e-3f && std::fabs(len(l.end - l.joint) - 0.28f) < 1e-3f);
    assert(len(l.end - Vec3{0.4f, -0.2f, 0}) < 1e-3f && l.joint.y < -0.05f && !l.stretched);
    // out of reach: stretches up to the limit, then points straight at the target
    Limb s = twoBone({0, 0, 0}, {0.7f, 0, 0}, {0, -1, 0}, 0.3f, 0.28f, 1.6f);
    assert(s.stretched && len(s.end - Vec3{0.7f, 0, 0}) < 1e-3f);
    Limb far = twoBone({0, 0, 0}, {2, 0, 0}, {0, -1, 0}, 0.3f, 0.28f, 1.6f);
    assert(std::fabs(far.end.x - 0.58f * 1.6f) < 1e-3f);
    // body yaw: inside the deadzone the torso keeps its heading; beyond it follows
    BodyState st;
    updateBodyYaw(st, yaw(0), false, 0.011f);
    assert(std::fabs(updateBodyYaw(st, yaw(0.5f), false, 0.011f)) < 1e-4f);
    float y = updateBodyYaw(st, yaw(1.2f), false, 0.011f);
    assert(std::fabs(y - (1.2f - 0.785f)) < 1e-3f);
    // full solve: hands reached, feet on the floor
    BodyDims d;
    BodyState bs;
    Vec3 hands[2] = {{-0.25f, 1.2f, -0.3f}, {0.25f, 1.2f, -0.3f}};
    bool valid[2] = {true, true};
    BodyPose b = solveBody(d, bs, {Quat{}, {0, 1.7f, 0}}, hands, valid, false, 0.011f);
    assert(len(b.arm[0].end - hands[0]) < 1e-3f && len(b.arm[1].end - hands[1]) < 1e-3f);
    assert(std::fabs(b.leg[0].end.y) < 1e-3f && b.pelvis.y < b.neck.y);
    // wire: round trip within quantization error
    Pose p[3] = {{normalize(Quat{0.1f, 0.3f, -0.2f, 0.9f}), {0.012f, 1.734f, -0.25f}},
                 {yaw(1.0f), {-0.3f, 1.1f, -0.4f}}, {normalize(Quat{-0.6f, 0.2f, 0.1f, 0.7f}), {0.3f, 1.0f, -0.35f}}};
    float t[2] = {0.25f, 1.0f}, g[2] = {0, 0.5f};
    wire::PosePacket pk = wire::encode(42, p, wire::HeadValid | wire::RightValid, t, g);
    wire::PosePacket rx;
    assert(wire::decode(&pk, sizeof pk, rx) && rx.sequence == 42 && rx.flags == (wire::HeadValid | wire::RightValid));
    for (int i = 0; i < 3; ++i) {
        Pose q = wire::posePart(rx, i);
        assert(len(q.p - p[i].p) < 0.001f);
        float dotq = std::fabs(q.q.x * p[i].q.x + q.q.y * p[i].q.y + q.q.z * p[i].q.z + q.q.w * p[i].q.w);
        assert(dotq > 0.9995f);  // < ~3.6 degrees
    }
    {  // finger curls ride in the packet behind FingersValid, 1/255 resolution, both hands independent
        core::wire::PosePacket p;
        const float l[5] = {0.1f, 0.0f, 0.5f, 1.0f, 2.0f}, r[5] = {0.7f, 1.0f, 0.25f, 0.0f, -1.0f};
        assert(!(p.flags & core::wire::FingersValid));
        core::wire::setCurls(p, 0, l);
        core::wire::setCurls(p, 1, r);
        assert(p.flags & core::wire::FingersValid);
        float bl[5], br[5];
        core::wire::curls(p, 0, bl);
        core::wire::curls(p, 1, br);
        for (int f = 0; f < 5; ++f) {
            assert(std::fabs(bl[f] - std::fmin(std::fmax(l[f], 0.0f), 1.0f)) < 0.003f);
            assert(std::fabs(br[f] - std::fmin(std::fmax(r[f], 0.0f), 1.0f)) < 0.003f);
        }
    }
    std::puts("body_wire_checks: ok");
    return 0;
}
