#pragma once
// Button prompts show the VR controllers (docs/re/R19_vr_button_prompts.md): the virtual pad device's control
// markup (xpad vt+0x38, XInput user 0 only) returns, for each pad control, the VR control that presses it
// (core/prompts.hpp: the game's Xbox glyph where the letter or shape matches, text otherwise), in the gameplay or
// the menu layout. Every prompt (HUD hints, tutorials, death screen, Netricsa and menu button bars) goes through
// it. Other pads and desktop play keep the engine's glyphs. Option prompts_vr (default 1).
namespace prompts {
bool install();
}
