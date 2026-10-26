#include "se4/wheel_panel.hpp"
#include "core/math.hpp"
#include "se4/build.hpp"
#include "se4/gamepad.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/player_vr.hpp"
#include "core/vr_controls.hpp"
#include "xr/input.hpp"
#include "xr/runtime.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace wheel_panel {
namespace {
void* g_dummyCommand;
bool g_enabled = true;
// The wheel's post-processing preset blurs and tints the whole world while it is open (vt+0xc8, R16 s2.3). In VR
// the world stays sharp behind the hand panel (user request); option wheel_blur=1 keeps the native blur.
using PpFn = void(__cdecl*)(uint8_t* wheel);
PpFn realPp;
void hookedPp(uint8_t*) {}

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

// ---- Spots beside the wheel (R18): Menu / Netricsa on the left, Quick load / Quick save on the right ----
// Actions go through the Fusion VR wheel's own button functions, still in SS4/SM, given a stand-in VR wheel whose
// +0x00 is the player's handle and whose hand pick lists (+0x18, count +0x20) are empty, so their "commit the hand's
// pick" step does nothing. Quick load is the VR wheel tick's own sequence (key, then the quick load native).
using AvailFn = int(__cdecl*)(uint8_t* button);
using ActFn = void(__cdecl*)(uint8_t* button, int hand);
using HandleFn = int32_t(__cdecl*)(uint8_t* entity);
using EntityFn = uint8_t*(__cdecl*)(uint8_t* entity);
using KeyFn = void(__cdecl*)(uint8_t* player, int32_t* key);
using LoadFn = void(__cdecl*)(void* menuMgr, int32_t* key, int arg);
using MenuFn = void(__cdecl*)(uint8_t* pi, int hand, const char* menu, float fade, void* params, void* owner);
struct Actions {
    AvailFn menuAvail, netricsaAvail, saveAvail;
    ActFn netricsa, save;
    HandleFn toHandle;
    EntityFn pi;
    KeyFn key;
    LoadFn load;
    MenuFn menu;
} g_act;
int g_pending = -1;
ULONGLONG g_pendingAt;

void runSpot(uint8_t* player, int spot, int hand) {
    alignas(16) static uint8_t vrwheel[0x100];
    alignas(16) uint8_t button[0x48] = {};
    memset(vrwheel, 0, sizeof vrwheel);
    *reinterpret_cast<int32_t*>(vrwheel) = g_act.toHandle(player);
    *reinterpret_cast<uint8_t**>(button + 8) = vrwheel;
    const int nativeHand = hand == 1 ? 0 : 1;  // engine hand 0 = right
    using core::controls::Spot;
    bool done = false;
    switch (spot) {
    case core::controls::SpotMenu:
        if ((done = g_act.menuAvail(button) != 0)) g_act.menu(g_act.pi(player), nativeHand, "", 0.5f, nullptr, nullptr);
        break;
    case core::controls::SpotNetricsa:
        if ((done = g_act.netricsaAvail(button) != 0)) g_act.netricsa(button, nativeHand);
        break;
    case core::controls::SpotQuickSave:
        *reinterpret_cast<int32_t*>(button + 0x40) = 1;
        if ((done = g_act.saveAvail(button) != 0)) g_act.save(button, nativeHand);
        break;
    case core::controls::SpotQuickLoad:
        if ((done = g_act.saveAvail(button) != 0)) {
            int32_t key[4] = {};
            g_act.key(player, key);
            g_act.load(*reinterpret_cast<void**>(g_act.pi(player) + 0xb8), key, 0);
        }
        break;
    }
    VRLOG("wheel_panel: %s spot %s", core::controls::spotName(spot), done ? "run" : "not available now (multiplayer, dead or busy)");
}

}  // namespace

void onTick(uint8_t* player) {
    if (!g_act.toHandle) return;
    if (const int a = gamepad::takeSpotAction(); a >= 0) g_pending = a, g_pendingAt = GetTickCount64();
    // After the wheel has closed (its menu screen is gone), so the action does not race the wheel's own close.
    if (g_pending < 0 || g_wasOpen || GetTickCount64() - g_pendingAt < 100) return;
    const int a = g_pending;
    g_pending = -1;
    runSpot(player, a & 0xf, a >> 4);
}

void beforeWheelDraw(uint8_t* wheel) {
    // Pointing at a spot: no weapon highlighted, so the wheel's close commits the current weapons (R18 s3.4).
    if (wheel && gamepad::wheelSpot() >= 0) {
        *reinterpret_cast<void**>(wheel + 0x78) = nullptr;
        *reinterpret_cast<void**>(wheel + 0x80) = nullptr;
        *reinterpret_cast<int32_t*>(wheel + 0xbc) = 0;
    }
}

bool spotsAvailable() { return g_act.toHandle != nullptr; }

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

bool panel(float out[7]) {
    if (!g_panel.valid) return false;
    const core::Pose& p = g_panel.pose;
    out[0] = p.q.x, out[1] = p.q.y, out[2] = p.q.z, out[3] = p.q.w, out[4] = p.p.x, out[5] = p.p.y, out[6] = p.p.z;
    return true;
}

bool install() {
    g_enabled = se4::options::get("wheel_panel", 1) != 0;
    g_dummyCommand = se4::build().at<void>(se4::gen::wheel_dummy_command);
    if (!g_enabled || !g_dummyCommand) {
        VRLOG("wheel_panel: %s - the wheel stays on the cinema screen", g_enabled ? "build lacks wheel symbols" : "disabled by option");
        g_enabled = false;
        return false;
    }
    {
        const auto& b = se4::build();
        Actions a{};
        a.menuAvail = reinterpret_cast<AvailFn>(b.at<void>(se4::gen::vr_menu_avail));
        a.netricsaAvail = reinterpret_cast<AvailFn>(b.at<void>(se4::gen::vr_netricsa_avail));
        a.saveAvail = reinterpret_cast<AvailFn>(b.at<void>(se4::gen::vr_qs_avail));
        a.netricsa = reinterpret_cast<ActFn>(b.at<void>(se4::gen::vr_netricsa_act));
        a.save = reinterpret_cast<ActFn>(b.at<void>(se4::gen::vr_quicksave_act));
        a.toHandle = reinterpret_cast<HandleFn>(b.at<void>(se4::gen::entity_to_handle));
        a.pi = reinterpret_cast<EntityFn>(b.at<void>(se4::gen::entity_pi));
        a.key = reinterpret_cast<KeyFn>(b.at<void>(se4::gen::player_key));
        a.load = reinterpret_cast<LoadFn>(b.at<void>(se4::gen::quick_load));
        a.menu = reinterpret_cast<MenuFn>(b.at<void>(se4::gen::menu_open_hand));
        if (se4::options::get("wheel_spots", 1) && a.menuAvail && a.netricsaAvail && a.saveAvail && a.netricsa && a.save && a.toHandle && a.pi &&
            a.key && a.load && a.menu)
            g_act = a, VRLOG("wheel_panel: spots beside the wheel: Menu, Netricsa, Quick load, Quick save");
        else
            VRLOG("wheel_panel: no wheel spots (option or build lacks the VR wheel button functions)");
    }
    const bool blur = se4::options::get("wheel_blur", 0) != 0;
    if (void* pp = se4::build().at<void>(se4::gen::wheel_pp); pp && !blur &&
        (MH_CreateHook(pp, reinterpret_cast<void*>(hookedPp), reinterpret_cast<void**>(&realPp)) != MH_OK || MH_EnableHook(pp) != MH_OK))
        VRLOG("wheel_panel: wheel blur hook FAILED");
    VRLOG("wheel_panel: the weapon wheel keeps the world in stereo and opens on a hand panel (world blur %s)", blur ? "native" : "off");
    return true;
}
}  // namespace wheel_panel
