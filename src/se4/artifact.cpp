#include "se4/artifact.hpp"
#include "core/vr_controls.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/names.hpp"
#include "se4/options.hpp"
#include "se4/patch.hpp"
#include "se4/player_vr.hpp"
#include "xr/input.hpp"
#include "xr/runtime.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>

namespace artifact {
namespace {
// Hooks pass four register arguments through (only the first one or two are known).
using StepFn = void(__cdecl*)(uint8_t* state, void* a2, void* a3, void* a4);
using HandleEvFn = void(__cdecl*)(uint8_t* state, uint8_t* ev, void* a3, void* a4);
using EntityFn = uint8_t*(__cdecl*)(int32_t handle);
using QueueFn = void*(__cdecl*)(void* mi, int create);
using SetSpeedFn = void(__cdecl*)(void* aq, float speed);
using GetSpeedFn = float(__cdecl*)(void* aq);
using LengthFn = float(__cdecl*)(void* aq, void* ident, int incl);
using MakeHandleFn = void(__cdecl*)(void* out16, void* obj, void* type);
using WorldFn = void*(__cdecl*)(void* state);
using CancelFn = void(__cdecl*)(void* world, const void* handle16);
using AuthorityFn = int(__cdecl*)(void* entity);
using ScheduleFn = void(__cdecl*)(void* state, float delay, void* callback, const char* name);
using TimeFn = void*(__cdecl*)(void* entity, void* out);
using IdentFn = void**(__cdecl*)(void** out, const char* name);
using FireFn = void(__cdecl*)(void* event, void* ctx, void* a3, void* a4);

StepFn realStep;
HandleEvFn realHandle;
FireFn realFire;
EntityFn handleEntity;
QueueFn animQueue;
SetSpeedFn setSpeed;
GetSpeedFn getSpeed;
LengthFn animLength;
MakeHandleFn makeHandle;
WorldFn stateWorld;
CancelFn timerCancel;
AuthorityFn isAuthority;
ScheduleFn schedule;
TimeFn entityTime;
IdentFn identCtor;
void* g_returnFn;              // CEntityState::Return, read from the step's own schedule call
const char* g_returnName;      // "CEntityState::Return"
const void* g_endType;         // the EEnd event type global (handler's EBegin compare + 8, verified)
void* g_identUse;

constexpr size_t kOwner = 0x18;  // state -> owner puppet (R17 s1.4 [D])
constexpr float kHeldSpeed = 1e-4f;  // the setter divides by the speed: never 0 (R17 s3.1)
int g_holdMs = 1500, g_timeoutS = 120;

template <class T> T& at(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

enum class Phase { Idle, Running, Held };
struct Track {
    uint8_t* state = nullptr;
    void* aq = nullptr;
    uint8_t* weapon = nullptr;
    float length = 0, speed = 1;
    double t0 = 0, heldAt = 0, heldAnim = 0;
    bool authority = false;
    int xrHand = 1;
    Phase phase = Phase::Idle;
    bool crushWas = true;  // a press already down when the hold starts must be released first
} g_t;
uint8_t* g_lastState;  // the state handled last (one hold per artifact)
volatile LONG g_holding[2];
volatile ULONGLONG g_stepTick;  // when the tracked state last stepped: a state that died without its end event lapses

double now(uint8_t* entity) {
    uint64_t t = 0;
    auto* p = static_cast<uint64_t*>(entityTime(entity, &t));
    return double(p ? *p : t) / 4294967296.0;  // 32.32 fixed seconds
}
bool isLocalVr(uint8_t* player) {
    const unsigned off = player_vr::vrdOffset();
    return player && off && player_vr::localVrd() && at<void*>(player, off) == player_vr::localVrd();
}
// The artifact weapon: its params resource file is SirianInjector.ep (SS4) / SirianInjector_SM.ep (SM) (R17 s2).
bool isArtifact(uint8_t* weapon) {
    const char* fn = se4::names::weaponFile(weapon);
    static uint8_t* logged;
    if (logged != weapon) logged = weapon, VRLOG("artifact: generic weapon animation \"Use\" with weapon params '%s'", fn ? fn : "?");
    return fn && strstr(fn, "SirianInjector");
}

void cancelEnd(uint8_t* state) {
    alignas(16) uint8_t h[16] = {};
    void* type = reinterpret_cast<void*(__cdecl*)(void*)>((*reinterpret_cast<void***>(state))[0])(state);
    makeHandle(h, state, type);
    timerCancel(stateWorld(state), h);
}

void release(const char* why) {
    if (g_t.phase == Phase::Idle) return;
    const double t = now(at<uint8_t*>(g_t.state, kOwner));
    const double animT = g_t.phase == Phase::Held ? g_t.heldAnim : (t - g_t.t0) * g_t.speed;
    if (g_t.phase == Phase::Held) setSpeed(g_t.aq, g_t.speed);
    if (g_t.authority) {
        const float rest = float(g_t.length - animT);
        schedule(g_t.state, rest > 0.05f ? rest : 0.05f, g_returnFn, g_returnName);
    }
    VRLOG("artifact: released (%s) at anim %.2f s of %.2f (held %.1f s)", why, animT, double(g_t.length),
          g_t.phase == Phase::Held ? t - g_t.heldAt : 0.0);
    if (xr::runtime().running()) xr::input().vibrate(xr::runtime().session(), g_t.xrHand, 1.0f, 0.12f);
    InterlockedExchange(&g_holding[0], 0), InterlockedExchange(&g_holding[1], 0);
    g_t = Track{};
}

// The hand holding the artifact crushes it: the trigger, Index squeeze force or the Frame grip's second stage.
bool crushInput(int xrHand) {
    const auto& h = xr::input().hand(xrHand);
    if (!h.active) return false;
    using core::controls::Kind;
    return h.trigger > 0.85f || core::controls::crushSqueeze(Kind(int(h.kind)), h.squeeze, h.buttons);
}

void __cdecl hookedStep(uint8_t* state, void* a2, void* a3, void* a4) {
    realStep(state, a2, a3, a4);
    const ULONGLONG nowTick = GetTickCount64();
    if (g_t.phase != Phase::Idle && state != g_t.state && nowTick - g_stepTick > 1000) {
        // The tracked state stopped stepping (world teardown, no end event): forget it, release the hand.
        VRLOG("artifact: the held artifact's state is gone - hand released");
        InterlockedExchange(&g_holding[0], 0), InterlockedExchange(&g_holding[1], 0);
        g_t = Track{};
    }
    if (g_t.phase == Phase::Idle || state == g_t.state) g_stepTick = nowTick;
    uint8_t* player = at<uint8_t*>(state, kOwner);
    const auto& b = se4::build();
    if (g_t.phase == Phase::Idle) {
        if (state == g_lastState || !isLocalVr(player) || !at<int32_t>(state, b.off(se4::gen::CPSGenericWeaponAnimation_es_bStartedPlayingAnim)))
            return;
        if (!g_identUse) identCtor(&g_identUse, "Use");  // interned on the simulation thread
        g_lastState = state;
        void* anim = at<void*>(state, b.off(se4::gen::CPSGenericWeaponAnimation_es_idAnimation));
        uint8_t* weapon = handleEntity(at<int32_t>(player, b.off(se4::gen::CPlayerActorPuppetEntity_en_henCurrentRightHandWeapon)));
        if (anim != g_identUse || !weapon || !isArtifact(weapon)) return;
        void* mi = at<void*>(weapon, 0x90);  // the weapon's model instance (R17 s1.6)
        void* aq = mi ? animQueue(mi, 1) : nullptr;
        if (!aq) return VRLOG("artifact: no anim queue on the artifact weapon %p", static_cast<void*>(weapon));
        g_t.state = state, g_t.aq = aq, g_t.weapon = weapon;
        g_t.length = animLength(aq, anim, 0);
        g_t.speed = getSpeed(aq);
        if (!(g_t.speed > 0.01f && g_t.speed < 100.0f)) g_t.speed = 1.0f;
        g_t.t0 = now(player);
        g_t.authority = isAuthority(player) != 0;
        g_t.xrHand = at<int32_t>(weapon, b.off(se4::gen::CWeaponEntity_en_bRightHandWeapon)) ? 1 : 0;
        g_t.phase = Phase::Running;
        g_t.crushWas = true;
        if (g_t.authority) cancelEnd(state);  // re-armed with the remaining time on release
        VRLOG("artifact: Sirian artifact in the %s hand (anim %.2f s, speed %.2f, authority %d) - hold at %d ms", g_t.xrHand ? "right" : "left",
              double(g_t.length), double(g_t.speed), int(g_t.authority), g_holdMs);
        return;
    }
    if (state != g_t.state) return;
    const double t = now(player);
    if (g_t.phase == Phase::Running && (t - g_t.t0) * g_t.speed >= g_holdMs / 1000.0) {
        g_t.heldAnim = (t - g_t.t0) * g_t.speed;
        g_t.heldAt = t;
        setSpeed(g_t.aq, kHeldSpeed);
        g_t.phase = Phase::Held;
        InterlockedExchange(&g_holding[g_t.xrHand], 1);
        VRLOG("artifact: held at anim %.2f s - crush it (trigger, Index squeeze, Frame grip second stage)", g_t.heldAnim);
        if (xr::runtime().running()) xr::input().vibrate(xr::runtime().session(), g_t.xrHand, 0.3f, 0.08f);
    }
    if (g_t.phase == Phase::Running) InterlockedExchange(&g_holding[g_t.xrHand], 1);
    const bool crush = crushInput(g_t.xrHand);
    if (crush && !g_t.crushWas) return release("crushed");
    g_t.crushWas = crush;
    if (g_t.phase == Phase::Held && t - g_t.heldAt > g_timeoutS) release("timeout");
}

// The state ends (death, level change, interrupt): never leave the queue slowed down.
void __cdecl hookedHandle(uint8_t* state, uint8_t* ev, void* a3, void* a4) {
    if (state == g_t.state && ev && g_endType && at<const void*>(ev, 8) == *static_cast<const void* const*>(g_endType)) {
        if (g_t.phase == Phase::Held) setSpeed(g_t.aq, g_t.speed);
        VRLOG("artifact: state ended during the hold");
        InterlockedExchange(&g_holding[0], 0), InterlockedExchange(&g_holding[1], 0);
        g_t = Track{};
    }
    realHandle(state, ev, a3, a4);
}
void __cdecl hookedFire(void* event, void* ctx, void* a3, void* a4) {
    if (g_lastState) VRLOG("artifact: global script anim event fired%s", g_t.phase == Phase::Held ? " DURING THE HOLD" : "");
    realFire(event, ctx, a3, a4);
}

template <class F> bool hook(void* fn, F* detour, F** real, const char* what) {
    if (!fn || MH_CreateHook(fn, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(real)) != MH_OK || MH_EnableHook(fn) != MH_OK)
        return VRLOG("artifact: %s hook FAILED", what), false;
    return true;
}
}  // namespace

// Lapses a second after the held state last stepped (no step at all after a level change, for instance).
bool holding(int hand) { return (hand == 0 || hand == 1) && g_holding[hand] != 0 && GetTickCount64() - g_stepTick < 1000; }

bool install() {
    if (!se4::options::get("artifact_crush", 1)) return VRLOG("artifact: crush disabled by option"), false;
    g_holdMs = se4::options::get("artifact_hold_ms", 1500);
    g_timeoutS = se4::options::get("artifact_timeout_s", 120);
    const auto& b = se4::build();
    auto* step = b.at<uint8_t>(se4::gen::gwa_step);
    auto* handler = b.at<uint8_t>(se4::gen::gwa_handle);
    handleEntity = reinterpret_cast<EntityFn>(b.at<void>(se4::gen::handle_entity));
    animQueue = reinterpret_cast<QueueFn>(b.at<void>(se4::gen::mi_anim_queue));
    setSpeed = reinterpret_cast<SetSpeedFn>(b.at<void>(se4::gen::aq_set_speed));
    getSpeed = reinterpret_cast<GetSpeedFn>(b.at<void>(se4::gen::aq_get_speed));
    animLength = reinterpret_cast<LengthFn>(b.at<void>(se4::gen::aq_anim_length));
    makeHandle = reinterpret_cast<MakeHandleFn>(b.at<void>(se4::gen::make_handle));
    stateWorld = reinterpret_cast<WorldFn>(b.at<void>(se4::gen::state_world));
    timerCancel = reinterpret_cast<CancelFn>(b.at<void>(se4::gen::timer_cancel));
    isAuthority = reinterpret_cast<AuthorityFn>(b.at<void>(se4::gen::is_authority));
    schedule = reinterpret_cast<ScheduleFn>(b.at<void>(se4::gen::state_schedule));
    entityTime = reinterpret_cast<TimeFn>(b.at<void>(se4::gen::entity_time));
    identCtor = reinterpret_cast<IdentFn>(b.at<void>(se4::gen::ident_ctor));
    if (!step || !handler || !handleEntity || !animQueue || !setSpeed || !getSpeed || !animLength || !makeHandle || !stateWorld || !timerCancel ||
        !isAuthority || !schedule || !entityTime || !identCtor || !b.off(se4::gen::CPSGenericWeaponAnimation_es_idAnimation) ||
        !b.off(se4::gen::CWeaponEntity_en_pwpWeaponParams) || !b.off(se4::gen::CPlayerActorPuppetEntity_en_henCurrentRightHandWeapon))
        return VRLOG("artifact: build lacks the generic weapon animation symbols - no crush"), false;
    // The end timer's callback and name: `lea r8,[rip+Return]; ... lea r9,[rip+name]` before the step's schedule call.
    for (size_t i = 0x20; i + 5 < 0x500 && !g_returnFn; ++i) {
        if (step[i] != 0xE8 || step + i + 5 + *reinterpret_cast<int32_t*>(step + i + 1) != reinterpret_cast<uint8_t*>(schedule)) continue;
        for (size_t k = i - 0x20; k < i; ++k) {
            if (step[k] == 0x4C && step[k + 1] == 0x8D && step[k + 2] == 0x05) g_returnFn = step + k + 7 + *reinterpret_cast<int32_t*>(step + k + 3);
            if (step[k] == 0x4C && step[k + 1] == 0x8D && step[k + 2] == 0x0D) g_returnName = reinterpret_cast<const char*>(step + k + 7 + *reinterpret_cast<int32_t*>(step + k + 3));
        }
    }
    if (!g_returnFn || !g_returnName || strcmp(g_returnName, "CEntityState::Return"))
        return VRLOG("artifact: state end timer not found in the step - no crush"), false;
    // EEnd type: the handler compares ev+8 with EBegin first (`cmp rax,[rip+EBegin]`); EEnd is the next global, which
    // the handler also compares with (R17 s1.5).
    const uint8_t* begin = nullptr;
    for (size_t i = 0; i + 7 < 0x40 && !begin; ++i)
        if (handler[i] == 0x48 && handler[i + 1] == 0x3B && handler[i + 2] == 0x05) begin = handler + i + 7 + *reinterpret_cast<const int32_t*>(handler + i + 3);
    for (size_t i = 0; begin && i + 7 < 0x400 && !g_endType; ++i)
        if (handler[i] == 0x48 && handler[i + 1] == 0x3B && (handler[i + 2] & 0xC7) == 0x05 &&
            handler + i + 7 + *reinterpret_cast<const int32_t*>(handler + i + 3) == begin + 8)
            g_endType = begin + 8;
    if (!g_endType) VRLOG("artifact: EEnd type not found in the handler - the hold is dropped only on release or timeout");
    if (!hook(step, hookedStep, &realStep, "generic weapon animation step") || !hook(handler, hookedHandle, &realHandle, "generic weapon animation event"))
        return false;
    if (auto* fire = b.at<void>(se4::gen::gsae_fire)) hook(fire, hookedFire, &realFire, "global script anim event");
    VRLOG("artifact: Sirian artifact crush active (hold at %d ms, timeout %d s, end timer %p '%s', EEnd type %p)", g_holdMs, g_timeoutS, g_returnFn,
          g_returnName, g_endType);
    return true;
}
}  // namespace artifact
