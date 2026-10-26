#include "se4/gfx_menu.hpp"
#include "core/upscaler_menu.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "up/integrate.hpp"
#include "up/upscaler.hpp"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace gfxmenu {
namespace {
using core::MenuUpscaler;
// Layouts (R28 s1.1, s1.2; identical in Sam4 616154 and SamSM 628492).
constexpr size_t kVtInit = 0x120, kVtStep = 0x178;  // CMSGraphicsOptions vtable slots: gfxopt_init, gfxopt_step
constexpr size_t kScreenFilter = 0x1f0;             // CMSGraphicsOptions::ms_pmwRescalingFilter (reflected), CMultichoiceWidget*
constexpr size_t kWidgetFlags = 0x10;               // bit 0x20 = hidden
constexpr uint32_t kHidden = 0x20;
constexpr size_t kWidgetLinks = 0x50, kWidgetLinkCount = 0x58;  // CWidgetCvarLink* array, int count
constexpr size_t kWidgetLabelWidth = 0x1d8;  // label cache: rebuilt while < 0 (the ctor sets -1)
constexpr size_t kLinkColl = 0x10;           // CWidgetCvarLink: +0 CString cvar name, +0x10 option collection
constexpr size_t kOptStride = 0x30, kOptValue = 0x10;  // collection {options*, int count}; option +0x10 CString value
constexpr int kStockOptions = 6;                       // ren_iRescalingFilter 0..5
const char* const kCvar = "ren_iRescalingFilter";

using PassFn = uintptr_t(__cdecl*)(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4);
using OptAddFn = void(__cdecl*)(void* coll, const void* label, const void* value);  // copies both CStrings
using StrCtorFn = void*(__cdecl*)(void* str, const char* text);
using StrDtorFn = void(__cdecl*)(void* str);
using StrAssignFn = void(__cdecl*)(void* str, const char* text);
PassFn realInit, realStep, realGet, realSet, realReset;
OptAddFn optAdd;
StrCtorFn strCtor;
StrDtorFn strDtor;
StrAssignFn strAssign;
uint8_t *g_init, *g_step, *g_get, *g_set, *g_reset;
int32_t* g_cvarValue;  // ren_iRescalingFilter, read for logging only
bool g_prepared, g_installed;
std::atomic<int> g_choice{0};  // MenuUpscaler shown in the list and requested from the upscaler
wchar_t g_statePath[MAX_PATH];

up::Kind kindOf(MenuUpscaler m) { return m == MenuUpscaler::Dlss ? up::Kind::DLSS : m == MenuUpscaler::Fsr ? up::Kind::FSR : up::Kind::Off; }
MenuUpscaler choiceOf(up::Kind k) { return k == up::Kind::DLSS ? MenuUpscaler::Dlss : k == up::Kind::FSR ? MenuUpscaler::Fsr : MenuUpscaler::Off; }
const char* label(MenuUpscaler m) { return m == MenuUpscaler::Dlss ? "DLSS" : m == MenuUpscaler::Fsr ? "FSR 3.1" : "off"; }
int cvarValue() { return g_cvarValue ? *g_cvarValue : -1; }

bool readState(MenuUpscaler& out) {
    HANDLE f = CreateFileW(g_statePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[1024];
    DWORD n = 0;
    ReadFile(f, buf, sizeof buf - 1, &n, nullptr);
    CloseHandle(f);
    buf[n] = 0;
    return core::parseMenuState(buf, out);
}

// The mod's own file next to the payload (written through a temporary file, then renamed over the old one).
void saveState(MenuUpscaler m) {
    char buf[256];
    const int n = core::formatMenuState(m, buf, sizeof buf);
    wchar_t tmp[MAX_PATH];
    wcscpy(tmp, g_statePath);
    wcscat(tmp, L".tmp");
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, buf, DWORD(n), &written, nullptr) && written == DWORD(n);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (ok && MoveFileExW(tmp, g_statePath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        VRLOG("menu: saved upscaler=%s (ss4smvr/upscaler_menu.txt)", core::menuName(m));
    else {
        DeleteFileW(tmp);
        VRLOG("menu: saving upscaler=%s FAILED (error %lu) - the choice applies to this session only", core::menuName(m), GetLastError());
    }
}

// The user changed the list: shown at once, applied by the upscaler at the next frame boundary, saved.
void choose(MenuUpscaler m) {
    if (MenuUpscaler(g_choice.exchange(int(m))) == m) return;
    upint::requestKind(kindOf(m));
    saveState(m);
}

bool isFilterLink(uintptr_t link) {
    const char* name = link ? *reinterpret_cast<const char* const*>(link) : nullptr;
    return name && !strcmp(name, kCvar);
}

// The link's collection holds the six stock options followed by `m`'s value (so wlink_get may report it: mc_sync then
// finds it by exact text; without it the row would show "Custom").
bool hasOption(uintptr_t link, MenuUpscaler m) {
    const uint8_t* coll = *reinterpret_cast<uint8_t* const*>(link + kLinkColl);
    if (!coll) return false;
    const uint8_t* items = *reinterpret_cast<uint8_t* const*>(coll);
    const int n = *reinterpret_cast<const int32_t*>(coll + 8);
    const char* want = core::menuValue(m);
    for (int i = kStockOptions; items && want && i < n && i < kStockOptions + 2; ++i) {
        const char* v = *reinterpret_cast<const char* const*>(items + i * kOptStride + kOptValue);
        if (v && !strcmp(v, want)) return true;
    }
    return false;
}

// `self` is a CMSGraphicsOptions (its vtable holds both hooked slots, R28 s5 step 2): its Rescaling Filter widget.
uint8_t* filterWidget(uintptr_t self) {
    if (!self) return nullptr;
    void* const* vt = *reinterpret_cast<void* const* const*>(self);
    if (!vt || vt[kVtInit / 8] != g_init || vt[kVtStep / 8] != g_step) {
        static void* const* logged;
        if (vt != logged) VRLOG("menu: screen %p vtable %p is not CMSGraphicsOptions (slots +0x120/+0x178) - untouched", reinterpret_cast<void*>(self), static_cast<const void*>(vt));
        logged = vt;
        return nullptr;
    }
    return *reinterpret_cast<uint8_t**>(self + kScreenFilter);
}

// Verifies the widget (R28 s5 step 2): exactly one link named ren_iRescalingFilter whose collection holds the stock
// options "0".."5", then either nothing (the mod appends one entry per available backend) or exactly the mod's
// entries. True when the mod's entries are present afterwards. A mismatch is logged once per widget and leaves it as is.
bool ensureEntries(uint8_t* w, const char* when) {
    static uint8_t* rejected;
    if (!w || w == rejected) return false;
    const char* why = nullptr;
    uintptr_t link = 0;
    uint8_t* coll = nullptr;
    int count = 0;
    void* const* links = *reinterpret_cast<void* const* const*>(w + kWidgetLinks);
    const int nLinks = *reinterpret_cast<const int32_t*>(w + kWidgetLinkCount);
    int named = 0;
    for (int i = 0; links && i < nLinks && i < 64; ++i)
        if (isFilterLink(reinterpret_cast<uintptr_t>(links[i]))) ++named, link = reinterpret_cast<uintptr_t>(links[i]);
    if (!links || nLinks <= 0 || nLinks > 64) why = "no cvar links";
    else if (named != 1) why = "not exactly one ren_iRescalingFilter link";
    else if (!(coll = *reinterpret_cast<uint8_t**>(link + kLinkColl))) why = "no option collection";
    else {
        const uint8_t* items = *reinterpret_cast<uint8_t* const*>(coll);
        count = *reinterpret_cast<const int32_t*>(coll + 8);
        if (!items || count < kStockOptions || count > kStockOptions + 2) why = "unexpected option count";
        for (int i = 0; !why && i < count; ++i) {
            const char* v = *reinterpret_cast<const char* const*>(items + i * kOptStride + kOptValue);
            MenuUpscaler m;
            if (i < kStockOptions ? !(v && v[0] == '0' + i && !v[1]) : !core::menuFromValue(v, m)) why = "unexpected option values";
        }
    }
    if (why) {
        rejected = w;
        VRLOG("menu: rescaling widget %p left stock at %s: %s (links %d, options %d)", static_cast<void*>(w), when, why, nLinks, count);
        return false;
    }
    if (count > kStockOptions) return true;
    // Plain-text labels: only "ETRS/DTRS/TTRS...=..." labels are translated (R28 s1.2).
    for (MenuUpscaler m : {MenuUpscaler::Dlss, MenuUpscaler::Fsr}) {
        if (!up::available(kindOf(m))) continue;
        void *l = nullptr, *v = nullptr;  // CString = one char*
        strCtor(&l, label(m));
        strCtor(&v, core::menuValue(m));
        optAdd(coll, &l, &v);
        strDtor(&l);
        strDtor(&v);
    }
    *reinterpret_cast<float*>(w + kWidgetLabelWidth) = -1.0f;  // rebuild the label cache
    const int now = *reinterpret_cast<const int32_t*>(coll + 8);
    VRLOG("menu: rescaling options appended at %s (link %p coll %p count %d->%d)", when, reinterpret_cast<void*>(link), static_cast<void*>(coll), count, now);
    return now > kStockOptions;
}

uintptr_t __cdecl hookedInit(uintptr_t self, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    const uintptr_t r = realInit(self, a2, a3, a4);
    ensureEntries(filterWidget(self), "init");
    return r;
}

// The engine shows the row only when the resolution exceeds the MPix cap (R28 s1.5): it stays visible while the mod's
// entries are in it. Entries are also appended here when the screen was built before the backends were known.
uintptr_t __cdecl hookedStep(uintptr_t self, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    const uintptr_t r = realStep(self, a2, a3, a4);
    uint8_t* w = filterWidget(self);
    if (w && ensureEntries(w, "step")) {
        uint32_t& flags = *reinterpret_cast<uint32_t*>(w + kWidgetFlags);
        if (flags & kHidden) {
            flags &= ~kHidden;
            static int logged;
            if (logged++ < 2) VRLOG("menu: rescaling row un-hidden (resolution under the Max 3D rendering Mpix cap)");
        }
    }
    return r;
}

// wlink_get(link, CString* out) -> out: the mod's value while its upscaler is chosen, so the row shows it.
uintptr_t __cdecl hookedGet(uintptr_t link, uintptr_t out, uintptr_t a3, uintptr_t a4) {
    const uintptr_t r = realGet(link, out, a3, a4);
    const MenuUpscaler m = MenuUpscaler(g_choice.load(std::memory_order_relaxed));
    if (m != MenuUpscaler::Off && out && isFilterLink(link) && hasOption(link, m)) strAssign(reinterpret_cast<void*>(out), core::menuValue(m));
    return r;
}

// wlink_set(link, const CString* value): a mod value never reaches Lua (the cvar is never written).
uintptr_t __cdecl hookedSet(uintptr_t link, uintptr_t value, uintptr_t a3, uintptr_t a4) {
    if (isFilterLink(link)) {
        const char* v = value ? *reinterpret_cast<const char* const*>(value) : nullptr;
        MenuUpscaler m;
        if (core::menuFromValue(v, m)) {
            if (up::available(kindOf(m))) {
                VRLOG("menu: rescaling select %s (cvar stays %d)", label(m), cvarValue());
                choose(m);
            } else VRLOG("menu: rescaling select %s: backend unavailable, ignored (cvar stays %d)", label(m), cvarValue());
            return 0;
        }
        if (MenuUpscaler(g_choice.load()) != MenuUpscaler::Off) {
            VRLOG("menu: rescaling stock %s -> mode off", v ? v : "(null)");
            choose(MenuUpscaler::Off);
        }
    }
    return realSet(link, value, a3, a4);
}

// wlink_reset(link): "Default" writes the cvar's default (not through wlink_set) and turns the mod's upscaler off.
uintptr_t __cdecl hookedReset(uintptr_t link, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    if (isFilterLink(link) && MenuUpscaler(g_choice.load()) != MenuUpscaler::Off) {
        VRLOG("menu: rescaling default/revert -> mode off");
        choose(MenuUpscaler::Off);
    }
    return realReset(link, a2, a3, a4);
}

int32_t rel32(const uint8_t* p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}
bool callsTo(const uint8_t* site, const void* target) { return site[0] == 0xE8 && site + 5 + rel32(site + 1) == target; }
int callsWithin(const uint8_t* fn, size_t len, const void* target) {
    int n = 0;
    for (size_t i = 0; i + 5 <= len; ++i) n += callsTo(fn + i, target);
    return n;
}
}  // namespace

bool prepare() {
    const auto& b = se4::build();
    g_init = b.at<uint8_t>(se4::gen::gfxopt_init);
    g_step = b.at<uint8_t>(se4::gen::gfxopt_step);
    g_get = b.at<uint8_t>(se4::gen::wlink_get);
    g_set = b.at<uint8_t>(se4::gen::wlink_set);
    g_reset = b.at<uint8_t>(se4::gen::wlink_reset);
    auto* add = b.at<uint8_t>(se4::gen::optcoll_add);
    auto* sync = b.at<uint8_t>(se4::gen::mc_sync);
    auto* setsel = b.at<uint8_t>(se4::gen::mc_setsel);
    auto* ctor = b.at<uint8_t>(se4::gen::cstring_ctor);
    auto* dtor = b.at<uint8_t>(se4::gen::cstring_dtor);
    auto* assign = b.at<uint8_t>(se4::gen::cstring_assign);
    g_cvarValue = b.at<int32_t>(se4::gen::ren_iRescalingFilter);
    // Prologues of the hooked functions and the call sites that make them the menu's read and write paths (R28 s1.3,
    // s7; the same bytes and offsets in all four exes).
    static const uint8_t kInit[8] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x48, 0x8D}, kStep[8] = {0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x81},
                         kGet[8] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83}, kSet[8] = {0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30},
                         kReset[8] = {0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x48, 0x48};
    const char* why = nullptr;
    if (!g_init || !g_step || !g_get || !g_set || !g_reset || !add || !sync || !setsel || !ctor || !dtor || !assign) why = "symbols missing in this build";
    else if (memcmp(g_init, kInit, 8) || memcmp(g_step, kStep, 8) || memcmp(g_get, kGet, 8) || memcmp(g_set, kSet, 8) || memcmp(g_reset, kReset, 8))
        why = "a hooked function's prologue does not match";
    else if (!callsTo(sync + 0x80, g_get)) why = "mc_sync does not read through wlink_get";
    else if (!callsTo(setsel + 0x111, g_set)) why = "mc_setsel does not write through wlink_set";
    else if (!callsTo(g_get + 0x37, dtor)) why = "wlink_get does not match (CString temporary)";
    else if (callsWithin(g_init, 0x4000, add) < kStockOptions) why = "gfxopt_init does not fill option collections";
    // Startup choice: options.txt `upscaler` when set (the manual override), else the menu's saved file, else off.
    se4::payloadDirectory(g_statePath, MAX_PATH - 32);
    wcscat(g_statePath, L"\\upscaler_menu.txt");
    MenuUpscaler saved = MenuUpscaler::Off;
    const bool haveFile = readState(saved);
    int start;
    if (se4::options::has("upscaler")) {
        start = se4::options::get("upscaler", 0);
        VRLOG("menu: startup upscaler from options.txt upscaler=%d%s%s%s (the menu still switches at runtime)", start, haveFile ? " (overrides upscaler_menu.txt " : "",
              haveFile ? core::menuName(saved) : "", haveFile ? ")" : "");
    } else {
        start = saved == MenuUpscaler::Dlss ? 2 : saved == MenuUpscaler::Fsr ? 1 : 0;
        VRLOG("menu: startup upscaler %s (%s)", core::menuName(saved), haveFile ? "ss4smvr/upscaler_menu.txt" : "no saved choice: pick DLSS or FSR 3.1 in Options > Graphics > Rescaling Filter");
    }
    up::setMenuStart(start);
    if (why) return VRLOG("menu: Rescaling Filter entries off: %s (the list stays stock)", why), false;
    optAdd = reinterpret_cast<OptAddFn>(add);
    strCtor = reinterpret_cast<StrCtorFn>(ctor);
    strDtor = reinterpret_cast<StrDtorFn>(dtor);
    strAssign = reinterpret_cast<StrAssignFn>(assign);
    g_prepared = true;
    return true;
}

void onUpscalerReady() {
    if (!g_prepared || g_installed) return;
    const bool dlss = up::available(up::Kind::DLSS), fsr = up::available(up::Kind::FSR);
    const char* why = nullptr;
    if (!dlss && !fsr) why = "no upscaler backend available";
    else if (!upint::flatReady()) why = "the flat upscaler path is not ready";
    if (why) return VRLOG("menu: Rescaling Filter entries off: %s (the list stays stock)", why);
    g_choice = int(choiceOf(up::active()));
    // All or nothing: create every hook, then enable them together.
    struct H {
        uint8_t* at;
        void* fn;
        PassFn* real;
    } hooks[] = {{g_init, reinterpret_cast<void*>(hookedInit), &realInit},
                 {g_step, reinterpret_cast<void*>(hookedStep), &realStep},
                 {g_get, reinterpret_cast<void*>(hookedGet), &realGet},
                 {g_set, reinterpret_cast<void*>(hookedSet), &realSet},
                 {g_reset, reinterpret_cast<void*>(hookedReset), &realReset}};
    int created = 0;
    bool ok = true;
    for (H& h : hooks) {
        if (MH_CreateHook(h.at, h.fn, reinterpret_cast<void**>(h.real)) != MH_OK) {
            ok = false;
            break;
        }
        ++created;
    }
    for (int i = 0; ok && i < created; ++i) ok = MH_QueueEnableHook(hooks[i].at) == MH_OK;
    ok = ok && MH_ApplyQueued() == MH_OK;
    if (!ok) {
        for (int i = 0; i < created; ++i) MH_RemoveHook(hooks[i].at);
        VRLOG("menu: Rescaling Filter hooks FAILED (the list stays stock)");
        return;
    }
    g_installed = true;
    VRLOG("menu: Rescaling Filter entries on: %s%s%s; current %s, ren_iRescalingFilter %d (never written)", dlss ? "DLSS" : "", dlss && fsr ? ", " : "",
          fsr ? "FSR 3.1" : "", label(MenuUpscaler(g_choice.load())), cvarValue());
}
}  // namespace gfxmenu
