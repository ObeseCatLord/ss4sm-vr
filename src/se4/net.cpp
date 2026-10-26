#include "se4/net.hpp"
#include "core/math.hpp"
#include "core/net_proto.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/player_vr.hpp"
#include "xr/input.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cstring>

namespace net {
namespace {
using namespace core::net;
using CtorFn = void(__cdecl*)(void* msg);
using AppendFn = void(__cdecl*)(void* array, const void* data, int len);
using ProcessFn = void(__cdecl*)(void* ctx, void* netif, uint8_t* msg);
using SendFn = void(__cdecl*)(void* netif, void* msg);
using SendOneFn = void(__cdecl*)(void* netif, void* msg, const int* slot);
using PiFn = uint8_t*(__cdecl*)(void* entity);
using WorldFn = uint8_t*(__cdecl*)(void* pi);

CtorFn nmCtor, nmDtor;
AppendFn appendBytes;
ProcessFn realProcess;
void* g_clientSend;
void* g_serverBroadcast;
SendOneFn serverSendOne;
PiFn entityPi;
WorldFn piWorld;
bool g_log;
constexpr uint32_t kSenderSlot = 0xd790;  // server: the client slot being processed (R13 s2.4)
constexpr uint32_t kRenderPose = 0x3c;     // renderable placement: quat xyzw +0x3c, position +0x4c (R9)

// Client state (simulation thread) and the shared tables (Process may run on another thread).
struct Client {
    void* netif = nullptr;
    bool acked = false, refused = false;
    int tries = 0;
    ULONGLONG lastHello = 0, lastPose = 0;
    uint32_t sequence = 0;
} g_client;
SRWLOCK g_lock = SRWLOCK_INIT;
Peers g_peers;  // server side
struct Remote {
    int32_t playerIndex = -1;
    core::wire::PosePacket pose;
    ULONGLONG at = 0;
} g_remote[Peers::kSlots + 1];

bool readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }

// The world's network interface (client or server object), validated by its send slot.
void* netifOf(void* player) {
    if (!player || !entityPi || !piWorld) return nullptr;
    uint8_t* pi = entityPi(player);
    uint8_t* world = pi ? piWorld(pi) : nullptr;
    if (!readable(world + 0x1c0, 8)) return nullptr;
    uint8_t* net = *reinterpret_cast<uint8_t**>(world + 0x1c0);
    if (!readable(net + 0x230, 8)) return nullptr;
    void* netif = *reinterpret_cast<void**>(net + 0x230);
    if (!readable(netif, 8) || !readable(*static_cast<void***>(netif), 0xf0)) return nullptr;
    void* send = (*static_cast<void***>(netif))[0xe0 / 8];
    return send == g_clientSend || send == g_serverBroadcast ? netif : nullptr;
}
bool isServer(void* netif) { return (*static_cast<void***>(netif))[0xe0 / 8] == g_serverBroadcast; }

// One mod message: built, sent (the engine clones it on enqueue) and destroyed like CallScriptRPC's.
void send(void* netif, uint32_t hash, bool reliable, const void* data, int len, const int* slot) {
    alignas(16) uint8_t msg[0x40] = {};
    nmCtor(msg);
    *reinterpret_cast<int32_t*>(msg + 0x18) = reliable ? 1 : 0;
    *reinterpret_cast<uint32_t*>(msg + 0x1c) = hash;
    appendBytes(msg + 0x20, data, len);
    if (slot) serverSendOne(netif, msg, slot);
    else reinterpret_cast<SendFn>((*static_cast<void***>(netif))[0xe0 / 8])(netif, msg);
    nmDtor(msg);
}

int32_t playerIndexOf(void* player) {
    const uint32_t off = se4::build().off(se4::gen::CPlayerActorPuppetEntity_en_iPlayerIndex);
    return player && off ? *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(player) + off) : -1;
}
Hello myHello(void* localPlayer) {
    Hello h;
    h.modVersion = 1;
    h.build = se4::build().table->build;
    h.textCrc = se4::build().table->textCrc32;
    h.playerIndex = localPlayer ? playerIndexOf(localPlayer) : -1;
    h.caps = localPlayer ? CapVr : 0;
    return h;
}

void storeRemote(const PoseMsg& m) {
    AcquireSRWLockExclusive(&g_lock);
    Remote* free = nullptr;
    for (auto& r : g_remote) {
        if (r.playerIndex == m.playerIndex) free = &r;
        else if (!free && (r.playerIndex < 0 || GetTickCount64() - r.at > 5000)) free = &r;
    }
    if (free) free->playerIndex = m.playerIndex, free->pose = m.pose, free->at = GetTickCount64();
    ReleaseSRWLockExclusive(&g_lock);
}

// The local VR player's head and hands relative to its renderable placement.
bool localPose(void* vrd, PoseMsg& out) {
    auto* player = static_cast<uint8_t*>(*static_cast<void**>(vrd));
    if (!player) return false;
    const float* q = reinterpret_cast<const float*>(player + kRenderPose);
    const core::Pose origin{core::normalize({q[0], q[1], q[2], q[3]}), {q[4], q[5], q[6]}};
    const core::Pose inv = core::inverse(origin);
    float head[7], hand[2][7];
    player_vr::headWorld(vrd, head);
    player_vr::controllerWorld(vrd, 1, hand[0]);  // engine role 1 = left
    player_vr::controllerWorld(vrd, 0, hand[1]);
    auto rel = [&](const float* p) { return core::compose(inv, {core::normalize({p[0], p[1], p[2], p[3]}), {p[4], p[5], p[6]}}); };
    const core::Pose poses[3] = {rel(head), rel(hand[0]), rel(hand[1])};
    const auto& in = xr::input();
    const float trig[2] = {in.hand(0).trigger, in.hand(1).trigger}, grip[2] = {in.hand(0).squeeze, in.hand(1).squeeze};
    uint8_t flags = core::wire::HeadValid;
    if (in.hand(0).active) flags |= core::wire::LeftValid;
    if (in.hand(1).active) flags |= core::wire::RightValid;
    if (grip[0] > 0.5f) flags |= core::wire::LeftGrip;
    if (grip[1] > 0.5f) flags |= core::wire::RightGrip;
    out.playerIndex = playerIndexOf(player);
    out.pose = core::wire::encode(++g_client.sequence, poses, flags, trig, grip);
    return core::finite(poses[0]) && core::finite(poses[1]) && core::finite(poses[2]);
}

void __cdecl hookedProcess(void* ctx, void* netif, uint8_t* msg) {
    const uint32_t h = msg ? *reinterpret_cast<uint32_t*>(msg + 0x1c) : 0;
    if (h != kHelloHash && h != kAckHash && h != kPoseHash) return realProcess(ctx, netif, msg);
    // Ours: never reaches the engine's name lookup or Lua.
    const void* data = *reinterpret_cast<void**>(msg + 0x20);
    const int n = *reinterpret_cast<int32_t*>(msg + 0x28);
    if (!netif || !readable(*static_cast<void***>(netif), 0xf0) || n <= 0 || n > 256) return;
    const bool server = isServer(netif);
    const int slot = server ? *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(netif) + kSenderSlot) : -1;
    if (server && h == kHelloHash) {
        Hello hello;
        if (!read(data, size_t(n), hello)) return;
        AcquireSRWLockExclusive(&g_lock);
        const Ack ack = g_peers.onHello(slot, hello, se4::build().table->build, se4::build().table->textCrc32);
        ReleaseSRWLockExclusive(&g_lock);
        // To that client only (no gate); a broadcast carries the player index for the client to match.
        if (g_peers.valid(slot)) send(netif, kAckHash, true, &ack, sizeof ack, &slot);
        else send(netif, kAckHash, true, &ack, sizeof ack, nullptr);
        VRLOG("net: Hello from slot %d (player %d, build %u, proto %u, caps %x) - %s (reason %u)", slot, hello.playerIndex,
              hello.build, hello.proto, hello.caps, ack.accepted ? "accepted" : "refused", ack.reason);
    } else if (server && h == kPoseHash) {
        PoseMsg pose;
        if (!read(data, size_t(n), pose)) return;
        AcquireSRWLockExclusive(&g_lock);
        const bool relay = g_peers.onPose(slot, pose);
        bool to[Peers::kSlots] = {};
        if (relay)
            for (int s = 0; s < Peers::kSlots; ++s) to[s] = s != slot && g_peers.slot[s].acked;
        ReleaseSRWLockExclusive(&g_lock);
        if (!relay) return;
        storeRemote(pose);  // the server's own view of that player
        for (int s = 0; s < Peers::kSlots; ++s)
            if (to[s]) send(netif, kPoseHash, false, &pose, sizeof pose, &s);
        static int logged;
        if (g_log && logged++ % 300 == 0) VRLOG("net: pose from slot %d (player %d, seq %u) relayed", slot, pose.playerIndex, pose.pose.sequence);
    } else if (!server && h == kAckHash) {
        Ack ack;
        if (!read(data, size_t(n), ack) || ack.magic != kMagic || g_client.acked) return;
        const int32_t mine = playerIndexOf(player_vr::localVrd() ? *static_cast<void**>(player_vr::localVrd()) : nullptr);
        if (ack.playerIndex != mine && ack.playerIndex >= 0 && mine >= 0) return;  // a broadcast Ack for another client
        g_client.acked = ack.accepted != 0, g_client.refused = !ack.accepted;
        VRLOG(ack.accepted ? "net: server acknowledged the VR channel (build %u)" : "net: server refused the VR channel (build %u, reason %u)",
              ack.serverBuild, ack.reason);
    } else if (!server && h == kPoseHash) {
        PoseMsg pose;
        if (!read(data, size_t(n), pose) || pose.pose.magic != core::wire::kMagic) return;
        storeRemote(pose);
        static int logged;
        if (g_log && logged++ % 300 == 0) VRLOG("net: relayed pose of player %d (seq %u)", pose.playerIndex, pose.pose.sequence);
    }
}
}  // namespace

bool install() {
    if (!se4::options::get("mp_channel", 1)) return false;
    const auto& b = se4::build();
    nmCtor = reinterpret_cast<CtorFn>(b.at<void>(se4::gen::nm_script_rpc_ctor));
    nmDtor = reinterpret_cast<CtorFn>(b.at<void>(se4::gen::nm_script_rpc_dtor));
    appendBytes = reinterpret_cast<AppendFn>(b.at<void>(se4::gen::rpc_args_append));
    g_clientSend = b.at<void>(se4::gen::net_client_send);
    g_serverBroadcast = b.at<void>(se4::gen::net_server_broadcast);
    serverSendOne = reinterpret_cast<SendOneFn>(b.at<void>(se4::gen::net_server_send_one));
    entityPi = reinterpret_cast<PiFn>(b.at<void>(se4::gen::entity_pi));
    piWorld = reinterpret_cast<WorldFn>(b.at<void>(se4::gen::pi_world));
    void* process = b.at<void>(se4::gen::nm_script_rpc_process);
    g_log = se4::options::get("mp_log", 0) != 0;
    if (!nmCtor || !nmDtor || !appendBytes || !g_clientSend || !g_serverBroadcast || !serverSendOne || !entityPi || !piWorld || !process ||
        !b.off(se4::gen::CPlayerActorPuppetEntity_en_iPlayerIndex))
        return VRLOG("net: build lacks the channel symbols - no VR channel"), false;
    if (MH_CreateHook(process, reinterpret_cast<void*>(hookedProcess), reinterpret_cast<void**>(&realProcess)) != MH_OK ||
        MH_EnableHook(process) != MH_OK)
        return VRLOG("net: script RPC hook FAILED"), false;
    VRLOG("net: VR channel on the script RPC message (handshake proto %u)", unsigned(kProto));
    return true;
}

void tick(void* vrd, bool localVr) {
    if (!realProcess || !vrd) return;
    void* player = *static_cast<void**>(vrd);
    void* netif = netifOf(player);
    if (!netif) return;
    if (netif != g_client.netif) {  // a new session: start over
        g_client = {};
        g_client.netif = netif;
        AcquireSRWLockExclusive(&g_lock);
        g_peers = {};
        ReleaseSRWLockExclusive(&g_lock);
    }
    const ULONGLONG now = GetTickCount64();
    if (isServer(netif)) {
        // A VR player on the server (listen host) sends its pose to every acknowledged client.
        if (!localVr || now - g_client.lastPose < 33) return;
        g_client.lastPose = now;
        PoseMsg pose;
        if (!localPose(vrd, pose)) return;
        bool to[Peers::kSlots] = {};
        AcquireSRWLockShared(&g_lock);
        for (int s = 0; s < Peers::kSlots; ++s) to[s] = g_peers.slot[s].acked;
        ReleaseSRWLockShared(&g_lock);
        for (int s = 0; s < Peers::kSlots; ++s)
            if (to[s]) send(netif, kPoseHash, false, &pose, sizeof pose, &s);
        return;
    }
    // Client: Hello until acknowledged (every 2 s, 3 tries); a desktop peer says Hello from a remote VR player's tick.
    if (!g_client.acked && !g_client.refused) {
        if (g_client.tries < 3 && now - g_client.lastHello > 2000) {
            const Hello h = myHello(localVr ? player : nullptr);
            send(netif, kHelloHash, true, &h, sizeof h, nullptr);
            g_client.lastHello = now, ++g_client.tries;
            VRLOG("net: Hello sent (try %d, player %d, caps %x)", g_client.tries, h.playerIndex, h.caps);
            if (g_client.tries == 3) VRLOG("net: no answer yet - the server may not run the mod (flat-compatible, no VR channel)");
        }
        return;
    }
    if (!g_client.acked || !localVr || now - g_client.lastPose < 33) return;
    g_client.lastPose = now;
    PoseMsg pose;
    if (localPose(vrd, pose)) send(netif, kPoseHash, false, &pose, sizeof pose, nullptr);
}

bool remotePose(int32_t playerIndex, core::wire::PosePacket& out) {
    bool ok = false;
    AcquireSRWLockShared(&g_lock);
    for (auto& r : g_remote)
        if (r.playerIndex == playerIndex && GetTickCount64() - r.at < 500) out = r.pose, ok = true;
    ReleaseSRWLockShared(&g_lock);
    return ok;
}
}  // namespace net
