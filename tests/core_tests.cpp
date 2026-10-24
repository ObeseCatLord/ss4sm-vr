// Host-side checks for engine-agnostic maths (no Windows, no OpenXR).
#undef NDEBUG
#include "core/math.hpp"
#include "core/stereo.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }

int main() {
    using namespace core;
    // compose/inverse round trip
    Pose a{normalize(Quat{0.1f, 0.7f, -0.2f, 0.6f}), {1, 2, 3}};
    Pose id = compose(a, inverse(a));
    assert(near(id.p.x, 0) && near(id.p.y, 0) && near(id.p.z, 0) && near(std::fabs(id.q.w), 1));
    // yaw rotates -Z forward towards -X for +90 degrees (right-handed, y up)
    Vec3 f = rotate(yaw(Pi / 2), {0, 0, -1});
    assert(near(f.x, -1) && near(f.z, 0));
    // matrix34 translation lands in indices 3/7/11 (engine Matrix3x4 layout)
    float m[12];
    matrix34({Quat{}, {4, 5, 6}}, m);
    assert(m[3] == 4 && m[7] == 5 && m[11] == 6 && m[0] == 1 && m[5] == 1 && m[10] == 1);
    // engine projection: symmetric FOV gives zero off-centre terms; shared vertical takes the union
    Fov L{-0.9f, 0.7f, 0.8f, -0.85f}, R{-0.7f, 0.9f, 0.82f, -0.8f};
    Fov s = sharedVertical(L, L, R);
    assert(s.left == L.left && s.right == L.right && s.up == 0.82f && s.down == -0.85f);
    float p[16];
    engineProjection({-0.8f, 0.8f, 0.8f, -0.8f}, 0.1f, 1000.f, p);
    assert(near(p[2], 0) && near(p[6], 0) && near(p[14], -1) && near(p[0], p[5]));
    engineProjection(L, 0.1f, 1000.f, p);
    // a point on the left frustum edge at depth n maps to x_ndc = -1
    float n = 0.1f, x = n * std::tan(L.left), z = -n;
    float clipX = p[0] * x + p[2] * z, clipW = p[14] * z;
    assert(near(clipX / clipW, -1, 1e-3f));
    std::puts("core_tests: ok");
    return 0;
}
