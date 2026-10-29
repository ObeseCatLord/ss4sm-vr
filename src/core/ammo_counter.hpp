#pragma once
// On-weapon ammo counter readout (src/se4/ammo_counter.cpp): the engine's own VR counter rules, drawn as seven-segment
// digits into an RGBA image for an OpenXR quad layer.
//   Engine (SS4 0x1402d4b70, Fusion's VR weapons): three digits "100" / "10" / "1"; leading zeros blank, the ones digit
//   always shown; colour AmmoCounterColor = (0x26, 0xcd, 0xf7) normally, red blinking (alpha 0x40 / 0xff) below a tenth
//   of the weapon's maximum.
//   Look (the SS2 VR mod's ammo screens, src/common/ammo_screen.hpp there): slanted hexagonal segments, a dark panel, unlit
//   segments only just above the panel (brighter "ghosts" read as an 8).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace core::ammo {

struct Rgba {
    uint8_t r, g, b, a;
};

inline constexpr Rgba kNormal{0x26, 0xcd, 0xf7, 0xff};
inline constexpr Rgba kLow{0xff, 0x00, 0x00, 0xff};

// The digit values shown, hundreds first: -1 blank. Counts above 999 show 999 (the engine's three places).
inline void digits(int count, int out[3]) {
    count = std::clamp(count, 0, 999);
    const int h = count / 100, t = count / 10 % 10, o = count % 10;
    out[0] = h ? h : -1;
    out[1] = (h || t) ? t : -1;
    out[2] = o;
}

// The engine's colour: below a tenth of the maximum, red blinking twice a second (dim half of each period).
inline Rgba colour(int count, int max, double seconds) {
    if (max <= 0 || float(count) / float(max) >= 0.1f) return kNormal;
    Rgba c = kLow;
    c.a = std::fmod(seconds, 0.5) < 0.25 ? 0xff : 0x40;
    return c;
}

// Seven segments a..g as polygons in a unit cell (x right 0..1, y down 0..1.6), slanted.
namespace detail {
inline bool segmentOn(int digit, int seg) {
    static const uint8_t kMask[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};  // bit 0 = a ... bit 6 = g
    return digit >= 0 && digit <= 9 && (kMask[digit] >> seg & 1);
}
// Distance-based hexagonal segment test: inside segment `seg` of a cell of width 1, height 1.6, thickness th.
inline bool inSegment(float x, float y, int seg, float th) {
    const float h = 1.6f, half = h / 2;
    // horizontal: a (top), g (middle), d (bottom); vertical: f (top-left), b (top-right), e (bottom-left), c (bottom-right)
    auto hseg = [&](float cy) {
        const float dy = std::fabs(y - cy);
        if (dy > th / 2) return false;
        const float inset = th / 2 + dy;  // pointed ends
        return x > inset && x < 1 - inset;
    };
    auto vseg = [&](float cx, float y0, float y1) {
        const float dx = std::fabs(x - cx);
        if (dx > th / 2) return false;
        const float inset = th / 2 + dx;
        return y > y0 + inset && y < y1 - inset;
    };
    switch (seg) {
    case 0: return hseg(th / 2);
    case 1: return vseg(1 - th / 2, 0, half);
    case 2: return vseg(1 - th / 2, half, h);
    case 3: return hseg(h - th / 2);
    case 4: return vseg(th / 2, half, h);
    case 5: return vseg(th / 2, 0, half);
    case 6: return hseg(half);
    }
    return false;
}
}  // namespace detail

// Draws the readout into rgba (w x h, rows top first, straight alpha): a rounded dark panel, three digit cells, lit
// segments in `lit`, unlit segments just above the panel. Returns false for a size too small to draw.
inline bool rasterize(uint8_t* rgba, int w, int h, int count, Rgba lit) {
    if (!rgba || w < 24 || h < 12) return false;
    int d[3];
    digits(count, d);
    const Rgba panel{8, 12, 14, 0xd8};
    const Rgba unlit{std::uint8_t(panel.r + 10), std::uint8_t(panel.g + 14), std::uint8_t(panel.b + 16), panel.a};
    const float margin = h * 0.14f;
    const float cellH = h - 2 * margin, cellW = cellH / 1.6f, gap = cellW * 0.28f;
    const float total = 3 * cellW + 2 * gap;
    const float x0 = (w - total) / 2, slant = 0.12f;  // italic lean, in cell widths per cell height
    const float radius = h * 0.18f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* px = rgba + (size_t(y) * w + x) * 4;
            // panel with rounded corners
            const float cx = std::clamp(x + 0.5f, radius, w - radius), cy = std::clamp(y + 0.5f, radius, h - radius);
            const float rx = x + 0.5f - cx, ry = y + 0.5f - cy;
            if (rx * rx + ry * ry > radius * radius) {
                std::memset(px, 0, 4);
                continue;
            }
            Rgba c = panel;
            const float v = (y + 0.5f - margin) / cellH * 1.6f;  // cell y 0..1.6
            if (v >= 0 && v <= 1.6f)
                for (int i = 0; i < 3; ++i) {
                    const float u = (x + 0.5f - (x0 + i * (cellW + gap))) / cellW + slant * (v - 0.8f);  // cell x 0..1
                    if (u < -0.1f || u > 1.1f) continue;
                    for (int s = 0; s < 7; ++s)
                        if (detail::inSegment(u, v, s, 0.2f)) c = detail::segmentOn(d[i], s) ? lit : unlit;
                }
            px[0] = c.r, px[1] = c.g, px[2] = c.b, px[3] = c.a;
        }
    return true;
}

}  // namespace core::ammo
