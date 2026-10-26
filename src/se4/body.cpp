// VR body pose on every machine (docs/re/R9_body_ik.md s3, s5A). One engine motion handler per player with
// CPlayerVRData, registered last on the player model's animation queue; after the native blend it re-aims
// both arms so each Left/RightWeapon bone lands on that hand's controller placement (the replicated pose
// for remote players). Legs, hips and spine stay native in this first version; the head follows the
// native look. Option body_ik (default 1; lab-verified 2026-10-26 on remote VR players, arms and fingers).
#include "se4/body.hpp"
#include "core/body_ik.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/player_vr.hpp"
#include "se4/names.hpp"
#include "se4/net.hpp"
#include "core/hand_pose.hpp"
#include "core/wire.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace body {
namespace {
using RegisterFn = void(__cdecl*)(void* renderable, void* handler, int flags);
using RenderableFn = uint8_t*(__cdecl*)(void* entity);

RegisterFn mhRegister;
using HandleFn = void*(__cdecl*)(int32_t handle);
HandleFn g_handleEntity;
void* g_identFn;
bool g_identReady;
void* g_baseVtbl[10];
void* g_vtbl[10];
void* g_ident[2][6];  // per side: Shoulder, Arm, ForeArm, Hand, Weapon (+ spare)
const char* const kBones[2][5] = {{"LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "LeftWeapon"},
                                  {"RightShoulder", "RightArm", "RightForeArm", "RightHand", "RightWeapon"}};
// Finger bones of the third-person body (R9 s2): <side>Hand<Finger><1..3>, thumb..pinky as core::Finger.
const char* const kFingerNames[core::FingerCount] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
void* g_fident[2][15];
constexpr int kMaxChain = 24;

// Engine-visible part first: vtable, list of queues it is registered on (engine-allocated), count, capacity.
struct Handler {
    void** vtbl;
    void* queues;
    uint32_t count, capacity;
    // mod fields
    uint8_t* player;
    ULONGLONG lastTick;
    // Per side: idents from the model root down to the upper arm's parent (resolved from the skeleton).
    void* chain[2][kMaxChain];
    int chainLen[2];
    bool resolved;
    // pose index cache
    const void* poseIdents;
    int poseCount;
    int index[2][kMaxChain + 5];
    // fingers: pose indices, skeleton rest rotations, smoothed curls
    bool fingersOk;
    int findex[2][15];
    core::Quat rest[2][15];
    float curl[2][core::FingerCount];
};
Handler g_handlers[32];
SRWLOCK g_lock = SRWLOCK_INIT;

template <class T> T& at(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

// Skeleton of the player's model: renderable (vt+0xc8) +0xd0 model instance, +0x18 configuration, +0x68
// skeleton, LOD0 at +0x68 when +0x60 > 0; bones map {IDENT, index} at lod+0x28 (count +0x20), bones at
// lod+0x10 (0xf0 each), parent IDENT at bone+0x08 (R9 s2; accessors 0x14004e5c0/0x140c111c0/0x140c10df0/
// 0x140c288d0/0x140c282a0, identical in all builds).
uint8_t* lod0(uint8_t* renderable) {
    uint8_t* mi = renderable ? at<uint8_t*>(renderable, 0xd0) : nullptr;
    uint8_t* cfg = mi ? at<uint8_t*>(mi, 0x18) : nullptr;
    uint8_t* skel = cfg ? at<uint8_t*>(cfg, 0x68) : nullptr;
    return skel && at<int32_t>(skel, 0x60) > 0 ? at<uint8_t*>(skel, 0x68) : nullptr;
}
uint8_t* boneOf(uint8_t* lod, void* ident) {
    int lo = 0, hi = at<int32_t>(lod, 0x20) - 1;
    auto* map = at<uint8_t*>(lod, 0x28);
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uintptr_t key = at<uintptr_t>(map, size_t(mid) * 0x10);
        if (key == uintptr_t(ident)) {
            int idx = at<int32_t>(map, size_t(mid) * 0x10 + 8);
            return idx < 0 ? nullptr : at<uint8_t*>(lod, 0x10) + size_t(idx) * 0xf0;
        }
        if (uintptr_t(ident) < key) hi = mid - 1;
        else lo = mid + 1;
    }
    return nullptr;
}

bool resolve(Handler& h, uint8_t* renderable) {
    uint8_t* lod = lod0(renderable);
    if (!lod) return false;
    for (int s = 0; s < 2; ++s) {
        for (int i = 0; i < 5; ++i)
            if (!boneOf(lod, g_ident[s][i])) return false;
        // parents of the shoulder up to the root, stored root first
        void* rev[kMaxChain];
        int n = 0;
        void* cur = g_ident[s][0];
        for (;;) {
            uint8_t* b = boneOf(lod, cur);
            void* parent = b ? at<void*>(b, 0x08) : nullptr;
            if (!parent || !boneOf(lod, parent) || n == kMaxChain) break;
            rev[n++] = parent;
            cur = parent;
        }
        for (int i = 0; i < n; ++i) h.chain[s][i] = rev[n - 1 - i];
        h.chainLen[s] = n;
    }
    // Fingers (optional): each joint's parent is the previous one, the first joint's is the hand; rest rotation at bone+0x48.
    h.fingersOk = g_fident[0][0] != nullptr;
    for (int s = 0; s < 2 && h.fingersOk; ++s)
        for (int f = 0; f < core::FingerCount && h.fingersOk; ++f)
            for (int j = 0; j < 3; ++j) {
                uint8_t* b = boneOf(lod, g_fident[s][f * 3 + j]);
                void* parent = j ? g_fident[s][f * 3 + j - 1] : g_ident[s][3];
                if (!b || at<void*>(b, 0x08) != parent) {
                    h.fingersOk = false;
                    break;
                }
                const float* q = &at<float>(b, 0x48);
                h.rest[s][f * 3 + j] = core::normalize({q[0], q[1], q[2], q[3]});
            }
    h.resolved = true;
    return true;
}

// Pose layout (R9 s3): +0x00 IDENT* per bone / +0x08 count; positions +0x10 (0x28 each, result vec3 +0x1c);
// rotations +0x20 (0x34 each, result quat xyzw +0x24); record +0x00 = "set" flag.
core::Pose local(uint8_t* pose, int i) {
    const float* p = reinterpret_cast<const float*>(at<uint8_t*>(pose, 0x10) + size_t(i) * 0x28 + 0x1c);
    const float* q = reinterpret_cast<const float*>(at<uint8_t*>(pose, 0x20) + size_t(i) * 0x34 + 0x24);
    return {{q[0], q[1], q[2], q[3]}, {p[0], p[1], p[2]}};
}
void setRotation(uint8_t* pose, int i, core::Quat q) {
    uint8_t* r = at<uint8_t*>(pose, 0x20) + size_t(i) * 0x34;
    at<int32_t>(r, 0) = 1;
    float* f = reinterpret_cast<float*>(r + 0x24);
    f[0] = q.x, f[1] = q.y, f[2] = q.z, f[3] = q.w;
}

// Finger curls for a side (0 left, 1 right) of this handler's player: the local VR player's own controllers, a remote
// one's from its relayed pose (net). False when there are none, or when that hand holds a weapon (its third-person
// animation already shapes the grip; the empty "Hands" item counts as empty).
bool curlsFor(Handler& h, void* vrd, int s, float curl[core::FingerCount]) {
    const auto& b = se4::build();
    const uint32_t woff = b.off(s ? se4::gen::CPlayerActorPuppetEntity_en_henCurrentRightHandWeapon
                                  : se4::gen::CPlayerActorPuppetEntity_en_henCurrentLeftHandWeapon);
    if (woff && at<int32_t>(h.player, woff) && g_handleEntity) {
        auto* weapon = static_cast<uint8_t*>(g_handleEntity(at<int32_t>(h.player, woff)));
        const char* file = weapon ? se4::names::weaponFile(weapon) : nullptr;
        if (weapon && !(file && strstr(file, "Hands"))) return false;
    }
    if (vrd == player_vr::localVrd()) {
        net::fingerCurls(s, curl);
        return true;
    }
    const uint32_t ioff = b.off(se4::gen::CPlayerActorPuppetEntity_en_iPlayerIndex);
    core::wire::PosePacket pkt;
    if (!ioff || !net::remotePose(at<int32_t>(h.player, ioff), pkt) || !(pkt.flags & core::wire::FingersValid)) return false;
    core::wire::curls(pkt, s, curl);
    return true;
}

// Bends one hand's fingers from the rest pose toward the palm by the curls. The palm normal comes from the rest
// geometry (index/middle/pinky bases in the hand frame), so no per-model axis is assumed.
void bendFingers(Handler& h, uint8_t* pose, int s, const float curl[core::FingerCount]) {
    const int* fx = h.findex[s];
    const core::Vec3 mid = local(pose, fx[core::Middle * 3]).p, idx = local(pose, fx[core::Index * 3]).p,
                     pinky = local(pose, fx[core::Pinky * 3]).p;
    const float ml = std::sqrt(core::dot(mid, mid));
    if (!(ml > 1e-5f)) return;
    const core::Vec3 n = core::cross(mid * (1.0f / ml), idx - pinky);
    const float nl = std::sqrt(core::dot(n, n));
    if (!(nl > 1e-6f)) return;
    const core::Vec3 palm = n * ((s == 0 ? 1.0f : -1.0f) / nl);  // left: +cross, right: -cross (fingers forward, palm down)
    const core::FingerAngles a = core::anglesFromCurls(curl);
    for (int f = 0; f < core::FingerCount; ++f) {
        const core::Quat* r = &h.rest[s][f * 3];
        const core::Vec3 dir = core::rotate(r[0], local(pose, fx[f * 3 + 1]).p);
        const float dl = std::sqrt(core::dot(dir, dir));
        if (!(dl > 1e-6f)) continue;
        core::Vec3 axis = core::cross(dir * (1.0f / dl), palm);
        const float al = std::sqrt(core::dot(axis, axis));
        if (!(al > 1e-4f)) continue;
        axis = axis * (1.0f / al);
        core::Quat chain{};  // rest rotation of joint j in the hand frame
        for (int j = 0; j < 3; ++j) {
            chain = core::multiply(chain, r[j]);
            const core::Vec3 ax = core::rotate(core::inverse(chain), axis);
            const float t = 0.5f * a.a[f][j], sn = std::sin(t);
            setRotation(pose, fx[f * 3 + j], core::normalize(core::multiply(r[j], {ax.x * sn, ax.y * sn, ax.z * sn, std::cos(t)})));
        }
    }
}

int __cdecl apply(Handler* h, void* /*ctx*/, uint8_t* pose) {
    if (!h->resolved || !h->player) return 0;
    void* vrd = at<void*>(h->player, player_vr::vrdOffset());
    if (!vrd) return 0;
    void** idents = at<void**>(pose, 0x00);
    int count = at<int32_t>(pose, 0x08);
    if (count <= 0 || at<int32_t>(pose, 0x18) != count || at<int32_t>(pose, 0x28) != count) return 0;
    if (h->poseIdents != idents || h->poseCount != count) {
        auto find = [&](void* id) {
            for (int i = 0; i < count; ++i)
                if (idents[i] == id) return i;
            return -1;
        };
        for (int s = 0; s < 2; ++s) {
            for (int i = 0; i < h->chainLen[s]; ++i) h->index[s][i] = find(h->chain[s][i]);
            for (int i = 0; i < 5; ++i) h->index[s][h->chainLen[s] + i] = find(g_ident[s][i]);
            for (int i = 0; i < 15; ++i) h->findex[s][i] = h->fingersOk ? find(g_fident[s][i]) : -1;
        }
        h->poseIdents = idents;
        h->poseCount = count;
    }
    // World -> model: renderable placement (+0x3c quat xyzw, +0x4c pos); model stretch assumed 1.
    auto renderable = reinterpret_cast<RenderableFn>((*reinterpret_cast<void***>(h->player))[0xc8 / 8])(h->player);
    if (!renderable) return 0;
    const float* rq = &at<float>(renderable, 0x3c);
    const float* rp = &at<float>(renderable, 0x4c);
    core::Pose model{core::normalize({rq[0], rq[1], rq[2], rq[3]}), {rp[0], rp[1], rp[2]}};
    core::Pose toModel = core::inverse(model);
    core::Vec3 downModel = core::rotate(toModel.q, {0, -1, 0});
    int changed = 0;
    for (int s = 0; s < 2; ++s) {
        const int* idx = h->index[s];
        int n = h->chainLen[s];
        bool ok = true;
        for (int i = 0; i < n + 5; ++i) ok &= idx[i] >= 0 && idx[i] < count;
        if (!ok) continue;
        core::Pose parent{};  // model space of the shoulder (chain root .. shoulder)
        for (int i = 0; i < n; ++i) parent = core::compose(parent, local(pose, idx[i]));
        parent = core::compose(parent, local(pose, idx[n + 0]));  // shoulder
        core::ArmLocals a{parent, local(pose, idx[n + 1]), local(pose, idx[n + 2]), local(pose, idx[n + 3]),
                          local(pose, idx[n + 4])};
        float p7[7];
        player_vr::controllerWorld(vrd, s == 1 ? 0 : 1, p7);  // engine role 0 = right
        core::Pose target{{p7[0], p7[1], p7[2], p7[3]}, {p7[4], p7[5], p7[6]}};
        if (p7[0] == 0 && p7[1] == 0 && p7[2] == 0 && p7[3] == 0) continue;
        target = core::compose(toModel, target);
        if (!core::finite(target)) continue;
        core::Pose armG = core::compose(parent, a.arm);
        core::Pose elbow = core::compose(armG, a.fore);
        core::Vec3 pole = elbow.p + downModel * 0.25f;
        core::retargetArm(a, target, pole);
        setRotation(pose, idx[n + 1], a.arm.q);
        setRotation(pose, idx[n + 2], a.fore.q);
        setRotation(pose, idx[n + 3], a.hand.q);
        changed = 1;
        bool fingers = h->fingersOk;
        for (int i = 0; i < 15 && fingers; ++i) fingers = h->findex[s][i] >= 0 && h->findex[s][i] < count;
        float curl[core::FingerCount];
        if (fingers && curlsFor(*h, vrd, s, curl)) {
            for (int f = 0; f < core::FingerCount; ++f) h->curl[s][f] += (curl[f] - h->curl[s][f]) * 0.35f;  // smooths 30 Hz updates
            bendFingers(*h, pose, s, h->curl[s]);
        }
    }
    return changed;
}

void* __cdecl dtor(Handler* h, unsigned) { return h; }  // static storage; the engine never deletes handlers (R9 s3)

void ensure(uint8_t* player) {
    if (!player) return;
    if (!g_identReady) {
        // IDENTs are interned on the simulation thread, like the engine's own callers (not at attach time).
        using IdentFn = void**(__cdecl*)(void** out, const char* name);
        for (int s = 0; s < 2; ++s)
            for (int i = 0; i < 5; ++i) reinterpret_cast<IdentFn>(g_identFn)(&g_ident[s][i], kBones[s][i]);
        char name[32];
        for (int s = 0; s < 2; ++s)
            for (int f = 0; f < core::FingerCount; ++f)
                for (int j = 0; j < 3; ++j) {
                    snprintf(name, sizeof name, "%sHand%s%d", s ? "Right" : "Left", kFingerNames[f], j + 1);
                    reinterpret_cast<IdentFn>(g_identFn)(&g_fident[s][f * 3 + j], name);
                }
        g_identReady = true;
    }
    auto renderable = reinterpret_cast<RenderableFn>((*reinterpret_cast<void***>(player))[0xc8 / 8])(player);
    if (!renderable) return;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    Handler* h = nullptr;
    for (auto& x : g_handlers)
        if (x.player == player) h = &x;
    if (!h)
        for (auto& x : g_handlers)
            if (!x.player || (x.count == 0 && now - x.lastTick > 5000)) {
                h = &x;
                break;
            }
    if (h && h->player != player) {
        if (h->count) h = nullptr;  // still registered somewhere: do not reuse
        else {
            memset(h, 0, sizeof *h);
            h->vtbl = g_vtbl;
            h->player = player;
        }
    }
    if (h) {
        h->lastTick = now;
        if (h->count == 0) {
            h->resolved = false;
            h->poseIdents = nullptr;
            if (resolve(*h, renderable)) {
                mhRegister(renderable, h, 0);
                VRLOG("body: IK handler %p registered for player %p (chains %d/%d, fingers %d)", static_cast<void*>(h), player,
                      h->chainLen[0], h->chainLen[1], int(h->fingersOk));
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool g_enabled;
}  // namespace

void onVrdTick(void** vrd) {
    if (g_enabled && vrd) ensure(static_cast<uint8_t*>(vrd[0]));  // vrd+0 = player (vrd_init)
}

bool install() {
    if (!se4::options::get("body_ik", 1)) return false;
    const auto& b = se4::build();
    mhRegister = reinterpret_cast<RegisterFn>(b.at<void>(se4::gen::mh_register));
    auto* baseCtor = b.at<uint8_t>(se4::gen::mh_base_ctor);
    auto* tick = b.at<void>(se4::gen::vrd_tick);
    auto* identFn = b.at<void>(se4::gen::ident_ctor);
    if (!mhRegister || !baseCtor || !tick || !identFn || baseCtor[0] != 0x48 || baseCtor[1] != 0x8D || baseCtor[2] != 0x05) {
        VRLOG("body: build lacks motion-handler symbols - body IK off");
        return false;
    }
    int32_t rel;
    memcpy(&rel, baseCtor + 3, 4);
    auto** base = reinterpret_cast<void**>(baseCtor + 7 + rel);  // CMotionHandler vtable
    memcpy(g_baseVtbl, base, sizeof g_baseVtbl);
    memcpy(g_vtbl, base, sizeof g_vtbl);
    g_vtbl[0] = reinterpret_cast<void*>(dtor);
    g_vtbl[4] = reinterpret_cast<void*>(apply);
    g_identFn = identFn;
    g_handleEntity = reinterpret_cast<HandleFn>(b.at<void>(se4::gen::handle_entity));
    g_enabled = true;  // ticked from the vrd step hook in src/se4/player_vr.cpp
    VRLOG("body: IK enabled (CMotionHandler vtable %p)", static_cast<void*>(base));
    return true;
}
}  // namespace body
