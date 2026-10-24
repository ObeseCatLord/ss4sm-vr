#pragma once
// Native projected crosshair for hand-aimed weapons (docs/re/R8_crosshair.md s8 A). SS4/SM contain the code
// that casts the aim ray from the weapon barrel and projects the game's own crosshair (textures, dispersion
// ring, colour coding, lock-on) onto that target, but an always-true predicate (merged into a shared
// `return 1` stub) keeps the head ray and the screen centre. The five calls to that predicate in the aim
// writer, the crosshair element and the object-holder element are redirected so they answer 0 for the local
// VR player only. The show predicate's own VR test (it hid the HUD crosshair for any player with vrd) is NOPed,
// and the crosshair's view call gets the eye offset of the eye being rendered so the crosshair has the right
// depth in both eyes (R8 addendum). Option native_crosshair (default 1).
namespace crosshair {
bool install();
}
