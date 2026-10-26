#pragma once
// VR controllers -> virtual gamepad. Every action reaches the game through its own gamepad bindings
// (DefaultGamepadBindings.lua, GBL_STANDARD), so commands stay native and multiplayer-safe:
//   LS move, RS look, RT plcmdFire, LT plcmdAltFire (left weapon when dual wielding), A plcmdY+ (jump),
//   B plcmdY- (crouch), X plcmdReload, LB plcmdShowWeaponWheel (hold: native wheel,
//   tap: previous weapon), RB plcmdToggleDualWielding, LS click plcmdToggleSprint, RS click plcmdUse,
//   Start menu. On Index/Touch the use/melee (kick, desktop E) is the right B, run through the engine's own VR use
//   path for the right hand (ButtonB is plcmdUse in DefaultVRInputControllerBindings), so it does not depend on the
//   saved pad layout; crouch is then the right stick click.
// The native weapon wheel (R3 s6) selects with the right stick for the primary hand and the left stick for
// the second hand. While the wheel button is held, each hand's displacement from where it was when the
// button went down acts as that hand's stick (move the hand towards the item), unless the physical
// stick is pushed further. While the wheel is held, the right hand's two face buttons (Index/Touch A, B)
// become D-pad up/down, which cycle the wheel's gadget ring (syscmdUp/Down; the gamepad mapping of those
// system commands is assumed to be the D-pad, runtime check pending) instead of jumping/crouching.
// Wheel spots (as Fusion's VR wheel buttons, beside the wheel): moving a wheel hand more than kSpotX to the side, past
// the wheel, points at a spot: left Menu (Netricsa above it), right Quick load (Quick save above it). That hand's
// stick is centred meanwhile, so releasing the wheel there keeps the weapon and runs the spot's action
// (src/se4/wheel_panel.cpp) instead of selecting.
// The gadget (left grip) does not go through the pad: src/se4/gadgets.cpp issues the player's quick-use HUD command
// directly, because pad Y's meaning depends on the saved gamepad layout.
// Per-profile layout (semantic action -> button):
//   Valve Index: wheel per hand (L/R trackpad hold; L tap previous weapon, R tap dual-wield toggle), jump R A or R grip,
//                use/melee kick R B, crouch R stick click, reload L A, menu L B, gadget L grip, sprint L stick click.
//   Oculus Touch: wheel Y, jump A or R grip, use/melee kick B, crouch R stick click, reload X, menu left menu, gadget L grip,
//                sprint L stick click (dual wielding through the wheel's second-hand selection).
//   HTC Vive (trackpads are the axes): wheel L trackpad click, jump R trackpad click, crouch R grip,
//                use R menu, menu L menu, gadget L grip (reload is automatic when empty).
//   Microsoft motion: wheel per hand (L/R trackpad click hold, as Index), jump R menu, crouch R grip,
//                menu L menu, gadget L grip, use R stick click, sprint L stick click.
//   Steam Frame (gamepad-style halves): wheel per hand (L/R bumper hold; L tap previous weapon, R tap dual-wield
//                toggle), jump R A or R grip, use/melee kick R B, reload R X, crouch R stick click, menu R menu or
//                L View, gadget L grip, sprint L stick click, D-pad up/down = pad D-pad.
//   Simple controller: trigger fires, menu opens the menu.
// Crushing a held item (a Sirian Artifact of Might): the trigger on every profile, plus a hard squeeze on Index
// (squeeze force) and the grip's second stage on the Steam Frame (crushSqueeze).
#include "core/math.hpp"
#include <cstdint>

namespace core::controls {

enum class Kind { Touch, Index, Vive, WMR, Simple, Frame, Unknown };

// Raw button bits (same values as xr::Button).
enum : uint32_t {
    Primary = 1u << 0, Secondary = 1u << 1, StickClick = 1u << 2, Menu = 1u << 3, TrackpadPress = 1u << 6,
    ButtonX = 1u << 7, ButtonY = 1u << 8, Bumper = 1u << 9, DpadUp = 1u << 10, DpadDown = 1u << 11, View = 1u << 14,
    SqueezeClick = 1u << 15,
};

// XInput wButtons bit values (kept local so this header stays portable).
enum : uint16_t {
    PadDpadUp = 0x0001, PadDpadDown = 0x0002, PadStart = 0x0010, PadLeftThumb = 0x0040, PadRightThumb = 0x0080, PadLB = 0x0100, PadRB = 0x0200,
    PadA = 0x1000, PadB = 0x2000, PadX = 0x4000, PadY = 0x8000,
};

struct Hand {
    bool active = false;
    float trigger = 0, squeeze = 0;
    float sx = 0, sy = 0;   // stick or trackpad, -1..1, +y up
    uint32_t buttons = 0;
    Vec3 pos{};             // grip position, play space metres
};

struct Frame {
    Kind kind = Kind::Unknown;
    uint64_t ms = 0;        // a millisecond clock (taps vs holds)
    Hand hand[2];           // 0 left, 1 right
    Quat head{0, 0, 0, 1};  // HMD orientation, play space
};

enum Spot : int8_t { SpotNone = -1, SpotMenu, SpotNetricsa, SpotQuickLoad, SpotQuickSave, SpotCount };
inline const char* spotName(int s) {
    return s == SpotMenu ? "Menu" : s == SpotNetricsa ? "Netricsa" : s == SpotQuickLoad ? "Quick Load" : s == SpotQuickSave ? "Quick Save" : "";
}
constexpr float kSpotX = 0.12f;   // sideways hand travel (m) past which the hand points at a spot
constexpr float kSpotUp = 0.05f;  // above this (m) the upper spot of that side

struct Pad {
    uint16_t buttons = 0;
    int8_t spot = SpotNone;        // the spot pointed at while a wheel is held
    int8_t spotAction = SpotNone;  // the spot the wheel was released on (one update)
    int8_t spotHand = 1;           // the hand that chose it (0 left, 1 right)
    bool kick = false;  // the use/melee (kick) button: src/se4/gadgets.cpp runs the engine's VR use path (not a pad button)
    float lt = 0, rt = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;
};

// Squeeze counts as a grip above this value, and releases below 60 % of it (hysteresis).
inline float gripThreshold(Kind k) { return k == Kind::Index ? 0.4f : k == Kind::Touch ? 0.65f : 0.5f; }

// A deliberate crush with the hand's grip (the trigger is checked by the caller): Index squeeze force well above
// the grab threshold, or the Steam Frame grip's second stage.
constexpr float kCrushForce = 0.85f;
inline bool crushSqueeze(Kind k, float squeeze, uint32_t buttons) {
    return (k == Kind::Index && squeeze >= kCrushForce) || (k == Kind::Frame && (buttons & SqueezeClick));
}

constexpr float kWheelRadius = 0.08f;     // hand travel (m) that reads as a full stick deflection
constexpr float kWheelDeadZone = 0.015f;  // hand jitter below this is ignored
constexpr uint64_t kTapMs = 250;          // a right trackpad press shorter than this is a tap (dual-wield toggle)
constexpr uint64_t kPulseMs = 120;        // how long a tap's pad button is held

struct Mapper {
    bool wheelHeld = false;
    Vec3 origin[2]{};
    Quat frame{0, 0, 0, 1};  // yaw-only head orientation when the wheel button went down
    bool grip[2]{};
    bool wheelSel[2]{true, true};  // which hands' selections the open wheel takes (per-hand wheels)
    bool rightPad = false;
    uint64_t rightPadAt = 0, dualPulseUntil = 0;
    int8_t spot = SpotNone, spotHand = 1;

    Pad update(const Frame& f) {
        Pad p;
        const Hand& L = f.hand[0];
        const Hand& R = f.hand[1];
        const float gt = gripThreshold(f.kind);
        for (int h = 0; h < 2; ++h) {
            float s = f.hand[h].squeeze;
            grip[h] = grip[h] ? s > gt * 0.6f : s > gt;
        }
        p.rt = R.trigger;
        p.lt = L.trigger;
        p.lx = L.sx, p.ly = L.sy;
        p.rx = R.sx, p.ry = R.sy;
        uint16_t b = 0;
        auto on = [](const Hand& h, uint32_t bit) { return (h.buttons & bit) != 0; };
        bool wheel = false;
        bool selL = true, selR = true;
        // Index / WMR: a wheel per hand. Holding a hand's trackpad opens the native wheel for that hand's weapon
        // only (both pads: both hands). The left tap stays the native wheel-button tap (previous weapon); a right
        // tap (shorter than kTapMs) toggles dual wielding.
        auto perHand = [&](bool lPad, bool rPad) {
            if (rPad && !rightPad) rightPadAt = f.ms;
            if (!rPad && rightPad && f.ms - rightPadAt < kTapMs) dualPulseUntil = f.ms + kPulseMs;
            rightPad = rPad;
            const bool rWheel = rPad && f.ms - rightPadAt >= kTapMs;
            wheel = lPad || rWheel;
            if (wheel) selL = lPad, selR = rWheel;
            if (f.ms < dualPulseUntil) b |= PadRB;
        };
        switch (f.kind) {
        case Kind::Index:
            perHand(on(L, TrackpadPress), on(R, TrackpadPress));
            if (on(R, Primary)) b |= PadA;
            p.kick = on(R, Secondary);
            if (grip[1]) b |= PadA;  // the right grip jumps too (user default); a busy grip reads as released here
            if (on(L, Primary)) b |= PadX;
            if (on(L, Secondary) || on(L, Menu) || on(R, Menu)) b |= PadStart;
            if (on(L, StickClick)) b |= PadLeftThumb;
            if (on(R, StickClick)) b |= PadB;
            break;
        case Kind::Touch:
            wheel = on(L, Secondary);
            if (on(R, Primary)) b |= PadA;
            p.kick = on(R, Secondary);
            if (grip[1]) b |= PadA;  // the right grip jumps too
            if (on(L, Primary)) b |= PadX;
            if (on(L, Menu)) b |= PadStart;
            if (on(L, StickClick)) b |= PadLeftThumb;
            if (on(R, StickClick)) b |= PadB;
            break;
        case Kind::Frame:
            perHand(on(L, Bumper), on(R, Bumper));
            if (on(R, Primary)) b |= PadA;
            p.kick = on(R, Secondary);
            if (grip[1]) b |= PadA;  // the right grip jumps too
            if (on(R, ButtonX)) b |= PadX;
            if (on(R, Menu) || on(L, View)) b |= PadStart;
            if (on(L, StickClick)) b |= PadLeftThumb;
            if (on(R, StickClick)) b |= PadB;
            if (on(L, DpadUp)) b |= PadDpadUp;
            if (on(L, DpadDown)) b |= PadDpadDown;
            break;
        case Kind::Vive:
            wheel = on(L, TrackpadPress);
            if (on(R, TrackpadPress)) b |= PadA;
            if (grip[1]) b |= PadB;
            if (on(R, Menu)) b |= PadRightThumb;
            if (on(L, Menu)) b |= PadStart;
            break;
        case Kind::WMR:
            perHand(on(L, TrackpadPress), on(R, TrackpadPress));
            if (on(R, Menu)) b |= PadA;
            if (grip[1]) b |= PadB;
            if (on(L, Menu)) b |= PadStart;
            if (on(L, StickClick)) b |= PadLeftThumb;
            if (on(R, StickClick)) b |= PadRightThumb;
            break;
        case Kind::Simple:
        case Kind::Unknown:
            if (on(L, Menu) || on(R, Menu)) b |= PadStart;
            break;
        }
        if (wheel && (f.kind == Kind::Index || f.kind == Kind::Touch || f.kind == Kind::Frame))
            b &= uint16_t(~PadA), p.kick = false;  // A/B cycle the gadget ring (src/se4/gadgets.cpp), not jump/kick
        if (wheel) {
            b |= PadLB;
            if (!wheelHeld || selL != wheelSel[0] || selR != wheelSel[1]) {  // (re)anchor when the wheel opens or a hand joins
                origin[0] = L.pos, origin[1] = R.pos;
                frame = yaw(yawAngle(f.head));
            }
            float sel[2][2];
            int8_t spots[2] = {SpotNone, SpotNone};
            for (int h = 0; h < 2; ++h) {
                Vec3 d = rotate(inverse(frame), f.hand[h].pos - origin[h]);
                if (f.hand[h].active && (h ? selR : selL) && std::fabs(d.x) > kSpotX)
                    spots[h] = d.x < 0 ? (d.y > kSpotUp ? SpotNetricsa : SpotMenu) : (d.y > kSpotUp ? SpotQuickSave : SpotQuickLoad);
                float x = d.x, y = d.y, len = std::sqrt(x * x + y * y);
                if (!f.hand[h].active || len < kWheelDeadZone) x = y = 0;
                else {
                    float m = std::fmin(len / kWheelRadius, 1.0f) / len;
                    x *= m, y *= m;
                }
                sel[h][0] = x, sel[h][1] = y;
            }
            // The physical stick wins when it is pushed further than the hand.
            auto pick = [](float& sx, float& sy, const float hs[2]) {
                if (hs[0] * hs[0] + hs[1] * hs[1] > sx * sx + sy * sy) sx = hs[0], sy = hs[1];
            };
            pick(p.lx, p.ly, sel[0]);
            pick(p.rx, p.ry, sel[1]);
            if (!selL || spots[0] != SpotNone) p.lx = p.ly = 0;  // a one-hand wheel leaves the other hand's weapon as it is;
            if (!selR || spots[1] != SpotNone) p.rx = p.ry = 0;  // a hand on a spot keeps its weapon
            spot = spots[1] != SpotNone ? spots[1] : spots[0];
            spotHand = spots[1] != SpotNone ? 1 : 0;
            p.spot = spot;
        } else if (wheelHeld && spot != SpotNone) {
            p.spotAction = spot;  // released on a spot
            spot = SpotNone;
        }
        p.spotHand = spotHand;
        wheelSel[0] = selL, wheelSel[1] = selR;
        wheelHeld = wheel;
        p.buttons = b;
        return p;
    }
};

}  // namespace core::controls
