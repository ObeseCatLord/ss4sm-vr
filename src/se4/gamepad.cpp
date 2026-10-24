#include "se4/gamepad.hpp"
#include "se4/log.hpp"
#include "se4/procaddr.hpp"
#include "xr/input.hpp"
#include "xr/runtime.hpp"
#include "core/vr_controls.hpp"
#include "se4/player_vr.hpp"
#include "se4/vehicles.hpp"
#include "se4/gadgets.hpp"
#include "se4/hands.hpp"
#include <xinput.h>
#include <cmath>
#include <cstring>

namespace gamepad {
namespace {
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using GetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
GetStateFn realGetState;
SetStateFn realSetState;
GetCapsFn realGetCaps;

SRWLOCK g_lock = SRWLOCK_INIT;
XINPUT_GAMEPAD g_pad{};
DWORD g_packet;
// The pad exists only when an OpenXR runtime is up (initialised at the game's Vulkan init, before its first
// gamepad poll); flat play never sees a phantom controller.
bool isActive() { return xr::runtime().active(); }
LONG g_polled;
volatile LONG g_polls, g_pollsOther;

SHORT axis(float v) {
    v = v < -1 ? -1 : v > 1 ? 1 : v;
    return SHORT(std::lround(v * 32767.0f));
}
BYTE trig(float v) { return BYTE(std::lround((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f)); }

DWORD WINAPI hookGetState(DWORD user, XINPUT_STATE* st) {
    if (user != 0 || !isActive()) {
        InterlockedIncrement(&g_pollsOther);
        return realGetState ? realGetState(user, st) : ERROR_DEVICE_NOT_CONNECTED;
    }
    InterlockedIncrement(&g_polls);
    if (!InterlockedExchange(&g_polled, 1)) VRLOG("gamepad: first game poll of user 0");
    AcquireSRWLockShared(&g_lock);
    st->dwPacketNumber = g_packet;
    st->Gamepad = g_pad;
    ReleaseSRWLockShared(&g_lock);
    return ERROR_SUCCESS;
}

DWORD WINAPI hookGetCaps(DWORD user, DWORD flags, XINPUT_CAPABILITIES* c) {
    if (user != 0 || !isActive()) return realGetCaps ? realGetCaps(user, flags, c) : ERROR_DEVICE_NOT_CONNECTED;
    memset(c, 0, sizeof *c);
    c->Type = XINPUT_DEVTYPE_GAMEPAD;
    c->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    c->Gamepad.wButtons = 0xF3FF;
    c->Gamepad.bLeftTrigger = c->Gamepad.bRightTrigger = 0xFF;
    c->Gamepad.sThumbLX = c->Gamepad.sThumbLY = c->Gamepad.sThumbRX = c->Gamepad.sThumbRY = SHORT(0xFFC0);
    c->Vibration.wLeftMotorSpeed = c->Vibration.wRightMotorSpeed = 0xFFFF;
    return ERROR_SUCCESS;
}

DWORD WINAPI hookSetState(DWORD user, XINPUT_VIBRATION* v) {
    if (user != 0 || !isActive()) return realSetState ? realSetState(user, v) : ERROR_DEVICE_NOT_CONNECTED;
    auto& rt = xr::runtime();
    if (rt.running()) {
        // Native rumble: left (low-frequency) motor -> left hand, right motor -> right hand.
        float l = v->wLeftMotorSpeed / 65535.0f, r = v->wRightMotorSpeed / 65535.0f;
        if (l > 0.01f) xr::input().vibrate(rt.session(), 0, l, 0.05f);
        if (r > 0.01f) xr::input().vibrate(rt.session(), 1, r, 0.05f);
    }
    return ERROR_SUCCESS;
}
}  // namespace

FARPROC interceptProc(HMODULE, const char* name, FARPROC real) {
    VRLOG("gamepad: game resolves %s", name);
    // The engine enumerates pads right after resolving XInput, seconds before its Vulkan init (where the XR
    // runtime used to start). Bring the runtime up now so the virtual pad is connected at that enumeration;
    // a pad reported missing there is never polled again.
    static bool tried;
    if (!tried) {
        tried = true;
        VRLOG("gamepad: XR runtime %s at XInput resolution", xr::runtime().ensure() ? "up" : "unavailable");
    }
    if (!strcmp(name, "XInputGetState") || !strcmp(name, "XInputGetStateEx")) {
        realGetState = reinterpret_cast<GetStateFn>(real);
        return reinterpret_cast<FARPROC>(hookGetState);
    }
    if (!strcmp(name, "XInputSetState")) {
        realSetState = reinterpret_cast<SetStateFn>(real);
        return reinterpret_cast<FARPROC>(hookSetState);
    }
    if (!strcmp(name, "XInputGetCapabilities")) {
        realGetCaps = reinterpret_cast<GetCapsFn>(real);
        return reinterpret_cast<FARPROC>(hookGetCaps);
    }
    return real;
}

bool active() { return isActive(); }

void logStats() {
    VRLOG("gamepad: polls user0 %ld other %ld, buttons %04x LX %d LY %d RT %u", InterlockedExchange(&g_polls, 0),
          InterlockedExchange(&g_pollsOther, 0), g_pad.wButtons, g_pad.sThumbLX, g_pad.sThumbLY, g_pad.bRightTrigger);
}

// Layout and the native weapon wheel's hand selection: core/vr_controls.hpp.
static_assert(int(xr::ControllerKind::Index) == int(core::controls::Kind::Index) &&
              int(xr::ControllerKind::Unknown) == int(core::controls::Kind::Unknown), "controller kinds must match");
static_assert(uint32_t(xr::BtnPrimary) == core::controls::Primary && uint32_t(xr::BtnSecondary) == core::controls::Secondary &&
              uint32_t(xr::BtnStickClick) == core::controls::StickClick && uint32_t(xr::BtnMenu) == core::controls::Menu &&
              uint32_t(xr::BtnTrackpadPress) == core::controls::TrackpadPress, "button bits must match");
static_assert(XINPUT_GAMEPAD_LEFT_SHOULDER == core::controls::PadLB && XINPUT_GAMEPAD_Y == core::controls::PadY &&
              XINPUT_GAMEPAD_START == core::controls::PadStart && XINPUT_GAMEPAD_DPAD_UP == core::controls::PadDpadUp &&
              XINPUT_GAMEPAD_DPAD_DOWN == core::controls::PadDpadDown, "XInput bits must match");
core::controls::Mapper g_mapper;

void publishFromXr() {
    using namespace core::controls;
    Frame f;
    for (int h = 0; h < 2; ++h) {
        const auto& x = xr::input().hand(h);
        Hand& o = f.hand[h];
        o.active = x.active;
        o.trigger = gadgets::triggerTaken(h) ? 0.0f : x.trigger;  // a gadget in this hand goes off by its own gesture
        o.squeeze = hands::squeezeTaken(h) ? 0.0f : x.squeeze;  // the off hand is on (or at) a two-handed weapon
        o.sx = x.stick.x, o.sy = x.stick.y;
        o.buttons = x.buttons;
        o.pos = {x.grip.position.x, x.grip.position.y, x.grip.position.z};
    }
    // Kind from the right hand (both hands share a profile in practice), else the left.
    auto k = xr::input().hand(1).kind != xr::ControllerKind::Unknown ? xr::input().hand(1).kind : xr::input().hand(0).kind;
    f.kind = Kind(int(k));
    f.ms = GetTickCount64();
    const auto& v = xr::runtime().views();
    if (v.valid) f.head = {v.head.orientation.x, v.head.orientation.y, v.head.orientation.z, v.head.orientation.w};
    const Pad m = g_mapper.update(f);
    XINPUT_GAMEPAD p{};
    // Grab steering: the hands' wheel angle is the native analog steer unless the stick is pushed (R7 s6b).
    float grabLx;
    p.sThumbLX = (vehicles::steerOverride(grabLx) && std::fabs(m.lx) < 0.25f) ? axis(grabLx) : axis(m.lx);
    p.sThumbLY = axis(m.ly);
    // With native_turn the engine turns the VR anchor from the device stick; the pad must not turn as well.
    p.sThumbRX = (player_vr::nativeTurn() && !g_mapper.wheelHeld) ? 0 : axis(m.rx);
    p.sThumbRY = g_mapper.wheelHeld ? axis(m.ry) : 0;  // pitch comes from the head outside the wheel
    p.bRightTrigger = trig(m.rt);
    p.bLeftTrigger = trig(m.lt);
    // While driving, the grips grab the wheel instead of using a gadget.
    p.wButtons = vehicles::driving() ? WORD(m.buttons & ~XINPUT_GAMEPAD_Y) : m.buttons;
    AcquireSRWLockExclusive(&g_lock);
    if (memcmp(&p, &g_pad, sizeof p)) {
        if (p.wButtons != g_pad.wButtons) VRLOG("gamepad: buttons %04x", p.wButtons);
        g_pad = p;
        ++g_packet;
    }
    static bool fed;
    if (!fed) VRLOG("gamepad: fed from XR controllers");
    fed = true;
    ReleaseSRWLockExclusive(&g_lock);
}
}  // namespace gamepad
