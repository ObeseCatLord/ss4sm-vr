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

    std::puts("upscaler menu checks passed");
    return 0;
}
