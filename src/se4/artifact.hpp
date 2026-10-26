#pragma once
// Sirian Artifact of Might (skill point crate) held in the VR hand and crushed on input (docs/re/R17_sirian_artifact.md).
// Using the crate plays the native generic weapon animation "Use" of the artifact (CPSGenericWeaponAnimation with the
// SirianInjector weapon); its ArtifactBroken anim event drives the crush VFX, the Netricsa skills tab and the weapon
// restore. For the local VR player the mod:
//   - lets the animation bring the artifact up into the hand, then holds its anim queue (speed ~0, time kept) at
//     artifact_hold_ms (default 1500: before the crack and the crush) and, on the authority, cancels the state's end
//     timer;
//   - releases both when the artifact is crushed by the hand holding it: the trigger, a hard squeeze on Index
//     (squeeze force) or the grip's second stage on the Steam Frame. Everything after the release is native;
//   - releases by itself after artifact_timeout_s (default 120; Siberian Mayhem grants the skill point only at the crush).
// While held, that hand's trigger and grip are kept from the virtual pad (no jump from the grip). Option artifact_crush (1).
namespace artifact {
bool install();
// Present thread: the artifact is held in this XR hand (0 left, 1 right) waiting to be crushed.
bool holding(int hand);
}  // namespace artifact
