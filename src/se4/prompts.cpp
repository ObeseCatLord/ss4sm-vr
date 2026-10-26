#include "se4/prompts.hpp"
#include "core/prompts.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/player_vr.hpp"
#include "se4/vr_device.hpp"
#include "xr/input.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cstdint>

namespace prompts {
namespace {
using MarkupFn = void*(__cdecl*)(uint8_t* self, void* out, int control);
using StrCtorFn = void*(__cdecl*)(void* str, const char* text);
MarkupFn realMarkup;
StrCtorFn strCtor;

void* __cdecl hookedMarkup(uint8_t* self, void* out, int control) {
    // The mod's virtual pad is XInput user 0 (self+0x20, R19 s2.1); only while the local player plays in VR.
    if (self && *reinterpret_cast<int32_t*>(self + 0x20) == 0 && vrdev::installed() && player_vr::localVrd()) {
        auto k = xr::input().hand(1).kind != xr::ControllerKind::Unknown ? xr::input().hand(1).kind : xr::input().hand(0).kind;
        char buf[64];
        if (const char* m = core::prompts::markup(core::controls::Kind(int(k)), control, vrdev::menuActive(), buf)) {
            strCtor(out, m);  // the caller's storage is uninitialised, as for the engine's own result
            return out;
        }
    }
    return realMarkup(self, out, control);
}
}  // namespace

bool install() {
    if (!se4::options::get("prompts_vr", 1)) return false;
    void* fn = se4::build().at<void>(se4::gen::xpad_control_markup);
    strCtor = reinterpret_cast<StrCtorFn>(se4::build().at<void>(se4::gen::cstring_ctor));
    if (!fn || !strCtor) return VRLOG("prompts: build lacks the pad markup symbols - Xbox prompts"), false;
    if (MH_CreateHook(fn, reinterpret_cast<void*>(hookedMarkup), reinterpret_cast<void**>(&realMarkup)) != MH_OK || MH_EnableHook(fn) != MH_OK)
        return VRLOG("prompts: pad markup hook FAILED"), false;
    VRLOG("prompts: button prompts show the VR controllers");
    return true;
}
}  // namespace prompts
