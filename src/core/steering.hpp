#pragma once
// Grab-to-steer for vehicles whose model has a steering wheel or handlebar bone (optional immersive mode;
// thumbstick driving always remains available). The wheel turns by the change in angle of the grabbing
// hands around the steering axis, averaged over both hands; the result is clamped to the vehicle's
// maximum steering angle and becomes an analog steer value for the native steering command.
// Handlebars use the same model (axis = the bar's pivot axis, radius = half bar width).
#include "core/math.hpp"

namespace core {

struct SteerAxis {
    Vec3 center;  // bone origin (world)
    Vec3 axis;    // unit rotation axis (world); positive rotation = steering left (right-hand rule)
    Vec3 zero;    // unit vector in the wheel plane marking angle 0 (e.g. towards 12 o'clock)
};

struct SteeringGrab {
    bool held[2]{};
    float handAngle[2]{};  // hand angle at grab / last update (radians, around axis)
    float angle = 0;       // current wheel angle (radians)

    static float angleOf(const SteerAxis& a, Vec3 hand) {
        Vec3 r = hand - a.center;
        r = r - a.axis * dot(r, a.axis);  // project into the wheel plane
        Vec3 y = cross(a.axis, a.zero);
        return std::atan2(dot(r, y), dot(r, a.zero));
    }
    // Hands must be within `grabRadius` of the rim (or bar) to start holding.
    static bool nearRim(const SteerAxis& a, Vec3 hand, float rimRadius, float grabRadius) {
        Vec3 r = hand - a.center;
        float along = dot(r, a.axis);
        Vec3 inPlane = r - a.axis * along;
        float radial = std::sqrt(dot(inPlane, inPlane)) - rimRadius;
        return std::sqrt(radial * radial + along * along) <= grabRadius;
    }

    // grip[i]: the hand is squeezing; hands[i]: hand positions (0 left, 1 right). Returns the wheel angle.
    float update(const SteerAxis& a, const Vec3 hands[2], const bool grip[2], float rimRadius, float maxAngle,
                 float grabRadius = 0.12f) {
        float delta = 0;
        int n = 0;
        for (int i = 0; i < 2; ++i) {
            if (!grip[i]) {
                held[i] = false;
                continue;
            }
            float now = angleOf(a, hands[i]);
            if (!held[i]) {
                if (!nearRim(a, hands[i], rimRadius, grabRadius)) continue;
                held[i] = true;  // grabbing never moves the wheel
                handAngle[i] = now;
                continue;
            }
            delta += std::remainder(now - handAngle[i], 2 * Pi);
            handAngle[i] = now;
            ++n;
        }
        if (n) angle += delta / float(n);
        if (angle > maxAngle) angle = maxAngle;
        if (angle < -maxAngle) angle = -maxAngle;
        return angle;
    }
    bool holding() const { return held[0] || held[1]; }
    // Analog steer for the native command: +1 full left, -1 full right.
    float steer(float maxAngle) const { return maxAngle > 1e-4f ? angle / maxAngle : 0.0f; }
    // When nobody holds the wheel it follows the native vehicle (e.g. stick steering) instead.
    void follow(float nativeAngle) {
        if (!holding()) angle = nativeAngle;
    }
};

}  // namespace core
