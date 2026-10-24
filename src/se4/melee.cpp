// Physical melee for the knife (docs/re/R12_melee_gadgets.md s2, s3, s5, s15). For players with CPlayerVRData
// the knife's canned body/view arc is skipped; instead, after each knife step the blade held in the hand is
// swept from its previous pose with the native segment cast, and hits above vr_fMinSwingToHitSpeed apply
// damage exactly as the knife's own swing test does. The engine applies health only on the authority, so the
// sweep can run on every machine; only the server's counts (server-side validation is the replicated hand
// pose itself). Option melee_physical (default 1).
#include "se4/melee.hpp"
#include "core/math.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/player_vr.hpp"
#include "xr/input.hpp"
#include "xr/runtime.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cmath>
#include <cstring>

namespace melee {
namespace {
using StepFn = void(__cdecl*)(uint8_t* weapon, void* stepArg);
using HandleFn = void*(__cdecl*)(int32_t handle);
using CastFn = void(__cdecl*)(uint8_t* res, void* world, const float* from, const float* to, void* catA, void* catB);
using ResCtorFn = void*(__cdecl*)(uint8_t* res);
using ResDtorFn = void(__cdecl*)(uint8_t* member);
using DamageCtorFn = void*(__cdecl*)(uint8_t* dmg);
using ParamsFn = uint8_t**(__cdecl*)(uint8_t* weapon, void** ref);
using ReleaseFn = void(__cdecl*)(void* obj, void* owner);
using IdentFn = void**(__cdecl*)(void** out, const char* name);

StepFn realStep, realSwingTest;
HandleFn handleEntity;
CastFn segmentCast;
ResCtorFn resCtor;
ResDtorFn resDtor;
DamageCtorFn damageCtor;
ParamsFn weaponParams;
ReleaseFn refRelease;
IdentFn identFn;
void* g_bullet;  // CName "bullet" (the knife's cast category)
float* g_minSpeed;

constexpr float kBladeStart = 0.04f, kBladeEnd = 0.30f;  // metres along the hand's -Z (knife held forward)
constexpr double kTargetCooldown = 0.35;                  // seconds before the same target can be hit again

struct Track {
    uint8_t* weapon = nullptr;
    core::Vec3 base{}, mid{}, tip{};
    LARGE_INTEGER t{};
    int32_t hit[8] = {};
    double hitTime[8] = {};
    bool valid = false;
};
Track g_tracks[16];
LARGE_INTEGER g_freq;

template <class T> T& at(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

Track& track(uint8_t* weapon) {
    Track* free = nullptr;
    for (auto& t : g_tracks) {
        if (t.weapon == weapon) return t;
        if (!t.weapon && !free) free = &t;
    }
    Track& t = free ? *free : g_tracks[uintptr_t(weapon) % 16];
    t = {};
    t.weapon = weapon;
    return t;
}

uint8_t* ownerOf(uint8_t* weapon) { return static_cast<uint8_t*>(handleEntity(at<int32_t>(weapon, 0x98))); }
void* vrdOf(uint8_t* player) { return player ? at<void*>(player, player_vr::vrdOffset()) : nullptr; }

double seconds(LARGE_INTEGER a, LARGE_INTEGER b) { return double(b.QuadPart - a.QuadPart) / double(g_freq.QuadPart); }

void hookedSwingTest(uint8_t* weapon, void* stepArg) {
    if (vrdOf(ownerOf(weapon))) return;  // VR owners hit with the hand-held blade instead (no canned arc)
    realSwingTest(weapon, stepArg);
}

void applyHit(uint8_t* weapon, uint8_t* owner, uint8_t* target, uint8_t* res, void* stepArg, core::Vec3 dir, uint8_t* params) {
    alignas(16) uint8_t dmg[0x100];
    memset(dmg, 0, sizeof dmg);
    damageCtor(dmg);
    using InflictorFn = void(__cdecl*)(uint8_t*, uint8_t*);
    (*reinterpret_cast<InflictorFn**>(owner))[0x3e8 / 8](owner, dmg);
    const auto& b = se4::build();
    at<uint32_t>(dmg, 0x78) |= 4;  // dmg_ulFlags, as the knife sets
    at<int32_t>(dmg, 0x64) = 2;    // DMT_CUT
    using CheckFn = int(__cdecl*)(uint8_t*, uint8_t*, int, uint64_t);
    if (!(*reinterpret_cast<CheckFn**>(target))[0x3f0 / 8](target, dmg, 2, at<uint64_t>(res, 0x68))) return;
    using IndexFn = int(__cdecl*)(uint8_t*);
    at<int32_t>(dmg, 0x68) = (*reinterpret_cast<IndexFn**>(weapon))[0x730 / 8](weapon);
    at<int32_t>(dmg, 0x60) = at<int32_t>(params, b.off(se4::gen::CMacheteWeaponParams_mwp_slDamage));
    at<float>(dmg, 0x48) = dir.x, at<float>(dmg, 0x4c) = dir.y, at<float>(dmg, 0x50) = dir.z;
    memcpy(dmg + 0x54, res + 0x28, 12);                  // hit point
    at<uint64_t>(dmg, 0x80) = at<uint64_t>(res, 0x60);   // hull part
    at<uint64_t>(dmg, 0x88) = at<uint64_t>(res, 0x68);
    at<uint64_t>(dmg, 0x90) = at<uint64_t>(res, 0x50);   // borrowed reference, as the native swing test does
    using ReceiveFn = void(__cdecl*)(uint8_t*, void*, uint8_t*);
    (*reinterpret_cast<ReceiveFn**>(target))[0x400 / 8](target, stepArg, dmg);
}

void sweep(uint8_t* weapon, void* stepArg) {
    uint8_t* owner = ownerOf(weapon);
    void* vrd = vrdOf(owner);
    if (!vrd) return;
    const auto& b = se4::build();
    const int role = at<int32_t>(weapon, b.off(se4::gen::CWeaponEntity_en_bRightHandWeapon)) ? 0 : 1;
    float p7[7];
    player_vr::controllerWorld(vrd, role, p7);
    if (p7[0] == 0 && p7[1] == 0 && p7[2] == 0 && p7[3] == 0) return;
    core::Pose hand{core::normalize({p7[0], p7[1], p7[2], p7[3]}), {p7[4], p7[5], p7[6]}};
    float scale = 1.0f;
    if (auto* ws = b.at<float>(se4::gen::vr_fWorldScale); ws && *ws > 0.01f) scale = 1.0f / *ws;
    core::Vec3 base = hand.p + core::rotate(hand.q, {0, 0, -kBladeStart * scale});
    core::Vec3 tip = hand.p + core::rotate(hand.q, {0, 0, -kBladeEnd * scale});
    core::Vec3 mid = (base + tip) * 0.5f;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    Track& t = track(weapon);
    if (!t.valid) {
        t.base = base, t.mid = mid, t.tip = tip, t.t = now, t.valid = true;
        return;
    }
    double dt = seconds(t.t, now);
    if (dt <= 1e-4) return;
    core::Vec3 dv = tip - t.tip;
    float speed = std::sqrt(core::dot(dv, dv)) / float(dt) / scale;  // metres per second
    const float minSpeed = g_minSpeed && *g_minSpeed > 0.1f ? *g_minSpeed : 2.0f;
    if (speed >= minSpeed && dt < 0.25) {
        void* ref = nullptr;
        uint8_t** pp = weaponParams(weapon, &ref);
        uint8_t* params = pp ? *pp : nullptr;
        if (params) {
            alignas(16) uint8_t res[0x200];
            memset(res, 0, sizeof res);
            resCtor(res);
            at<int32_t>(res, 0x00) = at<int32_t>(weapon, 0x98);  // ignore the owner
            at<float>(res, 0x98) = at<float>(params, b.off(se4::gen::CMacheteWeaponParams_mwp_fDamageRayRadius));
            core::Vec3 dir = dv * (1.0f / std::sqrt(core::dot(dv, dv)));
            const core::Vec3 from[3] = {t.tip, t.mid, base}, to[3] = {tip, mid, tip};
            double nowS = double(now.QuadPart) / double(g_freq.QuadPart);
            for (int i = 0; i < 3; ++i) {
                const float f[3] = {from[i].x, from[i].y, from[i].z}, e[3] = {to[i].x, to[i].y, to[i].z};
                segmentCast(res, at<void*>(weapon, 0x30), f, e, g_bullet, g_bullet);
                int32_t h = at<int32_t>(res, 0x08);
                auto* target = static_cast<uint8_t*>(h ? handleEntity(h) : nullptr);
                if (!target || target == owner) continue;
                bool recent = false;
                int slot = 0;
                for (int k = 0; k < 8; ++k) {
                    if (t.hit[k] == h && nowS - t.hitTime[k] < kTargetCooldown) recent = true;
                    if (t.hitTime[k] < t.hitTime[slot]) slot = k;
                }
                if (recent) continue;
                t.hit[slot] = h, t.hitTime[slot] = nowS;
                applyHit(weapon, owner, target, res, stepArg, dir, params);
                if (vrd == player_vr::localVrd() && xr::runtime().running())
                    xr::input().vibrate(xr::runtime().session(), role == 0 ? 1 : 0, 0.9f, 0.06f);
                static int logged;
                if (logged++ < 20) VRLOG("melee: hit %p at %.1f m/s (hand %s)", static_cast<void*>(target), speed, role ? "left" : "right");
            }
            resDtor(res + 0xa8);
            if (void* r = at<void*>(res, 0x50)) refRelease(r, res + 0x50);
        }
        if (ref) refRelease(ref, &ref);
    }
    t.base = base, t.mid = mid, t.tip = tip, t.t = now;
}

void hookedStep(uint8_t* weapon, void* stepArg) {
    realStep(weapon, stepArg);
    if (!g_bullet) identFn(&g_bullet, "bullet");  // CName interned on the simulation thread, like the engine's
    sweep(weapon, stepArg);
}

bool hook(void* target, void* detour, void** real, const char* what) {
    if (!target || MH_CreateHook(target, detour, real) != MH_OK || MH_EnableHook(target) != MH_OK) {
        VRLOG("melee: %s hook FAILED", what);
        return false;
    }
    return true;
}
}  // namespace

bool install() {
    if (!se4::options::get("melee_physical", 1)) return false;
    const auto& b = se4::build();
    handleEntity = reinterpret_cast<HandleFn>(b.at<void>(se4::gen::handle_entity));
    segmentCast = reinterpret_cast<CastFn>(b.at<void>(se4::gen::segment_cast));
    resCtor = reinterpret_cast<ResCtorFn>(b.at<void>(se4::gen::cast_result_ctor));
    resDtor = reinterpret_cast<ResDtorFn>(b.at<void>(se4::gen::cast_result_dtor));
    damageCtor = reinterpret_cast<DamageCtorFn>(b.at<void>(se4::gen::damage_ctor));
    weaponParams = reinterpret_cast<ParamsFn>(b.at<void>(se4::gen::weapon_params));
    refRelease = reinterpret_cast<ReleaseFn>(b.at<void>(se4::gen::ref_release));
    identFn = reinterpret_cast<IdentFn>(b.at<void>(se4::gen::ident_ctor));
    g_minSpeed = b.at<float>(se4::gen::vr_fMinSwingToHitSpeed);
    auto* step = b.at<void>(se4::gen::knife_step);
    auto* swing = b.at<void>(se4::gen::knife_swing_test);
    if (!handleEntity || !segmentCast || !resCtor || !resDtor || !damageCtor || !weaponParams || !refRelease || !identFn || !step || !swing) {
        VRLOG("melee: build lacks melee symbols - physical melee off");
        return false;
    }
    QueryPerformanceFrequency(&g_freq);
    if (!hook(swing, reinterpret_cast<void*>(hookedSwingTest), reinterpret_cast<void**>(&realSwingTest), "knife swing test") ||
        !hook(step, reinterpret_cast<void*>(hookedStep), reinterpret_cast<void**>(&realStep), "knife step"))
        return false;
    VRLOG("melee: physical knife active");
    return true;
}

}  // namespace melee
