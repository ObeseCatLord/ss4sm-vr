#include "se4/ammo_counter.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/names.hpp"
#include "se4/options.hpp"
#include "se4/player_vr.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ammo {
namespace {
constexpr size_t kParams = 0xa8;    // CWeaponEntity::en_pwpWeaponParams
constexpr size_t kVrModel = 0xd8;   // CWeaponParams::wp_pmcVRModel (R14, R17): set on the weapons whose _VR mesh has AmmoCounter
constexpr size_t kAmmo = 0xa90;     // CWeaponEntity vt: the count the engine's VR counter shows (0x1402d4b70)
constexpr size_t kMaxAmmo = 0xa98;  // CWeaponEntity vt: its maximum (the low-ammo tenth)
constexpr size_t kPlacement = 0xa8; // CEntity vt: world placement (P7: quat, position)

// Where the counter sits on a weapon, in the weapon's frame (x right, y up, -z along the barrel), metres, and how far it
// tilts back towards the eyes. The weapon's origin is the hand (the VR weapon placement puts the model at the
// controller), so the default is just above and behind the grip, on the line of sight over the gun.
struct Spot {
    const char* file;  // weapon params file stem (Databases/Weapons/<stem>.ep), matched as a prefix
    float up, back, width, tiltDeg;
};
const Spot kDefault{"", 0.085f, 0.03f, 0.045f, 40.0f};
const Spot kSpots[] = {
    {"Pistol", 0.07f, 0.02f, 0.04f, 40.0f},
    {"Revolver", 0.075f, 0.02f, 0.04f, 40.0f},
    {"RocketLauncher", 0.13f, 0.05f, 0.05f, 40.0f},
    {"Cannon", 0.13f, 0.06f, 0.055f, 40.0f},
};

bool g_on, g_skipNative = true;
volatile LONG g_enabled = 1;  // VR Controls > Ammo Counters (runtime)
int g_start = -1;             // the menu's saved choice (gfxmenu::prepare), before install
float g_up, g_back, g_size = -1;
using IntFn = int(__cdecl*)(uint8_t*);
using PlacementFn = float*(__cdecl*)(uint8_t*, float*);

struct Slot {
    Hand h;
    ULONGLONG tick = 0;
    uint8_t* weapon = nullptr;
};
Slot g_slot[2];  // OpenXR hand index
SRWLOCK g_lock = SRWLOCK_INIT;

const Spot& spotFor(uint8_t* weapon) {
    const char* file = se4::names::weaponFile(weapon);
    const char* stem = file ? strrchr(file, '/') : nullptr;
    stem = stem ? stem + 1 : file;
    if (stem)
        for (const Spot& s : kSpots)
            if (!strncmp(stem, s.file, strlen(s.file))) return s;
    return kDefault;
}

core::Quat axisAngle(core::Vec3 axis, float rad) {
    const float s = std::sin(rad / 2);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(rad / 2)};
}
core::Pose fromP7(const float* p) { return {core::normalize({p[0], p[1], p[2], p[3]}), {p[4], p[5], p[6]}}; }
}  // namespace

void setStart(int on) { g_start = on; }
void setEnabled(bool on) {
    if (InterlockedExchange(&g_enabled, on ? 1 : 0) != LONG(on)) VRLOG("ammo: counters %s", on ? "on" : "off");
}
bool enabled() { return g_on && g_enabled; }

bool install() {
    g_on = se4::options::get("ammo_counter", 1) != 0;  // 0: no counters at all (not even in the menu)
    if (!g_on) return false;
    g_enabled = g_start == 0 ? 0 : 1;
    g_skipNative = se4::options::get("ammo_counter_native", 1) != 0;
    g_up = se4::options::get("ammo_counter_up_cm", 0) / 100.0f;
    g_back = se4::options::get("ammo_counter_back_cm", 0) / 100.0f;
    if (se4::options::has("ammo_counter_size_cm")) g_size = std::clamp(se4::options::get("ammo_counter_size_cm", 5), 1, 20) / 100.0f;
    VRLOG("ammo: on-weapon ammo counters on%s", g_skipNative ? " (weapons with the engine's own VR counter keep it)" : " (on every weapon)");
    return true;
}

void onWeaponStep(uint8_t* weapon, uint8_t* player, bool rightHand) {
    if (!g_on || !weapon || !player) return;
    void* vrd = player_vr::localVrd();
    uint8_t* params = *reinterpret_cast<uint8_t**>(weapon + kParams);
    if (!vrd || !params) return;
    const int xrHand = rightHand ? 1 : 0;
    void** vt = *reinterpret_cast<void***>(weapon);
    const int max = reinterpret_cast<IntFn>(vt[kMaxAmmo / 8])(weapon);
    const bool native = g_skipNative && *reinterpret_cast<void**>(params + kVrModel);
    Slot s;
    s.weapon = weapon;
    s.tick = GetTickCount64();
    if (max > 0 && !native && g_enabled) {
        s.h.count = reinterpret_cast<IntFn>(vt[kAmmo / 8])(weapon);
        s.h.max = max;
        float w7[7] = {}, hand7[7] = {};
        const float* pw = reinterpret_cast<PlacementFn>(vt[kPlacement / 8])(weapon, w7);
        player_vr::controllerWorld(vrd, rightHand ? 0 : 1, hand7);
        const core::Pose wp = fromP7(pw ? pw : w7), hp = fromP7(hand7);
        if (core::finite(wp) && core::finite(hp)) {
            float scale = 1.0f;
            if (auto* ws = se4::build().at<float>(se4::gen::vr_fWorldScale); ws && *ws > 0.01f) scale = 1.0f / *ws;
            const Spot& sp = spotFor(weapon);
            // weapon frame relative to the grip (world units -> metres), then the spot in the weapon frame
            const core::Quat gripToWeapon = core::multiply(core::inverse(hp.q), wp.q);
            const core::Vec3 weaponInGrip = core::rotate(core::inverse(hp.q), wp.p - hp.p) * (1.0f / scale);
            const core::Vec3 spot{0, sp.up + g_up, sp.back + g_back};
            s.h.gripOffset = weaponInGrip + core::rotate(gripToWeapon, spot);
            // the quad faces +z (back, towards the shooter), tilted up about the weapon's x axis towards the eyes
            s.h.gripToSpot = core::normalize(core::multiply(gripToWeapon, axisAngle({1, 0, 0}, -sp.tiltDeg * 3.14159265f / 180.0f)));
            s.h.width = g_size > 0 ? g_size : sp.width;
            const core::Quat spotQ = core::normalize(core::multiply(wp.q, axisAngle({1, 0, 0}, -sp.tiltDeg * 3.14159265f / 180.0f)));
            s.h.worldC = wp.p + core::rotate(wp.q, spot) * scale;
            s.h.worldR = core::rotate(spotQ, {s.h.width / 2, 0, 0}) * scale;
            s.h.worldU = core::rotate(spotQ, {0, s.h.width / 4, 0}) * scale;
            s.h.visible = true;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    // A hand's weapon reports every step; a second weapon (dual wield) has its own hand.
    g_slot[xrHand] = s;
    ReleaseSRWLockExclusive(&g_lock);
    static uint8_t* logged[2];
    if (logged[xrHand] != weapon) {
        logged[xrHand] = weapon;
        const char* file = se4::names::weaponFile(weapon);
        VRLOG("ammo: %s hand %s: %s (ammo %d of %d)", rightHand ? "right" : "left", file ? file : "?",
              s.h.visible ? "counter on the weapon" : max <= 0 ? "no ammo counter (no ammo)" : native ? "engine's own counter (VR model)" : "no counter (no placement)",
              s.h.count, max);
    }
}

volatile ULONGLONG g_sceneTick;
void noteSceneDraw() { g_sceneTick = GetTickCount64(); }
bool sceneDrawing() { return GetTickCount64() - g_sceneTick < 200; }

Hand hand(int xrHand) {
    Hand h;
    if (xrHand < 0 || xrHand > 1) return h;
    AcquireSRWLockShared(&g_lock);
    const Slot s = g_slot[xrHand];
    ReleaseSRWLockShared(&g_lock);
    if (GetTickCount64() - s.tick > 250) return h;  // holstered, switched away or dropped
    return s.h;
}
}  // namespace ammo
