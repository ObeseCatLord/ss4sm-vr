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
#include "xr/runtime.hpp"
#include "se4/vr_device.hpp"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstring>

namespace vehicles {
namespace {
constexpr float kFallbackRim = 0.18f;  // metres: steering-wheel rim when the model has no grip attachments
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

// en_henRide may be any rideable (R7 s3): only a CVehicleEntity has the behaviour array read below. The engine's own
// check (vehicle_isa_site) gives the class global and the is-a function: is_a(entity->vt[0](entity), *classGlobal).
using ClassFn = void*(__cdecl*)(void*);
using IsAFn = bool(__cdecl*)(void*, void*);
void** g_vehicleClass;
IsAFn g_isA;
bool decodeVehicleCheck() {
    auto* site = se4::build().at<uint8_t>(se4::gen::vehicle_isa_site);
    if (!site || site[0x1f] != 0x48 || site[0x20] != 0x8B || site[0x21] != 0x15 || site[0x29] != 0xE8) return false;
    int32_t d, r;
    memcpy(&d, site + 0x22, 4);
    memcpy(&r, site + 0x2a, 4);
    g_vehicleClass = reinterpret_cast<void**>(site + 0x26 + d);
    g_isA = reinterpret_cast<IsAFn>(site + 0x2e + r);
    return true;
}
bool isVehicle(uint8_t* e) {
    if (!e || !g_isA || !g_vehicleClass || !*g_vehicleClass) return false;
    void* cls = reinterpret_cast<ClassFn>((*reinterpret_cast<void***>(e))[0])(e);
    return cls && g_isA(cls, *g_vehicleClass);
}

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

// Riding view (R24): the puppet predicate "is in third-person view" (vt+0xdf0) answers first person for the local VR
// player while riding, so the headset keeps the head camera at the seat instead of the chase camera that seats such
// as the ATV, chopper, tractor and SM snowmobile prefer. No option or cvar is written. Called for every puppet from
// the simulation and render paths, so it only compares pointers.
using ThirdPersonFn = int(__cdecl*)(uint8_t* puppet);
ThirdPersonFn realThirdPerson;
int __cdecl hookedThirdPerson(uint8_t* puppet) {
    void* vrd = player_vr::localVrd();
    const unsigned vrdOff = player_vr::vrdOffset();
    // vrd+0 is its player: compare first, so +vrdOff is read only on that (large enough) puppet.
    if (vrd && puppet && vrdOff && *static_cast<uint8_t**>(vrd) == puppet && at<void*>(puppet, vrdOff) == vrd &&
        vrdev::installed() && handleEntity(at<int32_t>(puppet, se4::build().off(se4::gen::CPuppetEntity_en_henRide)))) {
        static LONG once;
        if (!InterlockedExchange(&once, 1)) VRLOG("vehicles: riding view kept first person for the VR player");
        return 0;
    }
    return realThirdPerson(puppet);
}
bool installRidingView() {
    const auto& b = se4::build();
    void* fn = b.at<void>(se4::gen::is_third_person_view);
    if (!se4::options::get("ride_first_person", 1) || !fn || !handleEntity || !b.off(se4::gen::CPuppetEntity_en_henRide)) return false;
    if (MH_CreateHook(fn, reinterpret_cast<void*>(hookedThirdPerson), reinterpret_cast<void**>(&realThirdPerson)) != MH_OK ||
        MH_EnableHook(fn) != MH_OK)
        return VRLOG("vehicles: riding view hook FAILED"), false;
    VRLOG("vehicles: riding view first person in VR (%p)", fn);
    return true;
}

void release() {
    if (g_driving) vrdev::setSeatOffset(0.0f);
    g_driving = false;
    g_holding = false;
    g_grab = {};
    g_vehicle = nullptr;
    g_bone = nullptr;
}
}  // namespace

bool install() {
    const auto& b = se4::build();
    handleEntity = reinterpret_cast<HandleFn>(b.at<void>(se4::gen::handle_entity));
    installRidingView();
    if (!se4::options::get("vehicle_grab", 1)) return false;
    boneWorld = reinterpret_cast<BoneWorldFn>(b.at<void>(se4::gen::bone_world));
    attachWorld = reinterpret_cast<AttachWorldFn>(b.at<void>(se4::gen::attach_world));
    identFromString = reinterpret_cast<IdentFn>(b.at<void>(se4::gen::ident_ctor));
    followerSetRot = reinterpret_cast<SetRotFn>(b.at<void>(se4::gen::follower_setrot));
    eulerQuat = reinterpret_cast<EulerFn>(b.at<void>(se4::gen::euler_quat));
    g_animateFn = b.at<void>(se4::gen::wvb_animate);
    if (!decodeVehicleCheck()) {
        VRLOG("vehicles: the engine's vehicle class check was not found - grab steering off (a non-vehicle ride is not safe)");
        return false;
    }
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
    if (!vehicle || !isVehicle(vehicle) || handleEntity(at<int32_t>(vehicle, b.off(se4::gen::CPuppetEntity_en_henOperator))) != player) {
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
        // Seated height: on boarding the head is brought to ride_eye_cm above the seat (the player placement, where
        // the play-space floor sits while riding), whether the rider stands or sits; undone on leaving.
        const auto& v = xr::runtime().views();
        static const float eye = se4::options::get("ride_eye_cm", 80) / 100.0f;
        if (v.valid && se4::options::get("ride_seat_height", 1)) {
            vrdev::setSeatOffset(eye - v.head.position.y);
            VRLOG("vehicles: seated height %+.2f m (head %.2f m above the floor -> %.2f above the seat)", eye - v.head.position.y,
                  v.head.position.y, eye);
        }
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
    static const bool diag = se4::options::get("vehicles_log", 0) != 0;
    static ULONGLONG lastDiag;
    const bool logNow = diag && GetTickCount64() - lastDiag > 2000;
    if (logNow) {
        lastDiag = GetTickCount64();
        VRLOG("vehicles: bone %.2f %.2f %.2f axis %.2f %.2f %.2f rim %.3f grips L %.2f %.2f %.2f R %.2f %.2f %.2f", bonePose.p.x, bonePose.p.y,
              bonePose.p.z, axis.x, axis.y, axis.z, rim, gp[0][4], gp[0][5], gp[0][6], gp[1][4], gp[1][5], gp[1][6]);
    }
    if (rim < 0.03f || rim > 1.0f) {
        // No hand-grip attachments on this model (e.g. the Harvester: both resolve to the vehicle origin): a usual
        // steering-wheel rim around the bone, angle 0 towards the driver's head (only angle changes steer).
        float head7[7] = {};
        player_vr::headWorld(vrd, head7);
        core::Vec3 toHead = core::Vec3{head7[4], head7[5], head7[6]} - bonePose.p;
        toHead = toHead - axis * core::dot(toHead, axis);
        const float len = std::sqrt(core::dot(toHead, toHead));
        if (!(len > 1e-3f)) return;
        zero = toHead * (1.0f / len);
        rim = kFallbackRim;
    }
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
    if (logNow)
        VRLOG("vehicles: hands L %.2f %.2f %.2f R %.2f %.2f %.2f grip %d %d, wheel %.2f rad steer %.2f", hands[0].x, hands[0].y, hands[0].z,
              hands[1].x, hands[1].y, hands[1].z, int(grip[0]), int(grip[1]), g_angle.load(), g_steer.load());
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
