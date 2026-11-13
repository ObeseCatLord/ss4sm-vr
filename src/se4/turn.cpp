#include "se4/turn.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/patch.hpp"
#include "se4/player_vr.hpp"
#include <windows.h>
#include <climits>
#include <cmath>
#include <cstring>

namespace turn {
namespace {
struct TurnData {
    int32_t method, role, comfort;
    float axis;
    float sensitivity;  // read by the smooth-turn rate instead of vr_fRotationSensitivity (P5): turn_speed / 9.00009 rad/s
};
TurnData* g_turn;
bool g_installed;
bool g_sensRedirect;  // P5 in place
float g_speed = 150.0f;  // deg/s (smooth)
bool g_snap;
int g_hand = 2;          // 0 none, 1 left stick, 2 right stick, 3 either (the vr_iRotationController values)
bool g_vignette;
// The VR Controls menu's saved choices (gfxmenu::prepare, before install); options.txt snap_turn / turn_speed /
// turn_vignette win over them.
int g_startMethod = -1, g_startHand = -1, g_startSpeed = -1, g_startVignette = -1;

int32_t rel32To(const void* target, const uint8_t* next) {
    return int32_t(reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(next));
}
// `site` is a 6-byte `8B xx disp32` read: point its disp32 at `target`.
bool retargetRead(uint8_t* site, uint8_t modrm, const void* target) {
    if (!site || site[0] != 0x8B || site[1] != modrm) return false;
    uint8_t expect[6], now[6];
    memcpy(expect, site, 6);
    memcpy(now, site, 6);
    const int64_t dist64 = reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(site + 6);
    if (dist64 < INT32_MIN || dist64 > INT32_MAX) return false;
    const int32_t d = int32_t(dist64);
    memcpy(now + 2, &d, 4);
    return se4::patch::writeCode(site, expect, now, 6);
}
// `site` is an 8-byte SSE load/multiply from a rip-relative float (F3 0F 10|59 modrm(00 reg 101) disp32): retarget its
// disp32 to `target`.
bool retargetSse(uint8_t* site, const void* target) {
    if (!site || site[0] != 0xF3 || site[1] != 0x0F || (site[2] != 0x10 && site[2] != 0x59) || (site[3] & 0xC7) != 0x05) return false;
    uint8_t expect[8], now[8];
    memcpy(expect, site, 8);
    memcpy(now, site, 8);
    const int64_t d = reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(site + 8);
    if (d < INT32_MIN || d > INT32_MAX) return false;
    const int32_t d32 = int32_t(d);
    memcpy(now + 4, &d32, 4);
    return se4::patch::writeCode(site, expect, now, 8);
}
float sensitivityFor(float degPerSec) { return degPerSec * 0.0174533f / 9.00009f; }
}  // namespace

bool install() {
    const auto& b = se4::build();
    uint8_t* p1 = b.at<uint8_t>(se4::gen::vr_rot_method_read);
    uint8_t* p2 = b.at<uint8_t>(se4::gen::vr_rot_role);
    uint8_t* p3 = b.at<uint8_t>(se4::gen::vr_rot_stick_r_load);
    uint8_t* p4 = b.at<uint8_t>(se4::gen::vr_comfort_mode_read);
    if (!p1 || !p2 || !p3) return VRLOG("turn: build lacks the turn sites - no native turning"), false;
    g_turn = reinterpret_cast<TurnData*>(se4::patch::allocNearData(p3, sizeof(TurnData)));
    if (!g_turn) return VRLOG("turn: no memory near the image"), false;
    g_snap = se4::options::has("snap_turn") ? se4::options::get("snap_turn", 0) != 0 : g_startMethod == 2;
    g_speed = float(se4::options::has("turn_speed") ? se4::options::get("turn_speed", 150) : g_startSpeed > 0 ? g_startSpeed : 150);
    g_vignette = se4::options::has("turn_vignette") ? se4::options::get("turn_vignette", 0) != 0 : g_startVignette == 1;
    if (g_startHand >= 0) g_hand = g_startHand;
    *g_turn = {g_snap ? 2 : 1, 2, g_vignette ? 1 : 0, 0.0f, sensitivityFor(g_speed)};
    // P3: `mov rax,[rbx+0x10]; movss xmm8,[rax+0x14]` -> `movss xmm8,[rip+axis]; nop` (rax is dead after it).
    static const uint8_t kStick[10] = {0x48, 0x8B, 0x43, 0x10, 0xF3, 0x44, 0x0F, 0x10, 0x40, 0x14};
    uint8_t load[10] = {0xF3, 0x44, 0x0F, 0x10, 0x05, 0, 0, 0, 0, 0x90};
    const int32_t d = rel32To(&g_turn->axis, p3 + 9);
    memcpy(load + 5, &d, 4);
    // The stick load first: P1/P2 alone change nothing while the stale table holds 0.
    if (!se4::patch::writeCode(p3, kStick, load, sizeof load))
        return VRLOG("turn: stick load at %p not as expected - no native turning", static_cast<void*>(p3)), false;
    const bool ok1 = retargetRead(p1, 0x15, &g_turn->method), ok2 = retargetRead(p2, 0x05, &g_turn->role);
    const bool ok4 = p4 && retargetRead(p4, 0x05, &g_turn->comfort);
    // The VR Controls screen enables Rotation Sensitivity (smooth) or Rotation Angle (snap) from the same cvar (R33).
    uint8_t* p5 = b.at<uint8_t>(se4::gen::vrmenu_rot_method_read);
    const bool ok5 = p5 && retargetRead(p5, 0x0D, &g_turn->method);
    // P5: the smooth rate reads the mod's sensitivity, so turn_speed is not capped by the user's cvar (258 deg/s at 0.5).
    g_sensRedirect = retargetSse(b.at<uint8_t>(se4::gen::vr_rot_sens_read), &g_turn->sensitivity);
    static const char* const kHands[4] = {"no turning", "left stick", "right stick", "either stick"};
    g_installed = ok1 && ok2;
    VRLOG("turn: native turn patched (%s %.0f deg/s, %s, vignette %d; method %d role %d comfort %d menu %d speed %d at %p %p %p %p %p)",
          g_snap ? "snap" : "smooth", g_speed, kHands[g_hand & 3], g_turn->comfort, int(ok1), int(ok2), int(ok4), int(ok5), int(g_sensRedirect),
          static_cast<void*>(p1), static_cast<void*>(p2), static_cast<void*>(p3), static_cast<void*>(p4), static_cast<void*>(p5));
    return true;
}

bool installed() { return g_installed; }
void setStart(int method, int hand, int speed, int vignette) {
    g_startMethod = method, g_startHand = hand, g_startSpeed = speed, g_startVignette = vignette;
}
void setSnap(bool snap) {
    g_snap = snap;
    if (g_turn) g_turn->method = snap ? 2 : 1;
}
void setHand(int hand) { g_hand = hand < 0 || hand > 3 ? 2 : hand; }
void setSpeed(float degPerSec) {
    g_speed = degPerSec < 30 ? 30 : degPerSec > 720 ? 720 : degPerSec;
    if (g_turn) g_turn->sensitivity = sensitivityFor(g_speed);
}
void setVignette(bool on) {
    g_vignette = on;
    if (g_turn) g_turn->comfort = on ? 1 : 0;
}
bool snap() { return g_snap; }
int hand() { return g_hand; }
float speed() { return g_speed; }
bool vignette() { return g_vignette; }

void feed(float leftX, float rightX, bool on) {
    if (!g_turn) return;
    static ULONGLONG lastLog;
    if (g_turn->axis != 0.0f && GetTickCount64() - lastLog > 1000) {  // lab: native turning in use (yaw = vrd anchor)
        lastLog = GetTickCount64();
        VRLOG("turn: axis %.2f yaw %.3f (%s)", g_turn->axis, yaw(player_vr::localVrd()), g_snap ? "snap" : "smooth");
    }
    // The turning stick (Rotation Controller): right, left, whichever is pushed further, or none.
    const float x = g_hand == 2 ? rightX : g_hand == 1 ? leftX : g_hand == 3 ? (std::fabs(leftX) > std::fabs(rightX) ? leftX : rightX) : 0.0f;
    if (!on || !(std::fabs(x) > 0.15f)) {  // the mod's deadzone; 0 lets the engine's |x| <= 0.001 early-out apply
        g_turn->axis = 0.0f;
        return;
    }
    if (g_snap) {
        g_turn->axis = x;  // the engine snaps at |x| >= vr_fStrafeThreshold and re-arms below it
        return;
    }
    // Smooth: the engine turns at 9.00009 * vr_fRotationSensitivity * remap(|x|) rad/s, remap = (|x| - dz) / (1 - dz)
    // with dz = vr_fContinuousMovementDeadzone (R26 s3.2). Feed the value that gives turn_speed at full stick.
    const auto& b = se4::build();
    const float* sens = g_sensRedirect ? &g_turn->sensitivity : b.at<float>(se4::gen::vr_fRotationSensitivity);
    const float* dzp = b.at<float>(se4::gen::vr_fContinuousMovementDeadzone);
    const float maxRate = 9.00009f * (sens && *sens > 0.01f ? *sens : 0.5f);
    const float dz = dzp && *dzp >= 0.0f && *dzp < 0.9f ? *dzp : 0.2f;
    const float r = std::fmin(1.0f, g_speed * 0.0174533f / maxRate);
    const float c = (std::fabs(x) - 0.15f) / 0.85f;
    g_turn->axis = std::copysign(dz + (1.0f - dz) * r * std::fmin(1.0f, c), x);
}

float yaw(const void* vrd) { return vrd ? *reinterpret_cast<const float*>(static_cast<const uint8_t*>(vrd) + 0x674) : 0.0f; }
}  // namespace turn
