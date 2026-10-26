#include "se4/foliage.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/patch.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>

namespace foliage {
namespace {
int32_t* g_cvar;             // sha_iAllowTreeSwaying, read only
volatile int32_t* g_value;   // what every redirected read sees
std::atomic<int> g_mode{-1};
int g_labGame = -1;  // lab: option lab_foliage_sway stands in for the game's value (the lab copy's preset says 1)

int32_t rel32(const uint8_t* p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}
}  // namespace

bool install() {
    const auto& b = se4::build();
    g_cvar = b.at<int32_t>(se4::gen::sha_iAllowTreeSwaying);
    if (!g_cvar) return VRLOG("foliage: sha_iAllowTreeSwaying not in this build - sway stays the game's"), false;
    auto* base = reinterpret_cast<uint8_t*>(b.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t *text = nullptr, *end = nullptr;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (!memcmp(sec[i].Name, ".text", 6)) text = base + sec[i].VirtualAddress, end = text + sec[i].Misc.VirtualSize;
    if (!text) return false;
    // Rip-relative reads of the cvar: 8B /r (mov r32), 39 /r and 3B /r (cmp with a register), 83 /7 ib (cmp imm8).
    // Each candidate's disp32 must land exactly on the cvar from its own instruction end.
    constexpr int kMax = 12;
    uint8_t* disp[kMax];
    int ends[kMax], n = 0;
    for (uint8_t* p = text; p + 7 <= end; ++p) {
        int len;
        if ((p[0] == 0x8B || p[0] == 0x39 || p[0] == 0x3B) && (p[1] & 0xC7) == 0x05) len = 6;
        else if (p[0] == 0x83 && p[1] == 0x3D) len = 7;
        else continue;
        if (p + len + rel32(p + 2) != reinterpret_cast<uint8_t*>(g_cvar)) continue;
        if (n == kMax) return VRLOG("foliage: more than %d reads of sha_iAllowTreeSwaying - sway stays the game's", kMax), false;
        disp[n] = p + 2, ends[n] = len - 2, ++n;
    }
    if (n < 6) return VRLOG("foliage: %d reads of sha_iAllowTreeSwaying (expected 6) - sway stays the game's", n), false;
    g_value = reinterpret_cast<int32_t*>(se4::patch::allocNear(disp[0], sizeof(int32_t)));
    if (!g_value) return VRLOG("foliage: no memory near the image"), false;
    *g_value = *g_cvar;
    int done = 0;
    for (int i = 0; i < n; ++i) {
        const intptr_t d = reinterpret_cast<intptr_t>(g_value) - reinterpret_cast<intptr_t>(disp[i] + ends[i]);
        if (d != int32_t(d)) continue;
        const int32_t nd = int32_t(d);
        uint8_t old[4], now[4];
        memcpy(old, disp[i], 4);
        memcpy(now, &nd, 4);
        done += se4::patch::writeCode(disp[i], old, now, 4);
    }
    g_labGame = std::clamp(se4::options::get("lab_foliage_sway", -1), -1, 2);
    const int opt = std::clamp(se4::options::get("upscale_foliage_sway", -1), -1, 2);
    if (g_mode.load() == -1) g_mode = opt;
    VRLOG("foliage: %d of %d sha_iAllowTreeSwaying reads redirected (game value %d; while upscaling %s %d)", done, n, *g_cvar,
          g_mode.load() < 0 ? "the game's" : "at most", g_mode.load());
    return done == n;
}

void update(bool upscaling) {
    if (!g_value) return;
    const int m = g_mode.load(std::memory_order_relaxed), game = g_labGame >= 0 ? g_labGame : *g_cvar;
    const int v = upscaling && m >= 0 ? std::min(game, m) : game;
    if (*g_value != v) {
        *g_value = v;
        VRLOG("foliage: sway %d (game %d, %s)", v, game, upscaling ? "upscaling" : "not upscaling");
    }
}

void setMode(int mode) { g_mode = std::clamp(mode, -1, 2); }
int mode() { return g_mode.load(); }
}  // namespace foliage
