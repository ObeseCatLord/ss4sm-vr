// Grab-to-steer on the real steering bone (R7 s2/s3/s6b). Offsets of reflected members come from the
// generated member table; the CWheeledVehicleBehavior fields below (+0x08 vehicle, +0x10 params) are read off
// its animate step, identical in SS4 and SM (Sam4 0x140a509d0 / SM 0x140a50e30).
#include "se4/vehicles.hpp"
#include "core/math.hpp"
#include "core/steering.hpp"
#include "core/vr_controls.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/player_vr.hpp"
#include "xr/input.hpp"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstring>

namespace vehicles {
namespace {
using HandleFn = void*(__cdecl*)(int32_t handle);
using BoneWorldFn = float*(__cdecl*)(float* outP7, void* entity, void* ident);
using AttachWorldFn = uintptr_t(__cdecl*)(void* entity, float* outP7, void* ident);
using IdentFn = void**(__cdecl*)(void** out, const char* name);
using SetRotFn = void(__cdecl*)(void* follower, void* ident, const float* quat);
using EulerFn = float*(__cdecl*)(float* outQuat, const float* hpb);
using FloatFn = float(__cdecl*)(void* self);
using AnimateFn = void(__cdecl*)(uint8_t* behavior, void* p2);

constexpr uint32_t kBehaviorVehicle = 0x08, kBehaviorParams = 0x10;

HandleFn handleEntity;
BoneWorldFn boneWorld;
AttachWorldFn attachWorld;
IdentFn identFromString;
SetRotFn followerSetRot;
EulerFn eulerQuat;
AnimateFn realAnimate;
void* g_animateFn;
void* g_emptyIdent;
void* g_gripIdent[2];  // LeftHandSteeringHandle, RightHandSteeringHandle
core::Quat g_localAxisQ;  // unit rotation axis of +H in the bone's local frame (from the engine's Euler)
bool g_ready;

core::SteeringGrab g_grab;
uint8_t* g_vehicle;
void* g_bone;
std::atomic<bool> g_driving{false}, g_holding{false};
std::atomic<float> g_steer{0}, g_angle{0};

core::Pose fromP7(const float* p) { return {{p[0], p[1], p[2], p[3]}, {p[4], p[5], p[6]}}; }
template <class T> T& at(void* base, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

uint8_t* wheeledBehavior(uint8_t* vehicle) {
    uint32_t off = se4::build().off(se4::gen::CVehicleEntity_en_avbBehaviors);
    uint8_t** arr = at<uint8_t**>(vehicle, off);
    int n = at<int32_t>(vehicle, off + 8);
    for (int i = 0; arr && i < n && i < 16; ++i)
        if (arr[i] && (*reinterpret_cast<void***>(arr[i]))[0xa8 / 8] == g_animateFn) return arr[i];
    return nullptr;
}

void hookedAnimate(uint8_t* behavior, void* p2) {
    realAnimate(behavior, p2);
    if (!g_holding.load() || at<uint8_t*>(behavior, kBehaviorVehicle) != g_vehicle || !g_bone) return;
    const auto& b = se4::build();
    void* follower = at<void*>(g_vehicle, b.off(se4::gen::CVehicleEntity_en_mhSkeletonAdjuster));
    if (!follower) return;
    // Same composition as the native step: q_init (behavior) (x) H(angle), Hamilton product, xyzw.
    const float* qi = &at<float>(behavior, b.off(se4::gen::CWheeledVehicleBehavior_vb_qInitialSteeringWheelBoneRotation));
    float hpb[3] = {g_angle.load(), 0, 0}, qh[4];
    eulerQuat(qh, hpb);
    core::Quat q = core::multiply({qi[0], qi[1], qi[2], qi[3]}, {qh[0], qh[1], qh[2], qh[3]});
    const float out[4] = {q.x, q.y, q.z, q.w};
    followerSetRot(follower, g_bone, out);
}

void release() {
    g_driving = false;
    g_holding = false;
    g_grab = {};
    g_vehicle = nullptr;
    g_bone = nullptr;
}
}  // namespace

bool install() {
    if (!se4::options::get("vehicle_grab", 1)) return false;
    const auto& b = se4::build();
    handleEntity = reinterpret_cast<HandleFn>(b.at<void>(se4::gen::handle_entity));
    boneWorld = reinterpret_cast<BoneWorldFn>(b.at<void>(se4::gen::bone_world));
    attachWorld = reinterpret_cast<AttachWorldFn>(b.at<void>(se4::gen::attach_world));
    identFromString = reinterpret_cast<IdentFn>(b.at<void>(se4::gen::ident_ctor));
    followerSetRot = reinterpret_cast<SetRotFn>(b.at<void>(se4::gen::follower_setrot));
    eulerQuat = reinterpret_cast<EulerFn>(b.at<void>(se4::gen::euler_quat));
    g_animateFn = b.at<void>(se4::gen::wvb_animate);
    for (unsigned m = 0; m < se4::gen::MemCount; ++m)
        if (!b.off(se4::gen::Mem(m))) {
            VRLOG("vehicles: member %u unknown in this build - grab mode off", m);
            return false;
        }
    if (!handleEntity || !boneWorld || !attachWorld || !identFromString || !followerSetRot || !eulerQuat || !g_animateFn) {
        VRLOG("vehicles: build lacks vehicle symbols - grab mode off");
        return false;
    }
    if (MH_CreateHook(g_animateFn, reinterpret_cast<void*>(hookedAnimate), reinterpret_cast<void**>(&realAnimate)) != MH_OK ||
        MH_EnableHook(g_animateFn) != MH_OK) {
        VRLOG("vehicles: animate hook FAILED");
        return false;
    }
    VRLOG("vehicles: grab steering available");
    return true;
}

void step(uint8_t* player, void* vrd) {
    if (!g_animateFn || !realAnimate) return;
    const auto& b = se4::build();
    if (!g_ready) {
        // IDENTs are interned pointers; create them on the simulation thread like the engine's own callers.
        identFromString(&g_emptyIdent, "");
        identFromString(&g_gripIdent[0], "LeftHandSteeringHandle");
        identFromString(&g_gripIdent[1], "RightHandSteeringHandle");
        float hpb[3] = {0.1f, 0, 0}, q[4];
        eulerQuat(q, hpb);
        float s = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
        g_localAxisQ = s > 1e-6f ? core::Quat{q[0] / s, q[1] / s, q[2] / s, 0} : core::Quat{0, 1, 0, 0};
        g_ready = true;
        VRLOG("vehicles: engine heading axis (%.2f %.2f %.2f)", g_localAxisQ.x, g_localAxisQ.y, g_localAxisQ.z);
    }
    auto* vehicle = static_cast<uint8_t*>(handleEntity(at<int32_t>(player, b.off(se4::gen::CPuppetEntity_en_henRide))));
    if (!vehicle || handleEntity(at<int32_t>(vehicle, b.off(se4::gen::CPuppetEntity_en_henOperator))) != player) {
        if (g_driving) release();
        return;
    }
    uint8_t* wb = wheeledBehavior(vehicle);
    uint8_t* params = wb ? at<uint8_t*>(wb, kBehaviorParams) : nullptr;
    uint8_t* scheme = params ? at<uint8_t*>(params, b.off(se4::gen::CWheeledVehicleBehaviorParams_vbp_pwvsasSkeletonAdjusterScheme)) : nullptr;
    void* bone = scheme ? at<void*>(scheme, b.off(se4::gen::CWheeledVehicleSkeletonAdjusterScheme_wvsas_idSteeringWheelBone)) : nullptr;
    if (vehicle != g_vehicle) {
        release();
        g_vehicle = vehicle;
        VRLOG("vehicles: driving %p (steering bone %s)", static_cast<void*>(vehicle), bone && bone != g_emptyIdent ? "found" : "none");
    }
    g_driving = true;
    if (!bone || bone == g_emptyIdent) return;  // no cockpit wheel (chopper fork, mechs): thumbstick only
    g_bone = bone;
    const float maxRad = at<float>(scheme, b.off(se4::gen::CWheeledVehicleSkeletonAdjusterScheme_wvsas_fMaxSteeringWheelAngle)) * core::Pi / 180.0f;

    float bp[7], gp[2][7];
    boneWorld(bp, vehicle, bone);
    core::Pose bonePose = fromP7(bp);
    core::Vec3 axis = core::rotate(core::normalize(bonePose.q), {g_localAxisQ.x, g_localAxisQ.y, g_localAxisQ.z});
    float rim = 0;
    core::Vec3 zero{};
    for (int h = 0; h < 2; ++h) {
        memset(gp[h], 0, sizeof gp[h]);
        attachWorld(vehicle, gp[h], g_gripIdent[h]);
        core::Vec3 r = core::Vec3{gp[h][4], gp[h][5], gp[h][6]} - bonePose.p;
        r = r - axis * core::dot(r, axis);
        float len = std::sqrt(core::dot(r, r));
        rim += len * 0.5f;
        if (h == 1 && len > 1e-4f) zero = r * (1.0f / len);
    }
    if (rim < 0.03f || rim > 1.0f) return;  // grips missing or implausible: thumbstick only
    core::SteerAxis sa{bonePose.p, axis, zero};

    core::Vec3 hands[2];
    bool grip[2];
    for (int h = 0; h < 2; ++h) {
        float p7[7];
        player_vr::controllerWorld(vrd, h == 0 ? 1 : 0, p7);  // engine role 0 = right
        hands[h] = {p7[4], p7[5], p7[6]};
        const auto& x = xr::input().hand(h);
        grip[h] = x.active && x.squeeze > core::controls::gripThreshold(core::controls::Kind(int(x.kind)));
    }
    // Not holding: follow the visible native wheel so a grab starts from where the wheel is.
    void* steering = at<void*>(vehicle, b.off(se4::gen::CVehicleEntity_en_pvsSteering));
    if (steering && !g_grab.holding()) {
        auto vt = *reinterpret_cast<FloatFn**>(steering);
        float maxSteer = vt[0x48 / 8](steering);
        if (maxSteer > 1e-4f) g_grab.follow(vt[0x40 / 8](steering) / maxSteer * maxRad);
    }
    float angle = g_grab.update(sa, hands, grip, rim, maxRad, rim * 0.6f);
    g_angle = angle;
    g_steer = g_grab.steer(maxRad);
    bool holding = g_grab.holding();
    if (holding != g_holding.load()) VRLOG("vehicles: %s the steering wheel", holding ? "grabbed" : "released");
    g_holding = holding;
}

bool steerOverride(float& lx) {
    if (!g_holding.load()) return false;
    lx = -g_steer.load();  // native desired steer = -relMove.x; +steer = left (R7 s6b.3)
    return true;
}

bool driving() { return g_driving.load(); }
}  // namespace vehicles
