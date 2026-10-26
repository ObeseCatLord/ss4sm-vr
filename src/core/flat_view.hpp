#pragma once
// The flat (desktop) main view for the temporal upscalers (docs/re/R27_flat_upscaler.md s1.5, s2). Engine-agnostic;
// tests/flat_view_checks.cpp.
//   - P[16]: the engine's GL-style projection, row-major, clip = P * (x, y, z, 1) in view space (x right, y up,
//     -z forward), as built by the frustum helper (core/stereo.hpp engineProjection).
//   - W2V[12]: the engine's world-to-view 3x4 matrix in doubles, rows of 4 (R00 R01 R02 t0, ...): v = R w + t.
#include "core/math.hpp"
#include "core/reproject.hpp"
#include <cmath>
#include <cstdint>

namespace core {
// Shift the projection so the image content moves by (jx, jy) render pixels (x right, y down) at render size w x h:
// the matrix form of jittered() (P2 = (r+l)/(r-l), P6 = (t+b)/(t-b); P0 and P5 are unchanged).
inline void jitterProjectionMatrix(float P[16], float jx, float jy, uint32_t w, uint32_t h) {
    if (!w || !h) return;
    P[2] -= 2.0f * jx / float(w);
    P[6] += 2.0f * jy / float(h);
}

// A perspective projection of the frustum layout (orthographic and degenerate matrices are not).
inline bool isPerspective(const float P[16]) { return P[14] == -1.0f && P[0] != 0.0f && P[5] != 0.0f && std::isfinite(P[2]) && std::isfinite(P[6]); }

// Frustum tangents of a perspective P: tl = (P2-1)/P0, tr = (P2+1)/P0, tu = (P6+1)/P5, td = (P6-1)/P5.
inline Tangents projectionTangents(const float P[16]) {
    const double p0 = P[0], p2 = P[2], p5 = P[5], p6 = P[6];
    return {float((p2 - 1) / p0), float((p2 + 1) / p0), float((p6 + 1) / p5), float((p6 - 1) / p5)};
}

// Near/far planes of a perspective P, in double: n = P11/(P10-1), f = P11/(P10+1). A far plane so distant that the
// float P10 rounds to -1 (or past it) is taken as 1e7 * n (effectively infinite for the reverse-Z depth). False when
// the near plane is not positive.
inline bool projectionNearFar(const float P[16], float& n, float& f) {
    const double p10 = P[10], p11 = P[11];
    if (p10 - 1 == 0) return false;
    const double dn = p11 / (p10 - 1);
    if (!(dn > 0) || !std::isfinite(dn)) return false;
    const double df = p10 + 1 != 0 ? p11 / (p10 + 1) : 0;
    n = float(dn), f = float(df > dn && std::isfinite(df) ? df : dn * 1e7);
    return true;
}

// Quaternion of a rotation matrix (row-major 3x3, the convention of matrix34()).
inline Quat quatFromRotation(const double m[3][3]) {
    double x, y, z, w;
    const double tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        const double s = std::sqrt(tr + 1) * 2;
        w = 0.25 * s, x = (m[2][1] - m[1][2]) / s, y = (m[0][2] - m[2][0]) / s, z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        w = (m[2][1] - m[1][2]) / s, x = 0.25 * s, y = (m[0][1] + m[1][0]) / s, z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const double s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        w = (m[0][2] - m[2][0]) / s, x = (m[0][1] + m[1][0]) / s, y = 0.25 * s, z = (m[1][2] + m[2][1]) / s;
    } else {
        const double s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        w = (m[1][0] - m[0][1]) / s, x = (m[0][2] + m[2][0]) / s, y = (m[1][2] + m[2][1]) / s, z = 0.25 * s;
    }
    return normalize(Quat{float(x), float(y), float(z), float(w)});
}

// A proper rotation (orthonormal, determinant +1) to within `eps`.
inline bool isRotation(const double m[3][3], double eps = 1e-3) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const double d = m[i][0] * m[j][0] + m[i][1] * m[j][1] + m[i][2] * m[j][2];
            if (!(std::fabs(d - (i == j ? 1.0 : 0.0)) < eps)) return false;
        }
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    return det > 0;
}

// World-from-view pose of a rigid world-to-view matrix: q = quat(R^T), p = -R^T t. False when R is not a rotation.
inline bool viewPoseFromW2V(const double w2v[12], Pose& out) {
    double rt[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) rt[i][j] = w2v[j * 4 + i];
    if (!isRotation(rt)) return false;
    double p[3];
    for (int i = 0; i < 3; ++i) p[i] = -(rt[i][0] * w2v[3] + rt[i][1] * w2v[7] + rt[i][2] * w2v[11]);
    out = {quatFromRotation(rt), {float(p[0]), float(p[1]), float(p[2])}};
    return finite(out);
}

// Camera position -R^T t of a world-to-view matrix, in double (the engine keeps it at gfx+0xe48).
inline void viewPosition(const double w2v[12], double p[3]) {
    for (int i = 0; i < 3; ++i) p[i] = -(w2v[i] * w2v[3] + w2v[4 + i] * w2v[7] + w2v[8 + i] * w2v[11]);
}

// Previous view <- current view from both frames' world-to-view matrices and camera positions (-R^T t; the engine
// keeps them in doubles at gfx+0xe48): C2P = W2V_prev * W2V_cur^-1, i.e. R = Rp Rc^T, t = Rp (c_cur - c_prev).
// The translation comes from the position difference in double, so large world coordinates cancel exactly even
// though the rotations were built in float (R R^T = I only to ~1e-7, which times a 100 km t would be centimetres).
// The same pose as currentToPrevious() of the world-from-view poses. False when either matrix is not rigid.
inline bool currentToPreviousW2V(const double prev[12], const double prevPos[3], const double cur[12], const double curPos[3], Pose& out) {
    double rp[3][3], rc[3][3], r[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) rp[i][j] = prev[i * 4 + j], rc[i][j] = cur[i * 4 + j];
    if (!isRotation(rp) || !isRotation(rc)) return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = rp[i][0] * rc[j][0] + rp[i][1] * rc[j][1] + rp[i][2] * rc[j][2];
    const double d[3] = {curPos[0] - prevPos[0], curPos[1] - prevPos[1], curPos[2] - prevPos[2]};
    double t[3];
    for (int i = 0; i < 3; ++i) t[i] = rp[i][0] * d[0] + rp[i][1] * d[1] + rp[i][2] * d[2];
    out = {quatFromRotation(r), {float(t[0]), float(t[1]), float(t[2])}};
    return finite(out);
}
// The same with the positions derived from the matrices (-R^T t).
inline bool currentToPreviousW2V(const double prev[12], const double cur[12], Pose& out) {
    double pp[3], pc[3];
    viewPosition(prev, pp);
    viewPosition(cur, pc);
    return currentToPreviousW2V(prev, pp, cur, pc, out);
}

// The render box RenderView derives from the MPix limiter result for the current drawport (R27 s1.5; R25 s1.2):
// texture texW x texH, limiter output ow x oh, drawport box boxW x boxH. Without a 3D canvas (output == texture and
// no forced CAS) the box itself; otherwise the output clamped to gfx_pixMaxRenderTargetSize (maxRt > 0), scaled by
// box/texture with int64 truncation and snapped to the full output when within 0.99.
inline void flatRenderBox(int texW, int texH, int ow, int oh, int boxW, int boxH, bool forceCas, int maxRt, int& rbw, int& rbh) {
    if ((ow == texW && oh == texH && !forceCas) || texW <= 0 || texH <= 0) {
        rbw = boxW, rbh = boxH;
        return;
    }
    if (maxRt > 0) ow = ow < maxRt ? ow : maxRt, oh = oh < maxRt ? oh : maxRt;
    rbw = int(int64_t(boxW) * ow / texW);
    rbh = int(int64_t(boxH) * oh / texH);
    if (ow > 0 && float(rbw) / float(ow) > 0.99f) rbw = ow;
    if (oh > 0 && float(rbh) / float(oh) > 0.99f) rbh = oh;
}
}  // namespace core
