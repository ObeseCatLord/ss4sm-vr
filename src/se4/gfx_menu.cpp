#include "se4/gfx_menu.hpp"
#include "core/upscaler_menu.hpp"
#include "se4/build.hpp"
#include "se4/foliage.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "up/integrate.hpp"
#include "se4/ammo_counter.hpp"
#include "se4/turn.hpp"
#include "up/upscaler.hpp"
#include "vk/foveation.hpp"
#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
const char* const kMpixCvar = "ren_fMax3DRenderingMPix";
// R29 s2: CSliderWidget fields (both builds), the screen's menu object, a widget's parent handle and a panel's children.
constexpr size_t kSliderSize = 0x1b8;
constexpr size_t kSliderStep = 0x17c, kSliderMin = 0x180, kSliderMax = 0x184, kSliderShowValue = 0x1ac, kSliderShowValue2 = 0x1b0;
constexpr size_t kWidgetName = 0x08, kWidgetParent = 0x24;
constexpr size_t kPanelChildren = 0x28, kPanelChildCount = 0x30;
constexpr size_t kScreenForceCas = 0x1f8;  // CMSGraphicsOptions::ms_pcwForceCAS (reflected), the row after the filter
constexpr uint64_t kFmtString = 0xabcd0009;  // the engine's "%1" argument tag for a C string (R29 s2)

using PassFn = uintptr_t(__cdecl*)(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4);
using OptAddFn = void(__cdecl*)(void* coll, const void* label, const void* value);  // copies both CStrings
using StrCtorFn = void*(__cdecl*)(void* str, const char* text);
using StrDtorFn = void(__cdecl*)(void* str);
using StrAssignFn = void(__cdecl*)(void* str, const char* text);
using AllocFn = void*(__cdecl*)(size_t size, uintptr_t type);
using SliderCtorFn = void*(__cdecl*)(void* widget, void* menu);
using AddChildFn = void(__cdecl*)(void* widget, void* parent);
using SetLabelFn = void(__cdecl*)(void* widget, const char* label, int one);
using SetTooltipFn = void(__cdecl*)(void* widget, void* menu, const char* fmt, uint64_t tag, const char* text);
using BindFn = void*(__cdecl*)(void* widget, const char* cvar);
using IdentFn = void**(__cdecl*)(void** out, const char* name);
using HandleFn = void*(__cdecl*)(int32_t handle);
PassFn realInit, realStep, realGet, realSet, realReset, realSnap;
PassFn realOptionsInit, realSettingsInit, realVrctlInit;  // VR Controls (R33)
using ButtonCtorFn = void*(__cdecl*)(void* button, void* menu);
using ButtonLabelFn = void(__cdecl*)(void* button, const char* label);
using ButtonTargetFn = void(__cdecl*)(void* button, void* screenIdent, void* zero);
using ButtonAlignFn = void(__cdecl*)(void* button, int a, int b);
ButtonCtorFn buttonCtor;
ButtonLabelFn buttonLabel;
ButtonTargetFn buttonTarget;
ButtonAlignFn buttonAlign;
uintptr_t* g_buttonType;  // the submenu button's allocation type (decoded from settings_init)
bool g_vrMenuOk;
OptAddFn optAdd;
StrCtorFn strCtor;
StrDtorFn strDtor;
StrAssignFn strAssign;
AllocFn engineAlloc;
SliderCtorFn sliderCtor;
AddChildFn addChild;
SetLabelFn setLabel;
SetTooltipFn setTooltip;
BindFn wlinkBind;
IdentFn identCtor;
HandleFn handleObject;
uintptr_t* g_sliderType;  // the slider's allocation type global (decoded at slider_alloc_site)
bool g_sliderOk;          // the slider symbols and site verified
// R29 s5: a multichoice row like the filter row, from gfxopt_init's own construction of it (+0xd0c, +0xda4).
using CtorFn = void*(__cdecl*)(void* obj, void* arg);
using CollCtorFn = void*(__cdecl*)(void* coll);
CtorFn mcCtor;
CollCtorFn collCtor;
uintptr_t *g_mcType, *g_collType;
bool g_choiceOk;
uint8_t *g_init, *g_step, *g_get, *g_set, *g_reset, *g_snap;
int32_t* g_cvarValue;  // ren_iRescalingFilter, read for logging only
bool g_prepared, g_installed;
bool g_vr;  // VR menu mode: the choice is saved as vr_upscaler and the Mpix row becomes the VR render resolution
std::atomic<int> g_choice{0};  // MenuUpscaler shown in the list and requested from the upscaler
std::atomic<int> g_sharpPct{core::kSharpnessDefault};
std::atomic<int> g_sway{core::kSwayDefault};
std::atomic<int> g_fov{-1};  // the foveation row's mode (2 eye-tracked, 1 fixed, 0 off), -1 until the device is known
wchar_t g_statePath[MAX_PATH];
core::MenuState g_state;  // what the file holds (the other mode's keys are kept when saving)
SRWLOCK g_stateLock = SRWLOCK_INIT;
// Mpix collections converted to percentages (VR): their link reads report the mod's value. A screen is rebuilt on
// every open, so the last few are kept.
void* volatile g_pctColl[4];
volatile LONG g_pctCollNext;

up::Kind kindOf(MenuUpscaler m) { return m == MenuUpscaler::Dlss ? up::Kind::DLSS : m == MenuUpscaler::Fsr ? up::Kind::FSR : up::Kind::Off; }
MenuUpscaler choiceOf(up::Kind k) { return k == up::Kind::DLSS ? MenuUpscaler::Dlss : k == up::Kind::FSR ? MenuUpscaler::Fsr : MenuUpscaler::Off; }
const char* label(MenuUpscaler m) { return m == MenuUpscaler::Dlss ? "DLSS" : m == MenuUpscaler::Fsr ? "FSR 3.1" : "off"; }
int cvarValue() { return g_cvarValue ? *g_cvarValue : -1; }

bool readState(core::MenuState& out) {
    HANDLE f = CreateFileW(g_statePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[2048];
    DWORD n = 0;
    ReadFile(f, buf, sizeof buf - 1, &n, nullptr);
    CloseHandle(f);
    buf[n] = 0;
    return core::parseMenuState(buf, out);
}

// The mod's own file next to the payload (written through a temporary file, then renamed over the old one). `what`
// names the change for the log. Menu thread.
void saveState(const char* what) {
    char buf[1024];
    AcquireSRWLockExclusive(&g_stateLock);
    const int n = core::formatMenuState(g_state, buf, sizeof buf);
    ReleaseSRWLockExclusive(&g_stateLock);
    if (n <= 0 || n >= int(sizeof buf)) return;
    wchar_t tmp[MAX_PATH];
    wcscpy(tmp, g_statePath);
    wcscat(tmp, L".tmp");
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, buf, DWORD(n), &written, nullptr) && written == DWORD(n);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (ok && MoveFileExW(tmp, g_statePath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        VRLOG("menu: saved %s (ss4smvr/upscaler_menu.txt)", what);
    else {
        DeleteFileW(tmp);
        VRLOG("menu: saving %s FAILED (error %lu) - it applies to this session only", what, GetLastError());
    }
}

// The user changed the list: shown at once, applied by the upscaler at the next frame boundary, saved.
void choose(MenuUpscaler m) {
    if (MenuUpscaler(g_choice.exchange(int(m))) == m) return;
    upint::requestKind(kindOf(m));
    AcquireSRWLockExclusive(&g_stateLock);
    (g_vr ? g_state.hasVrUpscaler : g_state.hasUpscaler) = true;
    (g_vr ? g_state.vrUpscaler : g_state.upscaler) = m;
    ReleaseSRWLockExclusive(&g_stateLock);
    char what[48];
    snprintf(what, sizeof what, "%s=%s", g_vr ? "vr_upscaler" : "upscaler", core::menuName(m));
    saveState(what);
}

void chooseSharpness(int pct) {
    pct = std::clamp(pct, 0, 100);
    up::setSharpness(pct / 100.0f);
    if (g_sharpPct.exchange(pct) == pct) return;
    AcquireSRWLockExclusive(&g_stateLock);
    g_state.sharpness = pct;
    ReleaseSRWLockExclusive(&g_stateLock);
    char what[32];
    snprintf(what, sizeof what, "sharpness=%d", pct);
    saveState(what);
}

void chooseVrPercent(int pct) {
    if (upint::vrPercent() == pct) return;
    upint::setVrPercent(pct);
    AcquireSRWLockExclusive(&g_stateLock);
    g_state.vrPercent = pct;
    ReleaseSRWLockExclusive(&g_stateLock);
    char what[32];
    snprintf(what, sizeof what, "vr_render=%d", pct);
    saveState(what);
}

void chooseSway(int m) {
    foliage::setMode(m);
    if (g_sway.exchange(m) == m) return;
    AcquireSRWLockExclusive(&g_stateLock);
    g_state.hasSway = true;
    g_state.sway = m;
    ReleaseSRWLockExclusive(&g_stateLock);
    char what[40];
    snprintf(what, sizeof what, "foliage_sway=%s", core::swayName(m));
    saveState(what);
}

void chooseFov(int m) {
    fov::setMode(m == 2 ? fov::Mode::Dynamic : m == 1 ? fov::Mode::Fixed : fov::Mode::Off);
    if (g_fov.exchange(m) == m) return;
    AcquireSRWLockExclusive(&g_stateLock);
    g_state.foveation = m;
    ReleaseSRWLockExclusive(&g_stateLock);
    char what[40];
    snprintf(what, sizeof what, "foveation=%s", core::fovName(m));
    saveState(what);
}
int fovNow() {
    const fov::Mode m = fov::mode();
    return m == fov::Mode::Dynamic ? 2 : m == fov::Mode::Fixed ? 1 : 0;
}

const char* linkName(uintptr_t link) { return link ? *reinterpret_cast<const char* const*>(link) : nullptr; }
bool isFovLink(uintptr_t link) {
    const char* n = linkName(link);
    return n && !strcmp(n, core::kFovLink);
}
// ---- VR Controls (docs/re/R33_vr_controls_menu.md) ----
// Fusion's VR Controls screen (CMSVRControlsOptions) is still in SS4/SM; only its Options button was removed. The mod
// puts the button back (Options and the in-game Settings, VR only). On the screen, rows whose cvar the mod's turning
// overrides (R26: the engine reads the mod's values, no cvar is written) are renamed to these links and answered by the
// link hooks below; rows for teleporting and other things the mod does not use are hidden; an Ammo Counters row is added.
struct VrLink {
    const char* cvar;  // the native row's cvar (nullptr: a row the mod adds)
    const char* link;
};
const VrLink kVrLinks[] = {
    {"vr_iRotationMethod", "ss4smvr_turn_method"},     // 0 same as movement / 1 continuous / 2 discrete
    {"vr_iRotationController", "ss4smvr_turn_hand"},   // 0 none / 1 left / 2 right / 3 both
    {"vr_fRotationSensitivity", "ss4smvr_turn_speed"},  // slider 0.1..4.0: the engine's 9.00009 rad/s per unit
    {"vr_bEnableComfortLocomotion", "ss4smvr_turn_vignette"},
    {nullptr, "ss4smvr_ammo_counter"},
};
constexpr float kDegPerSensitivity = 9.00009f * 57.29578f;  // R26 s3.2: smooth turn rate per vr_fRotationSensitivity unit
// Rows hidden on the VR Controls screen: teleport/step locomotion (the mod moves continuously), Windows MR input, the
// native VR movement settings, and the weapon-selection placement (dead in SS4: no reader, R33 s2).
const char* const kVrHiddenCvars[] = {"vr_fTeleportMaxSpeed", "vr_bBlinkTeleport", "vr_bStepMovement", "vr_fStepMovementMaxSpeed",
                                      "vr_bTrackpadClickToMove", "vr_bWindowsMRJoystickMovement", "vr_vwsslWeaponSelectionLocation",
                                      "vr_fWeaponSelectionHeightDistanceFromHead", "vr_fWeaponSelectionDepthDistanceFromHead",
                                      "vr_fVRWeaponPitchAdjustment",
                                      // movement: the mod moves through the virtual pad, the native VR movement these
                                      // rows set gets centred sticks (vr_device.cpp), so they would do nothing
                                      "vr_iHeadOrientedStrafe", "vr_bStrafeMovement", "vr_bTrackpadRunFullSpeed",
                                      "vr_bAllowMovementOnRotationController"};
const char* const kVrHiddenNames[] = {"VRLocomotion", "WeaponSelectionStartLocation", "WeaponSelectionHeightDistanceFromHead",
                                      "WeaponSelectionDepthDistanceFromHead", "WeaponPitch"};

int vrLinkIndex(uintptr_t link) {
    const char* n = linkName(link);
    for (int i = 0; n && i < int(sizeof kVrLinks / sizeof kVrLinks[0]); ++i)
        if (!strcmp(n, kVrLinks[i].link)) return i;
    return -1;
}
void vrLinkValue(int i, char* buf, size_t n) {
    switch (i) {
    case 0: snprintf(buf, n, "%d", turn::snap() ? 2 : 1); break;
    case 1: snprintf(buf, n, "%d", turn::hand()); break;
    case 2: snprintf(buf, n, "%g", std::round(turn::speed() / kDegPerSensitivity * 100.0f) / 100.0f); break;
    case 3: snprintf(buf, n, "%d", turn::vignette() ? 1 : 0); break;
    default: snprintf(buf, n, "%d", ammo::enabled() ? 1 : 0); break;
    }
}
void vrLinkSet(int i, const char* v) {
    char* end = nullptr;
    const double d = v ? strtod(v, &end) : 0.0;
    if (!v || end == v || !std::isfinite(d)) return;
    char what[48];
    AcquireSRWLockExclusive(&g_stateLock);
    switch (i) {
    case 0: turn::setSnap(d >= 1.5), g_state.turnMethod = turn::snap() ? 2 : 1; break;
    case 1: turn::setHand(int(d)), g_state.turnHand = turn::hand(); break;
    case 2: turn::setSpeed(float(d) * kDegPerSensitivity), g_state.turnSpeed = int(std::lround(turn::speed())); break;
    case 3: turn::setVignette(d >= 0.5), g_state.turnVignette = turn::vignette() ? 1 : 0; break;
    default: ammo::setEnabled(d >= 0.5), g_state.ammoCounter = ammo::enabled() ? 1 : 0; break;
    }
    ReleaseSRWLockExclusive(&g_stateLock);
    // as saved (turn speed in deg/s, not the slider's sensitivity units)
    if (i == 2) snprintf(what, sizeof what, "%d", int(std::lround(turn::speed())));
    else vrLinkValue(i, what, sizeof what);
    char line[80];
    snprintf(line, sizeof line, "%s=%s", kVrLinks[i].link + 8, what);
    VRLOG("menu: VR Controls %s", line);
    saveState(line);
}
void vrLinkReset(int i) {  // the screen's Default: the mod's defaults (smooth, right stick, 150 deg/s, no vignette, counters on)
    char v[16];
    snprintf(v, sizeof v, "%g", i == 0 ? 1.0 : i == 1 ? 2.0 : i == 2 ? 150.0 / kDegPerSensitivity : i == 3 ? 0.0 : 1.0);
    vrLinkSet(i, v);
}

bool isSwayLink(uintptr_t link) {
    const char* n = linkName(link);
    return n && !strcmp(n, core::kSwayLink);
}
bool isSharpLink(uintptr_t link) {
    const char* n = linkName(link);
    return n && !strcmp(n, core::kSharpnessLink);
}
// A VR Mpix link whose collection the mod converted to percentages.
bool isPctLink(uintptr_t link) {
    if (!g_vr || !link) return false;
    const char* n = linkName(link);
    if (!n || strcmp(n, kMpixCvar)) return false;
    void* coll = *reinterpret_cast<void* const*>(link + kLinkColl);
    for (void* c : g_pctColl)
        if (c && c == coll) return true;
    return false;
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

// The panel holding the Rescaling Filter row (its parent handle) and the screen's menu object (the CMenuScreen
// getter is `[[screen+0xb8]+0x30]` in both builds, R29 s2).
uint8_t* panelOf(uint8_t* filter) {
    return filter ? static_cast<uint8_t*>(handleObject(*reinterpret_cast<const int32_t*>(filter + kWidgetParent))) : nullptr;
}
void* menuOf(uintptr_t self) {
    uint8_t* hub = *reinterpret_cast<uint8_t**>(self + 0xb8);
    return hub ? *reinterpret_cast<void**>(hub + 0x30) : nullptr;
}
int childCount(const uint8_t* panel) {
    const int n = *reinterpret_cast<const int32_t*>(panel + kPanelChildCount);
    return n >= 0 && n < 512 ? n : 0;
}
uint8_t** children(uint8_t* panel) { return *reinterpret_cast<uint8_t***>(panel + kPanelChildren); }
int childIndex(uint8_t* panel, const void* w) {
    uint8_t** c = children(panel);
    for (int i = 0; c && i < childCount(panel); ++i)
        if (c[i] == w) return i;
    return -1;
}
// The links of a widget (a CWidget base field), at most 64.
int linksOf(const uint8_t* w, uintptr_t*& out) {
    out = *reinterpret_cast<uintptr_t* const*>(w + kWidgetLinks);
    const int n = *reinterpret_cast<const int32_t*>(w + kWidgetLinkCount);
    return out && n > 0 && n <= 64 ? n : 0;
}

// Moves `w` (the panel's last child) right after `after`.
void placeAfter(uint8_t* panel, const void* after, uint8_t* w) {
    const int at = childIndex(panel, after), from = childIndex(panel, w);
    uint8_t** c = children(panel);
    if (at < 0 || from <= at + 1) return;
    for (int i = from; i > at + 1; --i) c[i] = c[i - 1];
    c[at + 1] = w;
}

// R29 s3: the "Upscaler Sharpening" slider (0..100 %, steps of 5), built like the engine's XML menu builder builds a
// slider, placed right after the Rescaling Filter / FidelityFX CAS rows. Its link is bound through the filter cvar (so
// the engine's bind-time snapshot reads a declared cvar) and then renamed: every read, write and reset of it is the
// mod's own (hookedGet / hookedSet / hookedReset), so no Lua runs and no cvar is touched for it.
uint8_t* addSharpnessSlider(uintptr_t self, uint8_t* filter) {
    if (!g_sliderOk || !filter) return nullptr;
    uint8_t* panel = panelOf(filter);
    void* menu = menuOf(self);
    if (!panel || !menu || childIndex(panel, filter) < 0) {
        VRLOG("menu: sharpening slider not added: panel %p menu %p (filter row %s)", static_cast<void*>(panel), menu,
              panel ? "not among its children" : "has no panel");
        return nullptr;
    }
    uint8_t* w = static_cast<uint8_t*>(engineAlloc(kSliderSize, *g_sliderType));
    if (!w) return nullptr;
    sliderCtor(w, menu);
    addChild(w, panel);
    void* id = nullptr;
    identCtor(&id, "UpscalerSharpness");
    *reinterpret_cast<void**>(w + kWidgetName) = id;
    setLabel(w, "Upscaler Sharpening", 1);
    setTooltip(w, menu, "%1", kFmtString,
               "Sharpening after DLSS or FSR 3.1 (AMD RCAS), 0 to 100 %. Applies while the mod's upscaler runs.");
    *reinterpret_cast<float*>(w + kSliderMin) = 0.0f;
    *reinterpret_cast<float*>(w + kSliderMax) = 100.0f;
    *reinterpret_cast<float*>(w + kSliderStep) = 5.0f;
    void* link = wlinkBind(w, kCvar);
    if (link) strAssign(link, core::kSharpnessLink);
    *reinterpret_cast<int32_t*>(w + kSliderShowValue) = 1;
    *reinterpret_cast<int32_t*>(w + kSliderShowValue2) = 0;
    // After the FidelityFX CAS checkbox (shown instead of the filter row by the stock step), else after the filter.
    uint8_t* cas = *reinterpret_cast<uint8_t**>(self + kScreenForceCas);
    placeAfter(panel, childIndex(panel, cas) >= 0 ? cas : filter, w);
    VRLOG("menu: sharpening slider %p added (link %p, row %d of %d, value %d%%)", static_cast<void*>(w), link, childIndex(panel, w),
          childCount(panel), g_sharpPct.load());
    return w;
}

// A multichoice row of the mod's (R29 s5), built like the filter row (multichoice, option collection, link) and placed
// after `after`; the link is renamed after binding, as for the slider, so its reads and writes are the mod's only.
struct ChoiceRow {
    const char *ident, *label, *tooltip, *link;
    const int* modes;
    int count;
    const char* (*optLabel)(int);
    const char* (*optValue)(int);
};
uint8_t* addChoiceRow(uintptr_t self, uint8_t* filter, uint8_t* after, const ChoiceRow& row) {
    if (!g_choiceOk || !g_sliderOk || !filter || !after) return nullptr;
    uint8_t* panel = panelOf(filter);
    void* menu = menuOf(self);
    if (!panel || !menu) return nullptr;
    uint8_t* w = static_cast<uint8_t*>(engineAlloc(0x1f0, *g_mcType));
    uint8_t* coll = static_cast<uint8_t*>(engineAlloc(0x18, *g_collType));
    if (!w || !coll) return nullptr;
    mcCtor(w, menu);
    collCtor(coll);
    addChild(w, panel);
    void* id = nullptr;
    identCtor(&id, row.ident);
    *reinterpret_cast<void**>(w + kWidgetName) = id;
    setLabel(w, row.label, 1);
    setTooltip(w, menu, "%1", kFmtString, row.tooltip);
    uint8_t* link = static_cast<uint8_t*>(wlinkBind(w, kCvar));
    if (!link) return nullptr;
    strAssign(link, row.link);
    *reinterpret_cast<uint8_t**>(link + kLinkColl) = coll;
    for (int i = 0; i < row.count; ++i) {
        void *l = nullptr, *v = nullptr;
        strCtor(&l, row.optLabel(row.modes[i]));
        strCtor(&v, row.optValue(row.modes[i]));
        optAdd(coll, &l, &v);
        strDtor(&l);
        strDtor(&v);
    }
    *reinterpret_cast<float*>(w + kWidgetLabelWidth) = -1.0f;
    placeAfter(panel, after, w);
    return w;
}

// R29 s5: "Foliage Sway (DLSS / FSR)": Game Setting / Off, applied while an upscaler runs (src/se4/foliage.hpp).
uint8_t* addSwayRow(uintptr_t self, uint8_t* filter, uint8_t* after) {
    uint8_t* w = addChoiceRow(self, filter, after,
                              {"FoliageSway", "Foliage Sway (DLSS / FSR)",
                               "Leaves and bushes sway without motion vectors, which DLSS and FSR cannot follow (shimmer). Off keeps foliage "
                               "still while the mod's upscaler runs; Game Setting keeps the game's own sway.",
                               core::kSwayLink, core::kSwayModes, 2, core::swayLabel, core::swayValue});
    if (w) VRLOG("menu: foliage sway row %p added (row %d, %s)", static_cast<void*>(w), childIndex(panelOf(filter), w), core::swayName(g_sway.load()));
    return w;
}

// R31: "Foveated Rendering" (VR, with attachment shading rates): Eye-Tracked (only when the runtime offers eye gaze) /
// Fixed / Off, switched at runtime (src/vk/foveation.hpp).
void addFovRow(uintptr_t self, uint8_t* filter, uint8_t* after) {
    if (!g_vr || !fov::available()) return;
    const bool gaze = fov::gazeSupported();
    uint8_t* w = addChoiceRow(self, filter, after,
                              {"FoveatedRendering", "Foveated Rendering",
                               gaze ? "Renders the edges of your view at a lower shading rate. Eye-Tracked keeps full detail where you look "
                                      "(the default on this headset); Fixed keeps it around the lens centre."
                                    : "Renders the edges of each lens at a lower shading rate for more performance. Fixed keeps full detail "
                                      "around the lens centre. Eye-Tracked needs a headset with eye tracking.",
                               core::kFovLink, gaze ? core::kFovModes : core::kFovModes + 1, gaze ? 3 : 2, core::fovLabel, core::fovValue});
    if (w) VRLOG("menu: foveation row %p added (row %d, %s)", static_cast<void*>(w), childIndex(panelOf(filter), w), core::fovName(fovNow()));
}

// R29 s4 (VR): the "Max 3D rendering Mpix" row (a multichoice bound to ren_fMax3DRenderingMPix, 8 numeric options)
// becomes the VR render resolution: its options' labels and values are rewritten to the percentages, and its link reads
// and writes are answered by the mod (the cvar keeps its value; the desktop list is untouched outside VR).
void convertMpixRow(uintptr_t self, uint8_t* filter) {
    if (!g_vr || !upint::vrSizeReady() || !filter) return;
    uint8_t* panel = panelOf(filter);
    void* menu = menuOf(self);
    if (!panel || !menu) return;
    const void* mcVtable = *reinterpret_cast<void* const*>(filter);
    uint8_t** c = children(panel);
    for (int i = 0; c && i < childCount(panel); ++i) {
        uint8_t* w = c[i];
        if (!w || *reinterpret_cast<void* const*>(w) != mcVtable) continue;  // multichoice rows only
        uintptr_t* links;
        const int n = linksOf(w, links);
        for (int k = 0; k < n; ++k) {
            const char* name = linkName(links[k]);
            if (!name || strcmp(name, kMpixCvar)) continue;
            uint8_t* coll = *reinterpret_cast<uint8_t**>(links[k] + kLinkColl);
            uint8_t* items = coll ? *reinterpret_cast<uint8_t**>(coll) : nullptr;
            const int count = coll ? *reinterpret_cast<const int32_t*>(coll + 8) : 0;
            const char* why = nullptr;
            if (!items || count != 8) why = "not 8 options";
            for (int o = 0; !why && o < count; ++o) {
                const char* v = *reinterpret_cast<const char* const*>(items + o * kOptStride + kOptValue);
                char* end = nullptr;
                if (!v || (strtod(v, &end), end == v || *end)) why = "a non-numeric option";
                else if (*reinterpret_cast<const int32_t*>(items + o * kOptStride + 0x28)) why = "an option with aliases";
            }
            if (why) {
                VRLOG("menu: VR render resolution row not made: Mpix row %p has %s", static_cast<void*>(w), why);
                return;
            }
            for (int o = 0; o < 8; ++o) {
                strAssign(items + o * kOptStride, core::vrPercentLabel(core::kVrPercents[o]));
                strAssign(items + o * kOptStride + kOptValue, core::vrPercentValue(core::kVrPercents[o]));
            }
            g_pctColl[InterlockedIncrement(&g_pctCollNext) & 3] = coll;
            setLabel(w, "VR Render Resolution", 1);
            setTooltip(w, menu, "%1", kFmtString,
                       "Render resolution in percent of the headset's full eye resolution (per axis). Below 100 % DLSS or FSR 3.1 "
                       "upscales to full resolution; without them the game's Rescaling Filter does.");
            *reinterpret_cast<float*>(w + kWidgetLabelWidth) = -1.0f;
            VRLOG("menu: Mpix row %p is the VR render resolution (%d%%)", static_cast<void*>(w), upint::vrPercent());
            return;
        }
    }
    VRLOG("menu: VR render resolution row not made: no Mpix row on the panel");
}

// Every widget of a screen (its root at screen+0x20), depth first, children before their parents' later siblings.
template <class F> void forEachWidget(uint8_t* w, int depth, F&& f) {
    if (!w || depth > 8) return;
    f(w);
    uint8_t** c = children(w);
    for (int i = 0; c && i < childCount(w); ++i) forEachWidget(c[i], depth + 1, f);
}
constexpr size_t kScreenRoot = 0x20, kButtonTarget = 0x1a0;  // R33: screen widget root; submenu button's target IDENT

// Options / in-game Settings (VR): a "VR Controls" button right after "Graphics Options", opening Fusion's screen.
void addVrButton(uintptr_t self, const char* which) {
    if (!g_vr || !g_vrMenuOk) return;
    void* graphics = nullptr;
    identCtor(&graphics, "CMSGraphicsOptions");
    uint8_t* after = nullptr;
    forEachWidget(*reinterpret_cast<uint8_t**>(self + kScreenRoot), 0, [&](uint8_t* w) {
        if (!after && *reinterpret_cast<void**>(w + kButtonTarget) == graphics) after = w;
    });
    uint8_t* list = after ? panelOf(after) : nullptr;
    void* menu = menuOf(self);
    if (!list || !menu) return VRLOG("menu: %s: no Graphics Options button found - no VR Controls button", which);
    uint8_t* b = static_cast<uint8_t*>(engineAlloc(0x1c8, *g_buttonType));
    if (!b) return;
    buttonCtor(b, menu);
    addChild(b, list);
    buttonLabel(b, "ETRSMenu.VRControls=VR Controls");
    void* target = nullptr;
    identCtor(&target, "CMSVRControlsOptions");
    buttonTarget(b, target, nullptr);
    buttonAlign(b, 0, 2);
    setTooltip(b, menu, "%1", kFmtString, "Turning, comfort, movement and the mod's VR options.");
    placeAfter(list, after, b);
    VRLOG("menu: %s: VR Controls button added (row %d)", which, childIndex(list, b));
}
uintptr_t __cdecl hookedOptionsInit(uintptr_t self, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    const uintptr_t r = realOptionsInit(self, a2, a3, a4);
    addVrButton(self, "Options");
    return r;
}
uintptr_t __cdecl hookedSettingsInit(uintptr_t self, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    const uintptr_t r = realSettingsInit(self, a2, a3, a4);
    addVrButton(self, "Settings");
    return r;
}

// The VR Controls screen after its own init: rename, hide and add rows (s above kVrLinks).
uintptr_t __cdecl hookedVrctlInit(uintptr_t self, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    const uintptr_t r = realVrctlInit(self, a2, a3, a4);
    if (!g_vr) return r;
    // No retitle: the screen title setter rebuilds the screen layout and would drop the rows the init just made.
    void* hiddenNames[sizeof kVrHiddenNames / sizeof kVrHiddenNames[0]] = {};
    for (size_t i = 0; i < sizeof kVrHiddenNames / sizeof kVrHiddenNames[0]; ++i) identCtor(&hiddenNames[i], kVrHiddenNames[i]);
    int renamed = 0, hidden = 0;
    uint8_t* anchor = nullptr;  // the last renamed row: the Ammo Counters row goes after it
    forEachWidget(*reinterpret_cast<uint8_t**>(self + kScreenRoot), 0, [&](uint8_t* w) {
        bool hide = false;
        for (void* id : hiddenNames) hide |= id && *reinterpret_cast<void**>(w + kWidgetName) == id;
        uintptr_t* links;
        const int n = linksOf(w, links);
        for (int k = 0; k < n; ++k) {
            const char* name = linkName(links[k]);
            if (!name) continue;
            for (const char* c : kVrHiddenCvars) hide |= !strcmp(name, c);
            for (const VrLink& v : kVrLinks)
                if (v.cvar && !strcmp(name, v.cvar)) {
                    strAssign(reinterpret_cast<void*>(links[k]), v.link);
                    ++renamed;
                    anchor = w;
                    break;
                }
        }
        if (hide) {
            *reinterpret_cast<uint32_t*>(w + kWidgetFlags) |= kHidden;
            ++hidden;
        }
    });
    if (anchor) {
        static const int kOnOff[2] = {1, 0};
        uint8_t* row = addChoiceRow(self, anchor, anchor,
                                    {"AmmoCounters", "Ammo Counters",
                                     "Shows each gun's ammo on the gun itself, as in Serious Sam Fusion 2017 in VR.",
                                     "ss4smvr_ammo_counter", kOnOff, 2, [](int m) { return m ? "On" : "Off"; },
                                     [](int m) { return m ? "1" : "0"; }});
        if (!row) VRLOG("menu: VR Controls: Ammo Counters row not added");
    }
    VRLOG("menu: VR Controls screen: %d rows answered by the mod, %d hidden", renamed, hidden);
    return r;
}

uintptr_t __cdecl hookedInit(uintptr_t self, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    const uintptr_t r = realInit(self, a2, a3, a4);
    uint8_t* filter = filterWidget(self);
    ensureEntries(filter, "init");
    uint8_t* slider = addSharpnessSlider(self, filter);
    uint8_t* sway = slider ? addSwayRow(self, filter, slider) : nullptr;
    addFovRow(self, filter, sway ? sway : slider);
    convertMpixRow(self, filter);
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
    if (out && isSharpLink(link)) {  // `out` is raw: wlink_get constructs it
        char buf[16];
        snprintf(buf, sizeof buf, "%d", g_sharpPct.load());
        strCtor(reinterpret_cast<void*>(out), buf);
        return out;
    }
    if (out && isSwayLink(link)) {
        strCtor(reinterpret_cast<void*>(out), core::swayValue(g_sway.load()));
        return out;
    }
    if (out && isFovLink(link)) {
        strCtor(reinterpret_cast<void*>(out), core::fovValue(fovNow()));
        return out;
    }
    if (const int vi = out ? vrLinkIndex(link) : -1; vi >= 0) {
        char buf[16];
        vrLinkValue(vi, buf, sizeof buf);
        strCtor(reinterpret_cast<void*>(out), buf);
        return out;
    }
    const uintptr_t r = realGet(link, out, a3, a4);
    if (out && isPctLink(link)) {
        strAssign(reinterpret_cast<void*>(out), core::vrPercentValue(core::snapVrPercent(upint::vrPercent())));
        return r;
    }
    const MenuUpscaler m = MenuUpscaler(g_choice.load(std::memory_order_relaxed));
    if (m != MenuUpscaler::Off && out && isFilterLink(link) && hasOption(link, m)) strAssign(reinterpret_cast<void*>(out), core::menuValue(m));
    return r;
}

// wlink_set(link, const CString* value): a mod value never reaches Lua (the cvar is never written).
uintptr_t __cdecl hookedSet(uintptr_t link, uintptr_t value, uintptr_t a3, uintptr_t a4) {
    if (isSharpLink(link)) {
        const char* v = value ? *reinterpret_cast<const char* const*>(value) : nullptr;
        char* end = nullptr;
        const double d = v ? strtod(v, &end) : 0.0;
        if (v && end != v && std::isfinite(d)) {
            const int pct = int(std::lround(std::clamp(d, 0.0, 100.0)));
            if (pct != g_sharpPct.load()) VRLOG("menu: sharpening %d%%", pct);
            chooseSharpness(pct);
        }
        return 0;  // the link names no cvar: nothing goes to Lua
    }
    if (isSwayLink(link)) {
        const char* v = value ? *reinterpret_cast<const char* const*>(value) : nullptr;
        int m;
        if (core::swayFromValue(v, m)) {
            VRLOG("menu: foliage sway %s", core::swayName(m));
            chooseSway(m);
        }
        return 0;
    }
    if (const int vi = vrLinkIndex(link); vi >= 0) {
        vrLinkSet(vi, value ? *reinterpret_cast<const char* const*>(value) : nullptr);
        return 0;
    }
    if (isFovLink(link)) {
        const char* v = value ? *reinterpret_cast<const char* const*>(value) : nullptr;
        int m;
        if (core::fovFromValue(v, m)) {
            VRLOG("menu: foveated rendering %s", core::fovName(m));
            chooseFov(m);
        }
        return 0;
    }
    if (isPctLink(link)) {
        const char* v = value ? *reinterpret_cast<const char* const*>(value) : nullptr;
        int pct;
        if (core::vrPercentFromValue(v, pct)) {
            VRLOG("menu: VR render resolution %d%% (ren_fMax3DRenderingMPix untouched)", pct);
            chooseVrPercent(pct);
            return 0;
        }
        // Anything else (KeepSettings' revert of the snapshot) is the engine's own write of the cvar's own value.
    }
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

// wlink_snapshot(link): the popup and Apply snapshot each link's cvar by name; the mod's rows name no cvar, so they
// keep the empty snapshot (otherwise the engine logs "Cannot get values for cvar ...").
uintptr_t __cdecl hookedSnap(uintptr_t link, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    if (isSharpLink(link) || isSwayLink(link) || isFovLink(link) || vrLinkIndex(link) >= 0) return 0;
    return realSnap(link, a2, a3, a4);
}

// wlink_reset(link): "Default" writes the cvar's default (not through wlink_set) and turns the mod's upscaler off.
uintptr_t __cdecl hookedReset(uintptr_t link, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    if (isSharpLink(link)) {
        VRLOG("menu: sharpening default %d%%", core::kSharpnessDefault);
        chooseSharpness(core::kSharpnessDefault);
        return 0;
    }
    if (isSwayLink(link)) {
        VRLOG("menu: foliage sway default %s", core::swayName(core::kSwayDefault));
        chooseSway(core::kSwayDefault);
        return 0;
    }
    if (const int vi = vrLinkIndex(link); vi >= 0) {
        vrLinkReset(vi);
        return 0;
    }
    if (isFovLink(link)) {  // automatic: eye-tracked with eye gaze, else off
        const int m = fov::gazeSupported() ? 2 : 0;
        VRLOG("menu: foveated rendering default %s", core::fovName(m));
        chooseFov(m);
        return 0;
    }
    if (isPctLink(link)) {
        VRLOG("menu: VR render resolution default %d%%", core::kVrPercentDefault);
        chooseVrPercent(core::kVrPercentDefault);
    }
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
    g_snap = b.at<uint8_t>(se4::gen::wlink_snapshot);
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
                         kReset[8] = {0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x48, 0x48}, kSnap[8] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B};
    const char* why = nullptr;
    if (!g_init || !g_step || !g_get || !g_set || !g_reset || !g_snap || !add || !sync || !setsel || !ctor || !dtor || !assign)
        why = "symbols missing in this build";
    else if (memcmp(g_init, kInit, 8) || memcmp(g_step, kStep, 8) || memcmp(g_get, kGet, 8) || memcmp(g_set, kSet, 8) || memcmp(g_reset, kReset, 8) ||
             memcmp(g_snap, kSnap, 8))
        why = "a hooked function's prologue does not match";
    else if (!callsTo(sync + 0x80, g_get)) why = "mc_sync does not read through wlink_get";
    else if (!callsTo(setsel + 0x111, g_set)) why = "mc_setsel does not write through wlink_set";
    else if (!callsTo(g_get + 0x37, dtor)) why = "wlink_get does not match (CString temporary)";
    else if (callsWithin(g_init, 0x4000, add) < kStockOptions) why = "gfxopt_init does not fill option collections";
    // Startup choice: options.txt `upscaler` when set (the manual override), else the menu's saved file (upscaler on
    // the desktop, vr_upscaler in VR), else off.
    g_vr = se4::g_vrEnabled;
    se4::payloadDirectory(g_statePath, MAX_PATH - 32);
    wcscat(g_statePath, L"\\upscaler_menu.txt");
    const bool haveFile = readState(g_state) && (g_vr ? g_state.hasVrUpscaler : g_state.hasUpscaler);
    const MenuUpscaler saved = g_vr ? g_state.vrUpscaler : g_state.upscaler;
    // Sharpening (both modes): option upscale_sharpness overrides the file; default 25 %. VR render resolution: the file,
    // default 100 %.
    const int sharp = se4::options::has("upscale_sharpness") ? se4::options::get("upscale_sharpness", core::kSharpnessDefault)
                      : g_state.sharpness >= 0                ? g_state.sharpness
                                                              : core::kSharpnessDefault;
    g_sharpPct = std::clamp(sharp, 0, 100);
    up::setSharpness(g_sharpPct.load() / 100.0f);
    if (g_vr) upint::setVrPercent(g_state.vrPercent > 0 ? g_state.vrPercent : core::kVrPercentDefault);
    // Foveation (VR): the saved row choice is the start unless options.txt sets foveation (fov::decide at device creation).
    if (g_vr && g_state.foveation >= 0) fov::setStartChoice(g_state.foveation);
    // VR Controls (R33): the saved turning and ammo counter choices (options.txt wins in turn::install / ammo::install).
    if (g_vr) {
        turn::setStart(g_state.turnMethod, g_state.turnHand, g_state.turnSpeed, g_state.turnVignette);
        ammo::setStart(g_state.ammoCounter);
    }
    // Foliage sway while upscaling: option upscale_foliage_sway (-1 game, 0 off; 1 / 2 cap the game's level) overrides the file.
    const int sway = se4::options::has("upscale_foliage_sway") ? std::clamp(se4::options::get("upscale_foliage_sway", core::kSwayDefault), -1, 2)
                     : g_state.hasSway                         ? g_state.sway
                                                               : core::kSwayDefault;
    foliage::setMode(sway);
    g_sway = sway == 0 ? 0 : -1;  // the row shows Off or Game Setting (lab levels 1 / 2 show as the latter)
    VRLOG("menu: startup foliage sway while upscaling: %s (level %d)", core::swayName(g_sway.load()), sway);
    VRLOG("menu: startup sharpening %d%% (%s)", g_sharpPct.load(),
          se4::options::has("upscale_sharpness") ? "options.txt upscale_sharpness" : g_state.sharpness >= 0 ? "upscaler_menu.txt" : "default");
    if (g_vr) VRLOG("menu: startup VR render resolution %d%% (%s)", upint::vrPercent(), g_state.vrPercent > 0 ? "upscaler_menu.txt" : "default");
    int start;
    if (se4::options::has("upscaler")) {
        start = se4::options::get("upscaler", 0);
        VRLOG("menu: startup upscaler from options.txt upscaler=%d%s%s%s (the menu still switches at runtime)", start, haveFile ? " (overrides upscaler_menu.txt " : "",
              haveFile ? core::menuName(saved) : "", haveFile ? ")" : "");
    } else {
        start = saved == MenuUpscaler::Dlss ? 2 : saved == MenuUpscaler::Fsr ? 1 : 0;
        VRLOG("menu: startup %supscaler %s (%s)", g_vr ? "VR " : "", core::menuName(haveFile ? saved : MenuUpscaler::Off),
              haveFile ? "ss4smvr/upscaler_menu.txt" : "no saved choice: pick DLSS or FSR 3.1 in Options > Graphics > Rescaling Filter");
    }
    up::setMenuStart(start);
    if (why) return VRLOG("menu: Rescaling Filter entries off: %s (the list stays stock)", why), false;
    optAdd = reinterpret_cast<OptAddFn>(add);
    strCtor = reinterpret_cast<StrCtorFn>(ctor);
    strDtor = reinterpret_cast<StrDtorFn>(dtor);
    strAssign = reinterpret_cast<StrAssignFn>(assign);
    // The sharpening slider (R29 s2): the XML builder's slider site gives the allocation type and its ctor call.
    auto* site = b.at<uint8_t>(se4::gen::slider_alloc_site);
    auto* alloc = b.at<uint8_t>(se4::gen::engine_alloc);
    auto* sctor = b.at<uint8_t>(se4::gen::slider_ctor);
    const char* swhy = nullptr;
    if (!site || !alloc || !sctor || !b.at<void>(se4::gen::widget_add_child) || !b.at<void>(se4::gen::widget_set_label) ||
        !b.at<void>(se4::gen::widget_set_tooltip) || !b.at<void>(se4::gen::wlink_bind) || !b.at<void>(se4::gen::ident_ctor) ||
        !b.at<void>(se4::gen::handle_entity))
        swhy = "symbols missing in this build";
    else if (site[0] != 0xB9 || rel32(site + 1) != int32_t(kSliderSize) || site[9] != 0x48 || site[10] != 0x8B || site[11] != 0x15 ||
             !callsTo(site + 0x13, alloc) || !callsTo(site + 0x23, sctor))
        swhy = "the slider site does not match";
    if (swhy) VRLOG("menu: sharpening slider off: %s (sharpening stays at %d%%; option upscale_sharpness sets it)", swhy, g_sharpPct.load());
    else {
        g_sliderType = reinterpret_cast<uintptr_t*>(site + 9 + 7 + rel32(site + 12));
        engineAlloc = reinterpret_cast<AllocFn>(alloc);
        sliderCtor = reinterpret_cast<SliderCtorFn>(sctor);
        addChild = reinterpret_cast<AddChildFn>(b.at<void>(se4::gen::widget_add_child));
        setLabel = reinterpret_cast<SetLabelFn>(b.at<void>(se4::gen::widget_set_label));
        setTooltip = reinterpret_cast<SetTooltipFn>(b.at<void>(se4::gen::widget_set_tooltip));
        wlinkBind = reinterpret_cast<BindFn>(b.at<void>(se4::gen::wlink_bind));
        identCtor = reinterpret_cast<IdentFn>(b.at<void>(se4::gen::ident_ctor));
        handleObject = reinterpret_cast<HandleFn>(b.at<void>(se4::gen::handle_entity));
        g_sliderOk = true;
    }
    // The filter row's own construction in gfxopt_init (R29 s5; the same offsets in all four exes): the multichoice
    // allocation `mov rdx,[rip+type]; mov ecx,0x1f0; call engine_alloc; test; je; mov rdx,rsi; mov rcx,rax; call ctor` at
    // +0xd0c and the collection's `mov rdx,[rip+type]; mov ecx,0x18; call engine_alloc; test; je; mov rcx,rax; call ctor`
    // at +0xda4.
    const uint8_t* mc = g_init ? g_init + 0xd0c : nullptr;
    const uint8_t* co = g_init ? g_init + 0xda4 : nullptr;
    static const uint8_t kMc[] = {0xB9, 0xF0, 0x01, 0x00, 0x00}, kCo[] = {0xB9, 0x18, 0x00, 0x00, 0x00};
    static const uint8_t kMcTail[] = {0x48, 0x85, 0xC0, 0x74, 0x0D, 0x48, 0x8B, 0xD6, 0x48, 0x8B, 0xC8}, kCoTail[] = {0x48, 0x85, 0xC0, 0x74, 0x0D, 0x48, 0x8B, 0xC8};
    if (g_sliderOk && mc && co && !memcmp(mc, "\x48\x8B\x15", 3) && !memcmp(mc + 7, kMc, 5) && callsTo(mc + 12, alloc) && !memcmp(mc + 17, kMcTail, 11) &&
        mc[28] == 0xE8 && !memcmp(co, "\x48\x8B\x15", 3) && !memcmp(co + 7, kCo, 5) && callsTo(co + 12, alloc) && !memcmp(co + 17, kCoTail, 8) &&
        co[25] == 0xE8) {
        g_mcType = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(mc) + 7 + rel32(mc + 3));
        g_collType = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(co) + 7 + rel32(co + 3));
        mcCtor = reinterpret_cast<CtorFn>(const_cast<uint8_t*>(mc) + 33 + rel32(mc + 29));
        collCtor = reinterpret_cast<CollCtorFn>(const_cast<uint8_t*>(co) + 30 + rel32(co + 26));
        g_choiceOk = true;
    } else VRLOG("menu: foliage sway row off: the filter row's construction does not match (option upscale_foliage_sway sets it)");
    // VR Controls (R33): the screen inits and the submenu button helpers; the button type from settings_init's
    // `mov rdx,[rip+type]; mov ecx,0x1c8; call engine_alloc`.
    if (g_vr && g_choiceOk) {
        auto* settings = b.at<uint8_t>(se4::gen::settings_init);
        buttonCtor = reinterpret_cast<ButtonCtorFn>(b.at<void>(se4::gen::menu_button_ctor));
        buttonLabel = reinterpret_cast<ButtonLabelFn>(b.at<void>(se4::gen::menu_button_label));
        buttonTarget = reinterpret_cast<ButtonTargetFn>(b.at<void>(se4::gen::menu_button_target));
        buttonAlign = reinterpret_cast<ButtonAlignFn>(b.at<void>(se4::gen::menu_button_align));
        for (int i = 0; settings && i < 0x800 - 17 && !g_buttonType; ++i) {
            const uint8_t* q = settings + i;
            if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x15 && q[7] == 0xB9 && rel32(q + 8) == 0x1c8 && callsTo(q + 12, alloc))
                g_buttonType = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(q) + 7 + rel32(q + 3));
        }
        g_vrMenuOk = settings && b.at<void>(se4::gen::vrctl_init) && buttonCtor && buttonLabel && buttonTarget && buttonAlign &&
                     g_buttonType;
        VRLOG("menu: VR Controls %s", g_vrMenuOk ? "ready (Options / Settings > VR Controls)" : "off: symbols or the button type not found");
    }
    g_prepared = true;
    return true;
}

void onUpscalerReady() {
    if (!g_prepared || g_installed) return;
    const bool dlss = up::available(up::Kind::DLSS), fsr = up::available(up::Kind::FSR);
    const char* why = nullptr;
    if (!dlss && !fsr) why = "no upscaler backend available";
    else if (!g_vr && !upint::flatReady()) why = "the flat upscaler path is not ready";
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
                 {g_reset, reinterpret_cast<void*>(hookedReset), &realReset},
                 {g_snap, reinterpret_cast<void*>(hookedSnap), &realSnap}};
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
    if (g_vrMenuOk) {  // VR Controls: the screen inits (the link hooks above answer its rows)
        const auto& b = se4::build();
        struct V {
            uint8_t* at;
            void* fn;
            PassFn* real;
        } vh[] = {{b.at<uint8_t>(se4::gen::options_init), reinterpret_cast<void*>(hookedOptionsInit), &realOptionsInit},
                  {b.at<uint8_t>(se4::gen::settings_init), reinterpret_cast<void*>(hookedSettingsInit), &realSettingsInit},
                  {b.at<uint8_t>(se4::gen::vrctl_init), reinterpret_cast<void*>(hookedVrctlInit), &realVrctlInit}};
        int on = 0;
        for (V& h : vh)
            if (h.at && MH_CreateHook(h.at, h.fn, reinterpret_cast<void**>(h.real)) == MH_OK && MH_EnableHook(h.at) == MH_OK) ++on;
        VRLOG("menu: VR Controls hooks %d of 3 on%s", on, vh[0].at ? "" : " (no Options screen in this build: Settings only)");
    }
    VRLOG("menu: Rescaling Filter entries on: %s%s%s; current %s, ren_iRescalingFilter %d (never written)", dlss ? "DLSS" : "", dlss && fsr ? ", " : "",
          fsr ? "FSR 3.1" : "", label(MenuUpscaler(g_choice.load())), cvarValue());
}
}  // namespace gfxmenu
