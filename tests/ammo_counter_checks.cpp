// The on-weapon ammo counter readout (src/core/ammo_counter.hpp): the engine's digit and colour rules, and the image.
#include "core/ammo_counter.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace core::ammo;

int main() {
    // Digits as the engine shows them: leading zeros blank, the ones digit always, capped at 999.
    int d[3];
    digits(0, d);
    assert(d[0] == -1 && d[1] == -1 && d[2] == 0);
    digits(7, d);
    assert(d[0] == -1 && d[1] == -1 && d[2] == 7);
    digits(40, d);
    assert(d[0] == -1 && d[1] == 4 && d[2] == 0);
    digits(305, d);
    assert(d[0] == 3 && d[1] == 0 && d[2] == 5);
    digits(5000, d);
    assert(d[0] == 9 && d[1] == 9 && d[2] == 9);
    digits(-3, d);
    assert(d[0] == -1 && d[1] == -1 && d[2] == 0);

    // Colour: normal at or above a tenth of the maximum, red blinking below it.
    Rgba c = colour(10, 100, 0.0);
    assert(c.r == kNormal.r && c.g == kNormal.g && c.b == kNormal.b && c.a == 0xff);
    c = colour(9, 100, 0.1);
    assert(c.r == 0xff && c.g == 0 && c.a == 0xff);
    c = colour(9, 100, 0.3);
    assert(c.r == 0xff && c.a == 0x40);
    c = colour(5, 0, 0.3);  // no maximum: never low
    assert(c.r == kNormal.r);

    // Image: transparent corners, opaque panel, lit pixels only where digits are lit.
    const int w = 192, h = 96;
    std::vector<uint8_t> img(size_t(w) * h * 4);
    assert(!rasterize(img.data(), 8, 8, 1, kNormal));
    auto litCount = [&](int count) {
        rasterize(img.data(), w, h, count, kNormal);
        int n = 0;
        for (size_t i = 0; i < img.size(); i += 4) n += img[i] == kNormal.r && img[i + 1] == kNormal.g && img[i + 2] == kNormal.b;
        return n;
    };
    const int one = litCount(1), eight = litCount(8), e88 = litCount(888), zero = litCount(0);
    assert(img[3] == 0);                          // top-left corner transparent (rounded panel)
    assert(img[(size_t(h / 2) * w + 4) * 4 + 3] > 0xc0);  // left edge of the panel, mid height: opaque
    assert(one > 0 && one < eight && eight < e88 && zero < eight);
    assert(e88 > 2 * eight);  // three 8s light about three times the segments of one
    std::printf("ammo counter checks passed (lit pixels: 1 -> %d, 8 -> %d, 888 -> %d)\n", one, eight, e88);
    return 0;
}
