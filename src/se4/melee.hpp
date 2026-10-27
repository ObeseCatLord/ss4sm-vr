#pragma once
#include <cstdint>
// Physical melee: the hand-held knife hits with its real blade sweep (docs/re/R12_melee_gadgets.md).
namespace melee {
bool install();
// A knife or generic melee weapon (its step is one of the two melee steps): no crosshair for it.
bool isMeleeWeapon(uint8_t* weapon);
// Every CPlayerVRData step (simulation thread): prompted melee by swing (docs/re/R30_prompted_melee_swing.md).
void onVrdTick(void** vrd);
}
