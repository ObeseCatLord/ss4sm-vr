#pragma once
#include <cstdint>
// Gadgets in the hand (docs/re/R12_melee_gadgets.md s7-s9). Not an immersive physics throw: as with the SS2 VR
// grenades, the hand holding the gadget sets it off with a gesture that fits it (core/gadget_use.hpp):
//   - the left squeeze takes the selected gadget into the hand (the native GadgetHolder, HUD command 9) or puts
//     it away again (the weapons held before, selected through the wheel's commit; HUD 9 with the holder up
//     would cycle to the next gadget); the right A/B cycle the gadget ring while the weapon wheel is held, also with one in hand;
//   - throwables (BlackHole, NerveGas, Decoy, MiniNuke, SM ArtilleryCall/Hoverboard): the other hand aims - the
//     native targeting arc and marker start at the gadget hand and follow the aiming hand's direction - and a
//     throwing motion of the gadget hand throws it with the native launch (speed, arc, target checks);
//   - CompanionDrone: a throwing motion; MedKit: the needle onto the other forearm; SeriousRage: to the mouth;
//     ExtraLife: to the chest; anything else (SeriousTime): the trigger.
// The gesture ends in the holder's own use through HUD command 0xc (a network call). While the gadget is in a
// hand that hand's trigger is kept from the virtual pad.
// Options: gadget_hand_aim (default 1: the module), gadget_motion (1; 0 = the trigger for every gadget, and the
// left squeeze quick-uses), gadget_aim_hand (1), gadget_throw_velocity (0: 1 = direction and speed from the
// hand's own velocity, scaled by throw_gain percent, default 200).
namespace gadgets {
bool install();
// Desktop (mp_guards) server: only the hooks that aim remote VR players' throws by their hands.
bool installAuthority();
// Simulation thread, once per tick of the local VR player (from its weapon steps).
void onTick(uint8_t* player);
// Present thread: the gadget in this XR hand (0 left, 1 right) takes its trigger.
bool triggerTaken(int hand);
// Present thread: an XR hand (0/1) to pulse once (a throw refused for lack of a target), or -1.
int takeBuzz();
}  // namespace gadgets
