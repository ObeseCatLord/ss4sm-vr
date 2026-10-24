#include "se4/crosshair.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/patch.hpp"
#include "se4/player_vr.hpp"
#include "se4/vr_device.hpp"
#include "core/math.hpp"
#include <cstdint>
#include <cstring>
#include <windows.h>

namespace crosshair {
namespace {
// Aim writer sites: RCX is the player being updated (R8 s4: `mov rcx, rsi` before both calls).
int __cdecl aimGate(uint8_t* player) {
    void* local = player_vr::localVrd();
    unsigned off = player_vr::vrdOffset();
    if (local && off && player && vrdev::installed() && *reinterpret_cast<void**>(player + off) == local) return 0;
    return 1;
}
// HUD element sites draw the local view's HUD; their RCX is not the player.
int __cdecl hudGate() { return player_vr::localVrd() && vrdev::installed() ? 0 : 1; }

// The crosshair's view (player vt+0xdf8, the head centre) composed with the eye being rendered, so each eye
// projects the barrel target from its own position and the crosshair gets the right depth (R8 addendum A2).
using ViewFn = float*(__cdecl*)(uint8_t* player, float* out, int flag);
float* __cdecl eyeView(uint8_t* player, float* out, int flag) {
    float* r = reinterpret_cast<ViewFn>((*reinterpret_cast<void***>(player))[0xdf8 / 8])(player, out, flag);
    static ULONGLONG last;
    static const bool diag = se4::options::get("crosshair_log", 0) != 0;
    if (diag && GetTickCount64() - last > 3000) {
        last = GetTickCount64();
        auto* show = se4::build().at<int32_t>(se4::gen::hud_bShowCrosshair);
        auto* opt = se4::build().at<int32_t>(se4::gen::prj_iGameOptionCrosshair);
        auto pred = reinterpret_cast<int(__cdecl*)(uint8_t*)>(se4::build().at<void>(se4::gen::crosshair_show));
        // Early-outs after the view call (R8 A6): puppet flags bit 13, wheel bit, weapon handle, +0x2568. SM's player
        // fields sit 8 bytes later, like its vrd.
        const int d = int(player_vr::vrdOffset()) - 0x25c8;
        VRLOG("crosshair: render reaches the view (eye %d, show predicate %d, hud_bShowCrosshair %d, flag13 %d, wheel bit %d, weapon %x, +0x2568 %llx)",
              vrdev::currentEye(), pred ? pred(player) : -1, show ? *show : -1, (*reinterpret_cast<uint32_t*>(player + 0xfa8 + d) >> 13) & 1,
              *reinterpret_cast<uint32_t*>(player + 0x269c + d) & 1, *reinterpret_cast<uint32_t*>(player + 0xf90 + d),
              static_cast<unsigned long long>(*reinterpret_cast<uint64_t*>(player + 0x2568 + d)));
        (void)opt;
        const float* hit = reinterpret_cast<const float*>(player + 0x2188 + d);
        VRLOG("crosshair:   aim hit %.2f %.2f %.2f dist %.2f, view %.2f %.2f %.2f", hit[0], hit[1], hit[2], hit[3], r ? r[4] : 0.f, r ? r[5] : 0.f,
              r ? r[6] : 0.f);
    }
    const unsigned off = player_vr::vrdOffset();
    float e[7];
    if (!r || !off || !player_vr::localVrd() || *reinterpret_cast<void**>(player + off) != player_vr::localVrd() ||
        !vrdev::eyeInHead(vrdev::currentEye(), e))
        return r;
    const core::Quat q{r[0], r[1], r[2], r[3]};
    const core::Vec3 p = core::Vec3{r[4], r[5], r[6]} + core::rotate(q, {e[4], e[5], e[6]});
    const core::Quat qe = core::multiply(q, core::Quat{e[0], e[1], e[2], e[3]});
    r[0] = qe.x, r[1] = qe.y, r[2] = qe.z, r[3] = qe.w, r[4] = p.x, r[5] = p.y, r[6] = p.z;
    return r;
}

// The show predicate (only reader of hud_bShowCrosshair) returns 0 for every player with vrd: the vanilla engine
// replaced the HUD crosshair by a laser sight in VR. NOP that test (`cmp qword [rbx+vrd],0; jne` at +0xa0) so the
// game's own crosshair is drawn, at the barrel target through the redirects below (R8 addendum A1).
bool showForVr() {
    auto* fn = se4::build().at<uint8_t>(se4::gen::crosshair_show);
    if (!fn) return VRLOG("crosshair: no show predicate in this build"), false;
    uint8_t* at = fn + 0xa0;
    const unsigned vrd = player_vr::vrdOffset() ? player_vr::vrdOffset() : *reinterpret_cast<uint32_t*>(at + 3);
    const uint8_t cmp[8] = {0x48, 0x83, 0xBB, uint8_t(vrd), uint8_t(vrd >> 8), 0, 0, 0};
    const uint8_t jne[6] = {0x0F, 0x85, 0xAF, 0, 0, 0}, nop6[6] = {0x66, 0x0F, 0x1F, 0x44, 0, 0};
    if (memcmp(at, cmp, 8) || (vrd != 0x25c8 && vrd != 0x25d0) || !se4::patch::writeCode(at + 8, jne, nop6, 6)) {
        VRLOG("crosshair: show predicate vrd test not found at %p - crosshair stays hidden in VR", static_cast<void*>(at));
        return false;
    }
    VRLOG("crosshair: show predicate %p no longer hides the crosshair for VR players (vrd +0x%x)", static_cast<void*>(fn), vrd);
    return true;
}
// Redirect the render's single view call (`call [rax+0xdf8]`, FF 90 F8 0D 00 00) to eyeView.
bool perEyeView(uint8_t* render) {
    const uint8_t call[6] = {0xFF, 0x90, 0xF8, 0x0D, 0, 0};
    uint8_t* site = nullptr;
    for (size_t i = 0; i + 6 < 0x1000 && !site; ++i)
        if (!memcmp(render + i, call, 6)) site = render + i;
    void* jump = site ? se4::patch::nearJump(site, reinterpret_cast<void*>(eyeView)) : nullptr;
    if (!jump) return VRLOG("crosshair: view call not found - crosshair without eye offset"), false;
    const intptr_t rel = static_cast<uint8_t*>(jump) - (site + 5);
    if (rel != int32_t(rel)) return false;
    uint8_t bytes[6] = {0xE8, 0, 0, 0, 0, 0x90};
    const int32_t r32 = int32_t(rel);
    memcpy(bytes + 1, &r32, 4);
    if (!se4::patch::writeCode(site, call, bytes, 6)) return false;
    VRLOG("crosshair: view call %p -> per-eye view", static_cast<void*>(site));
    return true;
}

struct Site {
    se4::gen::Sym fn;
    size_t len;
    int expected;
    void* gate;
    const char* what;
};
}  // namespace

bool install() {
    if (!se4::options::get("native_crosshair", 1)) {
        VRLOG("crosshair: native projected crosshair disabled by option");
        return false;
    }
    const Site sites[] = {
        {se4::gen::aim_writer, 0x1400, 2, reinterpret_cast<void*>(aimGate), "aim writer"},
        {se4::gen::crosshair_render, 0x1000, 2, reinterpret_cast<void*>(hudGate), "crosshair element"},
        {se4::gen::object_holder_render, 0x1200, 1, reinterpret_cast<void*>(hudGate), "object-holder element"},
    };
    // Verify every site in every function before changing any of them.
    uint8_t* found[3][4];
    for (int i = 0; i < 3; ++i) {
        auto* fn = se4::build().at<uint8_t>(sites[i].fn);
        int n = fn ? se4::patch::findReturnOneCalls(fn, sites[i].len, found[i], 4) : -1;
        if (n != sites[i].expected) {
            VRLOG("crosshair: %s has %d predicate calls, expected %d - not installed", sites[i].what, n, sites[i].expected);
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        void* jump = se4::patch::nearJump(found[i][0], sites[i].gate);
        for (int k = 0; k < sites[i].expected; ++k) {
            if (!jump || !se4::patch::retargetCall(found[i][k], jump)) {
                VRLOG("crosshair: retargeting %s call %p FAILED", sites[i].what, static_cast<void*>(found[i][k]));
                return false;
            }
            VRLOG("crosshair: %s predicate call %p -> VR gate", sites[i].what, static_cast<void*>(found[i][k]));
        }
    }
    if (showForVr()) perEyeView(se4::build().at<uint8_t>(se4::gen::crosshair_render));
    return true;
}
}  // namespace crosshair
