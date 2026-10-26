#include "se4/turn.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/patch.hpp"
#include <windows.h>
#include <cmath>
#include <cstring>

namespace turn {
namespace {
struct TurnData {
    int32_t method, role, comfort;
    float axis;
};
TurnData* g_turn;
float g_speed = 150.0f;  // deg/s (smooth)
bool g_snap;

int32_t rel32To(const void* target, const uint8_t* next) {
    return int32_t(reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(next));
}
// `site` is a 6-byte `8B xx disp32` read: point its disp32 at `target`.
bool retargetRead(uint8_t* site, uint8_t modrm, const void* target) {
    if (!site || site[0] != 0x8B || site[1] != modrm) return false;
    uint8_t expect[6], now[6];
    memcpy(expect, site, 6);
    memcpy(now, site, 6);
    const int32_t d = rel32To(target, site + 6);
    memcpy(now + 2, &d, 4);
    return se4::patch::writeCode(site, expect, now, 6);
}
}  // namespace

bool install() {
    const auto& b = se4::build();
    uint8_t* p1 = b.at<uint8_t>(se4::gen::vr_rot_method_read);
    uint8_t* p2 = b.at<uint8_t>(se4::gen::vr_rot_role);
    uint8_t* p3 = b.at<uint8_t>(se4::gen::vr_rot_stick_r_load);
    uint8_t* p4 = b.at<uint8_t>(se4::gen::vr_comfort_mode_read);
    if (!p1 || !p2 || !p3) return VRLOG("turn: build lacks the turn sites - no native turning"), false;
    g_turn = reinterpret_cast<TurnData*>(se4::patch::allocNear(p3, sizeof(TurnData)));
    if (!g_turn) return VRLOG("turn: no memory near the image"), false;
    g_snap = se4::options::get("snap_turn", 0) != 0;
    g_speed = float(se4::options::get("turn_speed", 150));
    *g_turn = {g_snap ? 2 : 1, 2, se4::options::get("turn_vignette", 0) ? 1 : 0, 0.0f};
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
    VRLOG("turn: native turn patched (%s %.0f deg/s, right hand, vignette %d; method %d role %d comfort %d at %p %p %p %p)",
          g_snap ? "snap" : "smooth", g_speed, g_turn->comfort, int(ok1), int(ok2), int(ok4), static_cast<void*>(p1),
          static_cast<void*>(p2), static_cast<void*>(p3), static_cast<void*>(p4));
    return true;
}

void feed(float x, bool on) {
    if (!g_turn) return;
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
    const float* sens = b.at<float>(se4::gen::vr_fRotationSensitivity);
    const float* dzp = b.at<float>(se4::gen::vr_fContinuousMovementDeadzone);
    const float maxRate = 9.00009f * (sens && *sens > 0.01f ? *sens : 0.5f);
    const float dz = dzp && *dzp >= 0.0f && *dzp < 0.9f ? *dzp : 0.2f;
    const float r = std::fmin(1.0f, g_speed * 0.0174533f / maxRate);
    const float c = (std::fabs(x) - 0.15f) / 0.85f;
    g_turn->axis = std::copysign(dz + (1.0f - dz) * r * std::fmin(1.0f, c), x);
}

float yaw(const void* vrd) { return vrd ? *reinterpret_cast<const float*>(static_cast<const uint8_t*>(vrd) + 0x674) : 0.0f; }
}  // namespace turn
