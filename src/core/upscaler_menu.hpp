#pragma once
// The upscaler choices of the game's Options > Graphics menu (src/se4/gfx_menu.cpp, docs/re/R28_rescaling_filter_menu.md
// s2, R29): the option values the mod appends to the ren_iRescalingFilter link (they never reach the cvar), the VR render
// resolution values, the sharpening link and the mod's own state file ss4smvr/upscaler_menu.txt. Engine-agnostic;
// tests/upscaler_menu_checks.cpp.
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace core {
enum class MenuUpscaler { Off = 0, Dlss = 1, Fsr = 2 };

// Option value strings appended to the collection: not numeric and not quoted, so the engine's option lookup matches
// them only by exact text (R28 s1.3).
inline constexpr const char* kMenuValueDlss = "ss4smvr:dlss";
inline constexpr const char* kMenuValueFsr = "ss4smvr:fsr31";

inline const char* menuValue(MenuUpscaler m) {
    return m == MenuUpscaler::Dlss ? kMenuValueDlss : m == MenuUpscaler::Fsr ? kMenuValueFsr : nullptr;
}
// True when `v` is one of the mod's option values (then `out` is its choice).
inline bool menuFromValue(const char* v, MenuUpscaler& out) {
    if (!v) return false;
    if (!strcmp(v, kMenuValueDlss)) return out = MenuUpscaler::Dlss, true;
    if (!strcmp(v, kMenuValueFsr)) return out = MenuUpscaler::Fsr, true;
    return false;
}
inline const char* menuName(MenuUpscaler m) { return m == MenuUpscaler::Dlss ? "dlss" : m == MenuUpscaler::Fsr ? "fsr" : "off"; }

// VR (docs/re/R29_upscaler_menu_vr_sharpening.md): the game's "Max 3D rendering Mpix" choices are replaced by
// percentages of the headset's full eye resolution (per axis). Values "ss4smvr:pct<N>", matched by exact text.
inline constexpr int kVrPercents[8] = {33, 50, 58, 67, 75, 83, 90, 100};
inline constexpr int kVrPercentDefault = 100;
inline const char* vrPercentValue(int pct) {
    static const char* const v[8] = {"ss4smvr:pct33", "ss4smvr:pct50", "ss4smvr:pct58", "ss4smvr:pct67",
                                     "ss4smvr:pct75", "ss4smvr:pct83", "ss4smvr:pct90", "ss4smvr:pct100"};
    for (int i = 0; i < 8; ++i)
        if (kVrPercents[i] == pct) return v[i];
    return nullptr;
}
inline const char* vrPercentLabel(int pct) {
    static const char* const l[8] = {"33%", "50%", "58%", "67%", "75%", "83%", "90%", "100%"};
    for (int i = 0; i < 8; ++i)
        if (kVrPercents[i] == pct) return l[i];
    return nullptr;
}
inline bool vrPercentFromValue(const char* v, int& pct) {
    for (int p : kVrPercents) {
        const char* want = vrPercentValue(p);
        if (v && want && !strcmp(v, want)) return pct = p, true;
    }
    return false;
}
// The listed percentage nearest to `pct` (ties to the larger).
inline int snapVrPercent(int pct) {
    int best = kVrPercents[0];
    for (int p : kVrPercents)
        if (std::abs(p - pct) <= std::abs(best - pct)) best = p;
    return best;
}

// The mod's slider for its RCAS sharpening (0..100 %, default 25), bound to this link name (no such cvar exists: the
// mod answers every read and swallows every write of it).
inline constexpr const char* kSharpnessLink = "ss4smvr_upscaler_sharpness";
inline constexpr int kSharpnessDefault = 25;

// Foliage sway while an upscaler runs (src/se4/foliage.hpp): -1 the game's own setting, 0 still (default: swaying leaves
// have no motion vectors and shimmer under DLSS / FSR, R29 s6). The row's values "ss4smvr:sway_game" / "ss4smvr:sway_off",
// matched by exact text.
inline constexpr const char* kSwayLink = "ss4smvr_foliage_sway";
inline constexpr int kSwayDefault = 0;
inline constexpr int kSwayModes[2] = {-1, 0};
inline const char* swayValue(int m) { return m == -1 ? "ss4smvr:sway_game" : m == 0 ? "ss4smvr:sway_off" : nullptr; }
inline const char* swayLabel(int m) { return m == -1 ? "Game Setting" : m == 0 ? "Off" : nullptr; }
inline const char* swayName(int m) { return m == -1 ? "game" : m == 0 ? "off" : nullptr; }
inline bool swayFromValue(const char* v, int& m) {
    for (int k : kSwayModes)
        if (v && !strcmp(v, swayValue(k))) return m = k, true;
    return false;
}

// The mod's state file ss4smvr/upscaler_menu.txt:
//   upscaler=off|dlss|fsr      desktop choice (also "fsr31", "0"/"1"/"2" as in options.txt: 0 off, 1 fsr, 2 dlss)
//   vr_upscaler=off|dlss|fsr   VR choice (same values)
//   vr_render=<percent>        VR render resolution, snapped to kVrPercents
//   sharpness=<0..100>         RCAS sharpening after either upscaler (desktop and VR)
//   foliage_sway=game|off      foliage sway while an upscaler runs (desktop and VR)
// '#' comments, blank lines, spaces, CRLF and other keys are ignored; per key the last valid line wins.
struct MenuState {
    bool hasUpscaler = false, hasVrUpscaler = false;
    MenuUpscaler upscaler = MenuUpscaler::Off, vrUpscaler = MenuUpscaler::Off;
    int sharpness = -1;  // -1: not set
    int vrPercent = -1;  // -1: not set
    bool hasSway = false;
    int sway = kSwayDefault;
};

// True when any key was found.
inline bool parseMenuState(const char* text, MenuState& out) {
    if (!text) return false;
    bool found = false;
    const char* p = text;
    while (*p) {
        const char* end = p;
        while (*end && *end != '\n' && *end != '\r') ++end;
        const char* stop = end;
        for (const char* h = p; h < end; ++h)
            if (*h == '#') {
                stop = h;
                break;
            }
        auto trim = [](const char*& a, const char*& b) {
            while (a < b && (*a == ' ' || *a == '\t')) ++a;
            while (b > a && (b[-1] == ' ' || b[-1] == '\t')) --b;
        };
        const char* eq = static_cast<const char*>(memchr(p, '=', size_t(stop - p)));
        if (eq) {
            const char *k = p, *ke = eq, *v = eq + 1, *ve = stop;
            trim(k, ke);
            trim(v, ve);
            auto is = [](const char* a, const char* b, const char* s) { return size_t(b - a) == strlen(s) && !strncmp(a, s, size_t(b - a)); };
            auto choice = [&](MenuUpscaler& m) {
                if (is(v, ve, "off") || is(v, ve, "0")) return m = MenuUpscaler::Off, true;
                if (is(v, ve, "dlss") || is(v, ve, "2")) return m = MenuUpscaler::Dlss, true;
                if (is(v, ve, "fsr") || is(v, ve, "fsr31") || is(v, ve, "1")) return m = MenuUpscaler::Fsr, true;
                return false;
            };
            // A plain non-negative integer of at most 3 digits, else -1.
            auto number = [&]() {
                if (v == ve || ve - v > 3) return -1;
                int n = 0;
                for (const char* c = v; c < ve; ++c) {
                    if (*c < '0' || *c > '9') return -1;
                    n = n * 10 + (*c - '0');
                }
                return n;
            };
            if (is(k, ke, "upscaler") && choice(out.upscaler)) out.hasUpscaler = found = true;
            else if (is(k, ke, "vr_upscaler") && choice(out.vrUpscaler)) out.hasVrUpscaler = found = true;
            else if (is(k, ke, "sharpness")) {
                const int n = number();
                if (n >= 0 && n <= 100) out.sharpness = n, found = true;
            } else if (is(k, ke, "foliage_sway")) {
                for (int m : kSwayModes)
                    if (is(v, ve, swayName(m))) out.sway = m, out.hasSway = found = true;
            } else if (is(k, ke, "vr_render")) {
                const int n = number();
                if (n >= 1 && n <= 100) out.vrPercent = snapVrPercent(n), found = true;
            }
        }
        p = end;
        while (*p == '\n' || *p == '\r') ++p;
    }
    return found;
}

// Desktop choice only (the original single-key form): false when the file has no valid upscaler line.
inline bool parseMenuState(const char* text, MenuUpscaler& out) {
    MenuState st;
    if (!parseMenuState(text, st) || !st.hasUpscaler) return false;
    out = st.upscaler;
    return true;
}

// The state file's contents; keys that are not set are left out. Returns the length written (snprintf semantics).
inline int formatMenuState(const MenuState& st, char* buf, size_t n) {
    int len = snprintf(buf, n,
                       "# Set in Options > Graphics (written by the mod): (vr_)upscaler off|dlss|fsr, vr_render %%,\n"
                       "# sharpness 0..100, foliage_sway game|off\n");
    auto add = [&](const char* fmt, auto v) {
        const int k = snprintf(len >= 0 && size_t(len) < n ? buf + len : nullptr, len >= 0 && size_t(len) < n ? n - size_t(len) : 0, fmt, v);
        len = len < 0 || k < 0 ? -1 : len + k;
    };
    if (st.hasUpscaler) add("upscaler=%s\n", menuName(st.upscaler));
    if (st.hasVrUpscaler) add("vr_upscaler=%s\n", menuName(st.vrUpscaler));
    if (st.vrPercent > 0) add("vr_render=%d\n", st.vrPercent);
    if (st.sharpness >= 0) add("sharpness=%d\n", st.sharpness);
    if (st.hasSway) add("foliage_sway=%s\n", swayName(st.sway));
    return len;
}
inline int formatMenuState(MenuUpscaler m, char* buf, size_t n) {
    MenuState st;
    st.hasUpscaler = true;
    st.upscaler = m;
    return formatMenuState(st, buf, n);
}
}  // namespace core
