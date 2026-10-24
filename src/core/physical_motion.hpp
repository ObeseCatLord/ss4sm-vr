#pragma once
// Ported from SS2 VR src/common/physical_motion.hpp (MIT, SS2VR contributors).
#include "core/math.hpp"

namespace core {
// A LOCAL-space sampling heuristic, not calibrated XR velocity or a combat
// timer. The caller owns source/context identity and the existing trigger gate.
struct PhysicalMotionSample {
    bool known = false, fresh = false, down = false, quiet = false;
};
struct PhysicalMotion {
    Pose previous{};
    uint64_t sequence = 0, tickMs = 0;
    bool seeded = false, down = false;
    PhysicalMotionSample observation{};

    void reset() { *this = {}; }
    static bool valid(Pose pose) {
        if (!finite(pose)) return false;
        const auto q = pose.q;
        const double n = double(q.x)*q.x + double(q.y)*q.y + double(q.z)*q.z + double(q.w)*q.w;
        return n >= .99 && n <= 1.01;
    }
    PhysicalMotionSample sample(Pose pose, uint64_t sourceSequence, uint64_t sourceTick,
                                uint64_t now, bool available) {
        if (!available || !sourceSequence || !sourceTick || sourceTick > now ||
            now - sourceTick > 200 || !valid(pose)) {
            reset();
            return {};
        }
        if (seeded && sourceSequence == sequence && sourceTick == tickMs)
            return {observation.known, false, observation.down};
        if (seeded && (sourceSequence <= sequence || sourceTick <= tickMs)) {
            down = false; observation = {};
            return {}; // Keep the high-water baseline; replay cannot reseed it.
        }
        const bool consecutive = seeded && sourceSequence > sequence && sourceTick > tickMs &&
                                 sourceTick - tickMs <= 100;
        const Pose before = previous;
        const uint64_t elapsed = consecutive ? sourceTick - tickMs : 0;
        previous = pose; sequence = sourceSequence; tickMs = sourceTick; seeded = true;
        if (!consecutive) {
            down = false; observation = {};
            return {};
        }
        const double dx = double(pose.p.x) - before.p.x;
        const double dy = double(pose.p.y) - before.p.y;
        const double dz = double(pose.p.z) - before.p.z;
        const double distance = std::sqrt(dx*dx + dy*dy + dz*dz);
        const auto a = before.q, b = pose.q;
        const double na = double(a.x)*a.x + double(a.y)*a.y + double(a.z)*a.z + double(a.w)*a.w;
        const double nb = double(b.x)*b.x + double(b.y)*b.y + double(b.z)*b.z + double(b.w)*b.w;
        const double cosine = std::abs(double(a.x)*b.x + double(a.y)*b.y + double(a.z)*b.z + double(a.w)*b.w) /
                              std::sqrt(na*nb);
        const double arc = 2 * std::acos(std::clamp(cosine, 0., 1.));
        // A discontinuity seeds a new baseline and supplies no release witness.
        if (distance > .75 || arc > 1.8) {
            down = false; observation = {};
            return {};
        }
        const double scale = 30. / double(elapsed);
        const double displacement = distance * scale, rotation = arc * scale;
        if (displacement > .04 || rotation > .18) down = true;
        else if (displacement < .016 && rotation < .07) down = false;
        observation = {true, true, down, displacement < .016 && rotation < .07};
        return observation;
    }
};
} // namespace core
