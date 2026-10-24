#include "se4/scope.hpp"
#include "core/math.hpp"
#include "core/stereo.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/player_vr.hpp"
#include "vk/eye_images.hpp"
#include "xr/input.hpp"
#include "xr/runtime.hpp"
#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace scope {
namespace {
using StepFn = void(__cdecl*)(uint8_t* mode);
using ParamsFn = uint8_t*(__cdecl*)(uint8_t* mode);
using HandleFn = uint8_t*(__cdecl*)(int32_t handle);
using BarrelFn = void(__cdecl*)(uint8_t* weapon, float* out, void* ctx, void* ident, int, int, int);
using CtxFn = void*(__cdecl*)();
using FovFn = float(__cdecl*)(uint8_t* player);
using HeadWorldFn = void*(__cdecl*)(void* vrd, float* out);
using HolderCreateFn = int(__cdecl*)(void* holder, void* gfx, int w, int h, int fmt, int p6, unsigned flags, int p8);
using HolderGetFn = void*(__cdecl*)(void* holder);
using HolderCtorFn = void*(__cdecl*)(void* mem);
using AddRefFn = void(__cdecl*)(void* obj, void* owner);
using AllocFn = void*(__cdecl*)(size_t size, uintptr_t tag);
using GfxTransitionFn = void(__cdecl*)(void* gfx, void* tex, int mode);

template <class T> T& at(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

StepFn realStep;
ParamsFn modeParams;
HandleFn handleEntity;
BarrelFn barrelPlacement;
CtxFn barrelCtx;
FovFn fovBase;
HeadWorldFn headWorld;
void* g_emptyIdent;  // the engine's empty CName (= default shooting attachment)
void**(__cdecl* g_identCtor)(void**, const char*);
volatile bool g_identReady;

int g_mode = 1;  // option scope
int g_res = 768;
float g_size = 0.04f, g_eyeUp = 0.09f, g_eyeBack = 0.03f;
bool g_reticle = true;

SRWLOCK g_lock = SRWLOCK_INIT;
// Written by the weapon step (simulation), read once per frame in slot 0x030.
struct Held {
    uint8_t* weapon = nullptr;
    uint8_t* player = nullptr;
    int role = 0;  // engine role: 0 right, 1 left
    float zoomFactor = 1;
    bool zoomed = false;  // native zoom handler active
    ULONGLONG tick = 0;
} g_held;

// This frame (main thread, slots 0x030 .. 0x038).
struct Frame {
    bool live = false;     // eye 3 renders the scope this frame
    core::Pose camera{};   // scope camera, world (engine units)
    float fovY = 0.4f;     // radians
    void* gfx = nullptr;
    void* rt = nullptr;    // slot 0x028(3) result
    int32_t savedMr = 0, savedSkip = 0;
} g_frame;

// For the layer (present thread).
struct Shared {
    bool live = false;
    int hand = 1;           // XR hand index: 0 left, 1 right
    core::Quat gripToBarrel{};
} g_shared;

void* g_holder;
int g_rtW, g_rtH;

uint8_t* localOwner(uint8_t* weapon) {
    const auto& b = se4::build();
    void* local = player_vr::localVrd();
    const unsigned vrdOff = player_vr::vrdOffset();
    if (!local || !vrdOff || !handleEntity) return nullptr;
    uint8_t* owner = handleEntity(at<int32_t>(weapon, b.off(se4::gen::CWeaponEntity_en_henOwner)));
    return owner && at<void*>(owner, vrdOff) == local ? owner : nullptr;
}

void hookedStep(uint8_t* mode) {
    realStep(mode);
    if (!g_identReady) {
        g_identCtor(&g_emptyIdent, "");  // interned on the simulation thread
        g_identReady = true;
    }
    uint8_t* weapon = at<uint8_t*>(mode, 8);
    uint8_t* player = weapon ? localOwner(weapon) : nullptr;
    if (!player) return;
    uint8_t* params = modeParams ? modeParams(mode) : nullptr;
    const auto& b = se4::build();
    Held h;
    h.weapon = weapon;
    h.player = player;
    h.role = at<int32_t>(weapon, b.off(se4::gen::CWeaponEntity_en_bRightHandWeapon)) ? 0 : 1;
    h.zoomFactor = params ? at<float>(params, 0xd8) : 1.0f;  // swmp_fZoomFactor (R14 s2.1)
    if (!(h.zoomFactor > 1.0f && h.zoomFactor < 100.0f)) h.zoomFactor = 4.0f;
    h.zoomed = at<uint8_t>(mode, 0x28) != 0;  // swm_szhZoomHandler.szh_bZoomActive
    h.tick = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    static bool announced;
    if (!announced || g_held.weapon != weapon) {
        announced = true;
        VRLOG("scope: scoped weapon %p in the %s hand (zoom factor %.2f)", weapon, h.role ? "left" : "right", h.zoomFactor);
    }
    g_held = h;
    ReleaseSRWLockExclusive(&g_lock);
}

core::Pose fromP7(const float* p) { return {core::normalize({p[0], p[1], p[2], p[3]}), {p[4], p[5], p[6]}}; }

void setCvar(se4::gen::Sym s, int32_t v, int32_t* saved) {
    if (auto* c = se4::build().at<int32_t>(s)) {
        if (saved) *saved = *c;
        *c = v;
    }
}
}  // namespace

void beginFrame() {
    g_frame.live = false;
    g_frame.rt = nullptr;
    if (!g_mode || !realStep) return;
    Held h;
    AcquireSRWLockShared(&g_lock);
    h = g_held;
    ReleaseSRWLockShared(&g_lock);
    void* vrd = player_vr::localVrd();
    if (!h.weapon || !vrd || !g_identReady || GetTickCount64() - h.tick > 250) {  // not stepped recently: holstered or dropped
        g_shared.live = false;
        return;
    }
    // Scope camera = the barrel placement the shots use.
    float p7[7] = {};
    barrelPlacement(h.weapon, p7, barrelCtx(), g_emptyIdent, 0, 0, 1);
    core::Pose cam = fromP7(p7);
    float hand7[7] = {};
    player_vr::controllerWorld(vrd, h.role, hand7);
    core::Pose hand = fromP7(hand7);
    if (!core::finite(cam) || !core::finite(hand)) return;
    const int xrHand = h.role == 0 ? 1 : 0;

    bool want = g_mode == 2 || h.zoomed;
    if (!want && g_mode == 1) {
        // Near an eye: the eyepiece (grip + offset in the barrel frame) within 20 cm of the head.
        const auto& v = xr::runtime().views();
        const auto& grip = xr::input().hand(xrHand).grip;
        if (v.valid && xr::input().hand(xrHand).active) {
            core::Quat q = core::multiply({grip.orientation.x, grip.orientation.y, grip.orientation.z, grip.orientation.w},
                                          core::multiply(core::inverse(hand.q), cam.q));
            core::Vec3 eyepiece = core::Vec3{grip.position.x, grip.position.y, grip.position.z} + core::rotate(q, {0, g_eyeUp, g_eyeBack});
            core::Vec3 d = eyepiece - core::Vec3{v.head.position.x, v.head.position.y, v.head.position.z};
            want = core::dot(d, d) < 0.2f * 0.2f;
        }
    }
    g_shared.hand = xrHand;
    g_shared.gripToBarrel = core::multiply(core::inverse(hand.q), cam.q);
    g_shared.live = want;
    if (!want) return;

    const float base = std::clamp(fovBase(h.player), 5.0f, 135.0f);
    g_frame.fovY = base / h.zoomFactor * core::Pi / 180.0f;
    g_frame.camera = cam;
    g_frame.live = true;
    setCvar(se4::gen::vr_iMixedRealityOutput, 1, &g_frame.savedMr);
    setCvar(se4::gen::vr_bSkipMixedRealityWeaponRendering, 1, &g_frame.savedSkip);
    setCvar(se4::gen::vr_iRenderMixedRealityVideo, 0, nullptr);
}

void endFrame() {
    if (!g_frame.live) return;
    g_frame.live = false;
    setCvar(se4::gen::vr_iMixedRealityOutput, g_frame.savedMr, nullptr);
    setCvar(se4::gen::vr_bSkipMixedRealityWeaponRendering, g_frame.savedSkip, nullptr);
    if (!g_frame.rt || !g_frame.gfx) return;
    // Hand the image over like the engine does for eyes 1/2 before SubmitEye: gfx vt+0x288(tex, 3) records the
    // transition to TRANSFER_SRC_OPTIMAL in the engine's command buffer (docs/re/EYE_TARGETS.md).
    auto transition = reinterpret_cast<GfxTransitionFn>((*reinterpret_cast<void***>(g_frame.gfx))[0x288 / 8]);
    transition(g_frame.gfx, g_frame.rt, 3);
    eyes::submitted(3);
}

void* target(void* gfx) {
    if (!g_frame.live) return nullptr;
    const auto& b = se4::build();
    auto alloc = reinterpret_cast<AllocFn>(b.at<void>(se4::gen::engine_alloc));
    auto ctor = reinterpret_cast<HolderCtorFn>(b.at<void>(se4::gen::holder_ctor));
    auto addref = reinterpret_cast<AddRefFn>(b.at<void>(se4::gen::ref_addref));
    auto create = reinterpret_cast<HolderCreateFn>(b.at<void>(se4::gen::holder_create));
    auto get = reinterpret_cast<HolderGetFn>(b.at<void>(se4::gen::holder_get));
    if (!alloc || !ctor || !addref || !create || !get) return nullptr;
    if (!g_holder) {
        void* mem = alloc(0x90, 0);
        if (!mem) return nullptr;
        g_holder = ctor(mem);
        addref(g_holder, &g_holder);
    }
    const bool resized = g_rtW != g_res || g_rtH != g_res;
    eyes::beginCapture(3, uint32_t(g_res), uint32_t(g_res));
    create(g_holder, gfx, g_res, g_res, 2, 0, 0, 0);
    eyes::endCapture(resized);
    if (resized) {
        g_rtW = g_rtH = g_res;
        VRLOG("scope: render target %dx%d", g_res, g_res);
    }
    void* wrapper = get(g_holder);
    void* rt = wrapper ? *reinterpret_cast<void**>(static_cast<uint8_t*>(wrapper) + 8) : nullptr;
    g_frame.gfx = gfx;
    g_frame.rt = rt;
    return rt;
}

void projection(float* m, float n, float f) {
    const float h = g_frame.fovY * 0.5f;
    core::engineProjection({-h, h, h, -h}, n, f, m);
}

void eyeTransform(float* m) {
    // The engine places eye 3 at head world (eye-3 anchor) o inverse(this). Return the head-to-scope-camera
    // transform so the camera lands exactly on the barrel placement; both ends are engine placements, so no
    // world-scale division (R14 s6 c.4).
    core::Pose e{};
    void* vrd = player_vr::localVrd();
    auto fn = se4::build().at<void>(se4::gen::vrd_head_world);
    if (g_frame.live && vrd && fn) {
        float p7[7] = {};
        reinterpret_cast<HeadWorldFn>(fn)(vrd, p7);
        core::Pose head = fromP7(p7);
        if (core::finite(head)) e = core::inverse(core::compose(core::inverse(head), g_frame.camera));
    }
    core::matrix34(e, m);
}

Layer layer(const XrPosef& rightGrip, const XrPosef& leftGrip) {
    Layer l;
    if (!g_shared.live) return l;
    const XrPosef& g = g_shared.hand ? rightGrip : leftGrip;
    core::Quat q = core::normalize(core::multiply({g.orientation.x, g.orientation.y, g.orientation.z, g.orientation.w}, g_shared.gripToBarrel));
    core::Vec3 p = core::Vec3{g.position.x, g.position.y, g.position.z} + core::rotate(q, {0, g_eyeUp, g_eyeBack});
    l.visible = true;
    l.pose.orientation = {q.x, q.y, q.z, q.w};
    l.pose.position = {p.x, p.y, p.z};
    l.size = g_size;
    l.reticle = g_reticle;
    return l;
}

bool install() {
    g_mode = std::clamp(se4::options::get("scope", 1), 0, 2);
    if (!g_mode) return false;
    g_res = std::clamp(se4::options::get("scope_res", 768), 256, 2048);
    g_size = std::clamp(se4::options::get("scope_size_cm", 4), 1, 20) / 100.0f;
    g_eyeUp = se4::options::get("scope_eye_up_cm", 9) / 100.0f;
    g_eyeBack = se4::options::get("scope_eye_back_cm", 3) / 100.0f;
    g_reticle = se4::options::get("scope_reticle", 1) != 0;
    const auto& b = se4::build();
    auto* step = b.at<void>(se4::gen::sniping_step);
    modeParams = reinterpret_cast<ParamsFn>(b.at<void>(se4::gen::mode_params));
    handleEntity = reinterpret_cast<HandleFn>(b.at<void>(se4::gen::handle_entity));
    barrelPlacement = reinterpret_cast<BarrelFn>(b.at<void>(se4::gen::barrel_placement));
    barrelCtx = reinterpret_cast<CtxFn>(b.at<void>(se4::gen::barrel_ctx));
    fovBase = reinterpret_cast<FovFn>(b.at<void>(se4::gen::fov_base));
    auto identCtor = reinterpret_cast<void**(__cdecl*)(void**, const char*)>(b.at<void>(se4::gen::ident_ctor));
    if (!step || !modeParams || !handleEntity || !barrelPlacement || !barrelCtx || !fovBase || !identCtor ||
        !b.at<void>(se4::gen::vr_iMixedRealityOutput) || !b.at<void>(se4::gen::vr_bSkipMixedRealityWeaponRendering) ||
        !b.off(se4::gen::CWeaponEntity_en_henOwner)) {
        VRLOG("scope: build lacks scope symbols - not installed");
        return false;
    }
    g_identCtor = identCtor;
    if (MH_CreateHook(step, reinterpret_cast<void*>(hookedStep), reinterpret_cast<void**>(&realStep)) != MH_OK || MH_EnableHook(step) != MH_OK) {
        VRLOG("scope: hook FAILED");
        realStep = nullptr;
        return false;
    }
    VRLOG("scope: picture-in-picture scopes on (mode %d, %dpx, %.0f cm)", g_mode, g_res, g_size * 100);
    return true;
}
}  // namespace scope
