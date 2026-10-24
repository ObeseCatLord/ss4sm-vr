#pragma once
// VR controllers -> virtual gamepad. Every action reaches the game through its own gamepad bindings
// (DefaultGamepadBindings.lua, GBL_STANDARD), so commands stay native and multiplayer-safe:
//   LS move, RS look, RT plcmdFire, LT plcmdAltFire (left weapon when dual wielding), A plcmdY+ (jump),
//   B plcmdY- (crouch), X plcmdReload, Y plcmdGadgetQuickUse, LB plcmdShowWeaponWheel (hold: native wheel,
//   tap: previous weapon), RB plcmdToggleDualWielding, LS click plcmdToggleSprint, RS click plcmdUse,
//   Start menu.
// The native weapon wheel (R3 s6) selects with the right stick for the primary hand and the left stick for
// the second hand. While the wheel button is held, each hand's displacement from where it was when the
// button went down acts as that hand's stick (move the hand towards the item), unless the physical
// stick is pushed further.
// Per-profile layout (semantic action -> button):
//   Valve Index: wheel L trackpad press, dual-wield R trackpad press, jump R A, crouch R B, reload L A,
//                menu L B, gadget L grip, use R stick click, sprint L stick click.
//   Oculus Touch: wheel Y, jump A, crouch B, reload X, menu left menu, gadget L grip, use R stick click,
//                sprint L stick click (dual wielding through the wheel's second-hand selection).
//   HTC Vive (trackpads are the axes): wheel L trackpad click, jump R trackpad click, crouch R grip,
//                use R menu, menu L menu, gadget L grip (reload is automatic when empty).
//   Microsoft motion: wheel L trackpad click, dual-wield R trackpad click, jump R menu, crouch R grip,
//                menu L menu, gadget L grip, use R stick click, sprint L stick click.
//   Simple controller: trigger fires, menu opens the menu.
#include "core/math.hpp"
#include <cstdint>

namespace core::controls {

enum class Kind { Touch, Index, Vive, WMR, Simple, Unknown };

// Raw button bits (same values as xr::Button).
enum : uint32_t { Primary = 1u << 0, Secondary = 1u << 1, StickClick = 1u << 2, Menu = 1u << 3, TrackpadPress = 1u << 6 };

// XInput wButtons bit values (kept local so this header stays portable).
enum : uint16_t {
    PadStart = 0x0010, PadLeftThumb = 0x0040, PadRightThumb = 0x0080, PadLB = 0x0100, PadRB = 0x0200,
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
    Hand hand[2];           // 0 left, 1 right
    Quat head{0, 0, 0, 1};  // HMD orientation, play space
};

struct Pad {
    uint16_t buttons = 0;
    float lt = 0, rt = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;
};

// Squeeze counts as a grip above this value, and releases below 60 % of it (hysteresis).
inline float gripThreshold(Kind k) { return k == Kind::Index ? 0.4f : k == Kind::Touch ? 0.65f : 0.5f; }

constexpr float kWheelRadius = 0.08f;     // hand travel (m) that reads as a full stick deflection
constexpr float kWheelDeadZone = 0.015f;  // hand jitter below this is ignored

struct Mapper {
    bool wheelHeld = false;
    Vec3 origin[2]{};
    Quat frame{0, 0, 0, 1};  // yaw-only head orientation when the wheel button went down
    bool grip[2]{};

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
        switch (f.kind) {
        case Kind::Index:
            wheel = on(L, TrackpadPress);
            if (on(R, TrackpadPress)) b |= PadRB;
            if (on(R, Primary)) b |= PadA;
            if (on(R, Secondary)) b |= PadB;
            if (on(L, Primary)) b |= PadX;
            if (on(L, Secondary) || on(L, Menu) || on(R, Menu)) b |= PadStart;
            if (on(L, StickClick)) b |= PadLeftThumb;
            if (on(R, StickClick)) b |= PadRightThumb;
            if (grip[0]) b |= PadY;
            break;
        case Kind::Touch:
            wheel = on(L, Secondary);
            if (on(R, Primary)) b |= PadA;
            if (on(R, Secondary)) b |= PadB;
            if (on(L, Primary)) b |= PadX;
            if (on(L, Menu)) b |= PadStart;
            if (on(L, StickClick)) b |= PadLeftThumb;
            if (on(R, StickClick)) b |= PadRightThumb;
            if (grip[0]) b |= PadY;
            break;
        case Kind::Vive:
            wheel = on(L, TrackpadPress);
            if (on(R, TrackpadPress)) b |= PadA;
            if (grip[1]) b |= PadB;
            if (on(R, Menu)) b |= PadRightThumb;
            if (on(L, Menu)) b |= PadStart;
            if (grip[0]) b |= PadY;
            break;
        case Kind::WMR:
            wheel = on(L, TrackpadPress);
            if (on(R, TrackpadPress)) b |= PadRB;
            if (on(R, Menu)) b |= PadA;
            if (grip[1]) b |= PadB;
            if (on(L, Menu)) b |= PadStart;
            if (on(L, StickClick)) b |= PadLeftThumb;
            if (on(R, StickClick)) b |= PadRightThumb;
            if (grip[0]) b |= PadY;
            break;
        case Kind::Simple:
        case Kind::Unknown:
            if (on(L, Menu) || on(R, Menu)) b |= PadStart;
            break;
        }
        if (wheel) {
            b |= PadLB;
            if (!wheelHeld) {
                origin[0] = L.pos, origin[1] = R.pos;
                frame = yaw(yawAngle(f.head));
            }
            float sel[2][2];
            for (int h = 0; h < 2; ++h) {
                Vec3 d = rotate(inverse(frame), f.hand[h].pos - origin[h]);
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
        }
        wheelHeld = wheel;
        p.buttons = b;
        return p;
    }
};

}  // namespace core::controls
