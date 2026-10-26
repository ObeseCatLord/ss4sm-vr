// Desktop upscaler menu choice: option values and the state file (core/upscaler_menu.hpp;
// docs/re/R28_rescaling_filter_menu.md s2, s5).
#undef NDEBUG
#include "core/upscaler_menu.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace core;

int main() {
    // Option values: only the mod's two strings are mod values; stock filter values and junk are not.
    MenuUpscaler m = MenuUpscaler::Off;
    assert(menuFromValue("ss4smvr:dlss", m) && m == MenuUpscaler::Dlss);
    assert(menuFromValue("ss4smvr:fsr31", m) && m == MenuUpscaler::Fsr);
    m = MenuUpscaler::Dlss;
    for (const char* v : {"0", "1", "5", "", "ss4smvr:", "ss4smvr:dlss ", "SS4SMVR:DLSS", "\"ss4smvr:dlss\""}) assert(!menuFromValue(v, m));
    assert(!menuFromValue(nullptr, m) && m == MenuUpscaler::Dlss);  // unchanged on a miss
    assert(menuValue(MenuUpscaler::Off) == nullptr);
    for (MenuUpscaler c : {MenuUpscaler::Dlss, MenuUpscaler::Fsr}) {
        MenuUpscaler back = MenuUpscaler::Off;
        assert(menuFromValue(menuValue(c), back) && back == c);
    }
    assert(!strcmp(menuName(MenuUpscaler::Off), "off") && !strcmp(menuName(MenuUpscaler::Dlss), "dlss") &&
           !strcmp(menuName(MenuUpscaler::Fsr), "fsr"));

    // State file round trip.
    char buf[256];
    for (MenuUpscaler c : {MenuUpscaler::Off, MenuUpscaler::Dlss, MenuUpscaler::Fsr}) {
        const int n = formatMenuState(c, buf, sizeof buf);
        assert(n > 0 && n < int(sizeof buf) && buf[n - 1] == '\n');
        MenuUpscaler back = c == MenuUpscaler::Off ? MenuUpscaler::Fsr : MenuUpscaler::Off;
        assert(parseMenuState(buf, back) && back == c);
    }
    // Hand-edited forms: spaces, tabs, CRLF, comments, other keys, numeric values as in options.txt, last line wins.
    struct Case {
        const char* text;
        bool ok;
        MenuUpscaler want;
    } cases[] = {
        {"upscaler=dlss", true, MenuUpscaler::Dlss},
        {"  upscaler \t=\t fsr  # mine\r\n", true, MenuUpscaler::Fsr},
        {"upscaler=fsr31\n", true, MenuUpscaler::Fsr},
        {"upscaler=2\r\n", true, MenuUpscaler::Dlss},
        {"upscaler=1", true, MenuUpscaler::Fsr},
        {"upscaler=0", true, MenuUpscaler::Off},
        {"# upscaler=dlss\nupscaler=off\n", true, MenuUpscaler::Off},
        {"upscaler=dlss\nupscaler=fsr\n", true, MenuUpscaler::Fsr},
        {"upscaler=fsr\nupscaler=bogus\n", true, MenuUpscaler::Fsr},
        {"other=dlss\nupscaler_quality=2\n", false, MenuUpscaler::Off},
        {"upscaler dlss\n", false, MenuUpscaler::Off},
        {"upscaler=DLSS\n", false, MenuUpscaler::Off},
        {"upscaler=-1\n", false, MenuUpscaler::Off},
        {"#upscaler=dlss", false, MenuUpscaler::Off},
        {"", false, MenuUpscaler::Off},
        {"\r\n\r\n", false, MenuUpscaler::Off},
    };
    for (const Case& c : cases) {
        MenuUpscaler got = MenuUpscaler::Off;
        const bool ok = parseMenuState(c.text, got);
        if (ok != c.ok || (ok && got != c.want)) std::printf("FAIL: \"%s\" -> %d %d\n", c.text, int(ok), int(got));
        assert(ok == c.ok);
        if (ok) assert(got == c.want);
    }
    assert(!parseMenuState(nullptr, m));
    // A truncated buffer still terminates (snprintf), and its prefix does not parse as a different choice.
    char small[8];
    formatMenuState(MenuUpscaler::Dlss, small, sizeof small);
    assert(strlen(small) == sizeof small - 1);

    // R29: the full state (desktop and VR choices, VR render resolution, sharpening).
    {
        MenuState st;
        assert(parseMenuState("upscaler=dlss\nvr_upscaler=fsr\nvr_render=67\nsharpness=40\n", st));
        assert(st.hasUpscaler && st.upscaler == MenuUpscaler::Dlss && st.hasVrUpscaler && st.vrUpscaler == MenuUpscaler::Fsr);
        assert(st.vrPercent == 67 && st.sharpness == 40);
        MenuState round;
        const int n = formatMenuState(st, buf, sizeof buf);
        assert(n > 0 && n < int(sizeof buf));
        assert(parseMenuState(buf, round) && round.hasUpscaler && round.upscaler == st.upscaler && round.hasVrUpscaler &&
               round.vrUpscaler == st.vrUpscaler && round.vrPercent == 67 && round.sharpness == 40);
        // Keys left unset stay out of the file and parse back as unset.
        MenuState only;
        only.sharpness = 0;
        formatMenuState(only, buf, sizeof buf);
        MenuState back;
        assert(parseMenuState(buf, back) && !back.hasUpscaler && !back.hasVrUpscaler && back.vrPercent == -1 && back.sharpness == 0);
        // The desktop-only parse ignores the VR key.
        MenuUpscaler d = MenuUpscaler::Fsr;
        assert(!parseMenuState("vr_upscaler=dlss\n", d) && d == MenuUpscaler::Fsr);
        // Ranges and snapping: sharpness 0..100, vr_render 1..100 snapped to the listed percentages.
        MenuState r;
        assert(!parseMenuState("sharpness=101\nsharpness=-1\nsharpness=abc\nvr_render=0\nvr_render=150\n", r));
        assert(r.sharpness == -1 && r.vrPercent == -1);
        assert(parseMenuState("vr_render=70\n", r) && r.vrPercent == 67);
        assert(parseMenuState("vr_render=80\n", r) && r.vrPercent == 83);
        assert(parseMenuState("vr_render=1\n", r) && r.vrPercent == 33);
        assert(parseMenuState("vr_render=100\nsharpness=100\n", r) && r.vrPercent == 100 && r.sharpness == 100);
    }
    // VR percent values: round trip, exact text only.
    for (int p : kVrPercents) {
        int back = 0;
        assert(vrPercentValue(p) && vrPercentLabel(p) && vrPercentFromValue(vrPercentValue(p), back) && back == p);
        assert(snapVrPercent(p) == p);
    }
    {
        int back = 7;
        for (const char* v : {"67", "ss4smvr:pct", "ss4smvr:pct66", "ss4smvr:pct67 ", "1.0", "0"}) assert(!vrPercentFromValue(v, back));
        assert(!vrPercentFromValue(nullptr, back) && back == 7);
        assert(!vrPercentValue(66) && !vrPercentLabel(0));
        assert(snapVrPercent(0) == 33 && snapVrPercent(200) == 100 && snapVrPercent(53) == 50 && snapVrPercent(54) == 58);
        assert(!strcmp(kSharpnessLink, "ss4smvr_upscaler_sharpness") && kSharpnessDefault == 25 && kVrPercentDefault == 100);
    }

    // Foliage sway: state key and row values.
    {
        MenuState st;
        assert(parseMenuState("foliage_sway=game\n", st) && st.hasSway && st.sway == -1);
        assert(parseMenuState("foliage_sway=off\n", st) && st.sway == 0);
        MenuState bad;
        assert(!parseMenuState("foliage_sway=2\nfoliage_sway=Off\nfoliage_sway=limited\n", bad) && !bad.hasSway && bad.sway == kSwayDefault);
        st.hasSway = true, st.sway = -1;
        formatMenuState(st, buf, sizeof buf);
        MenuState back;
        assert(parseMenuState(buf, back) && back.hasSway && back.sway == -1);
        for (int m : kSwayModes) {
            int r = 7;
            assert(swayValue(m) && swayLabel(m) && swayName(m) && swayFromValue(swayValue(m), r) && r == m);
        }
        int r = 7;
        assert(!swayFromValue("ss4smvr:sway", r) && !swayFromValue("ss4smvr:sway_limited", r) && !swayFromValue(nullptr, r) && r == 7 &&
               !swayValue(1) && !swayValue(2));
    }

    std::puts("upscaler menu checks passed");
    return 0;
}
