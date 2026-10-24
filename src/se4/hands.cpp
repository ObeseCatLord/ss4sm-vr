#include "se4/hands.hpp"
#include "core/math.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/player_vr.hpp"
#include "xr/input.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cmath>
#include <cstdint>

namespace hands {
namespace {
using StepFn = void(__cdecl*)(uint8_t* weapon, void* arg);
using HandleFn = uint8_t*(__cdecl*)(int32_t handle);
using BarrelFn = void(__cdecl*)(uint8_t* weapon, float* out, void* ctx, void* ident, int, int, int);
using CtxFn = void*(__cdecl*)();

template <class T> T& at(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

StepFn realStep;
HandleFn handleEntity;
BarrelFn barrelPlacement;
CtxFn barrelCtx;
void* g_gripIdent;      // LeftHandGripEffector (R15 s4.2)
uint32_t g_fireBits;    // player fire-button byte (Sam4 0x2338, SM 0x2340), read from the weapon step's own code

// Weapon params / flags (R15 s4.1; same in SM).
constexpr size_t kParams = 0xa8;            // en_pwpWeaponParams
constexpr size_t kFlags = 0x168;            // en_ulWeaponFlags
constexpr uint32_t kGripped = 0x20;         // WF_GRIPPED_WITH_TWO_HANDS
constexpr uint32_t kCheckedInitialFire = 0x800;
constexpr size_t kTwoHanded = 0x1b4;        // wp_bTwoHanded
constexpr size_t kSnapDistance = 0x214;     // wp_fHandSnapMinDistance (< 0: the player's VR params default)
constexpr float kDefaultSnap = 0.2f;        // stand-in for CPlayerVRParams+0xcc (unreached)

struct OffHand {
    bool engaged = false;   // grip intent
    bool pressed = false;   // squeeze above the press threshold last tick
    bool inReach = false;
    bool gripping = false;
    uint8_t* weapon = nullptr;
};
OffHand g_off[2];  // by XR hand
volatile LONG g_taken[2];

// The byte the grip test reads: the `test byte ptr [reg+disp32], reg8` of the step (R15 s4.2, both builds).
uint32_t findFireBits(const uint8_t* fn) {
    uint32_t found = 0;
    int hits = 0;
    for (int i = 0; i < 0xc00; ++i) {
        const uint8_t* p = fn + i;
        int o = (p[0] & 0xf0) == 0x40 ? 1 : 0;  // REX
        if (p[o] != 0x84 || (p[o + 1] >> 6) != 2 || (p[o + 1] & 7) == 4) continue;
        uint32_t disp = *reinterpret_cast<const uint32_t*>(p + o + 2);
        if (disp < 0x2000 || disp > 0x3000) continue;
        if (found && disp != found) return 0;
        found = disp;
        ++hits;
    }
    return hits >= 2 ? found : 0;
}

uint8_t* localOwner(uint8_t* weapon) {
    void* local = player_vr::localVrd();
    const unsigned vrdOff = player_vr::vrdOffset();
    if (!local || !vrdOff) return nullptr;
    uint8_t* owner = handleEntity(at<int32_t>(weapon, se4::build().off(se4::gen::CWeaponEntity_en_henOwner)));
    return owner && at<void*>(owner, vrdOff) == local ? owner : nullptr;
}

void hookedStep(uint8_t* weapon, void* arg) {
    uint8_t* player = weapon ? localOwner(weapon) : nullptr;
    uint8_t* params = player ? at<uint8_t*>(weapon, kParams) : nullptr;
    if (!params || !at<int32_t>(params, kTwoHanded) || !(at<uint32_t>(weapon, kFlags) & kCheckedInitialFire)) {
        realStep(weapon, arg);
        return;
    }
    const auto& b = se4::build();
    const bool right = at<int32_t>(weapon, b.off(se4::gen::CWeaponEntity_en_bRightHandWeapon)) != 0;
    const int offXr = right ? 0 : 1, offRole = right ? 1 : 0;  // engine role 0 right, 1 left
    OffHand& o = g_off[offXr];
    if (o.weapon != weapon) o = {}, o.weapon = weapon;

    // Reach: the off controller against the weapon's grip attachment, with the weapon's snap distance.
    float grip7[7] = {}, off7[7] = {};
    barrelPlacement(weapon, grip7, barrelCtx(), g_gripIdent, 0, 0, 1);
    player_vr::controllerWorld(player_vr::localVrd(), offRole, off7);
    const core::Vec3 d{grip7[4] - off7[4], grip7[5] - off7[5], grip7[6] - off7[6]};
    float snap = at<float>(params, kSnapDistance);
    if (!(snap >= 0.0f && snap < 2.0f)) snap = kDefaultSnap;
    o.inReach = core::dot(d, d) <= snap * snap;

    const auto& hs = xr::input().hand(offXr);
    const float s = hs.active ? hs.squeeze : 0.0f;
    const bool press = s > 0.6f, edge = press && !o.pressed;
    o.pressed = press;
    if (!o.engaged && edge && o.inReach) o.engaged = true;
    else if (o.engaged && s < 0.35f) o.engaged = false;

    const uint8_t mask = right ? 2 : 1;  // the other hand's fire bit (R15 s4.2)
    uint8_t& bits = at<uint8_t>(player, g_fireBits);
    const uint8_t saved = bits;
    bits = o.engaged ? uint8_t(saved | mask) : uint8_t(saved & ~mask);
    realStep(weapon, arg);
    bits = saved;

    const bool gripping = (at<uint32_t>(weapon, kFlags) & kGripped) != 0;
    if (o.engaged && !gripping && !o.gripping) o.engaged = false;  // the native test refused: wait for a new press
    if (gripping != o.gripping) VRLOG("hands: %s hand %s the two-handed weapon %p", offXr ? "right" : "left", gripping ? "grips" : "releases", weapon);
    o.gripping = gripping;
    InterlockedExchange(&g_taken[offXr], (o.inReach || gripping) ? 1 : 0);
}
}  // namespace

bool squeezeTaken(int hand) { return (hand == 0 || hand == 1) && g_taken[hand] != 0; }

bool install() {
    if (!se4::options::get("grip_squeeze", 1)) return false;
    const auto& b = se4::build();
    auto* step = b.at<uint8_t>(se4::gen::weapon_step);
    handleEntity = reinterpret_cast<HandleFn>(b.at<void>(se4::gen::handle_entity));
    barrelPlacement = reinterpret_cast<BarrelFn>(b.at<void>(se4::gen::barrel_placement));
    barrelCtx = reinterpret_cast<CtxFn>(b.at<void>(se4::gen::barrel_ctx));
    auto identCtor = reinterpret_cast<void**(__cdecl*)(void**, const char*)>(b.at<void>(se4::gen::ident_ctor));
    if (!step || !handleEntity || !barrelPlacement || !barrelCtx || !identCtor || !b.off(se4::gen::CWeaponEntity_en_henOwner)) {
        VRLOG("hands: build lacks grip symbols - not installed");
        return false;
    }
    g_fireBits = findFireBits(step);
    if (!g_fireBits) {
        VRLOG("hands: fire-button field not found in the weapon step - not installed");
        return false;
    }
    identCtor(&g_gripIdent, "LeftHandGripEffector");
    if (MH_CreateHook(step, reinterpret_cast<void*>(hookedStep), reinterpret_cast<void**>(&realStep)) != MH_OK || MH_EnableHook(step) != MH_OK) {
        VRLOG("hands: weapon step hook FAILED");
        return false;
    }
    VRLOG("hands: squeeze grips two-handed weapons (fire bits at player+0x%x)", g_fireBits);
    return true;
}
}  // namespace hands
