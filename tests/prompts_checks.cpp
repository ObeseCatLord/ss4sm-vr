// VR button prompt table checks (core/prompts.hpp).
#undef NDEBUG
#include "core/prompts.hpp"
#include <cassert>
#include <cstring>

using namespace core;
using namespace core::prompts;
using controls::Kind;

int main() {
    char buf[64];
    // Index: use/melee is the right B, crouch the right stick, the gadget the left grip
    assert(!strcmp(markup(Kind::Index, RightThumb, false, buf), "<img src=\"XboxButtonB\">"));
    assert(!strcmp(markup(Kind::Index, B, false, buf), "<img src=\"XboxThumbstickTriggerR\">"));
    assert(!strcmp(markup(Kind::Index, Y, false, buf), "L Grip"));
    // menus: B is back on Index/Touch/Frame
    assert(!strcmp(markup(Kind::Index, B, true, buf), "<img src=\"XboxButtonB\">"));
    assert(!strcmp(markup(Kind::Vive, B, true, buf), "R Grip"));
    // triggers on every profile, axes keep the engine glyph, unknown controllers too
    for (Kind k : {Kind::Index, Kind::Touch, Kind::Frame, Kind::Vive, Kind::WMR})
        assert(!strcmp(markup(k, RT, false, buf), "<img src=\"XboxTriggerButtonR\">"));
    assert(!markup(Kind::Index, AxisFirst, false, buf) && !markup(Kind::Unknown, A, false, buf));
    // every profile has a prompt for the gadget, Netricsa and use
    for (Kind k : {Kind::Index, Kind::Touch, Kind::Frame, Kind::Vive, Kind::WMR})
        assert(control(k, Y, false) && control(k, Left, false) && control(k, RightThumb, false));
    return 0;
}
