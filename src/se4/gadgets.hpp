#pragma once
#include <cstdint>
// Gadgets thrown from the hand (docs/re/R12_melee_gadgets.md s7, s8; gate R6). The GadgetHolder already follows the
// controller like any weapon. A throwable launches from its launch mode's activation fields: +0x20 quaternion
// (-Z = direction), +0x3c speed, +0x40 lifetime, filled from the head-based targeting. For the local VR player the
// mod overwrites direction (and speed, for a real throw) right after the activation: a throwing motion of the
// holding hand gives its own direction and speed (scaled by throw_gain, percent, default 200), a still hand throws
// along where it points at the native speed. The spawn stays native and on the authority. Option gadget_hand_aim
// (default 1).
namespace gadgets {
bool install();
// Simulation thread, once per tick of the local VR player: the left squeeze quick-uses the selected gadget.
void onTick(uint8_t* player);
}  // namespace gadgets
