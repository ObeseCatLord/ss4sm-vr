#pragma once
// Camera motion vectors and projection jitter for the temporal upscalers (docs/re/R13_mp_channel_upscaler.md s5).
// Engine-agnostic: eye frusta as tangents, rigid eye poses, the engine's reverse-Z depth (Vulkan depth
// d = (1 - z_gl) / 2 of the GL-style projection in core/stereo.hpp; cleared to 0 at the far plane, lab 2026-10-24).
// src/up/shaders/motion.comp is the GPU twin of reprojectPixel(); tests/reproject_checks.cpp covers both conventions.
#include "core/math.hpp"
#include "core/stereo.hpp"
#include <cmath>
#include <cstdint>

namespace core {
struct Tangents { float l, r, u, d; };  // tan of the OpenXR angles (l, d negative)

inline Tangents tangents(const Fov& f) { return {std::tan(f.left), std::tan(f.right), std::tan(f.up), std::tan(f.down)}; }
inline Fov angles(const Tangents& t) { return {std::atan(t.l), std::atan(t.r), std::atan(t.u), std::atan(t.d)}; }

// Shift the frustum so the image content moves by (jx, jy) render pixels (x right, y down): the jitter convention
// FSR's projection offset uses (clip x += 2jx/W, clip y -= 2jy/H) and DLSS takes the same values.
inline Tangents jittered(Tangents t, float jx, float jy, uint32_t w, uint32_t h) {
    const float dx = (t.r - t.l) / float(w), dy = (t.u - t.d) / float(h);
    return {t.l - jx * dx, t.r - jx * dx, t.u + jy * dy, t.d + jy * dy};
}

// Distance along -z of a reverse-Z depth sample for the projection's near/far.
// With z_gl = 1 - 2d in the GL projection this is f*n / (n + d*(f - n)): f at d = 0, n at d = 1.
inline float depthDistance(float d, float n, float f) { return f * n / (n + d * (f - n)); }

// Column-major 4x4 of a rigid pose (GLSL mat4 layout).
inline void rigidMatrix(const Pose& p, float m[16]) {
    const Quat q = p.q;
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0] = 1 - 2 * (yy + zz), m[1] = 2 * (xy + wz), m[2] = 2 * (xz - wy), m[3] = 0;
    m[4] = 2 * (xy - wz), m[5] = 1 - 2 * (xx + zz), m[6] = 2 * (yz + wx), m[7] = 0;
    m[8] = 2 * (xz + wy), m[9] = 2 * (yz - wx), m[10] = 1 - 2 * (xx + yy), m[11] = 0;
    m[12] = p.p.x, m[13] = p.p.y, m[14] = p.p.z, m[15] = 1;
}

// Previous eye space <- current eye space, from both frames' world-from-eye poses.
inline Pose currentToPrevious(const Pose& prevWorldEye, const Pose& curWorldEye) { return compose(inverse(prevWorldEye), curWorldEye); }

// CPU reference of the motion pass for one pixel centre (px, py in render pixels, y down): motion in pixels
// from the current to the previous position (DLSS / FSR convention: previous - current).
inline void reprojectPixel(float px, float py, float depth, uint32_t w, uint32_t h, const Tangents& cur, const Tangents& prev,
                           float n, float f, const Pose& curToPrev, float& mx, float& my) {
    const float u = px / float(w), v = py / float(h);
    const float dist = depthDistance(depth, n, f);
    const Vec3 pc{(cur.l + (cur.r - cur.l) * u) * dist, (cur.u + (cur.d - cur.u) * v) * dist, -dist};
    const Vec3 q = curToPrev.p + rotate(curToPrev.q, pc);
    const float qd = std::fmax(-q.z, 1e-4f);
    const float up = (q.x / qd - prev.l) / (prev.r - prev.l), vp = (q.y / qd - prev.u) / (prev.d - prev.u);
    mx = (up - u) * float(w);
    my = (vp - v) * float(h);
}
}  // namespace core
