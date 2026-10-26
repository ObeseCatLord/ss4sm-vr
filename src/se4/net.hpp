#pragma once
// The mod's multiplayer channel (docs/re/R13_mp_channel_upscaler.md s3, protocol in core/net_proto.hpp): mod messages
// travel as the engine's script RPC message under mod-private hashes. CNMScriptRPC::Process is detoured to consume
// them before the engine's name lookup; unmodded peers never get one unless they sent a Hello, and one sent to them
// only logs a line there. Handshake (Hello / Ack with version and build), then VR poses (64-byte core::wire packets,
// ~30 Hz, unreliable) relayed by the server to the other acknowledged clients. Option mp_channel (default 1),
// mp_log (diagnostics).
#include "core/wire.hpp"
#include <cstdint>

namespace net {
bool install();
// Simulation thread, from the vrd tick of every VR player (`localVr`: the local player's). Drives the client
// handshake and the local VR player's pose sends; rate-limited internally.
void tick(void* vrd, bool localVr);
// True while this machine is a client of a remote server (the latest session's network interface is a client's). The
// local VR player then sends ClientActionVR and turns natively (player_vr), so the server has its head and hands.
bool clientSession();
// Finger curls (thumb..pinky, 0..1) of the local XR hand `hand` (0 left, 1 right) from its controller state.
void fingerCurls(int hand, float curl[5]);
// The latest relayed pose of a remote player (by en_iPlayerIndex) and its age; false if none within 0.5 s.
bool remotePose(int32_t playerIndex, core::wire::PosePacket& out);
}  // namespace net
