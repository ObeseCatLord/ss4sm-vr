#pragma once
// The desktop upscaler choice of the game's Options > Graphics "Rescaling Filter" list (src/se4/gfx_menu.cpp,
// docs/re/R28_rescaling_filter_menu.md s2): the option values the mod appends to the ren_iRescalingFilter link (they
// never reach the cvar) and the mod's own state file ss4smvr/upscaler_menu.txt. Engine-agnostic;
// tests/upscaler_menu_checks.cpp.
#include <cstddef>
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

// State file: "upscaler=off|dlss|fsr" (also "fsr31", "0"/"1"/"2" as in options.txt: 0 off, 1 fsr, 2 dlss), '#'
// comments, blank lines, spaces, CRLF and other keys ignored; the last valid line wins. False when there is none.
inline bool parseMenuState(const char* text, MenuUpscaler& out) {
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
            if (is(k, ke, "upscaler")) {
                if (is(v, ve, "off") || is(v, ve, "0")) out = MenuUpscaler::Off, found = true;
                else if (is(v, ve, "dlss") || is(v, ve, "2")) out = MenuUpscaler::Dlss, found = true;
                else if (is(v, ve, "fsr") || is(v, ve, "fsr31") || is(v, ve, "1")) out = MenuUpscaler::Fsr, found = true;
            }
        }
        p = end;
        while (*p == '\n' || *p == '\r') ++p;
    }
    return found;
}

// The state file's contents for a choice. Returns the length written (snprintf semantics).
inline int formatMenuState(MenuUpscaler m, char* buf, size_t n) {
    return snprintf(buf, n,
                    "# Desktop upscaler chosen in Options > Graphics > Rescaling Filter (written by the mod; off | dlss | fsr)\n"
                    "upscaler=%s\n",
                    menuName(m));
}
}  // namespace core
