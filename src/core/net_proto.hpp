#pragma once
// The mod's multiplayer channel protocol (docs/re/R13_mp_channel_upscaler.md s3), engine-agnostic and host-tested.
// Messages ride the engine's script RPC message (CNMScriptRPC) under mod-private name hashes that are never interned,
// so an unmodded peer only logs "cannot resolve function name" and nothing else happens; the transport itself is
// src/se4/net.cpp.
//   Hello  client -> server, reliable, until acknowledged (every 2 s, 3 tries): version, build, player index, caps; once
//          acknowledged, again every kKeepaliveMs as a keepalive (the server forgets a slot not heard for kPeerTimeoutMs).
//   Ack    server -> that client, reliable: accepted or the reason; VR data flows only after an accepted Ack.
//   Pose   VR player -> server (unreliable, ~30 Hz); the server relays it to every other acknowledged client.
// Little-endian, packed. A peer with another protocol version or build is answered with accepted = 0 (no kick).
#include "core/wire.hpp"
#include <cstdint>
#include <cstring>

namespace core::net {

// The engine's name hash (Sam4 0x1410c2770): h = h * 0x1003f + (signed char)c.
constexpr uint32_t hash(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) h = h * 0x1003fu + uint32_t(int32_t(static_cast<signed char>(*s)));
    return h;
}
constexpr uint32_t kHelloHash = hash("SSVR.Hello.1"), kAckHash = hash("SSVR.Ack.1"), kPoseHash = hash("SSVR.Pose.1");
constexpr uint16_t kProto = 1;
constexpr uint32_t kMagic = 0x52565353;  // "SSVR"
constexpr uint64_t kKeepaliveMs = 5000, kPeerTimeoutMs = 15000;

enum Caps : uint32_t { CapVr = 1, CapBodyIk = 2, CapMelee = 4 };
enum Reason : uint8_t { Accepted = 0, BadProto = 1, BadBuild = 2, Malformed = 3 };

#pragma pack(push, 1)
struct Hello {
    uint32_t magic = kMagic;
    uint16_t proto = kProto;
    uint16_t modVersion = 0;
    uint32_t build = 0;        // 616154 (SS4) / 628492 (SM)
    uint32_t textCrc = 0;      // the exe's .text CRC (identifies the exact build)
    int32_t playerIndex = -1;  // en_iPlayerIndex of the sender's local player (-1: none yet)
    uint32_t caps = 0;
};
struct Ack {
    uint32_t magic = kMagic;
    uint16_t proto = kProto;
    uint8_t accepted = 0;
    uint8_t reason = 0;
    uint32_t serverBuild = 0;
    int32_t playerIndex = -1;  // the Hello's player index (lets a broadcast Ack find its client)
};
struct PoseMsg {
    int32_t playerIndex = -1;
    wire::PosePacket pose;
};
#pragma pack(pop)
static_assert(sizeof(Hello) == 24 && sizeof(Ack) == 16 && sizeof(PoseMsg) == 68, "wire sizes");

template <class T> bool read(const void* data, size_t n, T& out) {
    if (!data || n != sizeof(T)) return false;
    std::memcpy(&out, data, sizeof out);
    return true;
}

// The server's answer to a Hello.
inline Ack answer(const Hello& h, uint32_t serverBuild, uint32_t serverCrc) {
    Ack a;
    a.serverBuild = serverBuild;
    a.playerIndex = h.playerIndex;
    if (h.magic != kMagic) a.reason = Malformed;
    else if (h.proto != kProto) a.reason = BadProto;
    else if (h.build != serverBuild || h.textCrc != serverCrc) a.reason = BadBuild;
    else a.accepted = 1, a.reason = Accepted;
    return a;
}

// Server side: the acknowledged clients by connection slot (the engine has 18).
struct Peers {
    static constexpr int kSlots = 18;
    struct Peer {
        bool acked = false;
        int32_t playerIndex = -1;
        uint32_t caps = 0;
        uint32_t lastSequence = 0;
        bool anyPose = false;
        uint64_t heard = 0;  // the time (ms) of its last Hello or Pose
    } slot[kSlots];

    bool valid(int s) const { return s >= 0 && s < kSlots; }
    // Records a Hello from `s` and returns the Ack to send back to it. A keepalive Hello from the same accepted player
    // keeps its pose sequence.
    Ack onHello(int s, const Hello& h, uint32_t serverBuild, uint32_t serverCrc, uint64_t now = 0) {
        const Ack a = answer(h, serverBuild, serverCrc);
        if (valid(s)) {
            Peer& p = slot[s];
            const bool same = p.acked && a.accepted && p.playerIndex == h.playerIndex;
            p = {a.accepted != 0, h.playerIndex, h.caps, same ? p.lastSequence : 0, same && p.anyPose, now};
        }
        return a;
    }
    // A Pose from `s`: true when it should be relayed (acknowledged sender, its own player, newer than the last).
    bool onPose(int s, const PoseMsg& m, uint64_t now = 0) {
        if (!valid(s) || !slot[s].acked || m.playerIndex != slot[s].playerIndex || m.pose.magic != wire::kMagic) return false;
        Peer& p = slot[s];
        if (p.anyPose && int32_t(m.pose.sequence - p.lastSequence) <= 0) return false;
        p.lastSequence = m.pose.sequence, p.anyPose = true, p.heard = now;
        return true;
    }
    void drop(int s) {
        if (valid(s)) slot[s] = {};
    }
    // Forgets acknowledged slots not heard for `timeout` (a client that left: its slot may be reused by an unmodded one).
    // Returns how many were dropped.
    int expire(uint64_t now, uint64_t timeout = kPeerTimeoutMs) {
        int n = 0;
        for (auto& p : slot)
            if (p.acked && now - p.heard > timeout) p = {}, ++n;
        return n;
    }
};

}  // namespace core::net
