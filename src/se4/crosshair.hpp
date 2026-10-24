#pragma once
// Native projected crosshair for hand-aimed weapons (docs/re/R8_crosshair.md s8 A). SS4/SM contain the code
// that casts the aim ray from the weapon barrel and projects the game's own crosshair (textures, dispersion
// ring, colour coding, lock-on) onto that target, but an always-true predicate (merged into a shared
// `return 1` stub) keeps the head ray and the screen centre. The five calls to that predicate in the aim
// writer, the crosshair element and the object-holder element are redirected so they answer 0 for the local
// VR player only. The show predicate's own VR test (it hid the HUD crosshair for any player with vrd) is NOPed,
// and the crosshair's view call gets the eye offset of the eye being rendered so the crosshair has the right
// depth in both eyes (R8 addendum). Option native_crosshair (default 1).
// One crosshair per weapon (R8 A7, option crosshair_per_weapon, default 1): each hand's gun draws its own
// crosshair at its own barrel's target with its own spread; melee weapons (knife, generic melee) draw none.
#include <cstdint>
namespace crosshair {
bool install();
// HUD render (main thread), for the crosshair element: runs `draw` once per hand that should show a crosshair,
// with that pass's target distance (engine units); the element must be rendered inside `draw`. False: render the
// element once natively (not the local VR player, no weapon, or the render's own early-outs).
bool forEachPass(uint8_t* player, void (*draw)(void* user, float dist), void* user);
}
