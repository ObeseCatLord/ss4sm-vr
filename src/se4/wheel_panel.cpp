#include "se4/wheel_panel.hpp"
#include "core/math.hpp"
#include "se4/build.hpp"
#include "se4/gamepad.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "xr/input.hpp"
#include "xr/runtime.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace wheel_panel {
namespace {
using ElementFn = void(__cdecl*)(void** ctx, uint8_t* element);
ElementFn realElement;
void* g_wheelRender;
void* g_dummyCommand;
bool g_enabled = true;
float g_scale = 1.0f;
constexpr size_t kParams = 0x18, kPlaceQ = 0x18, kPlaceP = 0x28, kStretch = 0x34;  // element / params (R16 s2.1)

struct Panel {
    volatile LONG valid = 0;
    core::Pose pose;  // device play space, engine units (as slot 0x080's head)
} g_panel;
bool g_wasOpen;

float worldScale() {
    auto* s = se4::build().at<float>(se4::gen::vr_fWorldScale);
    return (s && *s > 0.01f) ? *s : 1.0f;
}
core::Vec3 vec(const XrVector3f& v) { return {v.x, v.y, v.z}; }

// The panel when the wheel opens: 18 cm ahead of the opening hand (both: their midpoint) along the view and 6 cm
// up, turned to face the eyes (yaw and pitch only).
void latch() {
    const auto& v = xr::runtime().views();
    const auto& L = xr::input().hand(0);
    const auto& R = xr::input().hand(1);
    const int h = gamepad::wheelHand();
    core::Vec3 hand;
    if (h == 0 && L.active) hand = vec(L.grip.position);
    else if (h == 1 && R.active) hand = vec(R.grip.position);
    else if (L.active && R.active) hand = (vec(L.grip.position) + vec(R.grip.position)) * 0.5f;
    else if (R.active || L.active) hand = vec((R.active ? R : L).grip.position);
    else return;
    if (!v.valid) return;
    const core::Vec3 head = vec(v.head.position);
    const core::Vec3 f = core::rotate({v.head.orientation.x, v.head.orientation.y, v.head.orientation.z, v.head.orientation.w}, {0, 0, -1});
    const float n = std::sqrt(f.x * f.x + f.z * f.z);
    const core::Vec3 flat = n > 1e-3f ? core::Vec3{f.x / n, 0, f.z / n} : core::Vec3{0, 0, -1};
    const core::Vec3 p = hand + flat * 0.18f + core::Vec3{0, 0.06f, 0};
    core::Vec3 d = p - head;
    const float dl = std::sqrt(core::dot(d, d));
    if (!(dl > 1e-3f)) return;
    d = d * (1.0f / dl);
    const float yawA = std::atan2(-d.x, -d.z), pitchA = std::asin(std::fmax(-1.0f, std::fmin(1.0f, d.y)));
    const core::Quat q = core::multiply(core::yaw(yawA), core::Quat{std::sin(pitchA / 2), 0, 0, std::cos(pitchA / 2)});
    g_panel.pose = {q, p * (1.0f / worldScale())};
    InterlockedExchange(&g_panel.valid, 1);
    VRLOG("wheel_panel: wheel opened by the %s - panel at %.2f %.2f %.2f m (head %.2f %.2f %.2f)",
          h == 0 ? "left hand" : h == 1 ? "right hand" : "both hands", p.x, p.y, p.z, head.x, head.y, head.z);
}

void hookedElement(void** ctx, uint8_t* e) {
    void** vt = e ? *reinterpret_cast<void***>(e) : nullptr;
    if (!vt || vt[0x80 / 8] != g_wheelRender || !g_panel.valid || !ctx[7] || !*static_cast<int32_t*>(ctx[7]) || !ctx[8]) {
        realElement(ctx, e);
        return;
    }
    uint8_t* params = *reinterpret_cast<uint8_t**>(e + kParams);
    float* anchor = static_cast<float*>(ctx[8]);
    if (!params) {
        realElement(ctx, e);
        return;
    }
    // anchor * placement = panel  =>  anchor = panel * inverse(placement)
    const float* pq = reinterpret_cast<const float*>(params + kPlaceQ);
    const float* pp = reinterpret_cast<const float*>(params + kPlaceP);
    const core::Pose place{core::normalize({pq[0], pq[1], pq[2], pq[3]}), {pp[0], pp[1], pp[2]}};
    const core::Pose a = core::compose(g_panel.pose, core::inverse(place));
    if (!core::finite(a)) {
        realElement(ctx, e);
        return;
    }
    float saved[7];
    memcpy(saved, anchor, sizeof saved);
    float& stretch = *reinterpret_cast<float*>(params + kStretch);
    const float savedStretch = stretch;
    anchor[0] = a.q.x, anchor[1] = a.q.y, anchor[2] = a.q.z, anchor[3] = a.q.w, anchor[4] = a.p.x, anchor[5] = a.p.y, anchor[6] = a.p.z;
    stretch = savedStretch * g_scale;
    realElement(ctx, e);
    stretch = savedStretch;
    memcpy(anchor, saved, sizeof saved);
    static ULONGLONG last;
    if (GetTickCount64() - last > 3000) {
        last = GetTickCount64();
        VRLOG("wheel_panel: wheel drawn on the hand panel (native placement %.2f %.2f %.2f, stretch %.3f)", pp[0], pp[1], pp[2], savedStretch);
    }
}
}  // namespace

bool wheelOpen(void* pi) {
    if (!g_enabled || !pi || !g_dummyCommand) return false;
    auto* mgr = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(pi) + 0xb8);
    void* top = mgr ? *reinterpret_cast<void**>(mgr + 0x28) : nullptr;
    const bool open = top && (*reinterpret_cast<void***>(top))[0x138 / 8] == g_dummyCommand;
    if (open && !g_wasOpen) latch();
    if (!open && g_wasOpen) InterlockedExchange(&g_panel.valid, 0);
    g_wasOpen = open;
    return open;
}

bool install() {
    g_enabled = se4::options::get("wheel_panel", 1) != 0;
    g_scale = se4::options::get("wheel_panel_scale", 100) / 100.0f;
    const auto& b = se4::build();
    g_wheelRender = b.at<void>(se4::gen::wheel_render);
    g_dummyCommand = b.at<void>(se4::gen::wheel_dummy_command);
    void* element = b.at<void>(se4::gen::hud_element_render);
    if (!g_enabled || !g_wheelRender || !g_dummyCommand || !element) {
        VRLOG("wheel_panel: %s - the wheel stays on the cinema screen", g_enabled ? "build lacks wheel symbols" : "disabled by option");
        g_enabled = false;
        return false;
    }
    if (MH_CreateHook(element, reinterpret_cast<void*>(hookedElement), reinterpret_cast<void**>(&realElement)) != MH_OK ||
        MH_EnableHook(element) != MH_OK) {
        VRLOG("wheel_panel: HUD element hook FAILED");
        g_enabled = false;
        return false;
    }
    VRLOG("wheel_panel: weapon wheel on a hand panel (scale %.2f)", g_scale);
    return true;
}
}  // namespace wheel_panel
