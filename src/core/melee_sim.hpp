#pragma once
// Physical melee weapon: a rigid body that follows the controller target through critically damped linear
// and angular springs, so heavier weapons lag and carry inertia. The game layer casts the swept blade
// (previous -> current segment) against the world each step and reports contacts back; a contact stops the
// weapon at the surface (no tunnelling) and removes velocity into it. Units: metres, seconds, kilograms.
// Weapon frame: -Z along the blade from the grip, +Y up.
#include "core/math.hpp"

namespace core {

struct MeleeParams {
    float mass = 2.0f;             // kg (from native weapon parameters; heavier = more lag)
    float stiffness = 900.0f;      // N/m per kg-equivalent; scaled by 1/mass below
    float angularStiffness = 600.0f;
    float maxSpeed = 25.0f;        // m/s clamp for the grip point
    float maxAngularSpeed = 40.0f; // rad/s clamp
    float bladeStart = 0.10f;      // metres from the grip along -Z where the striking part begins
    float bladeEnd = 0.90f;        // ... and ends (tip)
};

struct MeleeContact {
    bool hit = false;
    float fraction = 1;  // along the step, 0 = previous pose, 1 = current
    Vec3 normal{};       // world-space surface normal
};

struct Segment { Vec3 a, b; };

struct MeleeSim {
    Pose pose{};
    Vec3 velocity{};
    Vec3 angularVelocity{};  // world space, rad/s
    bool seeded = false;

    void reset(Pose start) {
        pose = start;
        velocity = angularVelocity = {};
        seeded = true;
    }

    // Spring frequencies follow the mass: omega = sqrt(k/m), critical damping c = 2*sqrt(k*m).
    void step(const Pose& target, float dt, const MeleeParams& p) {
        if (!seeded || !finite(target)) {
            reset(target);
            return;
        }
        if (dt <= 0) return;
        dt = dt > 0.05f ? 0.05f : dt;
        const float m = p.mass > 0.1f ? p.mass : 0.1f;
        // Linear: a = (k*(x_t - x) - c*v)/m with k = stiffness (independent of mass) so heavier lags more.
        const float k = p.stiffness, c = 2 * std::sqrt(k * m);
        Vec3 acc = ((target.p - pose.p) * k - velocity * c) * (1.0f / m);
        velocity = velocity + acc * dt;
        float speed = std::sqrt(dot(velocity, velocity));
        if (speed > p.maxSpeed) velocity = velocity * (p.maxSpeed / speed);
        pose.p = pose.p + velocity * dt;
        // Angular: error rotation target * inverse(current) as axis * angle.
        Quat err = normalize(multiply(normalize(target.q), inverse(normalize(pose.q))));
        if (err.w < 0) err = {-err.x, -err.y, -err.z, -err.w};
        float s = std::sqrt(err.x * err.x + err.y * err.y + err.z * err.z);
        Vec3 axisAngle{};
        if (s > 1e-6f) {
            float angle = 2 * std::atan2(s, err.w);
            axisAngle = Vec3{err.x, err.y, err.z} * (angle / s);
        }
        const float ka = p.angularStiffness, ca = 2 * std::sqrt(ka * m);
        Vec3 alpha = (axisAngle * ka - angularVelocity * ca) * (1.0f / m);
        angularVelocity = angularVelocity + alpha * dt;
        float w = std::sqrt(dot(angularVelocity, angularVelocity));
        if (w > p.maxAngularSpeed) angularVelocity = angularVelocity * (p.maxAngularSpeed / w);
        float wl = std::sqrt(dot(angularVelocity, angularVelocity));
        if (wl > 1e-6f) {
            float half = 0.5f * wl * dt, sn = std::sin(half) / wl;
            Quat dq{angularVelocity.x * sn, angularVelocity.y * sn, angularVelocity.z * sn, std::cos(half)};
            pose.q = normalize(multiply(dq, pose.q));
        }
    }

    Segment blade(const Pose& at, const MeleeParams& p) const {
        return {at.p + rotate(at.q, {0, 0, -p.bladeStart}), at.p + rotate(at.q, {0, 0, -p.bladeEnd})};
    }
    Segment blade(const MeleeParams& p) const { return blade(pose, p); }

    // World velocity of a point on the blade at distance d (metres along -Z): v + w x r.
    Vec3 pointVelocity(float d) const {
        Vec3 r = rotate(pose.q, {0, 0, -d});
        return velocity + cross(angularVelocity, r);
    }
    float tipSpeed(const MeleeParams& p) const {
        Vec3 v = pointVelocity(p.bladeEnd);
        return std::sqrt(dot(v, v));
    }

    // Apply a world contact found by sweeping previous->current blade: move back to the contact fraction,
    // push out along the normal and remove velocity into the surface (both linear and the tip's angular).
    void applyContact(const Pose& previous, const MeleeContact& c) {
        if (!c.hit) return;
        float f = c.fraction < 0 ? 0 : c.fraction > 1 ? 1 : c.fraction;
        pose.p = previous.p + (pose.p - previous.p) * f;
        // orientation: slerp-like blend via normalized lerp (small steps)
        Quat a = previous.q, b = pose.q;
        if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = {-b.x, -b.y, -b.z, -b.w};
        pose.q = normalize(Quat{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f});
        float vn = dot(velocity, c.normal);
        if (vn < 0) velocity = velocity - c.normal * vn;
        pose.p = pose.p + c.normal * 0.002f;
        angularVelocity = angularVelocity * 0.5f;
    }
};

}  // namespace core
