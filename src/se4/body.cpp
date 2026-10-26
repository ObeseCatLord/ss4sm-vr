// VR body pose on every machine (docs/re/R9_body_ik.md s3, s5A). One engine motion handler per player with
// CPlayerVRData, registered last on the player model's animation queue; after the native blend it re-aims
// both arms so each Left/RightWeapon bone lands on that hand's controller placement (the replicated pose
// for remote players). Legs, hips and spine stay native in this first version; the head follows the
// native look. Option body_ik (default 0 until verified on the headset).
#include "se4/body.hpp"
#include "core/body_ik.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/player_vr.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>

namespace body {
namespace {
using RegisterFn = void(__cdecl*)(void* renderable, void* handler, int flags);
using RenderableFn = uint8_t*(__cdecl*)(void* entity);

RegisterFn mhRegister;
void* g_identFn;
bool g_identReady;
void* g_baseVtbl[10];
void* g_vtbl[10];
void* g_ident[2][6];  // per side: Shoulder, Arm, ForeArm, Hand, Weapon (+ spare)
const char* const kBones[2][5] = {{"LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "LeftWeapon"},
                                  {"RightShoulder", "RightArm", "RightForeArm", "RightHand", "RightWeapon"}};
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
                VRLOG("body: IK handler %p registered for player %p (chains %d/%d)", static_cast<void*>(h), player,
                      h->chainLen[0], h->chainLen[1]);
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
    if (!se4::options::get("body_ik", 0)) return false;
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
    g_enabled = true;  // ticked from the vrd step hook in src/se4/player_vr.cpp
    VRLOG("body: IK enabled (CMotionHandler vtable %p)", static_cast<void*>(base));
    return true;
}
}  // namespace body
