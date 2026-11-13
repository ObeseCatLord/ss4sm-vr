// Multiplayer channel protocol checks (core/net_proto.hpp).
#undef NDEBUG
#include "core/net_proto.hpp"
#include <cassert>

using namespace core::net;

int main() {
    // the engine's hash: h = h * 0x1003f + (signed char)c
    static_assert(hash("") == 0 && hash("A") == 0x41 && hash("AB") == 0x41u * 0x1003fu + 0x42u, "hash");
    static_assert(hash("\xff") == 0xffffffffu, "hash sign-extends");
    assert(kHelloHash != kAckHash && kAckHash != kPoseHash && kHelloHash != kPoseHash);
    // the content's script RPCs are named RPC_<side>_<reliable_>name (Network.lua): ours never collide in form
    assert(hash("RPC_server_reliable_SSVR.Hello.1") != kHelloHash);

    // handshake: same build -> accepted; other build or protocol -> refused with the reason, no kick
    Hello h;
    h.build = 616154, h.textCrc = 0xf07c69b9, h.playerIndex = 2, h.caps = CapVr;
    Peers peers;
    Ack a = peers.onHello(3, h, 616154, 0xf07c69b9);
    assert(a.accepted && a.reason == Accepted && a.playerIndex == 2 && peers.slot[3].acked);
    Hello other = h;
    other.build = 628492;
    assert(!answer(other, 616154, 0xf07c69b9).accepted && answer(other, 616154, 0xf07c69b9).reason == BadBuild);
    other = h, other.proto = 9;
    assert(answer(other, 616154, 0xf07c69b9).reason == BadProto);
    other = h, other.magic = 0;
    assert(answer(other, 616154, 0xf07c69b9).reason == Malformed);
    Ack refused = peers.onHello(4, other, 616154, 0xf07c69b9);
    assert(!refused.accepted && !peers.slot[4].acked);

    // poses: only an acknowledged sender's own player, newer sequences only
    PoseMsg m;
    m.playerIndex = 2, m.pose.sequence = 10;
    assert(peers.onPose(3, m));
    assert(!peers.onPose(3, m));  // same sequence
    m.pose.sequence = 11;
    assert(peers.onPose(3, m));
    m.playerIndex = 5, m.pose.sequence = 12;
    assert(!peers.onPose(3, m));  // not its player
    m.playerIndex = 2;
    assert(!peers.onPose(4, m) && !peers.onPose(-1, m) && !peers.onPose(18, m));  // unacked / invalid slots
    m.pose.magic = 0, m.pose.sequence = 13;
    assert(!peers.onPose(3, m));  // malformed
    peers.drop(3);
    m.pose = {}, m.pose.sequence = 14;
    assert(!peers.onPose(3, m));

    // keepalive: a repeated Hello from the same player keeps the sequence; silence expires the slot
    {
        Peers k;
        k.onHello(1, h, 616154, 0xf07c69b9, 1000);
        PoseMsg q;
        q.playerIndex = 2, q.pose.sequence = 50;
        assert(k.onPose(1, q, 2000));
        k.onHello(1, h, 616154, 0xf07c69b9, 6000);
        assert(k.slot[1].acked && k.slot[1].anyPose && k.slot[1].lastSequence == 50);
        assert(!k.onPose(1, q, 6100));  // still not newer
        assert(k.expire(6000 + kPeerTimeoutMs) == 0 && k.slot[1].acked);
        assert(k.expire(6001 + kPeerTimeoutMs) == 1 && !k.slot[1].acked);
        Hello other2 = h;
        other2.playerIndex = 7;
        k.onHello(1, h, 616154, 0xf07c69b9, 1);
        k.onPose(1, q, 2);
        k.onHello(1, other2, 616154, 0xf07c69b9, 3);  // another player on the slot: a fresh sequence
        assert(k.slot[1].playerIndex == 7 && !k.slot[1].anyPose);
    }

    // reading checks the exact size
    Hello back;
    assert(read(&h, sizeof h, back) && back.playerIndex == 2 && !read(&h, sizeof h - 1, back) && !read(nullptr, sizeof h, back));
    return 0;
}
