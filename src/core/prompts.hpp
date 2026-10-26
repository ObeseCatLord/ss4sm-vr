#pragma once
// Button prompts for the VR controllers (docs/re/R19_vr_button_prompts.md). The game's prompts name the virtual
// pad's controls ("Press [A]"); for the local VR player each pad control is shown as the VR control that presses
// it (core/vr_controls.hpp): the game's own Xbox glyph where the VR button carries the same letter or shape
// (A/B/X/Y, triggers, sticks, bumpers, menu), short text otherwise ("L Grip", "R Trackpad"). Like Fusion's VR
// prompts, which used Xbox360 letter glyphs and text. nullptr keeps the engine's glyph (the stick axes).
#include "core/vr_controls.hpp"

namespace core::prompts {
// XInput control indices of the engine's xpad device (R19 s2.1).
enum Pad : int {
    Up = 0, Down, Left, Right, Start, Back, LeftThumb, RightThumb, LB, RB, A, B, X, Y, LT, RT, AxisFirst = 16, Count = 24
};


// The VR control for pad control `c` with controller kind `k` (gameplay layout, or the menu layout when `menu`).
// Returns either "img:<XboxImageName>" or plain text, or nullptr to keep the engine's own.
inline const char* control(controls::Kind k, int c, bool menu) {
    using controls::Kind;
    if (c >= AxisFirst) return nullptr;  // sticks / trackpads move: the stick glyphs fit every profile
    if (c == RT) return "img:XboxTriggerButtonR";
    if (c == LT) return "img:XboxTriggerButtonL";
    if (menu) {
        switch (c) {
        case A: return k == Kind::Vive ? "R Trackpad" : k == Kind::WMR ? "R Menu" : "img:XboxButtonA";
        case B: return k == Kind::Vive || k == Kind::WMR ? "R Grip" : "img:XboxButtonB";
        case LB: return k == Kind::Frame ? "img:XboxShoulderButtonL" : "L Grip";
        case RB: return k == Kind::Frame ? "img:XboxShoulderButtonR" : "R Grip";
        default: break;
        }
    }
    switch (k) {
    case Kind::Index:
        switch (c) {
        case A: return "img:XboxButtonA";                // right A (or the right grip)
        case B: return "img:XboxThumbstickTriggerR";     // crouch: right stick click
        case X: return "L A";                             // reload
        case Y: return "L Grip";                          // gadget
        case Start: return "L B";
        case LeftThumb: return "img:XboxThumbstickTriggerL";
        case RightThumb: return "img:XboxButtonB";       // use / melee: right B
        case LB: return "L Trackpad";                     // weapon wheel (hold)
        case RB: return "R Trackpad";                     // dual wield (tap)
        case Up: case Down: return c == Up ? "Wheel + R A" : "Wheel + R B";
        case Left: return "Wheel: Netricsa";              // the wheel's Netricsa spot
        default: return nullptr;
        }
    case Kind::Touch:
        switch (c) {
        case A: return "img:XboxButtonA";
        case B: return "img:XboxThumbstickTriggerR";
        case X: return "img:XboxButtonX";
        case Y: return "L Grip";
        case Start: return "img:XboxButtonMenu";
        case LeftThumb: return "img:XboxThumbstickTriggerL";
        case RightThumb: return "img:XboxButtonB";
        case LB: return "img:XboxButtonY";               // weapon wheel (hold Y)
        case RB: return "Wheel";
        case Up: case Down: return c == Up ? "Wheel + A" : "Wheel + B";
        case Left: return "Wheel: Netricsa";
        default: return nullptr;
        }
    case Kind::Frame:
        switch (c) {
        case A: return "img:XboxButtonA";
        case B: return "img:XboxThumbstickTriggerR";
        case X: return "img:XboxButtonX";
        case Y: return "L Grip";
        case Start: return "img:XboxButtonMenu";
        case LeftThumb: return "img:XboxThumbstickTriggerL";
        case RightThumb: return "img:XboxButtonB";
        case LB: return "img:XboxShoulderButtonL";
        case RB: return "img:XboxShoulderButtonR";
        case Up: return "img:XboxDPadU";
        case Down: return "img:XboxDPadD";
        case Left: return "Wheel: Netricsa";
        default: return nullptr;
        }
    case Kind::Vive:
        switch (c) {
        case A: return "R Trackpad";
        case B: return "R Grip";
        case Y: return "L Grip";
        case Start: return "L Menu";
        case RightThumb: return "R Menu";
        case LB: return "L Trackpad";
        case Left: return "Wheel: Netricsa";
        default: return nullptr;
        }
    case Kind::WMR:
        switch (c) {
        case A: return "R Menu";
        case B: return "R Grip";
        case Y: return "L Grip";
        case Start: return "L Menu";
        case LeftThumb: return "img:XboxThumbstickTriggerL";
        case RightThumb: return "img:XboxThumbstickTriggerR";
        case LB: return "L Trackpad";
        case RB: return "R Trackpad";
        case Left: return "Wheel: Netricsa";
        default: return nullptr;
        }
    default: return nullptr;  // simple controller / unknown: the engine's own
    }
}

// The engine markup for control(): an image tag for "img:" entries, else the text (the engine frames text).
// `buf` must hold 64 characters.
inline const char* markup(controls::Kind k, int c, bool menu, char* buf) {
    const char* s = control(k, c, menu);
    if (!s) return nullptr;
    if (s[0] == 'i' && s[1] == 'm' && s[2] == 'g' && s[3] == ':') {
        int n = 0;
        for (const char* p = "<img src=\""; *p;) buf[n++] = *p++;
        for (const char* p = s + 4; *p && n < 56;) buf[n++] = *p++;
        buf[n++] = '"', buf[n++] = '>', buf[n] = 0;
        return buf;
    }
    return s;
}
}  // namespace core::prompts
