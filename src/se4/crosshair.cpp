#include "se4/crosshair.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/patch.hpp"
#include "se4/player_vr.hpp"
#include "se4/vr_device.hpp"
#include <cstdint>

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
    return true;
}
}  // namespace crosshair
