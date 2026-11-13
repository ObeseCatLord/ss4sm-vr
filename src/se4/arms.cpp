#include "se4/arms.hpp"
#include "core/body_ik.hpp"
#include "core/math.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/player_vr.hpp"
#include "se4/hands.hpp"
#include "core/hand_pose.hpp"
#include "xr/input.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace arms {
namespace {
using QueueFn = void*(__cdecl*)(void* mi, int create);
using AddFn = void(__cdecl*)(void* queue, void* handler, int flags);
using PlacementFn = void*(__cdecl*)(uint8_t* weapon, const float* in, float* out);
using HeadWorldFn = void*(__cdecl*)(void* vrd, float* out);
using IdentFn = void**(__cdecl*)(void** out, const char* name);

template <class T> T& at(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

QueueFn animQueue;
using HideFn = void(__cdecl*)(void* controller);
HideFn hideController;
uint32_t g_ctrlOff[2];  // CVRController in CPlayerVRData: [0] left, [1] right (from the Hide*VRController scripts)

// The script wrappers end in `add rcx, imm32; call hide_controller` (R15 s5).
uint32_t addRcxImm(const uint8_t* fn) {
    for (int i = 0; i < 0x40; ++i)
        if (fn[i] == 0x48 && fn[i + 1] == 0x81 && fn[i + 2] == 0xC1) return *reinterpret_cast<const uint32_t*>(fn + i + 3);
    return 0;
}
AddFn addHandler;
PlacementFn realPlacement;
HeadWorldFn headWorld;
IdentFn identCtor;
void* g_vtbl[10];
// Arms.skl bones used here (probe 2026-10-24: UpperArm's parent is Root; arms run along local +X right / -X left;
// finger joints bend about local -Z on both sides).
enum Bone { UpperArm, LowerArm, Hand, WeaponBone, Finger0, kSideBones = Finger0 + 15 };
const char* const kSideNames[kSideBones] = {"UpperArm", "LowerArm", "Hand", "Weapon", "Thumb_01", "Thumb_02", "Thumb_03",
                                            "Index_01", "Index_02", "Index_03", "Middle_01", "Middle_02", "Middle_03",
                                            "Ring_01", "Ring_02", "Ring_03", "Pinky_01", "Pinky_02", "Pinky_03"};
void* g_root;
void* g_ident[2][kSideBones];  // [0] left (L_), [1] right (R_)
bool g_identReady;
bool g_free = true;  // option hands_free: the off arm follows its controller instead of parking
constexpr size_t kModel = 0x90;  // en_pmiWeapon (R15 s1.2)

// Engine-visible part first (R9 s3): vtable, queues it is registered on, count, capacity.
struct Handler {
    void** vtbl;
    void* queues;
    uint32_t count, capacity;
    // mod fields
    uint8_t* weapon;
    void* mi;
    volatile LONG seq;           // odd while the snapshot below is being written
    float root[12];              // weapon instance root, 3x4 row-major (rotation x scale | translation), world
    bool rootValid;
    int park;                    // arm to park: 0 none, 1 left, 2 right
    core::Pose parkWorld;        // shoulder behind the eyes, world; +Z = backwards
    int freeSide;                // arm that follows its controller: 0 none, 1 left, 2 right
    core::Vec3 shoulderWorld, poleWorld;
    core::Pose handWorld;        // the free hand's controller (engine world), target of its Weapon bone
    core::FingerAngles fingers;  // the free hand's finger flexion
    ULONGLONG tick;
    const void* poseIdents;
    int poseCount;
    int idxRoot;
    int idx[2][kSideBones];
    bool restValid;
    core::Quat rest[2][15];      // finger rest rotations (skeleton)
};
Handler g_handlers[32];  // one per weapon model instance seen recently (holstered instances may stay registered)
SRWLOCK g_lock = SRWLOCK_INIT;

Handler* find(uint8_t* weapon) {
    for (auto& h : g_handlers)
        if (h.weapon == weapon) return &h;
    return nullptr;
}

// Pose layout (R9 s3): +0x00 IDENT* per bone / +0x08 count; positions +0x10 (0x28 each, result vec3 +0x1c);
// rotations +0x20 (0x34 each, result quat xyzw +0x24); record +0x00 = "set" flag.
core::Pose local(uint8_t* pose, int i) {
    const float* p = reinterpret_cast<const float*>(at<uint8_t*>(pose, 0x10) + size_t(i) * 0x28 + 0x1c);
    const float* q = reinterpret_cast<const float*>(at<uint8_t*>(pose, 0x20) + size_t(i) * 0x34 + 0x24);
    return {{q[0], q[1], q[2], q[3]}, {p[0], p[1], p[2]}};
}
void setLocal(uint8_t* pose, int i, const core::Pose& l) {
    uint8_t* pr = at<uint8_t*>(pose, 0x10) + size_t(i) * 0x28;
    at<int32_t>(pr, 0) = 1;
    float* p = reinterpret_cast<float*>(pr + 0x1c);
    p[0] = l.p.x, p[1] = l.p.y, p[2] = l.p.z;
    uint8_t* rr = at<uint8_t*>(pose, 0x20) + size_t(i) * 0x34;
    at<int32_t>(rr, 0) = 1;
    float* q = reinterpret_cast<float*>(rr + 0x24);
    q[0] = l.q.x, q[1] = l.q.y, q[2] = l.q.z, q[3] = l.q.w;
}

void setRotation(uint8_t* pose, int i, core::Quat q) {
    uint8_t* r = at<uint8_t*>(pose, 0x20) + size_t(i) * 0x34;
    at<int32_t>(r, 0) = 1;
    float* f = reinterpret_cast<float*>(r + 0x24);
    f[0] = q.x, f[1] = q.y, f[2] = q.z, f[3] = q.w;
}

// World point/direction into the instance's model space (inverse of a uniformly scaled 3x4).
struct ModelFromWorld {
    float r[9], s2 = 1;  // rotation*scale rows; squared scale
    core::Vec3 t;
    explicit ModelFromWorld(const float* m) {
        r[0] = m[0], r[1] = m[1], r[2] = m[2], r[3] = m[4], r[4] = m[5], r[5] = m[6], r[6] = m[8], r[7] = m[9], r[8] = m[10];
        t = {m[3], m[7], m[11]};
        s2 = r[0] * r[0] + r[3] * r[3] + r[6] * r[6];
    }
    core::Vec3 dir(core::Vec3 d) const {  // M^-1 d = R^T d / s
        return core::Vec3{r[0] * d.x + r[3] * d.y + r[6] * d.z, r[1] * d.x + r[4] * d.y + r[7] * d.z, r[2] * d.x + r[5] * d.y + r[8] * d.z} *
               (1.0f / s2);
    }
    core::Vec3 point(core::Vec3 p) const { return dir(p - t); }
    // Model-from-world rotation (the inverse of the root's normalized rotation).
    core::Quat rotation() const {
        const float s = std::sqrt(s2), m00 = r[0] / s, m01 = r[1] / s, m02 = r[2] / s, m10 = r[3] / s, m11 = r[4] / s, m12 = r[5] / s,
                    m20 = r[6] / s, m21 = r[7] / s, m22 = r[8] / s;
        core::Quat q;
        const float tr = m00 + m11 + m22;
        if (tr > 0) {
            const float k = 0.5f / std::sqrt(tr + 1.0f);
            q = {(m21 - m12) * k, (m02 - m20) * k, (m10 - m01) * k, 0.25f / k};
        } else if (m00 > m11 && m00 > m22) {
            const float k = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
            q = {0.25f * k, (m01 + m10) / k, (m02 + m20) / k, (m21 - m12) / k};
        } else if (m11 > m22) {
            const float k = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
            q = {(m01 + m10) / k, 0.25f * k, (m12 + m21) / k, (m02 - m20) / k};
        } else {
            const float k = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
            q = {(m02 + m20) / k, (m12 + m21) / k, 0.25f * k, (m10 - m01) / k};
        }
        return core::inverse(core::normalize(q));
    }
};

int __cdecl apply(Handler* h, void*, uint8_t* pose) {
    if (!g_identReady) return 0;
    void** idents = at<void**>(pose, 0x00);
    const int count = at<int32_t>(pose, 0x08);
    if (count <= 0 || at<int32_t>(pose, 0x18) != count || at<int32_t>(pose, 0x28) != count) return 0;
    if (h->poseIdents != idents || h->poseCount != count) {
        auto find = [&](void* id) {
            for (int i = 0; i < count; ++i)
                if (idents[i] == id) return i;
            return -1;
        };
        h->idxRoot = find(g_root);
        for (int sd = 0; sd < 2; ++sd)
            for (int k = 0; k < kSideBones; ++k) h->idx[sd][k] = find(g_ident[sd][k]);
        h->poseIdents = idents;
        h->poseCount = count;
    }
    // Consistent snapshot of the simulation-side fields.
    float root[12];
    int park, freeSide;
    core::Pose parkWorld, handWorld;
    core::Vec3 shoulderWorld, poleWorld;
    core::FingerAngles fingers;
    for (int tries = 0;; ++tries) {
        const LONG s0 = h->seq;
        memcpy(root, h->root, sizeof root);
        park = h->rootValid ? h->park : 0;
        freeSide = h->rootValid ? h->freeSide : 0;
        parkWorld = h->parkWorld, handWorld = h->handWorld;
        shoulderWorld = h->shoulderWorld, poleWorld = h->poleWorld;
        fingers = h->fingers;
        if (!(s0 & 1) && s0 == h->seq) break;
        if (tries > 8) return 0;
    }
    if ((!park && !freeSide) || GetTickCount64() - h->tick > 500 || h->idxRoot < 0) return 0;
    const ModelFromWorld mw(root);
    const core::Pose rootModel = local(pose, h->idxRoot);  // Root is the skeleton's top bone (probe 2026-10-24)
    const core::Pose inv = core::inverse(rootModel);
    int changed = 0;
    if (park) {
        const int iArm = h->idx[park - 1][UpperArm];
        core::Vec3 back = mw.dir(core::rotate(parkWorld.q, {0, 0, 1}));
        const float len = std::sqrt(core::dot(back, back));
        if (iArm >= 0 && len > 1e-6f) {
            back = back * (1.0f / len);
            core::Pose l;
            l.p = inv.p + core::rotate(inv.q, mw.point(parkWorld.p));
            l.q = core::fromTo(park == 1 ? core::Vec3{-1, 0, 0} : core::Vec3{1, 0, 0}, core::rotate(inv.q, back));
            if (core::finite(l)) setLocal(pose, iArm, l), changed = 1;
        }
    }
    if (freeSide) {
        const int sd = freeSide - 1;
        const int* ix = h->idx[sd];
        bool ok = true;
        for (int k = 0; k < kSideBones; ++k) ok &= ix[k] >= 0 && ix[k] < count;
        if (ok) {
            core::ArmLocals a{rootModel, local(pose, ix[UpperArm]), local(pose, ix[LowerArm]), local(pose, ix[Hand]),
                              local(pose, ix[WeaponBone])};
            a.arm.p = inv.p + core::rotate(inv.q, mw.point(shoulderWorld));
            const core::Pose target{core::normalize(core::multiply(mw.rotation(), handWorld.q)), mw.point(handWorld.p)};
            const core::Vec3 pole = mw.point(poleWorld);
            if (core::finite(target) && core::finite(a.arm)) {
                core::retargetArm(a, target, pole);
                setLocal(pose, ix[UpperArm], a.arm);
                setRotation(pose, ix[LowerArm], a.fore.q);
                setRotation(pose, ix[Hand], a.hand.q);
                // Fingers: rest rotation, then flexion about local -Z.
                for (int f = 0; f < core::FingerCount; ++f)
                    for (int j = 0; j < 3; ++j) {
                        const int b = Finger0 + f * 3 + j;
                        const core::Quat restQ = h->restValid ? h->rest[sd][f * 3 + j] : local(pose, ix[b]).q;
                        const float t = fingers.a[f][j] * 0.5f;
                        setRotation(pose, ix[b], core::normalize(core::multiply(restQ, {0, 0, -std::sin(t), std::cos(t)})));
                    }
                changed = 1;
            }
        }
    }
    return changed;
}
void* __cdecl dtor(Handler* h, unsigned) { return h; }  // static storage (R9 s3)

void* hookedPlacement(uint8_t* weapon, const float* in, float* out) {
    uint8_t* params = weapon ? at<uint8_t*>(weapon, 0xa8) : nullptr;  // en_pwpWeaponParams
    int32_t savedFlag;
    float savedSnap;
    const bool raised = hands::raiseTwoHanded(weapon, params, savedFlag, savedSnap);
    void* r = realPlacement(weapon, in, out);
    if (raised) hands::restoreTwoHanded(params, savedFlag, savedSnap);
    // One writer at a time keeps the seqlock valid (onWeaponStep writes under g_lock too). Try only: a step that holds
    // the lock right now keeps the previous root for this frame, and a nested call can never deadlock.
    Handler* h = nullptr;
    if (weapon && out && TryAcquireSRWLockExclusive(&g_lock)) {
        h = find(weapon);
        if (h) {
            InterlockedIncrement(&h->seq);
            memcpy(h->root, out, sizeof h->root);
            h->rootValid = true;
            InterlockedIncrement(&h->seq);
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    if (h) {
        static bool logged;
        if (!logged) {
            logged = true;
            VRLOG("arms: weapon %p root %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", weapon, out[0], out[1], out[2], out[3],
                  out[4], out[5], out[6], out[7], out[8], out[9], out[10], out[11]);
        }
    }
    return r;
}
}  // namespace

// Finger rest rotations from the instance's skeleton (LOD 0; bone map {IDENT, index} at +0x28/+0x20, bones at
// +0x10 stride 0xf0, rest rotation +0x48; R9 s2, read the same way in src/se4/body.cpp).
void readRest(Handler& h, uint8_t* mi) {
    h.restValid = false;
    uint8_t* cfg = at<uint8_t*>(mi, 0x18);
    uint8_t* skel = cfg ? at<uint8_t*>(cfg, 0x68) : nullptr;
    uint8_t* lod = skel && at<int32_t>(skel, 0x60) > 0 ? at<uint8_t*>(skel, 0x68) : nullptr;
    if (!lod) return;
    const int n = at<int32_t>(lod, 0x20);
    auto* map = at<uint8_t*>(lod, 0x28);
    auto bone = [&](void* id) -> uint8_t* {
        for (int m = 0; m < n; ++m)
            if (at<void*>(map, size_t(m) * 0x10) == id) {
                const int i = at<int32_t>(map, size_t(m) * 0x10 + 8);
                return i < 0 ? nullptr : at<uint8_t*>(lod, 0x10) + size_t(i) * 0xf0;
            }
        return nullptr;
    };
    for (int sd = 0; sd < 2; ++sd)
        for (int k = 0; k < 15; ++k) {
            uint8_t* b = bone(g_ident[sd][Finger0 + k]);
            if (!b) return;
            const float* q = &at<float>(b, 0x48);
            h.rest[sd][k] = core::normalize({q[0], q[1], q[2], q[3]});
        }
    h.restValid = true;
}

void onWeaponStep(uint8_t* weapon, bool rightHand, bool gripped) {
    if (!animQueue) return;
    if (!g_identReady) {
        identCtor(&g_root, "Root");
        char name[32];
        for (int sd = 0; sd < 2; ++sd)
            for (int k = 0; k < kSideBones; ++k) {
                snprintf(name, sizeof name, "%c_%s", sd ? 'R' : 'L', kSideNames[k]);
                identCtor(&g_ident[sd][k], name);
            }
        g_identReady = true;
    }
    void* mi = at<void*>(weapon, kModel);
    void* vrd = player_vr::localVrd();
    if (!mi || !vrd) return;
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    Handler* h = find(weapon);
    if (!h)
        for (auto& x : g_handlers)
            if (!x.weapon || (x.count == 0 && now - x.tick > 5000)) {
                h = &x;
                break;
            }
    if (h && h->weapon != weapon) {
        if (h->count) h = nullptr;  // still registered on a queue: never reuse
        else {
            memset(h, 0, sizeof *h);
            h->vtbl = g_vtbl;
            h->weapon = weapon;
        }
    }
    if (h) {
        float p7[7] = {};
        headWorld(vrd, p7);
        const core::Pose head{core::normalize({p7[0], p7[1], p7[2], p7[3]}), {p7[4], p7[5], p7[6]}};
        float ws = 1.0f;
        if (auto* s = se4::build().at<float>(se4::gen::vr_fWorldScale); s && *s > 0.01f) ws = *s;
        // The off arm: free (follows its controller) or parked behind the eyes; gripping leaves it to the native pose.
        const int off = rightHand ? 1 : 2;  // 1 left, 2 right
        const bool freeArm = g_free && !gripped && rightHand;  // a left weapon's off arm belongs to the right weapon
        float c7[7] = {};
        if (freeArm) player_vr::controllerWorld(vrd, off == 1 ? 1 : 0, c7);  // engine role 0 right, 1 left
        const bool haveCtrl = c7[0] || c7[1] || c7[2] || c7[3];
        core::FingerAngles fingers;
        if (freeArm) {
            const auto& hs = xr::input().hand(off == 1 ? 0 : 1);
            core::ControllerFingers cf{hs.trigger, (hs.buttons & xr::BtnTriggerTouch) != 0, hs.squeeze,
                                       (hs.buttons & (xr::BtnStickTouch | xr::BtnPrimary | xr::BtnSecondary)) != 0};
            float curl[core::FingerCount];
            core::curlsFromController(cf, curl);
            fingers = core::anglesFromCurls(curl);
        }
        const core::Vec3 side = core::rotate(head.q, {off == 1 ? -0.17f / ws : 0.17f / ws, 0, 0});
        const core::Vec3 down = core::rotate(head.q, {0, -1.0f / ws, 0});
        InterlockedIncrement(&h->seq);
        h->park = (freeArm && haveCtrl) || gripped ? (rightHand ? 0 : 2) : (rightHand ? 1 : 2);
        h->freeSide = freeArm && haveCtrl ? off : 0;
        h->parkWorld = core::compose(head, {{}, {0, -0.05f / ws, 0.30f / ws}});  // behind and a little below the eyes
        h->shoulderWorld = head.p + side + down * 0.2f + core::rotate(head.q, {0, 0, 0.05f / ws});
        h->poleWorld = h->shoulderWorld + down * 0.35f + side * 1.2f;
        if (haveCtrl) h->handWorld = {core::normalize({c7[0], c7[1], c7[2], c7[3]}), {c7[4], c7[5], c7[6]}};
        // The free hand replaces that controller's model (re-asserted every tick; nothing native shows it again).
        if (freeArm && haveCtrl && hideController && g_ctrlOff[off - 1]) hideController(static_cast<uint8_t*>(vrd) + g_ctrlOff[off - 1]);
        core::smooth(h->fingers, fingers, 1.0f / 60.0f);
        h->tick = now;
        InterlockedIncrement(&h->seq);
        if (h->count == 0 || h->mi != mi) {
            h->mi = mi;
            h->poseIdents = nullptr;
            readRest(*h, static_cast<uint8_t*>(mi));
            void* queue = animQueue(mi, 1);
            if (queue) {
                addHandler(queue, h, 0);
                VRLOG("arms: handler %p on weapon %p (model %p, %s hand)", static_cast<void*>(h), weapon, mi, rightHand ? "right" : "left");
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool install() {
    if (!se4::options::get("hands", 1)) return false;
    const auto& b = se4::build();
    animQueue = reinterpret_cast<QueueFn>(b.at<void>(se4::gen::mi_anim_queue));
    addHandler = reinterpret_cast<AddFn>(b.at<void>(se4::gen::queue_add_handler));
    headWorld = reinterpret_cast<HeadWorldFn>(b.at<void>(se4::gen::vrd_head_world));
    identCtor = reinterpret_cast<IdentFn>(b.at<void>(se4::gen::ident_ctor));
    auto* place = b.at<void>(se4::gen::weapon_placement);
    auto* baseCtor = b.at<uint8_t>(se4::gen::mh_base_ctor);
    if (!animQueue || !addHandler || !headWorld || !identCtor || !place || !baseCtor || baseCtor[0] != 0x48 || baseCtor[1] != 0x8D ||
        baseCtor[2] != 0x05) {
        VRLOG("arms: build lacks arms symbols - not installed");
        animQueue = nullptr;
        return false;
    }
    int32_t rel;
    memcpy(&rel, baseCtor + 3, 4);
    memcpy(g_vtbl, reinterpret_cast<void**>(baseCtor + 7 + rel), sizeof g_vtbl);  // CMotionHandler vtable
    g_vtbl[0] = reinterpret_cast<void*>(dtor);
    g_vtbl[4] = reinterpret_cast<void*>(apply);
    if (MH_CreateHook(place, reinterpret_cast<void*>(hookedPlacement), reinterpret_cast<void**>(&realPlacement)) != MH_OK ||
        MH_EnableHook(place) != MH_OK) {
        VRLOG("arms: weapon placement hook FAILED");
        animQueue = nullptr;
        return false;
    }
    g_free = se4::options::get("hands_free", 1) != 0;
    hideController = reinterpret_cast<HideFn>(b.at<void>(se4::gen::hide_controller));
    if (auto* l = b.at<uint8_t>(se4::gen::hide_left_ctrl_script)) g_ctrlOff[0] = addRcxImm(l);
    if (auto* r = b.at<uint8_t>(se4::gen::hide_right_ctrl_script)) g_ctrlOff[1] = addRcxImm(r);
    if (g_ctrlOff[0] > 0x2000 || g_ctrlOff[1] > 0x2000) g_ctrlOff[0] = g_ctrlOff[1] = 0;
    VRLOG("arms: VR controllers at vrd+0x%x (left) / +0x%x (right)", g_ctrlOff[0], g_ctrlOff[1]);
    VRLOG("arms: off arm %s on first-person weapons", g_free ? "follows its controller" : "parked");
    return true;
}
}  // namespace arms
