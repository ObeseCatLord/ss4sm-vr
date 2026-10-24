#pragma once
// Engine-facing stereo maths (no engine or OpenXR types).
#include <algorithm>
#include <cmath>

namespace core {
struct Fov { float left, right, up, down; };  // OpenXR angles in radians (left/down negative)

// Serious Engine VR projection (device slot 0x070): each eye keeps its own horizontal extent, the vertical
// extent is shared by both eyes (max up, min down). Row-major 4x4 as built by the engine's frustum helper
// (Sam4 0x141111290 / Fusion 0x22b9d30): m0=2n/(r-l) m2=(r+l)/(r-l) m5=2n/(t-b) m6=(t+b)/(t-b)
// m10=-(f+n)/(f-n) m11=-2fn/(f-n) m14=-1.
inline Fov sharedVertical(const Fov& eye, const Fov& a, const Fov& b) {
    return {eye.left, eye.right, std::max(a.up, b.up), std::min(a.down, b.down)};
}
inline void engineProjection(const Fov& f, float n, float farZ, float m[16]) {
    for (int i = 0; i < 16; ++i) m[i] = 0;
    float l = n * std::tan(f.left), r = n * std::tan(f.right), t = n * std::tan(f.up), b = n * std::tan(f.down);
    m[0] = 2 * n / (r - l), m[2] = (r + l) / (r - l);
    m[5] = 2 * n / (t - b), m[6] = (t + b) / (t - b);
    m[10] = -(farZ + n) / (farZ - n), m[11] = -2 * farZ * n / (farZ - n);
    m[14] = -1;
}
}  // namespace core
