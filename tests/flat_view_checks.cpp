// Flat main view maths for the upscalers (core/flat_view.hpp; docs/re/R27_flat_upscaler.md s1.5, s2): projection
// jitter on the engine matrix, tangents and near/far from it, camera pose and current-to-previous from the engine's
// double world-to-view matrices, and RenderView's render box.
#undef NDEBUG
#include "core/flat_view.hpp"
#include "core/reproject.hpp"
#include "core/stereo.hpp"
#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

using namespace core;

namespace {
bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
bool sameRotation(Quat a, Quat b, float eps) {
    const float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);  // q and -q are the same rotation
    return d > 1.0f - eps;
}
bool samePose(const Pose& a, const Pose& b, float eps) {
    return sameRotation(a.q, b.q, eps) && near(a.p.x, b.p.x, eps) && near(a.p.y, b.p.y, eps) && near(a.p.z, b.p.z, eps);
}
// The engine's world-to-view matrix of a world-from-view pose, in double (rigid inverse of the placement).
void w2vOf(const Pose& worldView, const double origin[3], double m[12]) {
    float r[12];
    matrix34(worldView, r);  // world-from-view rotation in r[0..2], r[4..6], r[8..10]
    double p[3] = {origin[0] + worldView.p.x, origin[1] + worldView.p.y, origin[2] + worldView.p.z};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[i * 4 + j] = r[j * 4 + i];  // R = transpose
        m[i * 4 + 3] = -(m[i * 4] * p[0] + m[i * 4 + 1] * p[1] + m[i * 4 + 2] * p[2]);
    }
}
Vec3 normalize3(Vec3 v) { return v * (1.0f / std::sqrt(dot(v, v))); }
Quat axisAngle(Vec3 a, float rad) {
    const float s = std::sin(rad / 2);
    return normalize({a.x * s, a.y * s, a.z * s, std::cos(rad / 2)});
}
}  // namespace

int main() {
    // Tangents and near/far from the engine's matrix (the flat persp_fov layout, R27 s1.1): 90 deg horizontal at
    // 16:9 shown at 16:10, n 0.1, f 1000 (and the engine's default far 200000).
    const float hx = 1.0f, hy = 1.0f / (16.0f / 9.0f);
    const Fov fov{-std::atan(hx), std::atan(hx), std::atan(hy * 1.6f / (16.0f / 9.0f)), -std::atan(hy * 1.6f / (16.0f / 9.0f))};
    float P[16];
    engineProjection(fov, 0.1f, 1000.0f, P);
    assert(isPerspective(P));
    const Tangents t = projectionTangents(P), e = tangents(fov);
    assert(near(t.l, e.l, 1e-5f) && near(t.r, e.r, 1e-5f) && near(t.u, e.u, 1e-5f) && near(t.d, e.d, 1e-5f));
    float n = 0, f = 0;
    assert(projectionNearFar(P, n, f) && near(n, 0.1f, 1e-5f) && near(f, 1000.0f, 1.0f));
    engineProjection(fov, 0.1f, 200000.0f, P);
    assert(projectionNearFar(P, n, f) && near(n, 0.1f, 1e-5f) && f > 100000.0f);  // float P10 limits the far estimate
    // A far plane beyond float precision (P10 == -1): effectively infinite.
    engineProjection(fov, 0.1f, 1e9f, P);
    assert(P[10] == -1.0f && projectionNearFar(P, n, f) && near(n, 0.1f, 1e-5f) && f >= 1e6f);
    // An asymmetric (off-centre) frustum.
    const Fov skew{-0.9f, 0.6f, 0.5f, -0.7f};
    engineProjection(skew, 0.25f, 500.0f, P);
    const Tangents ts = projectionTangents(P), es = tangents(skew);
    assert(near(ts.l, es.l, 1e-5f) && near(ts.r, es.r, 1e-5f) && near(ts.u, es.u, 1e-5f) && near(ts.d, es.d, 1e-5f));
    // Orthographic and degenerate matrices are not perspective.
    float O[16] = {};
    O[0] = O[5] = 1.0f, O[10] = -0.01f, O[15] = 1.0f;
    assert(!isPerspective(O));
    assert(!projectionNearFar(O, n, f));

    // Jitter on the matrix == jittered() on the tangents (R27 s1.5): the existing sign switches keep their meaning.
    for (const Fov& fv : {fov, skew})
        for (auto [jx, jy] : {std::pair{0.25f, -0.375f}, std::pair{-0.5f, 0.5f}, std::pair{0.0f, 0.1f}}) {
            const uint32_t w = 1707, h = 960;
            float A[16], B[16];
            engineProjection(fv, 0.1f, 1000.0f, A);
            jitterProjectionMatrix(A, jx, jy, w, h);
            engineProjection(angles(jittered(tangents(fv), jx, jy, w, h)), 0.1f, 1000.0f, B);
            for (int i = 0; i < 16; ++i) assert(near(A[i], B[i], 2e-5f * std::max(1.0f, std::fabs(B[i]))));
            // The shift is jx/w of the frustum width: a point on the view axis moves by jx render pixels.
            const Tangents tj = projectionTangents(A), t0 = tangents(fv);
            assert(near((t0.l - tj.l) / (t0.r - t0.l) * float(w), jx, 1e-3f));
            assert(near((tj.u - t0.u) / (t0.u - t0.d) * float(h), jy, 1e-3f));
        }
    float Z[16];
    engineProjection(fov, 0.1f, 1000.0f, Z);
    float Z0[16];
    for (int i = 0; i < 16; ++i) Z0[i] = Z[i];
    jitterProjectionMatrix(Z, 0.3f, 0.3f, 0, 0);  // no render size: unchanged
    for (int i = 0; i < 16; ++i) assert(Z[i] == Z0[i]);

    // Camera pose from the engine's world-to-view doubles.
    const double origin0[3] = {0, 0, 0};
    const Pose a{axisAngle(normalize3({0.3f, 1.0f, -0.2f}), 0.8f), {1.5f, -2.0f, 3.25f}};
    double m[12];
    w2vOf(a, origin0, m);
    Pose back;
    assert(viewPoseFromW2V(m, back) && samePose(back, a, 1e-5f));
    double pos[3];
    viewPosition(m, pos);
    assert(std::fabs(pos[0] - 1.5) < 1e-5 && std::fabs(pos[1] + 2.0) < 1e-5 && std::fabs(pos[2] - 3.25) < 1e-5);
    // Every quaternion branch (trace > 0 and the three diagonal cases).
    for (Vec3 ax : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}, Vec3{0.6f, -0.48f, 0.64f}})
        for (float ang : {0.0f, 1.0f, 3.0f, 3.14159f}) {
            const Pose p{axisAngle(ax, ang), {0, 0, 0}};
            w2vOf(p, origin0, m);
            assert(viewPoseFromW2V(m, back) && samePose(back, p, 1e-5f));
        }
    // A non-rigid matrix (scaled or mirrored) is rejected.
    w2vOf(a, origin0, m);
    double s[12];
    for (int i = 0; i < 12; ++i) s[i] = m[i];
    for (int j = 0; j < 3; ++j) s[j] *= 1.1;
    assert(!viewPoseFromW2V(s, back));
    for (int i = 0; i < 12; ++i) s[i] = m[i];
    for (int j = 0; j < 4; ++j) s[j] = -s[j];
    assert(!viewPoseFromW2V(s, back));

    // Current-to-previous from the two matrices == currentToPrevious() of the poses.
    const Pose prev{axisAngle(normalize3({0, 1, 0}), 0.40f), {2.0f, 1.7f, -5.0f}};
    const Pose cur{axisAngle(normalize3({0.1f, 1, 0}), 0.43f), {2.05f, 1.7f, -5.1f}};
    double mp[12], mc[12];
    w2vOf(prev, origin0, mp);
    w2vOf(cur, origin0, mc);
    Pose c2p;
    assert(currentToPreviousW2V(mp, mc, c2p));
    assert(samePose(c2p, currentToPrevious(prev, cur), 1e-5f));
    // Same camera: identity.
    assert(currentToPreviousW2V(mc, mc, c2p) && samePose(c2p, Pose{}, 1e-6f));
    // Far from the world origin (100 km), with the engine's double camera positions: millimetre motion stays exact
    // although the rotations come from float quaternions (and float world positions would quantise it to ~8 mm).
    const double far0[3] = {100000.0, 2500.0, -70000.0};
    auto posOf = [&](const Pose& p, double out[3]) {
        out[0] = far0[0] + p.p.x, out[1] = far0[1] + p.p.y, out[2] = far0[2] + p.p.z;
    };
    double pp[3], pc[3];
    const Pose p1{axisAngle(normalize3({0, 1, 0}), 1.2f), {0, 0, 0}};
    for (const Pose& p2 : {Pose{p1.q, {0.001f, 0, 0}},                                          // 1 mm strafe
                           Pose{axisAngle(normalize3({0.2f, 1, 0.1f}), 1.21f), {0.002f, -0.001f, 0.003f}}}) {  // and turn
        w2vOf(p1, far0, mp);
        w2vOf(p2, far0, mc);
        posOf(p1, pp);
        posOf(p2, pc);
        assert(currentToPreviousW2V(mp, pp, mc, pc, c2p));
        const Pose ref = currentToPrevious(p1, p2);  // the same motion near the origin
        assert(samePose(c2p, ref, 1e-6f));
        assert(near(std::sqrt(dot(c2p.p, c2p.p)), std::sqrt(dot(ref.p, ref.p)), 1e-7f));
    }
    // A strafe at constant depth gives uniform horizontal motion vectors of the expected size.
    {
        const Pose q1{Quat{}, {0, 0, 0}}, q2{Quat{}, {0.1f, 0, 0}};  // 10 cm to the right, looking down -z
        w2vOf(q1, far0, mp);
        w2vOf(q2, far0, mc);
        posOf(q1, pp);
        posOf(q2, pc);
        assert(currentToPreviousW2V(mp, pp, mc, pc, c2p));
        const Tangents tt = tangents(fov);
        const float d = 0.5f;  // reverse-Z depth sample
        const float dist = depthDistance(d, 0.1f, 1000.0f);
        float mx0, my0, mx1, my1;
        reprojectPixel(100.5f, 200.5f, d, 1920, 1200, tt, tt, 0.1f, 1000.0f, c2p, mx0, my0);
        reprojectPixel(1800.5f, 900.5f, d, 1920, 1200, tt, tt, 0.1f, 1000.0f, c2p, mx1, my1);
        const float expect = 0.1f / dist / (tt.r - tt.l) * 1920.0f;  // prev - cur: the point was further right
        assert(near(mx0, mx1, 1e-2f) && near(my0, 0.0f, 1e-2f) && near(my1, 0.0f, 1e-2f));
        assert(near(mx0, expect, 1e-2f * std::max(1.0f, expect)));
    }

    // RenderView's render box (R27 s1.5).
    int rbw, rbh;
    flatRenderBox(2560, 1440, 2560, 1440, 2560, 1440, false, 16384, rbw, rbh);  // identity: no 3D canvas
    assert(rbw == 2560 && rbh == 1440);
    flatRenderBox(2560, 1440, 2560, 1440, 2560, 1080, false, 16384, rbw, rbh);  // letterbox: the box
    assert(rbw == 2560 && rbh == 1080);
    flatRenderBox(2560, 1440, 1707, 960, 2560, 1440, false, 16384, rbw, rbh);  // phase B 1.5
    assert(rbw == 1707 && rbh == 960);
    flatRenderBox(2560, 1440, 1707, 960, 2560, 1080, false, 16384, rbw, rbh);  // phase B, letterboxed
    assert(rbw == 1707 && rbh == 720);
    flatRenderBox(2560, 1440, 2560, 1440, 2550, 1440, true, 16384, rbw, rbh);  // forced CAS: snapped within 0.99
    assert(rbw == 2560 && rbh == 1440);
    flatRenderBox(2560, 1440, 2560, 1440, 2550, 1440, false, 16384, rbw, rbh);  // no canvas: the box as is
    assert(rbw == 2550 && rbh == 1440);
    flatRenderBox(8192, 4320, 8000, 4000, 8192, 4320, false, 4096, rbw, rbh);  // clamped to the maximum target size
    assert(rbw == 4096 && rbh == 4000);
    std::puts("flat view checks passed");
    return 0;
}
