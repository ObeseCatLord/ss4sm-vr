// VR controller -> virtual gamepad mapping checks (core/vr_controls.hpp).
#undef NDEBUG
#include "core/vr_controls.hpp"
#include <cassert>
#include <cmath>

using namespace core;
using namespace core::controls;

static Frame idle(Kind k) {
    Frame f;
    f.kind = k;
    for (auto& h : f.hand) h.active = true;
    f.hand[0].pos = {-0.2f, 1.0f, -0.3f};
    f.hand[1].pos = {0.2f, 1.0f, -0.3f};
    return f;
}

int main() {
    // Index: buttons, triggers and sticks pass through
    {
        Mapper m;
        Frame f = idle(Kind::Index);
        f.hand[1].trigger = 0.7f;
        f.hand[0].sx = 0.5f;
        f.hand[1].buttons = Primary | StickClick;
        f.hand[0].buttons = Primary;
        Pad p = m.update(f);
        assert(p.rt == 0.7f && p.lx == 0.5f);
        assert(p.buttons == (PadA | PadRightThumb | PadX));
        // the left squeeze is the gadget, issued as a HUD command (src/se4/gadgets.cpp), never a pad button
        f.hand[1].buttons = f.hand[0].buttons = 0;
        f.hand[0].squeeze = 1.0f;
        assert(m.update(f).buttons == 0);
    }
    // Vive: the right grip crouches, with hysteresis: on above 0.5, still on at 0.35, off below 0.3
    {
        Mapper m;
        Frame f = idle(Kind::Vive);
        f.hand[1].squeeze = 0.55f;
        assert(m.update(f).buttons == PadB);
        f.hand[1].squeeze = 0.35f;
        assert(m.update(f).buttons == PadB);
        f.hand[1].squeeze = 0.25f;
        assert(m.update(f).buttons == 0);
    }
    // Touch: a resting squeeze (0.5) is not a grip; Y holds the wheel
    {
        Mapper m;
        Frame f = idle(Kind::Touch);
        f.hand[0].squeeze = 0.5f;
        f.hand[0].buttons = Secondary;
        assert(m.update(f).buttons == PadLB);
    }
    // Wheel: hand displacement relative to the opening pose becomes the stick, in the head's yaw frame
    {
        Mapper m;
        Frame f = idle(Kind::Index);
        f.head = yaw(Pi / 2);  // facing -X (yaw +90 deg about +Y turns -Z towards -X)
        f.hand[0].buttons = TrackpadPress;  // both trackpads held: one wheel for both hands
        f.hand[1].buttons = TrackpadPress;
        f.ms = 1000;
        m.update(f);
        f.ms = 1300;  // the right pad is held past the tap time
        Pad p = m.update(f);
        assert(p.buttons & PadLB);
        assert(p.rx == 0 && p.ry == 0 && p.lx == 0 && p.ly == 0);  // opening does not select
        // right hand moves 4 cm up and 4 cm to the user's right (user's right = world -Z when facing -X)
        f.hand[1].pos = f.hand[1].pos + Vec3{0, 0.04f, -0.04f};
        p = m.update(f);
        assert(std::fabs(p.rx - 0.5f) < 1e-3f && std::fabs(p.ry - 0.5f) < 1e-3f);
        // a large move saturates at length 1
        f.hand[1].pos = f.hand[1].pos + Vec3{0, 0.5f, 0};
        p = m.update(f);
        assert(std::fabs(std::sqrt(p.rx * p.rx + p.ry * p.ry) - 1) < 1e-3f);
        // jitter inside the dead zone selects nothing for the left hand
        f.hand[0].pos = f.hand[0].pos + Vec3{0.005f, 0, 0};
        p = m.update(f);
        assert(p.lx == 0 && p.ly == 0);
        // a stick pushed further than the hand wins
        f.hand[0].sx = 0, f.hand[0].sy = -0.9f;
        p = m.update(f);
        assert(p.ly == -0.9f);
        // releasing the wheel buttons returns to plain sticks; reopening re-anchors
        f.hand[0].buttons = 0;
        f.hand[1].buttons = 0;
        f.hand[0].sy = 0;
        p = m.update(f);
        assert(!(p.buttons & PadLB) && p.rx == 0 && p.ry == 0);
        f.hand[0].buttons = TrackpadPress;
        p = m.update(f);
        assert(p.rx == 0 && p.ry == 0);
    }
    // Per-hand wheels (Index): the left pad picks only the left hand's weapon, the right pad (held) only the right;
    // a right tap toggles dual wielding.
    {
        Mapper m;
        Frame f = idle(Kind::Index);
        f.ms = 100;
        f.hand[0].buttons = TrackpadPress;
        Pad p = m.update(f);
        assert(p.buttons & PadLB);
        f.hand[0].pos = f.hand[0].pos + Vec3{0, 0.08f, 0};
        f.hand[1].pos = f.hand[1].pos + Vec3{0, 0.08f, 0};
        f.hand[1].sy = 0.9f;  // even a pushed right stick does not pick for the right hand
        p = m.update(f);
        assert(std::fabs(p.ly - 1) < 1e-3f && p.rx == 0 && p.ry == 0);
        f.hand[0].buttons = 0, f.hand[1].sy = 0;
        p = m.update(f);
        assert(!(p.buttons & PadLB));
        // right pad: a tap is RB for kPulseMs, no wheel
        f.ms = 1000, f.hand[1].buttons = TrackpadPress;
        p = m.update(f);
        assert(!(p.buttons & (PadLB | PadRB)));
        f.ms = 1100, f.hand[1].buttons = 0;
        p = m.update(f);
        assert(p.buttons & PadRB);
        f.ms = 1300;
        assert(!(m.update(f).buttons & PadRB));
        // right pad held: the right hand's wheel, left selections ignored
        f.ms = 2000, f.hand[1].buttons = TrackpadPress;
        m.update(f);
        f.ms = 2300;
        p = m.update(f);
        assert((p.buttons & PadLB) && p.rx == 0 && p.ry == 0);
        f.hand[1].pos = f.hand[1].pos + Vec3{0.08f, 0, 0};
        f.hand[0].pos = f.hand[0].pos + Vec3{0.08f, 0, 0};
        p = m.update(f);
        assert(std::fabs(p.rx - 1) < 1e-3f && p.lx == 0 && p.ly == 0);
        // releasing after a hold is not a tap
        f.ms = 2400, f.hand[1].buttons = 0;
        assert(!(m.update(f).buttons & PadRB));
    }
    // Wheel held: the right face buttons are taken by the gadget ring (HUD commands), so no jump/crouch
    {
        Mapper m;
        Frame f = idle(Kind::Index);
        f.hand[0].buttons = TrackpadPress;
        f.hand[1].buttons = Primary;
        assert(m.update(f).buttons == PadLB);
        f.hand[1].buttons = Secondary;
        assert(m.update(f).buttons == PadLB);
        f.hand[0].buttons = 0;
        assert(m.update(f).buttons == PadB);
    }
    // Vive: trackpad press is not also reported as a stick click (sprint)
    {
        Mapper m;
        Frame f = idle(Kind::Vive);
        f.hand[0].buttons = TrackpadPress | StickClick;
        Pad p = m.update(f);
        assert(p.buttons == PadLB);
    }
    return 0;
}
