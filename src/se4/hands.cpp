#include "se4/ammo_counter.hpp"
#include "se4/hands.hpp"
#include "core/math.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/names.hpp"
#include "se4/options.hpp"
#include "se4/player_vr.hpp"
#include "se4/arms.hpp"
#include "se4/gadgets.hpp"
#include "se4/net.hpp"
#include "core/wire.hpp"
#include "xr/input.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace hands {
namespace {
using StepFn = void(__cdecl*)(uint8_t* weapon, void* arg);
using HandleFn = uint8_t*(__cdecl*)(int32_t handle);
using BarrelFn = void(__cdecl*)(uint8_t* weapon, float* out, void* ctx, void* ident, int, int, int);
using CtxFn = void*(__cdecl*)();

template <class T> T& at(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

StepFn realStep;
HandleFn handleEntity;
BarrelFn barrelPlacement;
CtxFn barrelCtx;
using TimeFn = void*(__cdecl*)(void* entity, void* out);
TimeFn entityTime;
void**(__cdecl* g_identCtor)(void**, const char*);
void* g_gripIdent;
bool g_identReady;      // LeftHandGripEffector (R15 s4.2)
uint32_t g_fireBits;    // player fire-button byte (Sam4 0x2338, SM 0x2340), read from the weapon step's own code

// Weapon params / flags (R15 s4.1; same in SM).
constexpr size_t kParams = 0xa8;            // en_pwpWeaponParams
constexpr size_t kFlags = 0x168;            // en_ulWeaponFlags
constexpr uint32_t kGripped = 0x20;         // WF_GRIPPED_WITH_TWO_HANDS
constexpr uint32_t kCheckedInitialFire = 0x800;
constexpr size_t kTwoHanded = 0x1b4;        // wp_bTwoHanded
constexpr float kForegripAhead = 0.25f;     // m: the support hand's place ahead of the main hand along the barrel
constexpr float kForegripReach = 0.15f;     // m: how near it the off hand must be to grip
constexpr size_t kSnapDistance = 0x214;     // wp_fHandSnapMinDistance (< 0: the player's VR params default)
constexpr float kDefaultSnap = 0.2f;        // stand-in for CPlayerVRParams+0xcc (unreached)

struct OffHand {
    bool engaged = false;   // grip intent
    bool pressed = false;   // squeeze above the press threshold last tick
    bool inReach = false;
    bool gripping = false;
    uint8_t* weapon = nullptr;
};
OffHand g_off[2];  // by XR hand
volatile LONG g_taken[2];
volatile ULONGLONG g_takenTick[2];  // when the latch was last set: it lapses when no two-hand step renews it

// ---- lab probe (option hands_probe): the first-person arms skeleton and the held weapon's grip parameters ----
const char* const kArmsBones[] = {"Root", "ViewMover", "L_UpperArm", "R_UpperArm", "L_LowerArm", "R_LowerArm", "L_Hand", "R_Hand",
                                  "L_LowerArm_Twist", "R_LowerArm_Twist", "L_Thumb_01", "L_Thumb_02", "L_Thumb_03", "L_Index_01",
                                  "L_Index_02", "L_Index_03", "L_Middle_01", "L_Middle_02", "L_Middle_03", "L_Ring_01", "L_Ring_02",
                                  "L_Ring_03", "L_Pinky_01", "L_Pinky_02", "L_Pinky_03", "L_Weapon", "R_Thumb_01", "R_Thumb_02",
                                  "R_Thumb_03", "R_Index_01", "R_Index_02", "R_Index_03", "R_Middle_01", "R_Middle_02", "R_Middle_03",
                                  "R_Ring_01", "R_Ring_02", "R_Ring_03", "R_Pinky_01", "R_Pinky_02", "R_Pinky_03", "R_Weapon"};
constexpr int kArmsBoneCount = int(sizeof(kArmsBones) / sizeof(kArmsBones[0]));
void* g_armsIdent[kArmsBoneCount];
bool g_probe;

const char* armsName(void* ident) {
    for (int i = 0; i < kArmsBoneCount; ++i)
        if (g_armsIdent[i] == ident) return kArmsBones[i];
    return "?";
}

// Skeleton LOD 0 of a model instance (R9 s2, as src/se4/body.cpp reads it).
uint8_t* modelLod0(uint8_t* mi) {
    uint8_t* cfg = mi ? at<uint8_t*>(mi, 0x18) : nullptr;
    uint8_t* skel = cfg ? at<uint8_t*>(cfg, 0x68) : nullptr;
    return skel && at<int32_t>(skel, 0x60) > 0 ? at<uint8_t*>(skel, 0x68) : nullptr;
}

void probeWeapon(uint8_t* weapon, uint8_t* params) {
    VRLOG("hands probe: weapon %p vptr %p two-handed %d orientation %d snap %.3f single-arm anims %d", weapon, at<void*>(weapon, 0),
          at<int32_t>(params, kTwoHanded), at<int32_t>(params, 0x1bc), at<float>(params, kSnapDistance), at<int32_t>(params, 0x1c4));
    static bool dumped;
    uint8_t* lod = modelLod0(at<uint8_t*>(weapon, 0x90));  // en_pmiWeapon
    if (dumped || !lod) return;
    dumped = true;
    const int n = at<int32_t>(lod, 0x20);
    auto* map = at<uint8_t*>(lod, 0x28);
    VRLOG("hands probe: first-person skeleton, %d bones", n);
    for (int m = 0; m < n && m < 64; ++m) {
        void* ident = at<void*>(map, size_t(m) * 0x10);
        const int idx = at<int32_t>(map, size_t(m) * 0x10 + 8);
        if (idx < 0) continue;
        uint8_t* bone = at<uint8_t*>(lod, 0x10) + size_t(idx) * 0xf0;
        const float* q = &at<float>(bone, 0x48);
        const float* p = &at<float>(bone, 0x58);
        VRLOG("hands probe:   %-16s parent %-16s rest q %.3f %.3f %.3f %.3f p %.3f %.3f %.3f", armsName(ident), armsName(at<void*>(bone, 8)),
              q[0], q[1], q[2], q[3], p[0], p[1], p[2]);
    }
}

// The byte the grip test reads: the `test byte ptr [reg+disp32], reg8` of the step (R15 s4.2, both builds).
uint32_t findFireBits(const uint8_t* fn) {
    uint32_t found = 0;
    int hits = 0;
    for (int i = 0; i < 0xc00; ++i) {
        const uint8_t* p = fn + i;
        int o = (p[0] & 0xf0) == 0x40 ? 1 : 0;  // REX
        if (p[o] != 0x84 || (p[o + 1] >> 6) != 2 || (p[o + 1] & 7) == 4) continue;
        uint32_t disp = *reinterpret_cast<const uint32_t*>(p + o + 2);
        if (disp < 0x2000 || disp > 0x3000) continue;
        if (found && disp != found) return 0;
        found = disp;
        ++hits;
    }
    return hits >= 2 ? found : 0;
}

uint8_t* localOwner(uint8_t* weapon) {
    void* local = player_vr::localVrd();
    const unsigned vrdOff = player_vr::vrdOffset();
    if (!local || !vrdOff) return nullptr;
    uint8_t* owner = handleEntity(at<int32_t>(weapon, se4::build().off(se4::gen::CWeaponEntity_en_henOwner)));
    return owner && at<void*>(owner, vrdOff) == local ? owner : nullptr;
}

// SS4/SM content sets wp_bTwoHanded on no weapon (lab probe 2026-10-24), which leaves the native grip dormant. A
// weapon counts as two-handable when its foregrip (LeftHandGripEffector) sits at least 12 cm from the main hand;
// the flag is then raised only around the two engine functions that grip with it (this step and the weapon
// placement), so IsTwoHanded elsewhere (dual-wield rules) stays native.
struct TwoHandable {
    uint8_t* weapon;
    int state;  // 0 unknown, 1 yes, 2 no
};
TwoHandable g_two[16];
bool g_gripAll = true;

// Keyed by the weapon's params (one per weapon type): an entity address can be reused by another weapon.
int& twoHandState(uint8_t* weapon) {
    uint8_t* key = at<uint8_t*>(weapon, kParams);
    if (!key) key = weapon;
    for (auto& t : g_two)
        if (t.weapon == key) return t.state;
    static int next;
    TwoHandable& t = g_two[next++ % 16];
    t = {key, 0};
    return t.state;
}

bool oneHandedByName(const char* file) {
    static const char* const oneHanded[] = {"Pistol", "Revolver", "Knife", "Machete", "GenericMelee", "Gadget", "Hands", "Injector",
                                            "Sign", "Melee"};
    for (const char* k : oneHanded)
        if (file && strstr(file, k)) return true;
    return false;
}

bool decideTwoHandable(uint8_t* weapon, int mainRole) {
    int& st = twoHandState(weapon);
    if (st) return st == 1;
    float grip7[7] = {}, hand7[7] = {};
    barrelPlacement(weapon, grip7, barrelCtx(), g_gripIdent, 0, 0, 1);
    player_vr::controllerWorld(player_vr::localVrd(), mainRole, hand7);
    if (!(hand7[0] || hand7[1] || hand7[2] || hand7[3])) return false;
    const core::Vec3 d{grip7[4] - hand7[4], grip7[5] - hand7[5], grip7[6] - hand7[6]};
    float ws = 1.0f;
    if (auto* sc = se4::build().at<float>(se4::gen::vr_fWorldScale); sc && *sc > 0.01f) ws = *sc;
    const float dist = std::sqrt(core::dot(d, d)) * ws;
    // Two-handable by weapon (lab 2026-10-26: the grip attachment's distance does not tell, e.g. the assault rifle's sits
    // 0.11 m from the hand, the knife reports 0.50, the minigun has none; wp_bTwoHanded is 0 on every SS4 weapon).
    const char* file = se4::names::weaponFile(weapon);
    st = file ? (oneHandedByName(file) ? 2 : 1) : (dist >= 0.12f ? 1 : 2);
    const core::Vec3 local = core::rotate(core::inverse(core::normalize({hand7[0], hand7[1], hand7[2], hand7[3]})), d) * ws;
    const char* base = file ? strrchr(file, '/') : nullptr;
    uint8_t* params = at<uint8_t*>(weapon, kParams);
    VRLOG("hands: weapon %p (%s, native two-handed %d) foregrip %.2f m from the hand (hand frame %.3f %.3f %.3f): %s", weapon,
          base ? base + 1 : file ? file : "?", params ? at<int32_t>(params, kTwoHanded) : -1, dist, local.x, local.y, local.z,
          st == 1 ? "two-handable" : "one-handed");
    return st == 1;
}

// Raises wp_bTwoHanded (and a usable snap distance) for one engine call.
struct TwoHandScope {
    uint8_t* params = nullptr;
    int32_t savedFlag = 0;
    float savedSnap = 0;
    void raise(uint8_t* p) {
        params = p;
        savedFlag = at<int32_t>(p, kTwoHanded), savedSnap = at<float>(p, kSnapDistance);
        at<int32_t>(p, kTwoHanded) = 1;
        if (!(savedSnap >= 0.0f && savedSnap < 2.0f)) at<float>(p, kSnapDistance) = kDefaultSnap;
    }
    ~TwoHandScope() {
        if (params) at<int32_t>(params, kTwoHanded) = savedFlag, at<float>(params, kSnapDistance) = savedSnap;
    }
};

bool g_authority;  // remote VR players' two-hand grips are applied here (host, server, single player; never a client)

// A remote VR player's two-handed hold on the authority (review 2026-11-12): the client grips locally, but the weapon
// placement that aims its shots is built here. Same rules as the local hold: a right-hand two-handable weapon, the off
// hand free and at the foregrip, its squeeze (the relayed LeftGrip flag) held.
bool remoteTwoHand(uint8_t* weapon, void* arg) {
    if (!g_authority || net::clientSession() || !weapon) return false;
    const auto& b = se4::build();
    const unsigned vrdOff = player_vr::vrdOffset();
    uint8_t* owner = vrdOff ? handleEntity(at<int32_t>(weapon, b.off(se4::gen::CWeaponEntity_en_henOwner))) : nullptr;
    void* vrd = owner ? at<void*>(owner, vrdOff) : nullptr;
    uint8_t* params = at<uint8_t*>(weapon, kParams);
    if (!vrd || vrd == player_vr::localVrd() || !params || !at<int32_t>(weapon, b.off(se4::gen::CWeaponEntity_en_bRightHandWeapon)) ||
        !(at<uint32_t>(weapon, kFlags) & kCheckedInitialFire))
        return false;
    const size_t leftOff = b.off(se4::gen::CPlayerActorPuppetEntity_en_henCurrentLeftHandWeapon);
    const size_t idxOff = b.off(se4::gen::CPlayerActorPuppetEntity_en_iPlayerIndex);
    if (!leftOff || !idxOff || at<int32_t>(owner, leftOff) != 0) return false;
    const bool native = at<int32_t>(params, kTwoHanded) != 0;
    const char* file = se4::names::weaponFile(weapon);
    if (!native && (!g_gripAll || !file || oneHandedByName(file))) return false;
    core::wire::PosePacket pkt;
    const bool squeeze = net::remotePose(at<int32_t>(owner, idxOff), pkt) && (pkt.flags & core::wire::LeftGrip);
    float off7[7] = {}, main7[7] = {};
    player_vr::controllerWorld(vrd, 1, off7);
    player_vr::controllerWorld(vrd, 0, main7);
    if (!(main7[0] || main7[1] || main7[2] || main7[3]) || !(off7[0] || off7[1] || off7[2] || off7[3])) return false;
    float wsc = 1.0f;
    if (auto* sc = b.at<float>(se4::gen::vr_fWorldScale); sc && *sc > 0.01f) wsc = *sc;
    const core::Vec3 gripP = core::Vec3{main7[4], main7[5], main7[6]} +
                             core::rotate(core::normalize({main7[0], main7[1], main7[2], main7[3]}), {0, 0, -kForegripAhead / wsc});
    const core::Vec3 d = gripP - core::Vec3{off7[4], off7[5], off7[6]};
    float snap = at<float>(params, kSnapDistance);
    if (!(snap >= 0.0f && snap < 2.0f)) snap = kDefaultSnap;
    snap = std::fmax(snap, kForegripReach / wsc);
    const bool engaged = squeeze && core::dot(d, d) <= snap * snap;
    uint8_t& bits = at<uint8_t>(owner, g_fireBits);
    const uint8_t mask = 2;  // a right-hand weapon: the left hand's fire bit (R15 s4.2)
    const uint8_t saved = bits;
    bits = engaged ? uint8_t(saved | mask) : uint8_t(saved & ~mask);
    {
        TwoHandScope th;
        if (!native) th.raise(params);
        realStep(weapon, arg);
    }
    bits = uint8_t((bits & ~mask) | (saved & mask));
    if (engaged && !(at<uint32_t>(weapon, kFlags) & kGripped) && entityTime) {  // as the local grab does
        memcpy(static_cast<uint8_t*>(weapon) + 0x26c, main7, sizeof main7);
        uint64_t t = 0;
        if (auto* pt = static_cast<uint64_t*>(entityTime(weapon, &t))) at<uint64_t>(weapon, 0x288) = *pt;
        at<uint32_t>(weapon, kFlags) |= kGripped;
    }
    return true;
}

void hookedStep(uint8_t* weapon, void* arg) {
    if (!g_identReady) {
        g_identReady = true;
        // IDENTs are interned on the simulation thread, like the engine's own callers (not at attach time).
        g_identCtor(&g_gripIdent, "LeftHandGripEffector");
        if (g_probe)
            for (int i = 0; i < kArmsBoneCount; ++i) g_identCtor(&g_armsIdent[i], kArmsBones[i]);
    }
    uint8_t* player = weapon ? localOwner(weapon) : nullptr;
    if (!player && remoteTwoHand(weapon, arg)) return;
    uint8_t* params = player ? at<uint8_t*>(weapon, kParams) : nullptr;
    if (player) gadgets::onTick(player);  // once per tick (it keeps its own tick stamp)
    static uint8_t* lastProbed;
    if (g_probe && params && weapon != lastProbed) {
        lastProbed = weapon;
        probeWeapon(weapon, params);
    }
    const bool rightHand = params && at<int32_t>(weapon, se4::build().off(se4::gen::CWeaponEntity_en_bRightHandWeapon)) != 0;
    if (params) {  // the local player's weapon per hand, logged on change (lab tests select by name)
        static uint8_t* inHand[2];
        if (inHand[rightHand] != weapon) {
            inHand[rightHand] = weapon;
            const char* file = se4::names::weaponFile(weapon);
            VRLOG("hands: %s hand weapon %s", rightHand ? "right" : "left", file ? file : "?");
        }
    }
    if (params) ammo::onWeaponStep(weapon, player, rightHand);  // on-weapon ammo counter (src/se4/ammo_counter.hpp)
    const bool native = params && at<int32_t>(params, kTwoHanded) != 0;
    // Only a right-hand weapon takes the left hand as its support hand (a left weapon is the dual-wield one).
    // The off hand must be free: no second weapon in it (dual wielding).
    const size_t leftOff = se4::build().off(se4::gen::CPlayerActorPuppetEntity_en_henCurrentLeftHandWeapon);
    const bool offFree = !player || !leftOff || at<int32_t>(player, leftOff) == 0;
    const bool twoHand = params && offFree && (native || (g_gripAll && rightHand && decideTwoHandable(weapon, 0)));
    {
        const int offXr = rightHand ? 0 : 1;
        const auto& hs = xr::input().hand(offXr);
        static ULONGLONG last;
        if (params && hs.squeeze > 0.5f && GetTickCount64() - last > 1000) {
            last = GetTickCount64();
            VRLOG("hands: squeeze on weapon %p (two-hand %d native %d flags %x)", weapon, int(twoHand), int(native), at<uint32_t>(weapon, kFlags));
        }
    }
    if (!params || !twoHand || !(at<uint32_t>(weapon, kFlags) & kCheckedInitialFire)) {
        if (params && rightHand) InterlockedExchange(&g_taken[0], 0);  // the left hand is not on this weapon
        realStep(weapon, arg);
        if (params) arms::onWeaponStep(weapon, at<int32_t>(weapon, se4::build().off(se4::gen::CWeaponEntity_en_bRightHandWeapon)) != 0, false);
        return;
    }
    const auto& b = se4::build();
    const bool right = at<int32_t>(weapon, b.off(se4::gen::CWeaponEntity_en_bRightHandWeapon)) != 0;
    const int offXr = right ? 0 : 1, offRole = right ? 1 : 0;  // engine role 0 right, 1 left
    OffHand& o = g_off[offXr];
    if (o.weapon != weapon) o = {}, o.weapon = weapon;

    // Reach: the off controller against the weapon's grip attachment, with the weapon's snap distance.
    float grip7[7] = {}, off7[7] = {}, main7[7] = {};
    barrelPlacement(weapon, grip7, barrelCtx(), g_gripIdent, 0, 0, 1);
    player_vr::controllerWorld(player_vr::localVrd(), offRole, off7);
    player_vr::controllerWorld(player_vr::localVrd(), right ? 0 : 1, main7);
    // The foregrip for reach: 25 cm ahead of the main hand along the barrel (-Z). The models' LeftHandGripEffector is
    // not a foregrip in VR (lab 2026-10-26: 0.10 m to the side of the hand on the assault rifle, missing on several
    // weapons); the native two-hand aim only needs the two hands.
    float wsc = 1.0f;
    if (auto* sc = se4::build().at<float>(se4::gen::vr_fWorldScale); sc && *sc > 0.01f) wsc = *sc;
    const core::Vec3 gripP = core::Vec3{main7[4], main7[5], main7[6]} +
                             core::rotate(core::normalize({main7[0], main7[1], main7[2], main7[3]}), {0, 0, -kForegripAhead / wsc});
    (void)grip7;
    const core::Vec3 d = gripP - core::Vec3{off7[4], off7[5], off7[6]};
    float snap = at<float>(params, kSnapDistance);
    if (!(snap >= 0.0f && snap < 2.0f)) snap = kDefaultSnap;
    snap = std::fmax(snap, kForegripReach / wsc);
    o.inReach = core::dot(d, d) <= snap * snap;
    {
        static ULONGLONG last;
        if (xr::input().hand(offXr).squeeze > 0.5f && GetTickCount64() - last > 1000) {
            last = GetTickCount64();
            VRLOG("hands:   off hand %.3f from the foregrip (snap %.3f), in reach %d", std::sqrt(core::dot(d, d)), snap, int(o.inReach));
        }
    }

    const auto& hs = xr::input().hand(offXr);
    const float s = hs.active ? hs.squeeze : 0.0f;
    const bool press = s > 0.6f, edge = press && !o.pressed;
    o.pressed = press;
    if (!o.engaged && edge && o.inReach) o.engaged = true;
    else if (o.engaged && s < 0.35f) o.engaged = false;

    const uint8_t mask = right ? 2 : 1;  // the other hand's fire bit (R15 s4.2)
    uint8_t& bits = at<uint8_t>(player, g_fireBits);
    const uint8_t saved = bits;
    bits = o.engaged ? uint8_t(saved | mask) : uint8_t(saved & ~mask);
    {
        TwoHandScope th;
        if (!native) th.raise(params);
        realStep(weapon, arg);
    }
    bits = uint8_t((bits & ~mask) | (saved & mask));  // only the bit raised here; keep what the step changed

    bool gripping = (at<uint32_t>(weapon, kFlags) & kGripped) != 0;
    if (o.engaged && !gripping && o.inReach && entityTime) {
        // The native grab test places the grip attachment through a weapon virtual (vt+0x6d8) that finds nothing
        // on SS4 weapons (lab 2026-10-24), so it never grabs. Grab exactly as it would (0x1402df227..0x1402df25d):
        // keep the one-hand pose for the blend (+0x26c P7), stamp the time (+0x288), set the flag.
        float main7[7] = {};
        player_vr::controllerWorld(player_vr::localVrd(), right ? 0 : 1, main7);
        memcpy(static_cast<uint8_t*>(weapon) + 0x26c, main7, sizeof main7);
        uint64_t t = 0;
        if (auto* pt = static_cast<uint64_t*>(entityTime(weapon, &t))) at<uint64_t>(weapon, 0x288) = *pt;
        at<uint32_t>(weapon, kFlags) |= kGripped;
        gripping = true;
    }
    if (o.engaged && !gripping && !o.gripping) o.engaged = false;  // out of reach: wait for a new press
    if (gripping != o.gripping) VRLOG("hands: %s hand %s the two-handed weapon %p", offXr ? "right" : "left", gripping ? "grips" : "releases", weapon);
    o.gripping = gripping;
    InterlockedExchange(&g_taken[offXr], (o.inReach || gripping) ? 1 : 0);
    g_takenTick[offXr] = GetTickCount64();
    arms::onWeaponStep(weapon, right, gripping);
}
}  // namespace

bool raiseTwoHanded(uint8_t* weapon, uint8_t* params, int32_t& savedFlag, float& savedSnap) {
    if (!weapon || !params || at<int32_t>(params, kTwoHanded) || !(at<uint32_t>(weapon, kFlags) & kGripped)) return false;
    savedFlag = at<int32_t>(params, kTwoHanded), savedSnap = at<float>(params, kSnapDistance);
    at<int32_t>(params, kTwoHanded) = 1;
    return true;
}
void restoreTwoHanded(uint8_t* params, int32_t savedFlag, float savedSnap) {
    at<int32_t>(params, kTwoHanded) = savedFlag, at<float>(params, kSnapDistance) = savedSnap;
}

// Lapses 250 ms after the last two-hand step (weapon switched, holstered, dual wield, vehicle: no step renews it).
bool squeezeTaken(int hand) { return (hand == 0 || hand == 1) && g_taken[hand] != 0 && GetTickCount64() - g_takenTick[hand] < 250; }

bool install(bool localVr) {
    if (!se4::options::get("grip_squeeze", 1)) return false;
    g_authority = se4::options::get("mp_two_hand", 1) != 0;
    const auto& b = se4::build();
    auto* step = b.at<uint8_t>(se4::gen::weapon_step);
    handleEntity = reinterpret_cast<HandleFn>(b.at<void>(se4::gen::handle_entity));
    barrelPlacement = reinterpret_cast<BarrelFn>(b.at<void>(se4::gen::barrel_placement));
    barrelCtx = reinterpret_cast<CtxFn>(b.at<void>(se4::gen::barrel_ctx));
    auto identCtor = reinterpret_cast<void**(__cdecl*)(void**, const char*)>(b.at<void>(se4::gen::ident_ctor));
    if (!step || !handleEntity || !barrelPlacement || !barrelCtx || !identCtor || !b.off(se4::gen::CWeaponEntity_en_henOwner)) {
        VRLOG("hands: build lacks grip symbols - not installed");
        return false;
    }
    g_fireBits = findFireBits(step);
    if (!g_fireBits) {
        VRLOG("hands: fire-button field not found in the weapon step - not installed");
        return false;
    }
    g_identCtor = identCtor;
    g_probe = se4::options::get("hands_probe", 0) != 0;
    g_gripAll = se4::options::get("grip_all", 1) != 0;
    entityTime = reinterpret_cast<TimeFn>(b.at<void>(se4::gen::entity_time));
    if (MH_CreateHook(step, reinterpret_cast<void*>(hookedStep), reinterpret_cast<void**>(&realStep)) != MH_OK || MH_EnableHook(step) != MH_OK) {
        VRLOG("hands: weapon step hook FAILED");
        return false;
    }
    VRLOG("hands: squeeze grips two-handed weapons (fire bits at player+0x%x)%s%s", g_fireBits, localVr ? "" : " - remote VR players only",
          g_authority ? ", remote VR players' grips applied where this machine is the authority" : "");
    return true;
}
}  // namespace hands
