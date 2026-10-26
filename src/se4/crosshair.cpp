#include "se4/crosshair.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/melee.hpp"
#include "se4/options.hpp"
#include "se4/patch.hpp"
#include "se4/player_vr.hpp"
#include "se4/vr_device.hpp"
#include "core/math.hpp"
#include <cstdint>
#include <cstring>
#include <windows.h>
#include <MinHook.h>
#include <cmath>

namespace crosshair {
namespace {
// Aim writer sites: RCX is the player being updated (R8 s4: `mov rcx, rsi` before both calls).
int __cdecl aimGate(uint8_t* player) {
    void* local = player_vr::localVrd();
    unsigned off = player_vr::vrdOffset();
    if (local && off && player && vrdev::installed() && *reinterpret_cast<void**>(player + off) == local) return 0;
    return 1;
}
// HUD element sites draw the local view's HUD; their RCX is not the player.
int __cdecl hudGate() { return player_vr::localVrd() && vrdev::installed() ? 0 : 1; }

// The crosshair's view (player vt+0xdf8, the head centre) composed with the eye being rendered, so each eye
// projects the barrel target from its own position and the crosshair gets the right depth (R8 addendum A2).
using ViewFn = float*(__cdecl*)(uint8_t* player, float* out, int flag);
float* __cdecl eyeView(uint8_t* player, float* out, int flag) {
    float* r = reinterpret_cast<ViewFn>((*reinterpret_cast<void***>(player))[0xdf8 / 8])(player, out, flag);
    static ULONGLONG last;
    static const bool diag = se4::options::get("crosshair_log", 0) != 0;
    if (diag && GetTickCount64() - last > 3000) {
        last = GetTickCount64();
        auto* show = se4::build().at<int32_t>(se4::gen::hud_bShowCrosshair);
        auto* opt = se4::build().at<int32_t>(se4::gen::prj_iGameOptionCrosshair);
        auto pred = reinterpret_cast<int(__cdecl*)(uint8_t*)>(se4::build().at<void>(se4::gen::crosshair_show));
        // Early-outs after the view call (R8 A6): puppet flags bit 13, wheel bit, weapon handle, +0x2568. SM's player
        // fields sit 8 bytes later, like its vrd.
        const int d = int(player_vr::vrdOffset()) - 0x25c8;
        VRLOG("crosshair: render reaches the view (eye %d, show predicate %d, hud_bShowCrosshair %d, flag13 %d, wheel bit %d, weapon %x, +0x2568 %llx)",
              vrdev::currentEye(), pred ? pred(player) : -1, show ? *show : -1, (*reinterpret_cast<uint32_t*>(player + 0xfa8 + d) >> 13) & 1,
              *reinterpret_cast<uint32_t*>(player + 0x269c + d) & 1, *reinterpret_cast<uint32_t*>(player + 0xf90 + d),
              static_cast<unsigned long long>(*reinterpret_cast<uint64_t*>(player + 0x2568 + d)));
        (void)opt;
        const float* hit = reinterpret_cast<const float*>(player + 0x2188 + d);
        float hw[7] = {};
        if (auto headWorld = reinterpret_cast<void*(__cdecl*)(void*, float*)>(se4::build().at<void>(se4::gen::vrd_head_world)))
            if (player_vr::localVrd()) headWorld(player_vr::localVrd(), hw);
        VRLOG("crosshair:   aim hit %.2f %.2f %.2f dist %.2f, view %.2f %.2f %.2f, vrd head world %.2f %.2f %.2f", hit[0], hit[1], hit[2], hit[3],
              r ? r[4] : 0.f, r ? r[5] : 0.f, r ? r[6] : 0.f, hw[4], hw[5], hw[6]);
    }
    const unsigned off = player_vr::vrdOffset();
    float e[7];
    if (!r || !off || !player_vr::localVrd() || *reinterpret_cast<void**>(player + off) != player_vr::localVrd() ||
        !vrdev::eyeInHead(vrdev::currentEye(), e))
        return r;
    const core::Quat q{r[0], r[1], r[2], r[3]};
    const core::Vec3 p = core::Vec3{r[4], r[5], r[6]} + core::rotate(q, {e[4], e[5], e[6]});
    static int burst;
    if (diag && burst < 12 && GetTickCount64() - last > 1000)
        ++burst, VRLOG("crosshair:   eye %d offset %.4f %.4f %.4f rot %.3f %.3f %.3f %.3f -> view %.3f %.3f %.3f (head q %.3f %.3f %.3f %.3f)", vrdev::currentEye(),
                       e[4], e[5], e[6], e[0], e[1], e[2], e[3], p.x, p.y, p.z, r[0], r[1], r[2], r[3]);
    const core::Quat qe = core::multiply(q, core::Quat{e[0], e[1], e[2], e[3]});
    r[0] = qe.x, r[1] = qe.y, r[2] = qe.z, r[3] = qe.w, r[4] = p.x, r[5] = p.y, r[6] = p.z;
    return r;
}

// The show predicate (only reader of hud_bShowCrosshair) returns 0 for every player with vrd: the vanilla engine
// replaced the HUD crosshair by a laser sight in VR. NOP that test (`cmp qword [rbx+vrd],0; jne` at +0xa0) so the
// game's own crosshair is drawn, at the barrel target through the redirects below (R8 addendum A1).
bool showForVr() {
    auto* fn = se4::build().at<uint8_t>(se4::gen::crosshair_show);
    if (!fn) return VRLOG("crosshair: no show predicate in this build"), false;
    uint8_t* at = fn + 0xa0;
    const unsigned vrd = player_vr::vrdOffset() ? player_vr::vrdOffset() : *reinterpret_cast<uint32_t*>(at + 3);
    const uint8_t cmp[8] = {0x48, 0x83, 0xBB, uint8_t(vrd), uint8_t(vrd >> 8), 0, 0, 0};
    const uint8_t jne[6] = {0x0F, 0x85, 0xAF, 0, 0, 0}, nop6[6] = {0x66, 0x0F, 0x1F, 0x44, 0, 0};
    if (memcmp(at, cmp, 8) || (vrd != 0x25c8 && vrd != 0x25d0) || !se4::patch::writeCode(at + 8, jne, nop6, 6)) {
        VRLOG("crosshair: show predicate vrd test not found at %p - crosshair stays hidden in VR", static_cast<void*>(at));
        return false;
    }
    VRLOG("crosshair: show predicate %p no longer hides the crosshair for VR players (vrd +0x%x)", static_cast<void*>(fn), vrd);
    return true;
}
// Redirect the render's single view call (`call [rax+0xdf8]`, FF 90 F8 0D 00 00) to eyeView.
bool perEyeView(uint8_t* render) {
    const uint8_t call[6] = {0xFF, 0x90, 0xF8, 0x0D, 0, 0};
    uint8_t* site = nullptr;
    for (size_t i = 0; i + 6 < 0x1000 && !site; ++i)
        if (!memcmp(render + i, call, 6)) site = render + i;
    void* jump = site ? se4::patch::nearJump(site, reinterpret_cast<void*>(eyeView)) : nullptr;
    if (!jump) return VRLOG("crosshair: view call not found - crosshair without eye offset"), false;
    const intptr_t rel = static_cast<uint8_t*>(jump) - (site + 5);
    if (rel != int32_t(rel)) return false;
    uint8_t bytes[6] = {0xE8, 0, 0, 0, 0, 0x90};
    const int32_t r32 = int32_t(rel);
    memcpy(bytes + 1, &r32, 4);
    if (!se4::patch::writeCode(site, call, bytes, 6)) return false;
    VRLOG("crosshair: view call %p -> per-eye view", static_cast<void*>(site));
    return true;
}

// ---- One crosshair per weapon (R8 A7) ----
// The native element draws one crosshair: the right barrel's target (+0x2188/+0x2194/+0x21e0) with the larger
// spread of both hands. The HUD hook (src/se4/hud_vr.cpp) runs it once per hand that holds a gun (melee weapons
// get none); during each pass these call-site redirects inside the crosshair render, its ring draw and the show
// predicate make it read that pass's weapon, hit entity, target and barrel placement, and ignore the other hand.
// The left barrel's target is cast on the simulation thread right after the native aim writer, into mod-owned
// data only (no player field is written). With no pass active every redirect behaves natively.
using HandleFn = uint8_t*(__cdecl*)(int32_t handle);
using RingFn = void(__cdecl*)(void* element, void* view, void* proj, float scale, uint32_t colour, const float* target, float dist, void* tex);
using OtherFn = uint8_t*(__cdecl*)(uint8_t* weapon);
using PlacementFn = float*(__cdecl*)(uint8_t* player, float* out, void* ctx, int mode);
using AimFn = void(__cdecl*)(uint8_t* player);
using BarrelFn = void(__cdecl*)(uint8_t* weapon, float* out, void* ctx, void* ident, int, int, int);
using CtxFn = void*(__cdecl*)();
using IdentFn = void**(__cdecl*)(void** out, const char* name);
using CastFn = void(__cdecl*)(void* res, void* world, const float* from, const float* to, void* catA, void* catB);
using ResFn = void(__cdecl*)(void* res);
using ReleaseFn = void(__cdecl*)(void* obj, void* owner);

HandleFn handleEntity;
RingFn realRing;
OtherFn otherHand;
AimFn realAim;
BarrelFn barrelPlacement;
CtxFn barrelCtx;
IdentFn identCtor;
CastFn segmentCast;
ResFn resCtor, resDtor;
ReleaseFn refRelease;
void* g_aimCategory;  // CName "camera_aim_ray", the player aim writer's cast category (R8 s4)
void* g_emptyIdent;   // the engine's empty CName (default shooting attachment)
bool g_perWeapon;

template <class T> T& at(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

// The left barrel's aim result, written by the simulation step (main thread, like the HUD render: R8 A7.1).
struct LeftAim {
    uint8_t* weapon = nullptr;
    float target[3]{}, dist = 0, place[7]{};
    int32_t hit = 0;
    bool valid = false;
} g_left;

struct PassState {
    uint8_t* native;  // what the right-weapon handle resolves to
    uint8_t* weapon;  // the weapon of this pass
    bool left;
} * g_pass;

uint8_t* __cdecl passWeapon(int32_t h) {
    uint8_t* e = handleEntity(h);
    return g_pass && e == g_pass->native ? g_pass->weapon : e;
}
uint8_t* __cdecl passHit(int32_t h) { return handleEntity(g_pass && g_pass->left ? g_left.hit : h); }
void __cdecl ringPass(void* element, void* view, void* proj, float scale, uint32_t colour, const float* target, float dist, void* tex) {
    if (g_pass && g_pass->left) target = g_left.target, dist = g_left.dist;
    realRing(element, view, proj, scale, colour, target, dist, tex);
}
uint8_t* __cdecl noOther(uint8_t* weapon) { return g_pass ? nullptr : otherHand(weapon); }
// The ring's mode-1 view (`call [rax+0x340]`): the shoot placement, whose position and axes size the ring.
float* __cdecl passPlacement(uint8_t* player, float* out, void* ctx, int mode) {
    if (g_pass && g_pass->left && mode == 1) {
        memcpy(out, g_left.place, sizeof g_left.place);
        return out;
    }
    return reinterpret_cast<PlacementFn>((*reinterpret_cast<void***>(player))[0x340 / 8])(player, out, ctx, mode);
}

size_t weaponField(bool left) {
    return se4::build().off(left ? se4::gen::CPlayerActorPuppetEntity_en_henCurrentLeftHandWeapon
                                 : se4::gen::CPlayerActorPuppetEntity_en_henCurrentRightHandWeapon);
}
bool isLocalVr(uint8_t* player) {
    const unsigned off = player_vr::vrdOffset();
    return player && off && player_vr::localVrd() && vrdev::installed() && at<void*>(player, off) == player_vr::localVrd();
}

// Left barrel ray, as the aim writer casts the right one: from the barrel along its -Z.
void castLeft(uint8_t* player) {
    g_left.valid = false;
    uint8_t* right = handleEntity(at<int32_t>(player, weaponField(false)));
    uint8_t* left = handleEntity(at<int32_t>(player, weaponField(true)));
    if (!left || left == right || melee::isMeleeWeapon(left)) return;
    if (!g_aimCategory) identCtor(&g_aimCategory, "camera_aim_ray");  // interned on the simulation thread
    if (!g_emptyIdent) identCtor(&g_emptyIdent, "");
    float p7[7] = {};
    barrelPlacement(left, p7, barrelCtx(), g_emptyIdent, 0, 0, 1);
    const core::Quat q = core::normalize({p7[0], p7[1], p7[2], p7[3]});
    const core::Vec3 o{p7[4], p7[5], p7[6]}, dir = core::rotate(q, {0, 0, -1});
    if (!core::finite(core::Pose{q, o})) return;
    constexpr float kMax = 1000.0f;  // as the native VR laser sight (R8 s4)
    const core::Vec3 end = o + dir * kMax;
    alignas(16) uint8_t res[0x200];
    memset(res, 0, sizeof res);
    resCtor(res);
    at<int32_t>(res, 0x00) = at<int32_t>(left, se4::build().off(se4::gen::CWeaponEntity_en_henOwner));  // ignore the owner
    const float from[3] = {o.x, o.y, o.z}, to[3] = {end.x, end.y, end.z};
    segmentCast(res, at<void*>(player, 0x30), from, to, g_aimCategory, g_aimCategory);
    core::Vec3 hitP{at<float>(res, 0x28), at<float>(res, 0x2c), at<float>(res, 0x30)};
    const int32_t hit = at<int32_t>(res, 0x08);
    core::Vec3 d = hitP - o;
    float dist = std::sqrt(core::dot(d, d));
    if (!(dist > 1e-3f && dist <= kMax * 1.01f) || core::dot(d, dir) <= 0) hitP = end, dist = kMax;  // nothing hit
    resDtor(res + 0xa8);
    if (void* r = at<void*>(res, 0x50)) refRelease(r, res + 0x50);
    g_left.weapon = left;
    g_left.target[0] = hitP.x, g_left.target[1] = hitP.y, g_left.target[2] = hitP.z;
    g_left.dist = dist;
    g_left.hit = hit;
    memcpy(g_left.place, p7, sizeof p7);
    g_left.valid = true;
}
void __cdecl hookedAim(uint8_t* player) {
    realAim(player);
    if (g_perWeapon && isLocalVr(player)) castLeft(player);
}

// `E8` sites: a call to `target` in [fn, fn+len). With `field`, only calls preceded within 0x20 bytes by
// `mov ecx,[reg+field]` (the handle the call resolves).
int findCalls(uint8_t* fn, size_t len, void* target, uint32_t field, uint8_t** out, int max) {
    int n = 0;
    for (size_t i = 0x20; fn && i + 5 < len && n < max; ++i) {
        if (fn[i] != 0xE8 || fn + i + 5 + *reinterpret_cast<int32_t*>(fn + i + 1) != target) continue;
        bool ok = !field;
        for (size_t k = i - 0x20; field && k + 4 <= i && !ok; ++k)
            ok = *reinterpret_cast<uint32_t*>(fn + k) == field && fn[k - 2] == 0x8B && (fn[k - 1] & 0xF8) == 0x88;
        if (ok) out[n++] = fn + i;
    }
    return n;
}
bool installPerWeapon() {
    const auto& b = se4::build();
    handleEntity = reinterpret_cast<HandleFn>(b.at<void>(se4::gen::handle_entity));
    realRing = reinterpret_cast<RingFn>(b.at<void>(se4::gen::crosshair_ring));
    otherHand = reinterpret_cast<OtherFn>(b.at<void>(se4::gen::other_hand_weapon));
    barrelPlacement = reinterpret_cast<BarrelFn>(b.at<void>(se4::gen::barrel_placement));
    barrelCtx = reinterpret_cast<CtxFn>(b.at<void>(se4::gen::barrel_ctx));
    identCtor = reinterpret_cast<IdentFn>(b.at<void>(se4::gen::ident_ctor));
    segmentCast = reinterpret_cast<CastFn>(b.at<void>(se4::gen::segment_cast));
    resCtor = reinterpret_cast<ResFn>(b.at<void>(se4::gen::cast_result_ctor));
    resDtor = reinterpret_cast<ResFn>(b.at<void>(se4::gen::cast_result_dtor));
    refRelease = reinterpret_cast<ReleaseFn>(b.at<void>(se4::gen::ref_release));
    auto* render = b.at<uint8_t>(se4::gen::crosshair_render);
    auto* ring = reinterpret_cast<uint8_t*>(realRing);
    auto* show = b.at<uint8_t>(se4::gen::crosshair_show);
    auto* aim = b.at<uint8_t>(se4::gen::aim_writer);
    if (!handleEntity || !realRing || !otherHand || !barrelPlacement || !barrelCtx || !identCtor || !segmentCast || !resCtor || !resDtor ||
        !refRelease || !render || !show || !aim || !weaponField(false) || !weaponField(true) || !b.off(se4::gen::CWeaponEntity_en_henOwner))
        return VRLOG("crosshair: build lacks the per-weapon symbols - one crosshair"), false;
    const int d = int(player_vr::vrdOffset()) - 0x25c8;  // SM's player fields sit 8 bytes later
    // R8 A7.2: R1/G1/P1 right-weapon lookups, R2 hit entity, R5 ring call, G2 other hand, G4 mode-1 placement.
    struct Call {
        uint8_t* fn;
        size_t len;
        void* target;
        uint32_t field;
        void* thunk;
        const char* what;
        uint8_t* site;
    } calls[] = {
        {render, 0x900, reinterpret_cast<void*>(handleEntity), uint32_t(0xf90 + d), reinterpret_cast<void*>(passWeapon), "render weapon", nullptr},
        {render, 0x900, reinterpret_cast<void*>(handleEntity), uint32_t(0x21e0 + d), reinterpret_cast<void*>(passHit), "render hit entity", nullptr},
        {render, 0x900, ring, 0, reinterpret_cast<void*>(ringPass), "ring call", nullptr},
        {ring, 0x900, reinterpret_cast<void*>(handleEntity), uint32_t(0xf90 + d), reinterpret_cast<void*>(passWeapon), "ring weapon", nullptr},
        {ring, 0x900, reinterpret_cast<void*>(otherHand), 0, reinterpret_cast<void*>(noOther), "ring other hand", nullptr},
        {show, 0x200, reinterpret_cast<void*>(handleEntity), uint32_t(0xf90 + d), reinterpret_cast<void*>(passWeapon), "show weapon", nullptr},
    };
    for (auto& c : calls) {
        uint8_t* found[2];
        if (findCalls(c.fn, c.len, c.target, c.field, found, 2) != 1)
            return VRLOG("crosshair: per-weapon site '%s' not found exactly once - one crosshair", c.what), false;
        c.site = found[0];
    }
    // G4: the second `call [rax+0x340]` in the ring, right after `mov r9d,1; mov rcx,r15`.
    const uint8_t view[6] = {0xFF, 0x90, 0x40, 0x03, 0, 0}, mode1[6] = {0x41, 0xB9, 1, 0, 0, 0};
    uint8_t* g4 = nullptr;
    int n4 = 0;
    for (size_t i = 9; i + 6 < 0x900; ++i)
        if (!memcmp(ring + i, view, 6) && !memcmp(ring + i - 9, mode1, 6)) g4 = ring + i, ++n4;
    if (n4 != 1) return VRLOG("crosshair: ring mode-1 view call not found exactly once (%d) - one crosshair", n4), false;
    void* aimTramp = nullptr;
    if (MH_CreateHook(aim, reinterpret_cast<void*>(hookedAim), &aimTramp) != MH_OK) return VRLOG("crosshair: aim writer hook FAILED"), false;
    realAim = reinterpret_cast<AimFn>(aimTramp);
    for (auto& c : calls) {
        void* jump = se4::patch::nearJump(c.site, c.thunk);
        if (!jump || !se4::patch::retargetCall(c.site, jump)) return VRLOG("crosshair: retargeting %s FAILED", c.what), false;
        VRLOG("crosshair: %s call %p -> per-weapon pass", c.what, static_cast<void*>(c.site));
    }
    void* jump = se4::patch::nearJump(g4, reinterpret_cast<void*>(passPlacement));
    const intptr_t rel = jump ? static_cast<uint8_t*>(jump) - (g4 + 5) : 0;
    uint8_t bytes[6] = {0xE8, 0, 0, 0, 0, 0x90};
    const int32_t r32 = int32_t(rel);
    memcpy(bytes + 1, &r32, 4);
    if (!jump || rel != r32 || !se4::patch::writeCode(g4, view, bytes, 6)) return VRLOG("crosshair: ring mode-1 view patch FAILED"), false;
    if (MH_EnableHook(aim) != MH_OK) return VRLOG("crosshair: aim writer hook enable FAILED"), false;
    g_perWeapon = true;
    VRLOG("crosshair: one crosshair per weapon (left barrel cast after the aim writer %p)", static_cast<void*>(aim));
    return true;
}

struct Site {
    se4::gen::Sym fn;
    size_t len;
    int expected;
    void* gate;
    const char* what;
};
}  // namespace

bool install() {
    if (!se4::options::get("native_crosshair", 1)) {
        VRLOG("crosshair: native projected crosshair disabled by option");
        return false;
    }
    const Site sites[] = {
        {se4::gen::aim_writer, 0x1400, 2, reinterpret_cast<void*>(aimGate), "aim writer"},
        {se4::gen::crosshair_render, 0x1000, 2, reinterpret_cast<void*>(hudGate), "crosshair element"},
        {se4::gen::object_holder_render, 0x1200, 1, reinterpret_cast<void*>(hudGate), "object-holder element"},
    };
    // Verify every site in every function before changing any of them.
    uint8_t* found[3][4];
    for (int i = 0; i < 3; ++i) {
        auto* fn = se4::build().at<uint8_t>(sites[i].fn);
        int n = fn ? se4::patch::findReturnOneCalls(fn, sites[i].len, found[i], 4) : -1;
        if (n != sites[i].expected) {
            VRLOG("crosshair: %s has %d predicate calls, expected %d - not installed", sites[i].what, n, sites[i].expected);
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        void* jump = se4::patch::nearJump(found[i][0], sites[i].gate);
        for (int k = 0; k < sites[i].expected; ++k) {
            if (!jump || !se4::patch::retargetCall(found[i][k], jump)) {
                VRLOG("crosshair: retargeting %s call %p FAILED", sites[i].what, static_cast<void*>(found[i][k]));
                return false;
            }
            VRLOG("crosshair: %s predicate call %p -> VR gate", sites[i].what, static_cast<void*>(found[i][k]));
        }
    }
    if (showForVr()) perEyeView(se4::build().at<uint8_t>(se4::gen::crosshair_render));
    if (se4::options::get("crosshair_per_weapon", 1)) installPerWeapon();
    return true;
}

bool forEachPass(uint8_t* player, void (*draw)(void* user, float dist), void* user) {
    if (!g_perWeapon || !isLocalVr(player)) return false;
    const int d = int(player_vr::vrdOffset()) - 0x25c8;
    // The render's own early-outs (R8 A7.2 R0): consumed hide bit 13, or the weapon wheel: one native call.
    if ((at<uint32_t>(player, 0xfa8 + d) >> 13 & 1) || (at<uint32_t>(player, 0x269c + d) & 1)) return false;
    uint8_t* right = handleEntity(at<int32_t>(player, weaponField(false)));
    uint8_t* left = handleEntity(at<int32_t>(player, weaponField(true)));
    if (!right && !left) return false;  // no weapon (riding): the native branch
    if (right && !melee::isMeleeWeapon(right)) {
        PassState p{right, right, false};
        g_pass = &p;
        float dist = at<float>(player, 0x2194 + d);
        draw(user, dist);
        g_pass = nullptr;
    }
    if (left && left != right && !melee::isMeleeWeapon(left) && g_left.valid && g_left.weapon == left) {
        PassState p{right, left, true};
        g_pass = &p;
        draw(user, g_left.dist);
        g_pass = nullptr;
    }
    return true;
}
}  // namespace crosshair
