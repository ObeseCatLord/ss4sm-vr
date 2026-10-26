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

// In VR the controls a prompt names are the pad's: a prompt asked for another device (the keyboard and mouse become
// the last used device whenever the VR menu pointer moves the mouse) is resolved through the "GamepadX" router,
// which holds the pad bindings and forwards to the virtual pad (R19 s1.2-1.3).
using CommandFn = void*(__cdecl*)(void* out, void* bindings, const int32_t* device, uint64_t command);
CommandFn realCommand;
void* const* g_devices;  // the engine's input devices (array pointer) and count, from control_markup's own loads
const int32_t* g_deviceCount;
void* g_routerMarkup;
int routerIndex() {
    if (!g_devices || !g_deviceCount || !*g_devices) return -1;
    auto** devs = static_cast<void***>(*g_devices);
    for (int i = 0; i < *g_deviceCount && i < 64; ++i)
        if (devs[i] && (*reinterpret_cast<void***>(devs[i]))[0x38 / 8] == g_routerMarkup) return i;
    return -1;
}
bool g_log;
void logResult(const char* what, int dev, uint64_t command, void* out) {
    static uint64_t seen[64];
    static int n;
    if (!g_log || n >= 64) return;
    for (int i = 0; i < n; ++i)
        if (seen[i] == (command ^ uint64_t(uint32_t(dev)) << 48)) return;
    seen[n++] = command ^ uint64_t(uint32_t(dev)) << 48;
    auto readable = [](const void* p) { return p && reinterpret_cast<uintptr_t>(p) > 0x10000 && !IsBadReadPtr(p, 16); };
    const char* cmd = readable(reinterpret_cast<const void*>(command)) ? reinterpret_cast<const char*>(command) : "?";
    const char* text = out && readable(*static_cast<const void* const*>(out)) ? *static_cast<const char* const*>(out) : "?";
    VRLOG("prompts: %s device %d command %llx '%.40s' -> '%.80s'", what, dev, static_cast<unsigned long long>(command), cmd, text);
}
void* __cdecl hookedCommand(void* out, void* bindings, const int32_t* device, uint64_t command) {
    if (device && vrdev::installed() && player_vr::localVrd()) {
        const int router = routerIndex();
        if (router >= 0 && *device != router) {
            void* r = realCommand(out, bindings, &router, command);
            logResult("command (via the pad router)", *device, command, out);
            return r;
        }
    }
    void* r = realCommand(out, bindings, device, command);
    logResult("command", device ? *device : -1, command, out);
    return r;
}

// The last used device (pi+0xb0) decides which variant some prompt bars show (keyboard keys vs pad glyphs). The VR
// menu pointer moves the mouse, so in VR the pad router is kept as the last used device.
using SwitchFn = void(__cdecl*)(uint8_t* pi, void* a2, void* a3, void* a4);
SwitchFn realSwitch;
void __cdecl hookedSwitch(uint8_t* pi, void* a2, void* a3, void* a4) {
    realSwitch(pi, a2, a3, a4);
    if (pi && vrdev::installed() && player_vr::localVrd())
        if (const int router = routerIndex(); router >= 0) *reinterpret_cast<int32_t*>(pi + 0xb0) = router;
}

void* __cdecl hookedMarkup(uint8_t* self, void* out, int control) {
    // The mod's virtual pad is XInput user 0 (self+0x20, R19 s2.1); only while the local player plays in VR.
    if (self && *reinterpret_cast<int32_t*>(self + 0x20) == 0 && vrdev::installed() && player_vr::localVrd()) {
        auto k = xr::input().hand(1).kind != xr::ControllerKind::Unknown ? xr::input().hand(1).kind : xr::input().hand(0).kind;
        char buf[64];
        if (const char* m = core::prompts::markup(core::controls::Kind(int(k)), control, vrdev::menuActive(), buf)) {
            strCtor(out, m);  // the caller's storage is uninitialised, as for the engine's own result
            if (g_log) {
                static int n;
                if (n++ < 40) VRLOG("prompts: pad control %d -> '%s'", control, m);
            }
            return out;
        }
    }
    return realMarkup(self, out, control);
}
}  // namespace

bool install() {
    if (!se4::options::get("prompts_vr", 1)) return false;
    g_log = se4::options::get("prompts_log", 0) != 0;
    void* fn = se4::build().at<void>(se4::gen::xpad_control_markup);
    strCtor = reinterpret_cast<StrCtorFn>(se4::build().at<void>(se4::gen::cstring_ctor));
    if (!fn || !strCtor) return VRLOG("prompts: build lacks the pad markup symbols - Xbox prompts"), false;
    if (MH_CreateHook(fn, reinterpret_cast<void*>(hookedMarkup), reinterpret_cast<void**>(&realMarkup)) != MH_OK || MH_EnableHook(fn) != MH_OK)
        return VRLOG("prompts: pad markup hook FAILED"), false;
    // control_markup: `cmp eax,[rip+count]` then `mov rax,[rip+devices]` (R19 s1.1).
    auto* cm = se4::build().at<uint8_t>(se4::gen::control_markup);
    g_routerMarkup = se4::build().at<void>(se4::gen::router_control_markup);
    void* command = se4::build().at<void>(se4::gen::command_markup);
    for (int i = 0; cm && i < 0x40; ++i) {
        if (!g_deviceCount && cm[i] == 0x3B && cm[i + 1] == 0x05)
            g_deviceCount = reinterpret_cast<const int32_t*>(cm + i + 6 + *reinterpret_cast<int32_t*>(cm + i + 2));
        if (!g_devices && cm[i] == 0x48 && cm[i + 1] == 0x8B && cm[i + 2] == 0x05)
            g_devices = reinterpret_cast<void* const*>(cm + i + 7 + *reinterpret_cast<int32_t*>(cm + i + 3));
    }
    if (command && g_devices && g_deviceCount && g_routerMarkup &&
        (MH_CreateHook(command, reinterpret_cast<void*>(hookedCommand), reinterpret_cast<void**>(&realCommand)) != MH_OK ||
         MH_EnableHook(command) != MH_OK))
        VRLOG("prompts: command markup hook FAILED");
    if (void* sw = se4::build().at<void>(se4::gen::device_switch); sw && g_devices && g_routerMarkup &&
        (MH_CreateHook(sw, reinterpret_cast<void*>(hookedSwitch), reinterpret_cast<void**>(&realSwitch)) != MH_OK || MH_EnableHook(sw) != MH_OK))
        VRLOG("prompts: device switch hook FAILED");
    VRLOG("prompts: button prompts show the VR controllers (devices %p, count %p, router %p)", static_cast<const void*>(g_devices),
          static_cast<const void*>(g_deviceCount), g_routerMarkup);
    return true;
}
}  // namespace prompts
