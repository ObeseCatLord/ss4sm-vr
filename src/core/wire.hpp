#pragma once
// Compact network encoding of a VR player's tracked pose (head + two hands, relative to the player's
// origin), sent unreliably ~30 Hz and relayed by the server. Receivers solve IK locally (core/body_ik.hpp);
// a pose older than 0.5 s falls back to the native body. Design follows eduke32_vr's VRBodyPose notes;
// layout and quantization are this mod's own. Little-endian, 64 bytes.
#include "core/math.hpp"
#include <cstdint>
#include <cstring>

namespace core::wire {

constexpr uint16_t kMagic = 0x5653;  // "SV"
constexpr uint8_t kVersion = 1;

enum PoseFlags : uint8_t {
    HeadValid = 1, LeftValid = 2, RightValid = 4, LeftGrip = 8, RightGrip = 16, Crouch = 32,
};

#pragma pack(push, 1)
struct PosePacket {
    uint16_t magic = kMagic;
    uint8_t version = kVersion;
    uint8_t flags = 0;
    uint32_t sequence = 0;     // sender frame counter (drop older)
    int16_t pos[3][3]{};       // head, left, right: millimetres relative to the player origin (+-32 m)
    uint32_t rot[3]{};         // smallest-three quaternions, 10 bits per component + 2-bit index
    uint8_t trigger[2]{}, grip[2]{};  // 0..255
    uint8_t weaponSlot[2]{};   // native weapon id per hand (presentation only)
    uint8_t reserved[20]{};
};
#pragma pack(pop)
static_assert(sizeof(PosePacket) == 64, "PosePacket must stay 64 bytes");

inline int16_t packMm(float m) {
    float v = std::round(m * 1000.0f);
    return int16_t(v < -32767 ? -32767 : v > 32767 ? 32767 : v);
}
inline float unpackMm(int16_t v) { return float(v) / 1000.0f; }

// Smallest-three: drop the largest |component| (sign fixed positive), store the other three in
// [-1/sqrt2, 1/sqrt2] with 10 bits each.
inline uint32_t packQuat(Quat q) {
    q = normalize(q);
    float c[4] = {q.x, q.y, q.z, q.w};
    int big = 0;
    for (int i = 1; i < 4; ++i)
        if (std::fabs(c[i]) > std::fabs(c[big])) big = i;
    float sign = c[big] < 0 ? -1.0f : 1.0f;
    uint32_t out = uint32_t(big) << 30;
    int shift = 20;
    const float range = 0.70710678f;
    for (int i = 0; i < 4; ++i) {
        if (i == big) continue;
        float v = c[i] * sign / range;  // -1..1
        v = v < -1 ? -1 : v > 1 ? 1 : v;
        uint32_t qv = uint32_t(std::lround((v * 0.5f + 0.5f) * 1023.0f));
        out |= qv << shift;
        shift -= 10;
    }
    return out;
}
inline Quat unpackQuat(uint32_t p) {
    int big = int(p >> 30);
    float c[4];
    int shift = 20;
    float sum = 0;
    const float range = 0.70710678f;
    for (int i = 0; i < 4; ++i) {
        if (i == big) continue;
        float v = (float((p >> shift) & 1023u) / 1023.0f - 0.5f) * 2.0f * range;
        c[i] = v;
        sum += v * v;
        shift -= 10;
    }
    c[big] = std::sqrt(std::fmax(0.0f, 1.0f - sum));
    return normalize(Quat{c[0], c[1], c[2], c[3]});
}

inline PosePacket encode(uint32_t seq, const Pose poses[3], uint8_t flags, const float trig[2], const float grip[2]) {
    PosePacket p;
    p.sequence = seq;
    p.flags = flags;
    for (int i = 0; i < 3; ++i) {
        p.pos[i][0] = packMm(poses[i].p.x);
        p.pos[i][1] = packMm(poses[i].p.y);
        p.pos[i][2] = packMm(poses[i].p.z);
        p.rot[i] = packQuat(poses[i].q);
    }
    for (int h = 0; h < 2; ++h) {
        p.trigger[h] = uint8_t(std::lround(std::fmin(std::fmax(trig[h], 0.0f), 1.0f) * 255));
        p.grip[h] = uint8_t(std::lround(std::fmin(std::fmax(grip[h], 0.0f), 1.0f) * 255));
    }
    return p;
}

inline bool decode(const void* data, size_t size, PosePacket& out) {
    if (size != sizeof(PosePacket)) return false;
    std::memcpy(&out, data, sizeof out);
    return out.magic == kMagic && out.version == kVersion;
}

inline Pose posePart(const PosePacket& p, int i) {
    return {unpackQuat(p.rot[i]), {unpackMm(p.pos[i][0]), unpackMm(p.pos[i][1]), unpackMm(p.pos[i][2])}};
}

}  // namespace core::wire
