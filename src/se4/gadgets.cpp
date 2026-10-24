#include "se4/gadgets.hpp"
#include "core/body_ik.hpp"
#include "core/math.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/player_vr.hpp"
#include "se4/hands.hpp"
#include "xr/input.hpp"
#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gadgets {
namespace {
using ActivateFn = uintptr_t(__cdecl*)(uint8_t* mode);
using HandleFn = uint8_t*(__cdecl*)(int32_t handle);
ActivateFn realActivate;
HandleFn handleEntity;
float g_gain = 2.0f;
constexpr float kThrowSpeed = 1.6f;  // m/s of the hand that counts as a throw (core/grenade_throw.hpp)
constexpr float kMaxSpeed = 250.0f;  // the projectile init clamps here (R12 s8.4)

template <class T> T& at(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

uint8_t* localOwner(uint8_t* weapon) {
    void* local = player_vr::localVrd();
    const unsigned vrdOff = player_vr::vrdOffset();
    if (!local || !vrdOff) return nullptr;
    uint8_t* owner = handleEntity(at<int32_t>(weapon, se4::build().off(se4::gen::CWeaponEntity_en_henOwner)));
    return owner && at<void*>(owner, vrdOff) == local ? owner : nullptr;
}

// Direction (and speed for a real throw) from the holding hand into the launch mode's fields.
void applyHandAim(uint8_t* mode, const char* when) {
    uint8_t* holder = mode ? at<uint8_t*>(mode, 8) : nullptr;
    if (!holder || !localOwner(holder)) return;
    const float nativeSpeed = at<float>(mode, 0x3c);
    VRLOG("gadgets: %s (speed %.2f, lifetime %.2f)", when, nativeSpeed, at<float>(mode, 0x40));
    if (!(nativeSpeed > 0.0f && nativeSpeed < 1e4f)) return;  // no valid launch parameters
    const int role = at<int32_t>(holder, se4::build().off(se4::gen::CWeaponEntity_en_bRightHandWeapon)) ? 0 : 1;
    const int xrHand = role == 0 ? 1 : 0;
    float c7[7] = {};
    player_vr::controllerWorld(player_vr::localVrd(), role, c7);
    const auto& hs = xr::input().hand(xrHand);
    if (!(c7[0] || c7[1] || c7[2] || c7[3]) || !hs.active) return;
    const core::Quat handWorld = core::normalize({c7[0], c7[1], c7[2], c7[3]});
    const core::Quat gripXr = core::normalize({hs.grip.orientation.x, hs.grip.orientation.y, hs.grip.orientation.z, hs.grip.orientation.w});
    const core::Quat xrToWorld = core::multiply(handWorld, core::inverse(gripXr));  // the play space's anchor rotation
    float ws = 1.0f;
    if (auto* s = se4::build().at<float>(se4::gen::vr_fWorldScale); s && *s > 0.01f) ws = *s;

    core::Vec3 dir = core::rotate(handWorld, {0, 0, -1});  // where the hand points
    float speed = nativeSpeed;
    bool thrown = false;
    if (hs.velocityValid) {
        const core::Vec3 v = core::rotate(xrToWorld, {hs.gripLinearVelocity.x, hs.gripLinearVelocity.y, hs.gripLinearVelocity.z});
        const float mps = std::sqrt(core::dot(v, v));
        if (mps > kThrowSpeed) {
            dir = v * (1.0f / mps);
            speed = std::clamp(mps * g_gain / ws, nativeSpeed * 0.5f, kMaxSpeed);
            thrown = true;
        }
    }
    const core::Quat q = core::fromTo({0, 0, -1}, dir);
    if (!core::finite(core::Pose{q, {}})) return;
    at<float>(mode, 0x20) = q.x, at<float>(mode, 0x24) = q.y, at<float>(mode, 0x28) = q.z, at<float>(mode, 0x2c) = q.w;
    at<float>(mode, 0x3c) = speed;
    VRLOG("gadgets: %s from the %s hand at %.1f (native %.1f)", thrown ? "thrown" : "aimed", xrHand ? "right" : "left", speed, nativeSpeed);
}

uintptr_t hookedActivate(uint8_t* mode) {
    const uintptr_t r = realActivate(mode);
    applyHandAim(mode, "launch activation");
    return r;
}

using LaunchFn = void(__cdecl*)(uint8_t* mode);
LaunchFn realLaunch;
void hookedLaunch(uint8_t* mode) {
    applyHandAim(mode, "launch");
    realLaunch(mode);
}
}  // namespace

void onTick(uint8_t* player) {
    // Gadget quick use from the left squeeze, through the player's own HUD command (the path plcmdGadgetQuickUse
    // takes: player vt+0x11b0 OnHudCommand(1, 0xc), controller input 0x14040f4b7; a network call). The pad's Y is
    // not used: its meaning depends on the player's saved gamepad layout (it opened Netricsa in the lab profile).
    static bool pressed;
    const auto& hs = xr::input().hand(0);
    const float sq = hs.active ? hs.squeeze : 0.0f;
    const bool down = pressed ? sq > 0.35f : sq > 0.6f;
    if (!player) return;
    if (down && !pressed && !hands::squeezeTaken(0)) {
        using HudFn = void(__cdecl*)(uint8_t* player, int arg, int command);
        reinterpret_cast<HudFn>((*reinterpret_cast<void***>(player))[0x11b0 / 8])(player, 1, 0xc);
        VRLOG("gadgets: quick use from the left squeeze");
    }
    pressed = down;
    // While the weapon wheel is held (left trackpad press / Touch Y), the right A/B cycle the gadget ring through
    // the HUD's own next/previous commands (0xd/0xe, R12 s7) instead of pad buttons.
    static bool aWas, bWas;
    const auto& lh = xr::input().hand(0);
    const auto& rh = xr::input().hand(1);
    const bool wheel = (lh.buttons & xr::BtnTrackpadPress) || (lh.kind == xr::ControllerKind::Touch && (lh.buttons & xr::BtnSecondary));
    const bool a = wheel && (rh.buttons & xr::BtnPrimary), b = wheel && (rh.buttons & xr::BtnSecondary);
    using HudFn = void(__cdecl*)(uint8_t* player, int arg, int command);
    auto hud = reinterpret_cast<HudFn>((*reinterpret_cast<void***>(player))[0x11b0 / 8]);
    if (a && !aWas) hud(player, 1, 0xd), VRLOG("gadgets: next gadget");
    if (b && !bWas) hud(player, 1, 0xe), VRLOG("gadgets: previous gadget");
    aWas = a, bWas = b;
}

bool install() {
    if (!se4::options::get("gadget_hand_aim", 1)) return false;
    g_gain = std::clamp(se4::options::get("throw_gain", 200), 50, 1000) / 100.0f;
    const auto& b = se4::build();
    auto* act = b.at<void>(se4::gen::gadget_launch_activate);
    handleEntity = reinterpret_cast<HandleFn>(b.at<void>(se4::gen::handle_entity));
    if (!act || !handleEntity || !b.off(se4::gen::CWeaponEntity_en_henOwner)) {
        VRLOG("gadgets: build lacks gadget symbols - not installed");
        return false;
    }
    if (MH_CreateHook(act, reinterpret_cast<void*>(hookedActivate), reinterpret_cast<void**>(&realActivate)) != MH_OK ||
        MH_EnableHook(act) != MH_OK) {
        VRLOG("gadgets: launch hook FAILED");
        return false;
    }
    if (auto* launch = b.at<void>(se4::gen::gadget_launch);
        launch && (MH_CreateHook(launch, reinterpret_cast<void*>(hookedLaunch), reinterpret_cast<void**>(&realLaunch)) != MH_OK ||
                   MH_EnableHook(launch) != MH_OK))
        VRLOG("gadgets: spawn hook FAILED");
    VRLOG("gadgets: throwables aim with the hand (throw gain %.1f)", g_gain);
    return true;
}
}  // namespace gadgets
