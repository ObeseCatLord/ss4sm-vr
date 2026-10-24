// Camera motion vectors and jitter (core/reproject.hpp).
#undef NDEBUG
#include "core/reproject.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace core;

// Project a view-space point with the engine's projection and the reverse-Z mapping: pixel (y down) and depth.
static void project(const Tangents& t, float n, float f, Vec3 p, uint32_t w, uint32_t h, float& px, float& py, float& d) {
    float m[16];
    engineProjection(angles(t), n, f, m);
    const float cx = m[0] * p.x + m[2] * p.z, cy = m[5] * p.y + m[6] * p.z, cz = m[10] * p.z + m[11], cw = -p.z;
    const float xn = cx / cw, yn = cy / cw, zn = cz / cw;
    px = (xn * 0.5f + 0.5f) * float(w);
    py = (0.5f - yn * 0.5f) * float(h);  // y down
    d = (1 - zn) * 0.5f;
}

int main() {
    const uint32_t W = 1344, H = 1512;
    const Tangents t = tangents({-0.9f, 0.8f, 0.85f, -0.95f});
    const float n = 0.1f, f = 500.0f;

    // Depth decode round trip, and the reverse-Z clear value (0) is the far plane.
    for (float z : {0.15f, 1.0f, 7.5f, 120.0f}) {
        float px, py, d;
        project(t, n, f, {0.1f, -0.2f, -z}, W, H, px, py, d);
        assert(std::fabs(depthDistance(d, n, f) - z) < 1e-3f * z);
    }
    assert(std::fabs(depthDistance(0.0f, n, f) - f) < 1e-2f);
    assert(std::fabs(depthDistance(1.0f, n, f) - n) < 1e-5f);

    // A static camera has no motion.
    float mx, my;
    Pose still{};
    reprojectPixel(400.5f, 900.5f, 0.01f, W, H, t, t, n, f, still, mx, my);
    assert(std::fabs(mx) < 1e-3f && std::fabs(my) < 1e-3f);

    // Camera moves: a world point's pixel this frame and last frame; the motion vector maps current -> previous.
    const Pose prevEye{normalize(Quat{0.02f, 0.1f, 0, 1}), {0, 1.6f, 0}};
    const Pose curEye{normalize(Quat{0.0f, 0.13f, 0.01f, 1}), {0.05f, 1.62f, -0.03f}};
    const Vec3 world{-0.7f, 1.2f, -3.0f};
    auto viewOf = [&](const Pose& eye) { Pose inv = inverse(eye); return inv.p + rotate(inv.q, world); };
    float cpx, cpy, cd, ppx, ppy, pd;
    project(t, n, f, viewOf(curEye), W, H, cpx, cpy, cd);
    project(t, n, f, viewOf(prevEye), W, H, ppx, ppy, pd);
    reprojectPixel(cpx, cpy, cd, W, H, t, t, n, f, currentToPrevious(prevEye, curEye), mx, my);
    assert(std::fabs((cpx + mx) - ppx) < 0.05f && std::fabs((cpy + my) - ppy) < 0.05f);

    // Jitter: content moves by (jx, jy) pixels, x right and y down.
    const Tangents j = jittered(t, 0.25f, -0.4f, W, H);
    float jpx, jpy, jd;
    project(j, n, f, viewOf(curEye), W, H, jpx, jpy, jd);
    assert(std::fabs((jpx - cpx) - 0.25f) < 0.01f && std::fabs((jpy - cpy) + 0.4f) < 0.01f);

    // rigidMatrix is column-major and matches rotate + translate.
    float m[16];
    rigidMatrix(curEye, m);
    const Vec3 v{0.3f, -0.2f, 1.1f}, r = curEye.p + rotate(curEye.q, v);
    assert(std::fabs(m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] - r.x) < 1e-5f);
    assert(std::fabs(m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] - r.y) < 1e-5f);
    assert(std::fabs(m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] - r.z) < 1e-5f);
    std::puts("reproject_checks: ok");
}
