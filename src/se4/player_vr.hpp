#pragma once
// The engine's own VR player layer (CPlayerVRData: hand-placed weapons, barrel-from-muzzle shots,
// head-tracked camera, anchor turning, hand/head pose replication). It exists for a player only when the
// player's psd_bIsPlayingInVR is set at puppet init, and SS4/SM's only native setter call passes a
// constant 0. This module turns it on for the local VR user and guards the engine's unguarded VR paths.
// See docs/re/R3_vr_gameplay.md (sections 1, 2, 7, 9).
namespace player_vr {
// Hooks SetPlayingInVR (local user forced to 1 while our device is installed), guards the null-vrd and
// null-device paths, and patches vrd_init's two device slot 0x208 calls with a null-checking thunk.
bool install();
// Called once our device is in the engine global: VR-controller input mode for controller placement.
void onDeviceInstalled();
// Byte offset of the player's CPlayerVRData* in this build (SS4 0x25c8, SM 0x25d0), 0 when unknown.
unsigned vrdOffset();
// The local player's CPlayerVRData (last vrd_init with the local flag set), or nullptr.
void* localVrd();
}  // namespace player_vr
