#include "se4/gadgets.hpp"
#include "core/body_ik.hpp"
#include "core/gadget_use.hpp"
#include "core/math.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/player_vr.hpp"
#include "se4/hands.hpp"
#include "se4/gamepad.hpp"
#include "se4/wheel_panel.hpp"
#include "xr/input.hpp"
#include "xr/runtime.hpp"
#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace gadgets {
namespace {
using ModeFn = uintptr_t(__cdecl*)(uint8_t* mode);
using HandleFn = uint8_t*(__cdecl*)(int32_t handle);
using HolderFn = void(__cdecl*)(uint8_t* holder);
using ReadyFn = int(__cdecl*)(uint8_t* holder);
using TargetingFn = void(__cdecl*)(uint8_t* mode, void* arg);
using ViewFn = float*(__cdecl*)(uint8_t* player, float* out, void* arg, int mode);
using HudFn = void(__cdecl*)(uint8_t* player, int arg, int command);
ModeFn realActivate;
HandleFn handleEntity;
HolderFn realHolderStep, realHolderUse;
ReadyFn holderReady;
TargetingFn realTargeting;
ViewFn realView;
float g_gain = 2.0f;
bool g_motion = true, g_aimHand = true, g_throwVelocity = false;
constexpr float kThrowSpeed = 1.6f;  // m/s of the hand that counts as a throw (core/grenade_throw.hpp)
constexpr float kMaxSpeed = 250.0f;  // the projectile init clamps here (R12 s8.4)
constexpr int kHudQuickUse = 0xc, kHudSelect = 9, kHudNext = 0xd, kHudPrevious = 0xe;  // R12 s7
constexpr size_t kModeWeapon = 8, kModeParams = 0x10;                                // CWeaponMode (0x140090e60)

template <class T> T& at(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }
void hud(uint8_t* player, int command) { reinterpret_cast<HudFn>((*reinterpret_cast<void***>(player))[0x11b0 / 8])(player, 1, command); }

uint8_t* localOwner(uint8_t* weapon) {
    void* local = player_vr::localVrd();
    const unsigned vrdOff = player_vr::vrdOffset();
    if (!local || !vrdOff) return nullptr;
    uint8_t* owner = handleEntity(at<int32_t>(weapon, se4::build().off(se4::gen::CWeaponEntity_en_henOwner)));
    return owner && at<void*>(owner, vrdOff) == local ? owner : nullptr;
}
bool rightHanded(uint8_t* weapon) { return at<int32_t>(weapon, se4::build().off(se4::gen::CWeaponEntity_en_bRightHandWeapon)) != 0; }

// A readable NUL-terminated ASCII string at p (at least 3 letters), else null.
const char* asciiAt(const void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || reinterpret_cast<uintptr_t>(p) < 0x10000 || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        return nullptr;
    const char* s = static_cast<const char*>(p);
    const size_t room = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < std::min<size_t>(room, 128); ++i) {
        if (!s[i]) return i >= 3 ? s : nullptr;
        if (s[i] < 0x20 || s[i] > 0x7e) return nullptr;
    }
    return nullptr;
}
// The item's name key from its CTransString (gip_tsName): the first string found at the field or one pointer
// away (the layout of CTransString is not decoded; the lab log shows which one matched).
const char* nameKey(uint8_t* gip, int& where) {
    uint8_t* ts = gip + se4::build().off(se4::gen::CGadgetItemParams_gip_tsName);
    for (int i = 0; i < 2; ++i) {
        void* q = at<void*>(ts, i * 8);
        if (const char* s = asciiAt(q)) return where = i, s;
        MEMORY_BASIC_INFORMATION mbi;
        if (q && reinterpret_cast<uintptr_t>(q) > 0x10000 && VirtualQuery(q, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD))
            if (const char* s = asciiAt(*static_cast<void**>(q))) return where = 2 + i, s;
    }
    return where = -1, nullptr;
}

core::gadget::Use classify(uint8_t* gip) {
    static uint8_t* cacheKey[16];
    static core::gadget::Use cacheUse[16];
    for (int i = 0; i < 16; ++i)
        if (cacheKey[i] == gip) return cacheUse[i];
    int where = -1;
    const char* key = nameKey(gip, where);
    const int bot = at<int32_t>(gip, se4::build().off(se4::gen::CGadgetItemParams_gip_bgutBotUsageType));
    const auto use = core::gadget::classify(key, bot);
    static int next;
    cacheKey[next] = gip, cacheUse[next] = use, next = (next + 1) % 16;
    VRLOG("gadgets: gadget %p '%s' (name at %d, bot usage %d): %s", gip, key ? key : "?", where, bot, core::gadget::name(use));
    return use;
}

core::Pose pose(const XrPosef& p) {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}};
}

// The gadget in the hand (simulation thread).
struct Held {
    uint8_t* holder = nullptr;
    uint8_t* gip = nullptr;
    core::gadget::Activation act;
    ULONGLONG seen = 0;
    int xrHand = 1;
} g_held;
volatile LONG g_triggerTaken[2];
volatile LONG g_buzz;  // 1 + XR hand to pulse once (present thread)
// The targeting mode of the gadget in the hand (its last step) and the native target check the launch
// activation runs (0 = invalid or too close; R12 s8.2).
uint8_t* g_targetHolder;
uint8_t* g_targetMode;
using CheckFn = int(__cdecl*)(uint8_t* mode);
CheckFn targetCheck;

bool holderUp() { return g_held.holder && GetTickCount64() - g_held.seen < 250; }

void inHand(uint8_t* holder, uint8_t* player) {
    const auto& b = se4::build();
    uint8_t* gip = at<uint8_t*>(holder, b.off(se4::gen::CGadgetHolderWeaponEntity_en_pgipUsedGadget));
    if (!gip || at<int32_t>(holder, b.off(se4::gen::CGadgetHolderWeaponEntity_en_bQuickUse))) return;  // quick use fires natively
    const ULONGLONG now = GetTickCount64();
    if (g_held.holder != holder || g_held.gip != gip || now - g_held.seen > 500) {
        g_held.holder = holder, g_held.gip = gip;
        g_held.xrHand = rightHanded(holder) ? 1 : 0;
        const auto use = g_motion ? classify(gip) : core::gadget::Use::Trigger;
        g_held.act.begin(use);
        VRLOG("gadgets: %s in the %s hand: %s", g_motion ? "gadget" : "gadget (gadget_motion=0)", g_held.xrHand ? "right" : "left",
              core::gadget::name(use));
    }
    g_held.seen = now;
    InterlockedExchange(&g_triggerTaken[g_held.xrHand], 1);
    InterlockedExchange(&g_triggerTaken[1 - g_held.xrHand], 0);
    if (at<int32_t>(holder, b.off(se4::gen::CGadgetHolderWeaponEntity_en_bGadgetUsed)) || g_held.act.fired()) return;
    const bool ready = !holderReady || holderReady(holder);
    static ULONGLONG lastLog;
    if (now - lastLog > 1000) {
        lastLog = now;
        const auto& g = xr::input().hand(g_held.xrHand).grip.position;
        VRLOG("gadgets:   ready %d armed %d zone distance %.3f (hand %.2f %.2f %.2f)", int(ready), int(g_held.act.armed()),
              g_held.act.distance(), g.x, g.y, g.z);
    }
    if (!ready) return;  // still coming up (the native quick use waits for this too)

    const auto& hs = xr::input().hand(g_held.xrHand);
    const auto& os = xr::input().hand(1 - g_held.xrHand);
    const auto& v = xr::runtime().views();
    core::gadget::Frame f;
    f.handValid = hs.active, f.otherValid = os.active, f.headValid = v.valid;
    f.hand = pose(hs.grip), f.other = pose(os.grip), f.head = pose(v.head);
    f.trigger = hs.active ? hs.trigger : 0.0f;
    if (g_held.act.update(f, now)) {
        if (g_held.act.use() == core::gadget::Use::Throw && g_targetHolder == holder && g_targetMode && targetCheck &&
            !(at<int32_t>(g_targetMode, 0x30) && targetCheck(g_targetMode))) {
            // No valid target under the aiming hand (too close, nothing in range): the launch activation would refuse
            // it after the throw animation. Keep holding it; a buzz on the aiming hand says why.
            g_held.act.begin(core::gadget::Use::Throw);
            InterlockedExchange(&g_buzz, 1 + (1 - g_held.xrHand));
            VRLOG("gadgets: throw without a valid target - kept in the hand");
            return;
        }
        VRLOG("gadgets: %s -> use", core::gadget::name(g_held.act.use()));
        hud(player, kHudQuickUse);  // holder up, not quick use: the holder's own use (R12 s7), through the network call
    }
}

void hookedHolderStep(uint8_t* holder) {
    realHolderStep(holder);
    if (uint8_t* player = holder ? localOwner(holder) : nullptr) {
        onTick(player);
        inHand(holder, player);
    }
}
void hookedHolderUse(uint8_t* holder) {
    if (holder && localOwner(holder)) VRLOG("gadgets: holder use");
    realHolderUse(holder);
}

// Targeting: while the AOE targeting step of a VR player's gadget runs, the view placement it starts from
// (player vt+0x340 mode 0, the head in VR) is the aiming hand's: its orientation, placed so that the arc's
// first-person start offset lands at the gadget hand. The native arc, marker, target checks and the launch
// quaternion/speed/time then all follow that hand.
thread_local uint8_t* t_targeting;
// Every VR player's gadget holder -> its targeting mode (last step): the launch activation re-runs the targeting and
// must steer it by that player's hand too (a server steers remote VR players' throws). Simulation thread.
struct TargetOf {
    uint8_t* holder = nullptr;
    uint8_t* mode = nullptr;
    ULONGLONG seen = 0;
} g_targets[8];
bool vrOwned(uint8_t* holder) {
    const unsigned vrdOff = player_vr::vrdOffset();
    uint8_t* owner = holder && vrdOff ? handleEntity(at<int32_t>(holder, se4::build().off(se4::gen::CWeaponEntity_en_henOwner))) : nullptr;
    return owner && at<void*>(owner, vrdOff);
}
void noteTarget(uint8_t* holder, uint8_t* mode) {
    const ULONGLONG now = GetTickCount64();
    TargetOf* slot = nullptr;
    for (auto& t : g_targets)
        if (t.holder == holder) slot = &t;
    if (!slot)
        for (auto& t : g_targets)
            if (!slot || now - t.seen > now - slot->seen) slot = &t;  // the free or least recently seen entry
    *slot = {holder, mode, now};
}
uint8_t* targetOf(uint8_t* holder) {
    for (auto& t : g_targets)
        if (t.holder == holder && GetTickCount64() - t.seen < 2000) return t.mode;
    return nullptr;
}
void hookedTargeting(uint8_t* mode, void* arg) {
    uint8_t* const prev = t_targeting;
    t_targeting = mode;
    realTargeting(mode, arg);
    t_targeting = prev;
    uint8_t* holder = at<uint8_t*>(mode, kModeWeapon);
    if (holder && vrOwned(holder)) noteTarget(holder, mode);
    if (holder && holder == g_held.holder) g_targetHolder = holder, g_targetMode = mode;
}
float* hookedView(uint8_t* player, float* out, void* arg, int mode) {
    float* r = realView(player, out, arg, mode);
    const unsigned vrdOff = player_vr::vrdOffset();
    if (!t_targeting || mode != 0 || !r || !vrdOff) return r;
    void* vrd = at<void*>(player, vrdOff);  // any VR player: a server steers remote VR players' throws by their hands
    uint8_t* holder = at<uint8_t*>(t_targeting, kModeWeapon);
    if (!vrd || !holder) return r;
    const int gadgetRole = rightHanded(holder) ? 0 : 1;  // engine role 0 right, 1 left
    float aim7[7] = {}, hand7[7] = {};
    player_vr::controllerWorld(vrd, 1 - gadgetRole, aim7);
    player_vr::controllerWorld(vrd, gadgetRole, hand7);
    if (!(aim7[0] || aim7[1] || aim7[2] || aim7[3])) return r;
    const core::Quat q = core::normalize({aim7[0], aim7[1], aim7[2], aim7[3]});
    core::Vec3 off{};
    if (uint8_t* params = at<uint8_t*>(t_targeting, kModeParams)) {
        off = at<core::Vec3>(params, se4::build().off(se4::gen::CAOETargetingWeaponModeParams_wmp_vTrajectoryStartOffsetFirstPerson));
        if (!(core::dot(off, off) < 9.0f)) off = {};
    }
    const core::Vec3 p = core::Vec3{hand7[4], hand7[5], hand7[6]} - core::rotate(q, off);
    if (!core::finite(core::Pose{q, p})) return r;
    r[0] = q.x, r[1] = q.y, r[2] = q.z, r[3] = q.w, r[4] = p.x, r[5] = p.y, r[6] = p.z;
    static ULONGLONG last;
    if (GetTickCount64() - last > 2000) {
        last = GetTickCount64();
        VRLOG("gadgets: targeting aimed by the %s hand (start offset %.2f %.2f %.2f)", gadgetRole ? "right" : "left", off.x, off.y, off.z);
    }
    return r;
}

// gadget_throw_velocity=1: the old physical throw, direction and speed from the gadget hand's own velocity.
void applyHandVelocity(uint8_t* mode) {
    uint8_t* holder = mode ? at<uint8_t*>(mode, kModeWeapon) : nullptr;
    if (!holder || !localOwner(holder)) return;
    const float nativeSpeed = at<float>(mode, 0x3c);
    if (!(nativeSpeed > 0.0f && nativeSpeed < 1e4f)) return;  // no valid launch parameters
    const int role = rightHanded(holder) ? 0 : 1;
    const auto& hs = xr::input().hand(role == 0 ? 1 : 0);
    float c7[7] = {};
    player_vr::controllerWorld(player_vr::localVrd(), role, c7);
    if (!(c7[0] || c7[1] || c7[2] || c7[3]) || !hs.active || !hs.velocityValid) return;
    const core::Quat handWorld = core::normalize({c7[0], c7[1], c7[2], c7[3]});
    const core::Quat gripXr = core::normalize({hs.grip.orientation.x, hs.grip.orientation.y, hs.grip.orientation.z, hs.grip.orientation.w});
    const core::Quat xrToWorld = core::multiply(handWorld, core::inverse(gripXr));  // the play space's anchor rotation
    float ws = 1.0f;
    if (auto* s = se4::build().at<float>(se4::gen::vr_fWorldScale); s && *s > 0.01f) ws = *s;
    const core::Vec3 v = core::rotate(xrToWorld, {hs.gripLinearVelocity.x, hs.gripLinearVelocity.y, hs.gripLinearVelocity.z});
    const float mps = std::sqrt(core::dot(v, v));
    if (!(mps > kThrowSpeed)) return;
    const core::Quat q = core::fromTo({0, 0, -1}, v * (1.0f / mps));
    if (!core::finite(core::Pose{q, {}})) return;
    const float speed = std::clamp(mps * g_gain / ws, nativeSpeed * 0.5f, kMaxSpeed);
    at<float>(mode, 0x20) = q.x, at<float>(mode, 0x24) = q.y, at<float>(mode, 0x28) = q.z, at<float>(mode, 0x2c) = q.w;
    at<float>(mode, 0x3c) = speed;
    VRLOG("gadgets: thrown with the hand's velocity at %.1f (native %.1f)", speed, nativeSpeed);
}

uintptr_t hookedActivate(uint8_t* mode) {
    // The activation re-runs the targeting (targeting vt+0x248) before its check: steer that pass by the aiming
    // hand too, or it judges (and launches along) the head's target.
    uint8_t* holder = mode ? at<uint8_t*>(mode, kModeWeapon) : nullptr;
    uint8_t* const prev = t_targeting;
    if (g_aimHand && holder)
        if (uint8_t* tm = targetOf(holder)) t_targeting = tm;  // any VR player's holder (local or remote)
    const uintptr_t r = realActivate(mode);
    t_targeting = prev;
    if (!holder || !localOwner(holder)) return r;
    if (!(r & 0xff)) {
        // Refused (target too close / invalid): the holder goes back to idle (0x140776af0) and can be thrown again.
        VRLOG("gadgets: launch activation refused (target too close or invalid) - throw again");
        if (holder == g_held.holder) g_held.act.begin(g_held.act.use());
        return r;
    }
    VRLOG("gadgets: launch activation (speed %.2f, lifetime %.2f)", at<float>(mode, 0x3c), at<float>(mode, 0x40));
    if (g_throwVelocity) applyHandVelocity(mode);
    return r;
}

using LaunchFn = void(__cdecl*)(uint8_t* mode);
LaunchFn realLaunch;
void hookedLaunch(uint8_t* mode) {
    if (uint8_t* holder = mode ? at<uint8_t*>(mode, kModeWeapon) : nullptr; holder && localOwner(holder))
        VRLOG("gadgets: launch (speed %.2f)", at<float>(mode, 0x3c));
    realLaunch(mode);
}

// Put-away: the weapons in the hands before the gadget came out, selected again through the weapon wheel's own
// commit (0x140471450, a network call). The engine remembers them itself only for a quick use (player +0x22ac
// right / +0x22b0 left, read from the code of its restore 0x140490bc0); for a gadget taken into the hand the mod
// remembers them (player vt+0x1148 / vt+0x1150, the current right / left weapon, as that restore reads them).
using IndexFn = int(__cdecl*)(uint8_t* player);
using SelectFn = void(__cdecl*)(uint8_t* player, int right, int left);
int g_savedRight = -1, g_savedLeft = -1;
void rememberWeapons(uint8_t* player) {
    void** vt = *reinterpret_cast<void***>(player);
    g_savedRight = reinterpret_cast<IndexFn>(vt[0x1148 / 8])(player);
    g_savedLeft = reinterpret_cast<IndexFn>(vt[0x1150 / 8])(player);
}
void putAway(uint8_t* player) {
    int right = g_savedRight, left = g_savedLeft;
    if (auto* fn = se4::build().at<uint8_t>(se4::gen::weapons_restore); fn && fn[6] == 0x83 && fn[7] == 0xB9) {
        const int32_t offR = *reinterpret_cast<int32_t*>(fn + 8);
        if (offR > 0x1000 && offR < 0x4000 && at<int32_t>(player, offR) != -1)
            right = at<int32_t>(player, offR), left = at<int32_t>(player, offR + 4);
    }
    auto select = reinterpret_cast<SelectFn>(se4::build().at<void>(se4::gen::select_weapons_net));
    if (!select || right == -1) return VRLOG("gadgets: left squeeze: no weapon to go back to (%d, %d)", right, left);
    select(player, right, left);
    VRLOG("gadgets: left squeeze puts the gadget away (weapons %d, %d)", right, left);
}

template <class F> bool hook(se4::gen::Sym s, F* detour, F** real, const char* what) {
    void* fn = se4::build().at<void>(s);
    if (!fn) return VRLOG("gadgets: no %s in this build", what), false;
    if (MH_CreateHook(fn, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(real)) != MH_OK || MH_EnableHook(fn) != MH_OK)
        return VRLOG("gadgets: %s hook FAILED", what), false;
    return true;
}
}  // namespace

void onTick(uint8_t* player) {
    if (!player) return;
    static ULONGLONG lastTick;
    const ULONGLONG now = GetTickCount64();
    if (now == lastTick) return;  // once per tick, from whichever local weapon steps first
    lastTick = now;
    if (!holderUp()) InterlockedExchange(&g_triggerTaken[0], 0), InterlockedExchange(&g_triggerTaken[1], 0);
    wheel_panel::onTick(player);  // wheel spot actions (Menu, Netricsa, Quick save/load)
    // The left squeeze takes the selected gadget into the hand (HUD command 9, the GadgetSelection path that brings
    // up the GadgetHolder without quick use), or puts it away again. Through the player's own HUD command (player
    // vt+0x11b0, a network call); the pad's Y is not used, its meaning depends on the saved gamepad layout.
    static bool pressed;
    const auto& hs = xr::input().hand(0);
    const float sq = hs.active ? hs.squeeze : 0.0f;
    const bool down = pressed ? sq > 0.35f : sq > 0.6f;
    if (down && !pressed && !hands::squeezeTaken(0)) {
        // With the holder up, HUD 9 would cycle to the next gadget (0x140495b40 -> 0x140496d80), and a weapon-wheel
        // tap (previous weapon) does not leave the holder: put it away with a weapon selection instead.
        if (holderUp()) putAway(player);
        else {
            rememberWeapons(player);
            hud(player, g_motion ? kHudSelect : kHudQuickUse);
            VRLOG("gadgets: left squeeze %s", g_motion ? "takes the gadget into the hand" : "quick use");
        }
    }
    pressed = down;
    // Right B: the native use / melee kick (desktop E), through the engine's own VR use path for the right hand,
    // so it does not depend on the saved pad layout (gamepad::kick is already off while a wheel is held).
    static bool kickWas;
    const bool kick = gamepad::kick();
    if (kick && !kickWas) VRLOG("gadgets: right B - native use"), player_vr::useHand(0);
    kickWas = kick;
    // While a weapon wheel is held (either trackpad press / Touch Y), the right A/B cycle the gadget ring through
    // the HUD's own next/previous commands (R12 s7) instead of pad buttons.
    static bool aWas, bWas;
    const auto& rh = xr::input().hand(1);
    const bool wheel = gamepad::wheelHeld();  // either hand's wheel
    const bool a = wheel && (rh.buttons & xr::BtnPrimary), b = wheel && (rh.buttons & xr::BtnSecondary);
    if (a && !aWas) hud(player, kHudNext), VRLOG("gadgets: next gadget");
    if (b && !bWas) hud(player, kHudPrevious), VRLOG("gadgets: previous gadget");
    aWas = a, bWas = b;
}

int takeBuzz() { return int(InterlockedExchange(&g_buzz, 0)) - 1; }
bool triggerTaken(int hand) { return (hand == 0 || hand == 1) && g_triggerTaken[hand] != 0; }

bool installAuthority() {
    // A desktop (mp_guards) server: no hands of its own, but it runs remote VR players' gadget targeting and launch.
    if (!se4::options::get("gadget_hand_aim", 1) || !se4::options::get("gadget_aim_hand", 1)) return false;
    const auto& b = se4::build();
    handleEntity = reinterpret_cast<HandleFn>(b.at<void>(se4::gen::handle_entity));
    if (!handleEntity || !b.off(se4::gen::CWeaponEntity_en_henOwner) || !b.off(se4::gen::CWeaponEntity_en_bRightHandWeapon) ||
        !b.off(se4::gen::CAOETargetingWeaponModeParams_wmp_vTrajectoryStartOffsetFirstPerson))
        return false;
    g_aimHand = hook(se4::gen::aoe_targeting_step, hookedTargeting, &realTargeting, "targeting step") &&
                hook(se4::gen::player_view, hookedView, &realView, "player view") &&
                hook(se4::gen::gadget_launch_activate, hookedActivate, &realActivate, "launch activation");
    VRLOG("gadgets: remote VR players' throws aimed by their hands here %d", int(g_aimHand));
    return g_aimHand;
}

bool install() {
    if (!se4::options::get("gadget_hand_aim", 1)) return false;
    g_motion = se4::options::get("gadget_motion", 1) != 0;
    g_aimHand = se4::options::get("gadget_aim_hand", 1) != 0;
    g_throwVelocity = se4::options::get("gadget_throw_velocity", 0) != 0;
    g_gain = std::clamp(se4::options::get("throw_gain", 200), 50, 1000) / 100.0f;
    const auto& b = se4::build();
    handleEntity = reinterpret_cast<HandleFn>(b.at<void>(se4::gen::handle_entity));
    holderReady = reinterpret_cast<ReadyFn>(b.at<void>(se4::gen::gadget_holder_ready));
    targetCheck = reinterpret_cast<CheckFn>(b.at<void>(se4::gen::aoe_target_check));
    if (!handleEntity || !b.off(se4::gen::CWeaponEntity_en_henOwner) || !b.off(se4::gen::CGadgetHolderWeaponEntity_en_pgipUsedGadget) ||
        !b.off(se4::gen::CGadgetItemParams_gip_bgutBotUsageType)) {
        VRLOG("gadgets: build lacks gadget symbols - not installed");
        return false;
    }
    if (!hook(se4::gen::gadget_holder_step, hookedHolderStep, &realHolderStep, "holder step")) return false;
    hook(se4::gen::gadget_holder_use, hookedHolderUse, &realHolderUse, "holder use");
    hook(se4::gen::gadget_launch_activate, hookedActivate, &realActivate, "launch activation");
    hook(se4::gen::gadget_launch, hookedLaunch, &realLaunch, "launch");
    if (g_aimHand && b.off(se4::gen::CAOETargetingWeaponModeParams_wmp_vTrajectoryStartOffsetFirstPerson))
        g_aimHand = hook(se4::gen::aoe_targeting_step, hookedTargeting, &realTargeting, "targeting step") &&
                    hook(se4::gen::player_view, hookedView, &realView, "player view");
    VRLOG("gadgets: in-hand use (%s), throws aimed by the other hand %d, hand-velocity throws %d (gain %.1f)",
          g_motion ? "motions" : "trigger", int(g_aimHand), int(g_throwVelocity), g_gain);
    return true;
}
}  // namespace gadgets
