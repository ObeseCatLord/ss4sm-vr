#include "se4/hud_vr.hpp"
#include "core/math.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/patch.hpp"
#include "se4/vr_device.hpp"
#include "se4/player_vr.hpp"
#include "se4/wheel_panel.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace hud_vr {
namespace {
using ElementFn = void(__cdecl*)(void** ctx, uint8_t* element);
ElementFn realElement;
using FlushFn = void(__cdecl*)(void* canvas, void* gfx);
FlushFn flushCanvas;
void** g_gfx;  // the engine's gfx global, read from the element render's own `mov rcx,[rip+gfx]` (+0x93)
// Draw what this element queued now, with its own matrix: queue entries keep the object matrix current when the
// entry was created, so later elements would inherit the first one's (R8 A6).
void flush(void** ctx) {
    if (!flushCanvas || !g_gfx || !*g_gfx || !ctx[10]) return;
    if (void* canvas = *reinterpret_cast<void**>(static_cast<uint8_t*>(ctx[10]) + 0xb0)) flushCanvas(canvas, *g_gfx);
}
void* g_wheelRender;
void* g_crosshairRender;
bool g_noBatch = true;
// hud_vr_mode=0 (flat, default): the HUD render's immersive-flag call (+0x1e1, `E8` to the device+0x08 getter) answers 0,
// so the whole HUD takes its 2D branch in each eye image, as in cinema frames (R8 A5 primary fix).
int g_mode = 0;
using FlagFn = int(__cdecl*)();
FlagFn realFlag;
int __cdecl flatGate() {
    float p[7];
    // While the weapon wheel is open the HUD keeps its 3D branch: the wheel is drawn on the hand panel (the other
    // elements give way to it then).
    return g_mode == 0 && !wheel_panel::panel(p) ? 0 : realFlag();
}
bool gateHud() {
    auto* fn = se4::build().at<uint8_t>(se4::gen::hud_render);
    uint8_t* site = fn ? fn + 0x1e1 : nullptr;
    if (!site || site[0] != 0xE8) return VRLOG("hud_vr: HUD render flag call not found"), false;
    uint8_t* target = site + 5 + *reinterpret_cast<int32_t*>(site + 1);
    // the getter: mov rax,[rip+device]; test rax,rax; jne; ret; mov eax,[rax+8]; ret
    if (!(target[0] == 0x48 && target[1] == 0x8B && target[2] == 0x05 && target[13] == 0x8B && target[14] == 0x40 && target[15] == 0x08))
        return VRLOG("hud_vr: unexpected HUD render flag getter %p", static_cast<void*>(target)), false;
    realFlag = reinterpret_cast<FlagFn>(target);
    void* jump = se4::patch::nearJump(site, reinterpret_cast<void*>(flatGate));
    if (!jump || !se4::patch::retargetCall(site, jump)) return VRLOG("hud_vr: HUD render flag call retarget FAILED"), false;
    VRLOG("hud_vr: HUD render flag call %p gated (mode %d)", static_cast<void*>(site), g_mode);
    return true;
}
bool g_all2d = false;          // hud_vr_2d=1: every element but the wheel in 2D per eye (diagnostic)
bool g_crosshair3d = true;     // hud_crosshair_3d (default 1): crosshair at the target's depth on the 3D path
float g_distance = 1.2f, g_halfTan = 0.364f, g_wheelHeight = 0.45f;
// CHudElement +0x18 params; CHudElementParams +0x18 quat, +0x28 pos, +0x34 stretch (R16 s2.1, R8 A5)
constexpr size_t kParams = 0x18, kPlaceQ = 0x18, kPlaceP = 0x28, kStretch = 0x34;
// ctx: [7] &vr flag, [8] &anchor P7 (head-relative), [9] &drawport (R16 s2.1); drawport +0x10..+0x1c int box, +0x30 2D flag
constexpr size_t kDpBox = 0x10, kDp2D = 0x30;

struct Placement {
    float q[4], p[3], stretch;
};
void save(uint8_t* params, Placement& s) {
    memcpy(s.q, params + kPlaceQ, 16), memcpy(s.p, params + kPlaceP, 12), memcpy(&s.stretch, params + kStretch, 4);
}
void load(uint8_t* params, const Placement& s) {
    memcpy(params + kPlaceQ, s.q, 16), memcpy(params + kPlaceP, s.p, 12), memcpy(params + kStretch, &s.stretch, 4);
}
bool unauthored(const Placement& s) {
    return s.q[0] == 0 && s.q[1] == 0 && s.q[2] == 0 && s.q[3] == 1 && s.p[0] == 0 && s.p[1] == 0 && s.p[2] == 0 && s.stretch == 1;
}

void crosshair2d(void** ctx, uint8_t* e, uint8_t* dp) {
    int32_t& vr = *static_cast<int32_t*>(ctx[7]);
    auto* batching = se4::build().at<int32_t>(se4::gen::hud_bAllowBatching);
    const int32_t savedVr = vr, savedBatching = batching ? *batching : 0, saved2d = *reinterpret_cast<int32_t*>(dp + kDp2D);
    vr = 0;
    if (batching) *batching = 0;  // drawn now with its own pixel ortho, not in the HUD's 3D flush
    realElement(ctx, e);
    flush(ctx);
    if (batching) *batching = savedBatching;
    vr = savedVr;
    *reinterpret_cast<int32_t*>(dp + kDp2D) = saved2d;  // the flush still draws the other elements in 3D
}

// World pose of the HUD panel (device play space, engine units): hud_distance in front of the head, recentred
// when the head's forward leaves it by more than g_recenter radians (eduke32-style), eased over ~0.3 s.
float g_recenter = 0.61f;
core::Pose g_hudPose;
core::Quat g_hudTarget;
bool g_hudValid;
ULONGLONG g_hudEase;
float worldScale() {
    auto* s = se4::build().at<float>(se4::gen::vr_fWorldScale);
    return (s && *s > 0.01f) ? *s : 1.0f;
}
core::Quat yawPitch(core::Quat q) {
    const core::Vec3 f = core::rotate(q, {0, 0, -1});
    const float yawA = std::atan2(-f.x, -f.z), pitchA = std::asin(std::fmax(-1.0f, std::fmin(1.0f, f.y)));
    return core::multiply(core::yaw(yawA), core::Quat{std::sin(pitchA / 2), 0, 0, std::cos(pitchA / 2)});
}
core::Pose hudPanel(const core::Pose& head) {
    const core::Quat want = yawPitch(head.q);
    const ULONGLONG now = GetTickCount64();
    static ULONGLONG lastStep;
    if (g_hudValid && now - lastStep < 8) {  // one easing step per frame, not per element and eye
        g_hudPose.p = head.p + core::rotate(g_hudPose.q, {0, 0, -g_distance / worldScale()});
        return g_hudPose;
    }
    lastStep = now;
    if (!g_hudValid) g_hudTarget = want, g_hudPose.q = want, g_hudValid = true, g_hudEase = 0;
    const core::Vec3 a = core::rotate(g_hudTarget, {0, 0, -1}), b = core::rotate(want, {0, 0, -1});
    if (std::acos(std::fmax(-1.0f, std::fmin(1.0f, core::dot(a, b)))) > g_recenter) g_hudTarget = want, g_hudEase = now;
    // Ease the shown rotation towards the target (normalised lerp, shortest way).
    core::Quat t = g_hudTarget, c = g_hudPose.q;
    if (c.x * t.x + c.y * t.y + c.z * t.z + c.w * t.w < 0) t = {-t.x, -t.y, -t.z, -t.w};
    const float k = g_hudEase && now - g_hudEase < 1000 ? 0.15f : 1.0f;
    g_hudPose.q = core::normalize({c.x + (t.x - c.x) * k, c.y + (t.y - c.y) * k, c.z + (t.z - c.z) * k, c.w + (t.w - c.w) * k});
    g_hudPose.p = head.p + core::rotate(g_hudPose.q, {0, 0, -g_distance / worldScale()});
    return g_hudPose;
}

// Diagnostics (hud_vr_log=1): the elements the loop visits, their render function and he_bIsVisible (+0x10).
bool g_log;
void logElement(uint8_t* e, void** ctx) {
    static void* fns[64];
    static int8_t vis[64];
    void* fn = (*reinterpret_cast<void***>(e))[0x80 / 8];
    const int v = *reinterpret_cast<int32_t*>(e + 0x10) != 0;
    int i = 0;
    while (i < 64 && fns[i] && fns[i] != fn) ++i;
    if (i == 64 || (fns[i] == fn && vis[i] == v)) return;
    fns[i] = fn, vis[i] = int8_t(v);
    VRLOG("hud_vr:   element %p render %p visible %d (vr %d, hidden %d, menu %d)", static_cast<void*>(e), fn, v,
          ctx[7] ? *static_cast<int32_t*>(ctx[7]) : -1, ctx[0] ? *static_cast<int32_t*>(ctx[0]) : -1, ctx[1] ? *static_cast<int32_t*>(ctx[1]) : -1);
}

// The crosshair on the 3D path, on a plane matching the eye's own frustum at the target's distance: the element
// projects the barrel target to a pixel of this eye's image (src/se4/crosshair.cpp's per-eye view), and this
// plane maps every pixel back along its own ray, so the crosshair keeps its native look and screen position but
// sits at the target's depth in both eyes.
bool crosshairAtTarget(void** ctx, uint8_t* e, uint8_t* dp) {
    const int eye = vrdev::currentEye();
    float l, r, u, dn, e7[7];
    uint8_t* params = *reinterpret_cast<uint8_t**>(e + kParams);
    const int32_t* box = reinterpret_cast<const int32_t*>(dp + kDpBox);
    const float w = float(box[2] - box[0]), h = float(box[3] - box[1]);
    // The HUD's view entity (ctx[3]) is the player whose crosshair this is; only the local VR player's is moved.
    uint8_t* player = ctx[3] ? *static_cast<uint8_t**>(ctx[3]) : nullptr;
    const unsigned vrdOff = player_vr::vrdOffset();
    if (!player || !vrdOff || !player_vr::localVrd() || *reinterpret_cast<void**>(player + vrdOff) != player_vr::localVrd()) return false;
    if (!params || !(w > 16 && h > 16) || !vrdev::eyeFov(eye, l, r, u, dn) || !vrdev::eyeInHead(eye, e7)) return false;
    const int delta = int(vrdOff) - 0x25c8;  // SM's player fields sit 8 bytes later
    float dist = *reinterpret_cast<float*>(player + 0x2194 + delta);  // aim-ray hit distance (R8 s4)
    if (!(dist > 0.0f && dist < 1e5f)) dist = 50.0f;
    const float d = std::fmax(0.3f, std::fmin(dist, 200.0f)) * 0.97f;  // just in front of the surface
    const float tl = std::tan(l), tr = std::tan(r), tu = std::tan(u), td = std::tan(dn);
    Placement native, mine{{0, 0, 0, 1}, {d * tl, d * tu, -d}, d * (tr - tl) / w};
    save(params, native);
    float* anchor = static_cast<float*>(ctx[8]);
    float savedAnchor[7];
    memcpy(savedAnchor, anchor, sizeof savedAnchor);
    memcpy(anchor, e7, sizeof e7);  // head-relative anchor = the eye
    load(params, mine);
    realElement(ctx, e);
    flush(ctx);
    load(params, native);
    memcpy(anchor, savedAnchor, sizeof savedAnchor);
    (void)td;
    return true;
}

void hookedElement(void** ctx, uint8_t* e) {
    if (g_log && e) logElement(e, ctx);
    void** vt = e ? *reinterpret_cast<void***>(e) : nullptr;
    uint8_t* params = e ? *reinterpret_cast<uint8_t**>(e + kParams) : nullptr;
    uint8_t* dp = ctx[9] ? *static_cast<uint8_t**>(ctx[9]) : nullptr;
    if (!vt || !params || !dp || !ctx[7] || !*static_cast<int32_t*>(ctx[7]) || !ctx[8]) {
        realElement(ctx, e);
        return;
    }
    if (vt[0x80 / 8] == g_crosshairRender && g_crosshair3d && crosshairAtTarget(ctx, e, dp)) return;
    if (vt[0x80 / 8] == g_crosshairRender || (g_all2d && vt[0x80 / 8] != g_wheelRender)) return crosshair2d(ctx, e, dp);
    const int32_t* box = reinterpret_cast<const int32_t*>(dp + kDpBox);
    const float w = float(box[2] - box[0]), h = float(box[3] - box[1]);
    Placement native;
    save(params, native);
    if (!(w > 16 && h > 16 && w < 16384 && h < 16384) || !unauthored(native)) {
        realElement(ctx, e);
        return;
    }
    const bool wheel = vt[0x80 / 8] == g_wheelRender;
    float panel7[7], head7[7];
    const bool onHand = wheel && wheel_panel::panel(panel7);
    vrdev::headPlacement(head7);
    const core::Pose head{core::normalize({head7[0], head7[1], head7[2], head7[3]}), {head7[4], head7[5], head7[6]}};
    if (!onHand) {
        // The HUD panel: world-locked in front of the head, recentred (yaw and pitch) once the head has turned
        // more than hud_recenter degrees away from it, so it stays readable without swimming with every motion.
        const core::Pose pan = hudPanel(head);
        for (int i = 0; i < 4; ++i) panel7[i] = (&pan.q.x)[i];
        panel7[4] = pan.p.x, panel7[5] = pan.p.y, panel7[6] = pan.p.z;
    }
    // Pixels -> metres: the drawport's height becomes `height` metres, its centre the panel origin (y flipped).
    const float height = onHand ? g_wheelHeight : 2.0f * g_distance * g_halfTan;
    const float s = height / worldScale() / h;  // engine units per pixel (the anchor is in engine units)
    Placement mine{{0, 0, 0, 1}, {-0.5f * w * s, 0.5f * h * s, 0.0f}, s};
    float* anchor = static_cast<float*>(ctx[8]);
    float savedAnchor[7];
    memcpy(savedAnchor, anchor, sizeof savedAnchor);
    {
        // The anchor is head-relative (the HUD render cancels the head): anchor = inverse(head) * panel.
        const core::Pose pan{core::normalize({panel7[0], panel7[1], panel7[2], panel7[3]}), {panel7[4], panel7[5], panel7[6]}};
        const core::Pose a = core::compose(core::inverse(head), pan);
        if (core::finite(a)) anchor[0] = a.q.x, anchor[1] = a.q.y, anchor[2] = a.q.z, anchor[3] = a.q.w, anchor[4] = a.p.x, anchor[5] = a.p.y, anchor[6] = a.p.z;
    }
    load(params, mine);
    // Batched quads keep the object matrix of their queue entry and are drawn at the end of the HUD render; draw
    // this element's quads now, with its own panel matrix (hud_vr_batching=1 keeps the native batching).
    auto* batching = g_noBatch ? se4::build().at<int32_t>(se4::gen::hud_bAllowBatching) : nullptr;
    const int32_t savedBatching = batching ? *batching : 0;
    if (batching) *batching = 0;
    realElement(ctx, e);
    flush(ctx);
    if (batching) *batching = savedBatching;
    load(params, native);
    memcpy(anchor, savedAnchor, sizeof savedAnchor);
    static ULONGLONG last;
    if (GetTickCount64() - last > 5000) {
        last = GetTickCount64();
        VRLOG("hud_vr: HUD panel %.0fx%.0f px -> %.2f m high at %.2f m%s", w, h, height, g_distance, onHand ? " (wheel on the hand panel)" : "");
    }
}
}  // namespace

bool install() {
    if (!se4::options::get("hud_vr", 1)) return VRLOG("hud_vr: disabled by option"), false;
    g_distance = std::fmax(0.3f, se4::options::get("hud_distance_cm", 120) / 100.0f);
    const float fov = std::fmax(10.0f, std::fmin(100.0f, float(se4::options::get("hud_fov", 40))));
    g_halfTan = std::tan(fov * 0.5f * core::Pi / 180.0f);
    g_log = se4::options::get("hud_vr_log", 0) != 0;
    g_noBatch = se4::options::get("hud_vr_batching", 0) == 0;
    g_all2d = se4::options::get("hud_vr_2d", 0) != 0;
    g_mode = se4::options::get("hud_vr_mode", 0);
    if (g_mode == 0) gateHud();
    g_crosshair3d = se4::options::get("hud_crosshair_3d", 1) != 0;
    g_recenter = std::fmax(5.0f, float(se4::options::get("hud_recenter", 35))) * core::Pi / 180.0f;
    g_wheelHeight = std::fmax(0.1f, se4::options::get("wheel_panel_cm", 45) / 100.0f);
    const auto& b = se4::build();
    g_wheelRender = b.at<void>(se4::gen::wheel_render);
    g_crosshairRender = b.at<void>(se4::gen::crosshair_render);
    void* element = b.at<void>(se4::gen::hud_element_render);
    flushCanvas = reinterpret_cast<FlushFn>(b.at<void>(se4::gen::hud_canvas_flush));
    if (auto* code = static_cast<uint8_t*>(element))
        for (int i = 0; i < 0x100 && !g_gfx; ++i)
            if (code[i] == 0x48 && code[i + 1] == 0x8B && code[i + 2] == 0x0D)
                g_gfx = reinterpret_cast<void**>(code + i + 7 + *reinterpret_cast<int32_t*>(code + i + 3));
    VRLOG("hud_vr: gfx global %p, canvas flush %p", static_cast<void*>(g_gfx), reinterpret_cast<void*>(flushCanvas));
    if (!element || !g_wheelRender || !g_crosshairRender) return VRLOG("hud_vr: build lacks HUD symbols - not installed"), false;
    if (MH_CreateHook(element, reinterpret_cast<void*>(hookedElement), reinterpret_cast<void**>(&realElement)) != MH_OK ||
        MH_EnableHook(element) != MH_OK)
        return VRLOG("hud_vr: HUD element hook FAILED"), false;
    VRLOG("hud_vr: HUD on a %.0f-degree panel at %.2f m, wheel panel %.2f m, crosshair per eye", fov, g_distance, g_wheelHeight);
    return true;
}
}  // namespace hud_vr
