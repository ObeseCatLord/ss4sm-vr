#pragma once
// Small vector/quaternion/pose helpers. Adapted from SS2 VR src/common/math.hpp (MIT, SS2VR contributors).
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace core {
constexpr float Pi = 3.14159265358979323846f;
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Quat { float x = 0, y = 0, z = 0, w = 1; };
struct Pose { Quat q; Vec3 p; };

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Quat inverse(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat multiply(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat normalize(Quat q) {
    float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 1e-5f ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : Quat{};
}
inline Vec3 rotate(Quat q, Vec3 v) {
    Vec3 u{q.x, q.y, q.z};
    Vec3 t = cross(u, v) * 2;
    return v + t * q.w + cross(u, t);
}
inline Quat yaw(float a) { return {0, std::sin(a / 2), 0, std::cos(a / 2)}; }
inline Pose compose(Pose a, Pose b) { return {multiply(a.q, b.q), a.p + rotate(a.q, b.p)}; }
inline Pose inverse(Pose a) {
    Quat q = inverse(a.q);
    return {q, rotate(q, a.p * -1)};
}
inline bool finite(Pose p) {
    return std::isfinite(p.p.x) && std::isfinite(p.p.y) && std::isfinite(p.p.z) && std::isfinite(p.q.x) && std::isfinite(p.q.y) &&
           std::isfinite(p.q.z) && std::isfinite(p.q.w);
}
inline float yawAngle(Quat q) { return std::atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.y * q.y + q.z * q.z)); }
// Row-major 3x4 [r00 r01 r02 tx | r10 r11 r12 ty | r20 r21 r22 tz] (Serious Engine Matrix3x4 layout).
inline void matrix34(Pose p, float m[12]) {
    Quat q = normalize(p.q);
    float x = q.x, y = q.y, z = q.z, w = q.w;
    const float r[12] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), p.p.x,
                         2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), p.p.y,
                         2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), p.p.z};
    for (int i = 0; i < 12; ++i) m[i] = r[i];
}
}  // namespace core
