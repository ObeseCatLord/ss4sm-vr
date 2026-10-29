// Physical melee for the knife and the generic melee weapons (docs/re/R12_melee_gadgets.md s2, s3, s5, s15). For
// players with CPlayerVRData the weapons' canned body/view arc is skipped; instead, after each step, while the
// hand moves faster than vr_fMinSwingToHitSpeed:
//   - the blade held in the hand (five points along it) is swept from its previous pose with the native segment
//     cast, with the weapon's own damage ray radius;
//   - rays from the head through the arc the blade tip travelled, out to the weapon's native swing radius, so a
//     swing covers the area in front of the player as the desktop arc does (option melee_arc, default 1);
// and hits apply damage exactly as the weapon's own swing test does (knife: DMT_CUT; generic: gmwp_dtDamageType). The engine applies health only on the authority, so the
// sweep can run on every machine; only the server's counts (server-side validation is the replicated hand
// pose itself). Option melee_physical (default 1).
#include "se4/melee.hpp"
#include "core/math.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/patch.hpp"
#include "se4/player_vr.hpp"
#include "xr/input.hpp"
#include "xr/runtime.hpp"
#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace melee {
namespace {
using StepFn = void(__cdecl*)(uint8_t* weapon, void* stepArg);
using HandleFn = void*(__cdecl*)(int32_t handle);
using CastFn = void(__cdecl*)(uint8_t* res, void* world, const float* from, const float* to, void* catA, void* catB);
using ResCtorFn = void*(__cdecl*)(uint8_t* res);
using ResDtorFn = void(__cdecl*)(uint8_t* member);
using DamageCtorFn = void*(__cdecl*)(uint8_t* dmg);
using ParamsFn = uint8_t**(__cdecl*)(uint8_t* weapon, void** ref);
using ReleaseFn = void(__cdecl*)(void* obj, void* owner);
using IdentFn = void**(__cdecl*)(void** out, const char* name);

StepFn realStep, realSwingTest, realGenericStep, realGenericSwingTest;
using HeadFn = void*(__cdecl*)(void* vrd, float* out);
HeadFn headWorld;
bool g_arc = true;
void* g_knifeStep;  // vtable entries of the two melee steps (the hooks patch the code, not the vtables)
void* g_genericStep;

// Per-class params (all builds agree; docs/re/builds/members.json).
struct Kind {
    size_t radius, damage, reach, type;  // type 0: fixed DMT_CUT
};
Kind kindOf(bool generic) {
    const auto& b = se4::build();
    if (generic)
        return {b.off(se4::gen::CGenericMeleeWeaponParams_gmwp_fDamageRayRadius), b.off(se4::gen::CGenericMeleeWeaponParams_gmwp_slDamage),
                b.off(se4::gen::CGenericMeleeWeaponParams_gmwp_fSwingRadius), b.off(se4::gen::CGenericMeleeWeaponParams_gmwp_dtDamageType)};
    return {b.off(se4::gen::CMacheteWeaponParams_mwp_fDamageRayRadius), b.off(se4::gen::CMacheteWeaponParams_mwp_slDamage),
            b.off(se4::gen::CMacheteWeaponParams_mwp_fSwingRadius), 0};
}
HandleFn handleEntity;
CastFn segmentCast;
ResCtorFn resCtor;
ResDtorFn resDtor;
DamageCtorFn damageCtor;
ParamsFn weaponParams;
ReleaseFn refRelease;
IdentFn identFn;
void* g_bullet;  // CName "bullet" (the knife's cast category)
float* g_minSpeed;

constexpr float kBladeStart = 0.04f, kBladeEnd = 0.45f;  // metres along the hand's -Z (blade held forward)
constexpr int kBladePoints = 5;
constexpr double kTargetCooldown = 0.35;                  // seconds before the same target can be hit again

struct Track {
    uint8_t* weapon = nullptr;
    core::Vec3 pt[kBladePoints]{};
    LARGE_INTEGER t{};
    int32_t hit[8] = {};
    double hitTime[8] = {};
    bool valid = false;
};
Track g_tracks[16];
LARGE_INTEGER g_freq;

template <class T> T& at(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

Track& track(uint8_t* weapon) {
    Track* free = nullptr;
    for (auto& t : g_tracks) {
        if (t.weapon == weapon) return t;
        if (!t.weapon && !free) free = &t;
    }
    Track& t = free ? *free : g_tracks[uintptr_t(weapon) % 16];
    t = {};
    t.weapon = weapon;
    return t;
}

uint8_t* ownerOf(uint8_t* weapon) { return static_cast<uint8_t*>(handleEntity(at<int32_t>(weapon, 0x98))); }
void* vrdOf(uint8_t* player) { return player ? at<void*>(player, player_vr::vrdOffset()) : nullptr; }

double seconds(LARGE_INTEGER a, LARGE_INTEGER b) { return double(b.QuadPart - a.QuadPart) / double(g_freq.QuadPart); }

void hookedSwingTest(uint8_t* weapon, void* stepArg) {
    if (vrdOf(ownerOf(weapon))) return;  // VR owners hit with the hand-held blade instead (no canned arc)
    realSwingTest(weapon, stepArg);
}
void hookedGenericSwingTest(uint8_t* weapon, void* stepArg) {
    if (vrdOf(ownerOf(weapon))) return;
    realGenericSwingTest(weapon, stepArg);
}

void applyHit(uint8_t* weapon, uint8_t* owner, uint8_t* target, uint8_t* res, void* stepArg, core::Vec3 dir, uint8_t* params, const Kind& k) {
    alignas(16) uint8_t dmg[0x100];
    memset(dmg, 0, sizeof dmg);
    damageCtor(dmg);
    using InflictorFn = void(__cdecl*)(uint8_t*, uint8_t*);
    (*reinterpret_cast<InflictorFn**>(owner))[0x3e8 / 8](owner, dmg);
    const auto& b = se4::build();
    at<uint32_t>(dmg, 0x78) |= 4;  // dmg_ulFlags, as the knife sets
    const int type = k.type ? at<int32_t>(params, k.type) : 2;  // the generic weapon's damage type, else DMT_CUT
    at<int32_t>(dmg, 0x64) = type;
    using CheckFn = int(__cdecl*)(uint8_t*, uint8_t*, int, uint64_t);
    if (!(*reinterpret_cast<CheckFn**>(target))[0x3f0 / 8](target, dmg, type, at<uint64_t>(res, 0x68))) return;
    using IndexFn = int(__cdecl*)(uint8_t*);
    at<int32_t>(dmg, 0x68) = (*reinterpret_cast<IndexFn**>(weapon))[0x730 / 8](weapon);
    at<int32_t>(dmg, 0x60) = at<int32_t>(params, k.damage);
    at<float>(dmg, 0x48) = dir.x, at<float>(dmg, 0x4c) = dir.y, at<float>(dmg, 0x50) = dir.z;
    memcpy(dmg + 0x54, res + 0x28, 12);                  // hit point
    at<uint64_t>(dmg, 0x80) = at<uint64_t>(res, 0x60);   // hull part
    at<uint64_t>(dmg, 0x88) = at<uint64_t>(res, 0x68);
    at<uint64_t>(dmg, 0x90) = at<uint64_t>(res, 0x50);   // borrowed reference, as the native swing test does
    using ReceiveFn = void(__cdecl*)(uint8_t*, void*, uint8_t*);
    (*reinterpret_cast<ReceiveFn**>(target))[0x400 / 8](target, stepArg, dmg);
}

void sweep(uint8_t* weapon, void* stepArg, bool generic) {
    uint8_t* owner = ownerOf(weapon);
    void* vrd = vrdOf(owner);
    if (!vrd) return;
    const auto& b = se4::build();
    const Kind k = kindOf(generic);
    const int role = at<int32_t>(weapon, b.off(se4::gen::CWeaponEntity_en_bRightHandWeapon)) ? 0 : 1;
    float p7[7];
    player_vr::controllerWorld(vrd, role, p7);
    if (p7[0] == 0 && p7[1] == 0 && p7[2] == 0 && p7[3] == 0) return;
    core::Pose hand{core::normalize({p7[0], p7[1], p7[2], p7[3]}), {p7[4], p7[5], p7[6]}};
    float scale = 1.0f;
    if (auto* ws = b.at<float>(se4::gen::vr_fWorldScale); ws && *ws > 0.01f) scale = 1.0f / *ws;
    core::Vec3 pt[kBladePoints];
    for (int i = 0; i < kBladePoints; ++i) {
        const float d = kBladeStart + (kBladeEnd - kBladeStart) * float(i) / float(kBladePoints - 1);
        pt[i] = hand.p + core::rotate(hand.q, {0, 0, -d * scale});
    }
    const core::Vec3 tip = pt[kBladePoints - 1];
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    Track& t = track(weapon);
    if (!t.valid) {
        memcpy(t.pt, pt, sizeof pt), t.t = now, t.valid = true;
        return;
    }
    double dt = seconds(t.t, now);
    if (dt <= 1e-4) return;
    const core::Vec3 prevTip = t.pt[kBladePoints - 1];
    core::Vec3 dv = tip - prevTip;
    float speed = std::sqrt(core::dot(dv, dv)) / float(dt) / scale;  // metres per second
    const float minSpeed = g_minSpeed && *g_minSpeed > 0.1f ? *g_minSpeed : 2.0f;
    if (speed >= minSpeed && dt < 0.25) {
        void* ref = nullptr;
        uint8_t** pp = weaponParams(weapon, &ref);
        uint8_t* params = pp ? *pp : nullptr;
        if (params) {
            alignas(16) uint8_t res[0x200];
            memset(res, 0, sizeof res);
            resCtor(res);
            at<int32_t>(res, 0x00) = at<int32_t>(weapon, 0x98);  // ignore the owner
            at<float>(res, 0x98) = at<float>(params, k.radius);
            core::Vec3 dir = dv * (1.0f / std::sqrt(core::dot(dv, dv)));
            // Segments: each blade point from its previous position, the blade itself now, and the arc from the head.
            core::Vec3 from[kBladePoints + 1 + 5], to[kBladePoints + 1 + 5];
            int n = 0;
            for (int i = 0; i < kBladePoints; ++i) from[n] = t.pt[i], to[n++] = pt[i];
            from[n] = pt[0], to[n++] = tip;
            float head7[7] = {};
            if (g_arc && headWorld) headWorld(vrd, head7);
            if (g_arc && headWorld && (head7[4] || head7[5] || head7[6])) {
                const core::Vec3 eye{head7[4], head7[5], head7[6]};
                float reach = at<float>(params, k.reach);
                if (!(reach > 0.3f && reach < 20.0f)) reach = 2.0f;  // metres, in world units below
                for (int j = 0; j < 5; ++j) {
                    const core::Vec3 p = prevTip + (tip - prevTip) * (float(j) / 4.0f);
                    core::Vec3 d = p - eye;
                    const float len = std::sqrt(core::dot(d, d));
                    if (!(len > 1e-3f)) continue;
                    const float r = std::fmax(reach * scale, len + 0.2f * scale);  // at least just past the blade
                    from[n] = eye, to[n++] = eye + d * (r / len);
                }
            }
            double nowS = double(now.QuadPart) / double(g_freq.QuadPart);
            for (int i = 0; i < n; ++i) {
                const float f[3] = {from[i].x, from[i].y, from[i].z}, e[3] = {to[i].x, to[i].y, to[i].z};
                segmentCast(res, at<void*>(weapon, 0x30), f, e, g_bullet, g_bullet);
                int32_t h = at<int32_t>(res, 0x08);
                auto* target = static_cast<uint8_t*>(h ? handleEntity(h) : nullptr);
                if (!target || target == owner) continue;
                bool recent = false;
                int slot = 0;
                for (int q = 0; q < 8; ++q) {
                    if (t.hit[q] == h && nowS - t.hitTime[q] < kTargetCooldown) recent = true;
                    if (t.hitTime[q] < t.hitTime[slot]) slot = q;
                }
                if (recent) continue;
                t.hit[slot] = h, t.hitTime[slot] = nowS;
                applyHit(weapon, owner, target, res, stepArg, dir, params, k);
                if (vrd == player_vr::localVrd() && xr::runtime().running())
                    xr::input().vibrate(xr::runtime().session(), role == 0 ? 1 : 0, 0.9f, 0.06f);
                static int logged;
                if (logged++ < 20)
                    VRLOG("melee: %s hit %p at %.1f m/s (hand %s, %s)", generic ? "generic" : "knife", static_cast<void*>(target), speed,
                          role ? "left" : "right", i < kBladePoints + 1 ? "blade" : "arc");
            }
            resDtor(res + 0xa8);
            if (void* r = at<void*>(res, 0x50)) refRelease(r, res + 0x50);
        }
        if (ref) refRelease(ref, &ref);
    }
    memcpy(t.pt, pt, sizeof pt), t.t = now;
}

// Logs when a melee weapon starts stepping (after a second without), for the lab.
void noteInHand(bool generic) {
    static ULONGLONG last[2];
    const ULONGLONG now = GetTickCount64();
    if (now - last[generic] > 1000) VRLOG("melee: %s in hand", generic ? "generic melee weapon" : "knife");
    last[generic] = now;
}

void hookedStep(uint8_t* weapon, void* stepArg) {
    realStep(weapon, stepArg);
    noteInHand(false);
    if (!g_bullet) identFn(&g_bullet, "bullet");  // CName interned on the simulation thread, like the engine's
    sweep(weapon, stepArg, false);
}
void hookedGenericStep(uint8_t* weapon, void* stepArg) {
    realGenericStep(weapon, stepArg);
    noteInHand(true);
    if (!g_bullet) identFn(&g_bullet, "bullet");
    sweep(weapon, stepArg, true);
}

// ---- Prompted melee by swing (docs/re/R30_prompted_melee_swing.md, R31 s5) ----
// The engine's melee kill (Use/Melee on an enemy that shows the "Melee" prompt) is a synchronized animation pair that
// moves and locks the player. For VR players it becomes a swing instead: while the player's PNF_PLAYER_MELEE_AVAILABLE
// net flag is set (the authority sets it each step from melee_find, the same test as the prompt and the button), a hand
// that sweeps through melee_find's target fast enough starts the game's own melee kill on it, the enemy half only
// (nativeVictim): the enemy plays its melee death and the animation's damage event kills it; the player is not placed,
// locked or animated. The button no longer starts the canned kill for VR players, and the prompt shows no button glyph.
// Options melee_prompt_swing (default 1), melee_prompt_speed (cm/s, default 150).
constexpr size_t kNetFlags = 0x7a4;                // CPuppetEntity::en_pnfNetFlags (both builds)
constexpr uint32_t kMeleeAvailable = 0x200000;     // PNF_PLAYER_MELEE_AVAILABLE
using FindFn = int(__cdecl*)(uint8_t* player, int one, uint8_t** target, const char** anim, int* index);
using StartFn = void(__cdecl*)(uint8_t* player, int32_t hTarget, const char* anim, int index);
using ToHandleFn = int32_t(__cdecl*)(void* entity);
using CtxFn = void*(__cdecl*)();
FindFn meleeFind;
StartFn realStart;
ToHandleFn toHandle;
uint8_t* g_findFn;
float g_swingSpeed = 1.5f;
bool g_promptDebug;  // lab: option melee_prompt_debug
struct SwingTrack {
    void* vrd = nullptr;
    core::Vec3 hand[2]{};
    core::Vec3 rel[2]{};  // hand minus head: the swing speed ignores walking and riding
    LARGE_INTEGER t{};
    bool valid = false;
    double lastKill = 0;
    bool armed = false;
};
SwingTrack g_swings[8];

SwingTrack& swingTrack(void* vrd) {
    for (auto& t : g_swings)
        if (t.vrd == vrd) return t;
    for (auto& t : g_swings)
        if (!t.vrd) return t = {}, t.vrd = vrd, t;
    SwingTrack& t = g_swings[uintptr_t(vrd) / 64 % 8];
    return t = {}, t.vrd = vrd, t;
}

// The game's own melee kill, the enemy half only (R31 s5). The player's melee state (0x1405d0c00) runs
// PlayerMeleeSelection and sends the target the same melee event with +0x34 set; the target answers with its melee-victim
// state: AI and movement suspended, its own melee animation, and that animation's damage event kills it
// (DMT_PLAYERMELEE, health restore, corpse). The player's state (placement, lock, player animation) is never created.
// Runs where the game owns the target (server or single player); the victim state replicates to the clients.
using RemoteFn = int(__cdecl*)(void* entity);
using PtrFn = void*(__cdecl*)(void*);
using EntryFn = uint8_t*(__cdecl*)(void* table, int index);
using SelectFn = int(__cdecl*)(void* sender, uint8_t* player, uint8_t* target, const char* hull, const char* weapon, const char** anim,
                               float* placement, uint32_t* showWeapon, int32_t* damage);
using EventFn = void(__cdecl*)(uint8_t* event);
using SendFn = int(__cdecl*)(uint8_t* entity, void* sender, uint8_t* event);
RemoteFn entityRemote;
CtxFn meleeCtx;
PtrFn weaponDb, weaponTable;
EntryFn weaponEntry;
SelectFn meleeSelect;
EventFn eventCtor, eventDtor;
SendFn eventSend;

enum class Victim { Started, NotAuthority, NoWeapon, NoAnimation, Refused };
Victim nativeVictim(uint8_t* player, uint8_t* target, const char* hull, int index, const char** animOut) {
    if (entityRemote(target)) return Victim::NotAuthority;
    void* ctx = meleeCtx();
    // The weapon's melee name as melee_start finds it: entry(table(db(player)), index) +0x98.
    void* db = weaponDb(player);
    void* table = db ? weaponTable(db) : nullptr;
    uint8_t* entry = table && index >= 0 ? weaponEntry(table, index) : nullptr;
    if (!entry) return Victim::NoWeapon;
    alignas(16) uint8_t ev[0x80] = {};
    eventCtor(ev);
    const char* empty = at<const char*>(ev, 0x20);  // the engine's empty string (the ctor's default)
    const char* weapon = at<const char*>(entry, 0x98);
    const char* anim = empty;
    float placement[16] = {};
    uint32_t show = 0;
    int32_t damage = 0;
    if (!meleeSelect(ctx, player, target, hull ? hull : empty, weapon, &anim, placement, &show, &damage) || !anim || !*anim) {
        eventDtor(ev);
        return Victim::NoAnimation;
    }
    *animOut = anim;
    at<int32_t>(ev, 0x1c) = 0;          // not a test
    at<const char*>(ev, 0x20) = anim;   // the melee animation (the victim's of the same name)
    at<const char*>(ev, 0x28) = weapon;
    at<int32_t>(ev, 0x30) = damage;     // 0 from the selection: the animation's damage event kills
    at<int32_t>(ev, 0x34) = 1;          // to the victim
    at<float>(ev, 0x38) = 0.0f;         // start now (the native delay is the player's walk to the placement)
    at<uint8_t*>(ev, 0x40) = player;
    eventSend(target, ctx, ev);
    const bool ok = at<int32_t>(ev, 0x18) != 0;
    eventDtor(ev);
    return ok ? Victim::Started : Victim::Refused;
}

// The use action's melee_start call (melee_find called just before it): the canned kill. Skipped for VR players,
// who kill by swinging; DoUseMeleeAttack (scripts) and desktop players keep it.
void __cdecl hookedStart(uint8_t* player, int32_t hTarget, const char* anim, int index) {
    const uint8_t* ret = static_cast<const uint8_t*>(__builtin_return_address(0));
    bool fromUse = false;
    for (int back = 8; back <= 0x60 && !fromUse; ++back) {
        const uint8_t* c = ret - back;
        if (c[0] != 0xE8) continue;
        int32_t rel;
        memcpy(&rel, c + 1, 4);
        fromUse = c + 5 + rel == g_findFn;
    }
    if (fromUse && vrdOf(player)) {
        static int logged;
        if (logged++ < 4) VRLOG("melee: Use/Melee kill (%s) not started for a VR player - swing at the enemy instead", anim ? anim : "?");
        return;
    }
    realStart(player, hTarget, anim, index);
}
}  // namespace

void onVrdTick(void** vrdp) {
    if (!meleeFind || !vrdp || !vrdp[0]) return;
    auto* player = static_cast<uint8_t*>(vrdp[0]);
    void* vrd = vrdp;
    SwingTrack& st = swingTrack(vrd);
    core::Vec3 hand[2], rel[2];
    float head7[7] = {};
    if (headWorld) headWorld(vrd, head7);
    const core::Vec3 eye{head7[4], head7[5], head7[6]};
    const bool haveHead = head7[4] || head7[5] || head7[6];
    for (int r = 0; r < 2; ++r) {  // role 0 right, 1 left
        float p7[7];
        player_vr::controllerWorld(vrd, r, p7);
        hand[r] = {p7[4], p7[5], p7[6]};
        rel[r] = hand[r] - eye;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const bool armed = (at<uint32_t>(player, kNetFlags) & kMeleeAvailable) != 0;
    if (g_promptDebug && vrd == player_vr::localVrd()) {  // lab: the finder's answer once a second
        static ULONGLONG last;
        if (GetTickCount64() - last > 1000) {
            last = GetTickCount64();
            uint8_t* t = nullptr;
            const char* an = nullptr;
            int ix = -1;
            const int ok = meleeFind(player, 1, &t, &an, &ix);
            VRLOG("melee: debug finder %d target %p, net flags %08x", ok, static_cast<void*>(t), at<uint32_t>(player, kNetFlags));
        }
    }
    if (armed != st.armed) {
        st.armed = armed;
        static int logged;
        if (logged++ < 40) VRLOG("melee: prompt %s for VR player %p", armed ? "shown (swing to melee)" : "gone", static_cast<void*>(player));
    }
    const double dt = st.valid ? seconds(st.t, now) : 0.0;
    const double nowS = double(now.QuadPart) / double(g_freq.QuadPart);
    if (armed && st.valid && dt > 1e-4 && dt < 0.25 && nowS - st.lastKill > 0.5) {
        const auto& b = se4::build();
        float scale = 1.0f;
        if (auto* ws = b.at<float>(se4::gen::vr_fWorldScale); ws && *ws > 0.01f) scale = 1.0f / *ws;
        uint8_t* target = nullptr;
        const char* anim = nullptr;
        int index = -1;
        if (!g_bullet) identFn(&g_bullet, "bullet");
        if (meleeFind(player, 1, &target, &anim, &index) && target) {
            for (int r = 0; r < 2; ++r) {
                const core::Vec3 dr = rel[r] - st.rel[r];
                const float rdist = std::sqrt(core::dot(dr, dr));
                const float speed = rdist / float(dt) / scale;
                if (speed < g_swingSpeed || !(rdist > 1e-5f)) continue;
                core::Vec3 dv = hand[r] - st.hand[r];
                float dist = std::sqrt(core::dot(dv, dv));
                if (!(dist > 1e-5f)) dv = dr, dist = rdist;
                const core::Vec3 dir = dv * (1.0f / dist);
                // The hand's path (pushed 25 cm on along the swing) and the line from the head through the hand.
                core::Vec3 from[2] = {st.hand[r], eye}, to[2] = {hand[r] + dir * (0.25f * scale), hand[r]};
                core::Vec3 he = hand[r] - eye;
                const float hl = std::sqrt(core::dot(he, he));
                if (hl > 1e-3f) to[1] = hand[r] + he * (0.3f * scale / hl);
                alignas(16) uint8_t res[0x200];
                memset(res, 0, sizeof res);
                resCtor(res);
                at<int32_t>(res, 0x00) = toHandle(player);
                at<float>(res, 0x98) = 0.15f * scale;
                bool killed = false;
                for (int i = 0; i < (haveHead ? 2 : 1) && !killed; ++i) {
                    const float f[3] = {from[i].x, from[i].y, from[i].z}, e[3] = {to[i].x, to[i].y, to[i].z};
                    segmentCast(res, at<void*>(player, 0x30), f, e, g_bullet, g_bullet);
                    const int32_t h = at<int32_t>(res, 0x08);
                    if (g_promptDebug)
                        VRLOG("melee: debug swing %s at %.1f m/s, cast %d hit %p (target %p)", r ? "left" : "right", speed, i, h ? handleEntity(h) : nullptr,
                              static_cast<void*>(target));
                    if (!h || handleEntity(h) != target) continue;
                    const char* victimAnim = "";
                    const Victim v = nativeVictim(player, target, anim, index, &victimAnim);
                    killed = v == Victim::Started;
                    if (v != Victim::NotAuthority)
                        VRLOG("melee: %s-hand swing at %.1f m/s (%s) into %p: %s %s", r ? "left" : "right", speed, i ? "head line" : "hand path",
                              static_cast<void*>(target),
                              killed                       ? "native melee death started:"
                              : v == Victim::NoWeapon      ? "no weapon entry for index"
                              : v == Victim::NoAnimation   ? "PlayerMeleeSelection gave no animation"
                                                           : "the target refused the melee event",
                              killed ? victimAnim : "");
                    else killed = true;  // a client: the server runs the kill from the replicated hands
                }
                resDtor(res + 0xa8);
                if (void* rr = at<void*>(res, 0x50)) refRelease(rr, res + 0x50);
                if (killed) {
                    st.lastKill = nowS;
                    if (vrd == player_vr::localVrd() && xr::runtime().running())
                        xr::input().vibrate(xr::runtime().session(), r == 0 ? 1 : 0, 1.0f, 0.12f);
                    break;
                }
            }
        }
    }
    st.hand[0] = hand[0], st.hand[1] = hand[1], st.rel[0] = rel[0], st.rel[1] = rel[1], st.t = now, st.valid = true;
}

namespace {
// The prompt "%1 Melee" gets "{plcmdUse}" (the button glyph) at melee_prompt_site+0xf; in VR it gets "" instead.
bool patchPromptGlyph() {
    auto* site = se4::build().at<uint8_t>(se4::gen::melee_prompt_site);
    if (!site || site[0xf] != 0x4C || site[0x10] != 0x8D || site[0x11] != 0x0D) return false;
    int32_t rel;
    memcpy(&rel, site + 0x12, 4);
    const char* glyph = reinterpret_cast<const char*>(site + 0x16 + rel);
    if (strcmp(glyph, "{plcmdUse}")) return false;
    auto* empty = reinterpret_cast<char*>(se4::patch::allocNear(site, 8));
    if (!empty) return false;
    empty[0] = 0;
    const intptr_t d = reinterpret_cast<intptr_t>(empty) - reinterpret_cast<intptr_t>(site + 0x16);
    if (d != int32_t(d)) return false;
    const int32_t nd = int32_t(d);
    uint8_t now[4];
    memcpy(now, &nd, 4);
    return se4::patch::writeCode(site + 0x12, site + 0x12, now, 4);
}

bool hook(void* target, void* detour, void** real, const char* what);

// melee_find returns "no target" at once for a player with CPlayerVRData (`cmp [r12+vrd], r14; jne out` near its
// start; the canned kills never worked in VR), so neither the prompt nor the flag ever came up in VR. The swing kill
// replaces the canned animation, so that one jump is removed: the finder (and with it the prompt and the
// PNF_PLAYER_MELEE_AVAILABLE flag) answers for VR players exactly as for desktop players.
bool patchFinderVrCheck(uint8_t* find) {
    const uint32_t vrd = player_vr::vrdOffset();
    for (uint8_t* p = find; p < find + 0x100; ++p) {
        if (p[0] != 0x4D || p[1] != 0x39 || p[2] != 0xB4 || p[3] != 0x24) continue;
        uint32_t disp;
        memcpy(&disp, p + 4, 4);
        if (disp != vrd || p[8] != 0x0F || p[9] != 0x85) continue;
        static const uint8_t nops[6] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
        uint8_t old[6];
        memcpy(old, p + 8, 6);
        return se4::patch::writeCode(p + 8, old, nops, 6);
    }
    return false;
}

// Swing kills on every VR peer (the authority's damage counts), the button and prompt changes for the local VR player.
void installPrompted() {
    if (!se4::options::get("melee_prompt_swing", 1)) return;
    const auto& b = se4::build();
    auto* find = b.at<uint8_t>(se4::gen::melee_find);
    auto* start = b.at<uint8_t>(se4::gen::melee_start);
    toHandle = reinterpret_cast<ToHandleFn>(b.at<void>(se4::gen::entity_to_handle));
    entityRemote = reinterpret_cast<RemoteFn>(b.at<void>(se4::gen::melee_entity_remote));
    meleeCtx = reinterpret_cast<CtxFn>(b.at<void>(se4::gen::melee_script_ctx));
    weaponDb = reinterpret_cast<PtrFn>(b.at<void>(se4::gen::melee_weapon_db));
    weaponTable = reinterpret_cast<PtrFn>(b.at<void>(se4::gen::melee_weapon_table));
    weaponEntry = reinterpret_cast<EntryFn>(b.at<void>(se4::gen::melee_weapon_entry));
    meleeSelect = reinterpret_cast<SelectFn>(b.at<void>(se4::gen::melee_select));
    eventCtor = reinterpret_cast<EventFn>(b.at<void>(se4::gen::melee_event_ctor));
    eventDtor = reinterpret_cast<EventFn>(b.at<void>(se4::gen::melee_event_dtor));
    eventSend = reinterpret_cast<SendFn>(b.at<void>(se4::gen::melee_event_send));
    if (!find || !start || !toHandle || !handleEntity || !segmentCast || !resCtor || !entityRemote || !meleeCtx || !weaponDb ||
        !weaponTable || !weaponEntry || !meleeSelect || !eventCtor || !eventDtor || !eventSend) {
        VRLOG("melee: build lacks the prompted-melee symbols - prompted melee stays the button");
        return;
    }
    g_findFn = find;
    g_swingSpeed = std::clamp(se4::options::get("melee_prompt_speed", 150), 30, 1000) / 100.0f;
    g_promptDebug = se4::options::get("melee_prompt_debug", 0) != 0;
    headWorld = reinterpret_cast<HeadFn>(b.at<void>(se4::gen::vrd_head_world));
    if (!player_vr::vrdOffset() || !patchFinderVrCheck(find)) {
        VRLOG("melee: the melee finder's VR check was not found - prompted melee stays off in VR");
        return;
    }
    if (se4::g_vrEnabled) {
        if (!hook(start, reinterpret_cast<void*>(hookedStart), reinterpret_cast<void**>(&realStart), "melee start")) return;
        VRLOG("melee: prompt without a button glyph %s", patchPromptGlyph() ? "on" : "FAILED (site does not match)");
    }
    meleeFind = reinterpret_cast<FindFn>(find);
    VRLOG("melee: prompted melee by swing on (hand speed %.1f m/s)", g_swingSpeed);
}

bool hook(void* target, void* detour, void** real, const char* what) {
    if (!target || MH_CreateHook(target, detour, real) != MH_OK || MH_EnableHook(target) != MH_OK) {
        VRLOG("melee: %s hook FAILED", what);
        return false;
    }
    return true;
}
}  // namespace

bool install() {
    const auto& b = se4::build();
    handleEntity = reinterpret_cast<HandleFn>(b.at<void>(se4::gen::handle_entity));
    segmentCast = reinterpret_cast<CastFn>(b.at<void>(se4::gen::segment_cast));
    resCtor = reinterpret_cast<ResCtorFn>(b.at<void>(se4::gen::cast_result_ctor));
    resDtor = reinterpret_cast<ResDtorFn>(b.at<void>(se4::gen::cast_result_dtor));
    damageCtor = reinterpret_cast<DamageCtorFn>(b.at<void>(se4::gen::damage_ctor));
    weaponParams = reinterpret_cast<ParamsFn>(b.at<void>(se4::gen::weapon_params));
    refRelease = reinterpret_cast<ReleaseFn>(b.at<void>(se4::gen::ref_release));
    identFn = reinterpret_cast<IdentFn>(b.at<void>(se4::gen::ident_ctor));
    g_minSpeed = b.at<float>(se4::gen::vr_fMinSwingToHitSpeed);
    QueryPerformanceFrequency(&g_freq);
    installPrompted();
    if (!se4::options::get("melee_physical", 1)) return false;
    auto* step = b.at<void>(se4::gen::knife_step);
    auto* swing = b.at<void>(se4::gen::knife_swing_test);
    if (!handleEntity || !segmentCast || !resCtor || !resDtor || !damageCtor || !weaponParams || !refRelease || !identFn || !step || !swing) {
        VRLOG("melee: build lacks melee symbols - physical melee off");
        return false;
    }
    QueryPerformanceFrequency(&g_freq);
    g_arc = se4::options::get("melee_arc", 1) != 0;
    headWorld = reinterpret_cast<HeadFn>(b.at<void>(se4::gen::vrd_head_world));
    g_knifeStep = step;
    g_genericStep = b.at<void>(se4::gen::generic_melee_step);
    if (auto* gswing = b.at<void>(se4::gen::generic_melee_swing_test); g_genericStep && gswing && b.off(se4::gen::CGenericMeleeWeaponParams_gmwp_slDamage))
        if (!hook(gswing, reinterpret_cast<void*>(hookedGenericSwingTest), reinterpret_cast<void**>(&realGenericSwingTest), "generic swing test") ||
            !hook(g_genericStep, reinterpret_cast<void*>(hookedGenericStep), reinterpret_cast<void**>(&realGenericStep), "generic step"))
            g_genericStep = nullptr;
    if (!hook(swing, reinterpret_cast<void*>(hookedSwingTest), reinterpret_cast<void**>(&realSwingTest), "knife swing test") ||
        !hook(step, reinterpret_cast<void*>(hookedStep), reinterpret_cast<void**>(&realStep), "knife step"))
        return false;
    VRLOG("melee: physical knife%s active (head arc %d)", g_genericStep ? " and generic melee" : "", int(g_arc));
    return true;
}

bool isMeleeWeapon(uint8_t* weapon) {
    if (!weapon) return false;
    void* stepFn = (*reinterpret_cast<void***>(weapon))[0x208 / 8];
    return stepFn && (stepFn == g_knifeStep || stepFn == g_genericStep);
}
}  // namespace melee
