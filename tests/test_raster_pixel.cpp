#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/raster/canvas.hpp>
#include <prism/raster/pixel.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>

using namespace prism::core;
using prism::raster::Argb8888;
using prism::raster::Rgb565;

namespace {

struct Channels565 { int r, g, b; };

Channels565 fields(std::uint16_t p) { return {p >> 11, (p >> 5) & 0x3F, p & 0x1F}; }

std::uint16_t argb_to_565(std::uint32_t p) {
    return Rgb565::pack(Color::rgba(static_cast<std::uint8_t>(p >> 16), static_cast<std::uint8_t>(p >> 8),
                                    static_cast<std::uint8_t>(p)));
}

constexpr Color white = Color::rgba(255, 255, 255);
constexpr Color black_half = Color::rgba(0, 0, 0, 128);

// The formula Argb8888::over used while it took the framebuffer for opaque: one channel lerp, no
// division by the out alpha. The opaque path has to stay bit-identical to it.
constexpr std::uint32_t reference_over(std::uint32_t dst, Color src) {
    if (src.a == 0) return dst;
    if (src.a == 255) return prism::raster::argb(src);
    const auto channel = [](std::uint32_t s, std::uint32_t d, std::uint32_t a) {
        return (s * a + d * (255u - a) + 127u) / 255u;
    };
    const std::uint32_t r = channel(src.r, (dst >> 16) & 0xFFu, src.a);
    const std::uint32_t g = channel(src.g, (dst >> 8) & 0xFFu, src.a);
    const std::uint32_t b = channel(src.b, dst & 0xFFu, src.a);
    const std::uint32_t oa = src.a + ((dst >> 24) * (255u - src.a) + 127u) / 255u;
    return (oa << 24) | (r << 16) | (g << 8) | b;
}

} // namespace

TEST_CASE("Rgb565 packs the primaries and white") {
    CHECK(Rgb565::pack(Color::rgba(255, 0, 0)) == 0xF800);
    CHECK(Rgb565::pack(Color::rgba(0, 255, 0)) == 0x07E0);
    CHECK(Rgb565::pack(Color::rgba(0, 0, 255)) == 0x001F);
    CHECK(Rgb565::pack(white) == 0xFFFF);
}

TEST_CASE("Argb8888 packs as argb()") {
    CHECK(Argb8888::pack(Color::rgba(1, 2, 3, 4)) == prism::raster::argb(Color::rgba(1, 2, 3, 4)));
}

TEST_CASE("Rgb565 blends within one unit per channel of Argb8888 after packing") {
    const auto in565 = fields(Rgb565::over(Rgb565::pack(white), black_half));
    const auto in8888 = fields(argb_to_565(Argb8888::over(prism::raster::argb(white), black_half)));
    CHECK(std::abs(in565.r - in8888.r) <= 1);
    CHECK(std::abs(in565.g - in8888.g) <= 1);
    CHECK(std::abs(in565.b - in8888.b) <= 1);
}

TEST_CASE("Rgb565 over is the identity at alpha 0 and a plain pack at alpha 255") {
    CHECK(Rgb565::over(0x1234, Color::rgba(255, 0, 0, 0)) == 0x1234);
    CHECK(Rgb565::over(0x1234, Color::rgba(255, 0, 0)) == 0xF800);
}

TEST_CASE("Argb8888::over matches the canvas's 50% alpha over white") {
    std::array<std::uint32_t, 4> px{};
    prism::raster::Canvas canvas{px, 2, 2, 2};
    canvas.clear(white);
    canvas.blend_pixel(0, 0, black_half);
    CHECK(px[0] == Argb8888::over(prism::raster::argb(white), black_half));
    CHECK(px[0] == 0xFF7F7F7Fu);
}

TEST_CASE("a Canvas565 writes packed RGB565 pixels") {
    std::array<std::uint16_t, 16> px{};
    prism::raster::Canvas565 canvas{px, 4, 4, 4};
    canvas.clear(white);
    CHECK(px[5] == 0xFFFF);
    canvas.fill_rect(Rect{Point{X{0.f}, Y{0.f}}, Size{Width{2.f}, Height{2.f}}}, Color::rgba(255, 0, 0));
    CHECK(px[0] == 0xF800);
    CHECK(px[5] == 0xF800);
    CHECK(px[2] == 0xFFFF);
}

TEST_CASE("over onto an opaque destination is unchanged") {
    struct Case { std::uint32_t dst; Color src; };
    const std::array<Case, 7> cases{{
        {0xFF000000u, Color::rgba(0, 0, 0, 0)},
        {0xFF000000u, Color::rgba(255, 255, 255, 255)},
        {0xFF102030u, Color::rgba(200, 100, 50, 128)},
        {0xFFFFFFFFu, Color::rgba(10, 20, 30, 1)},
        {0xFF7F7F7Fu, Color::rgba(0, 0, 0, 64)},
        {0xFF0080FFu, Color::rgba(255, 0, 0, 254)},
        {0xFF123456u, Color::rgba(9, 8, 7, 200)},
    }};
    for (const auto& c : cases) {
        CHECK(Argb8888::over(c.dst, c.src) == reference_over(c.dst, c.src));
        CHECK((Argb8888::over(c.dst, c.src) >> 24) == 0xFFu);
    }
}

TEST_CASE("half red onto a transparent pixel keeps its colour") {
    // The destination has no alpha, so the source is all there is: the channels stay 255 and only
    // the alpha halves. A channel lerp that ignores the out alpha would give 128 here.
    CHECK(Argb8888::over(0x00000000u, Color::rgba(255, 0, 0, 128)) == 0x80FF0000u);
}

TEST_CASE("half red onto half blue divides the sums by the out alpha") {
    constexpr std::uint32_t dst = 0x800000FFu; // half-alpha blue
    constexpr Color src = Color::rgba(255, 0, 0, 128);
    constexpr std::uint32_t sa = 128, da = 128;
    const auto rounded = [](std::uint32_t num, std::uint32_t den) { return (num + den / 2) / den; };
    // Straight alpha: the source's colour is weighted by its alpha, the destination's by its own
    // alpha times what the source leaves, and that sum is divided by the out alpha. The weights
    // times 255 are kept whole, so the only rounding is the one division.
    const std::uint32_t den = sa * 255u + da * (255u - sa);
    const std::uint32_t out_a = rounded(den, 255u);
    const auto channel = [&](std::uint32_t s, std::uint32_t d) {
        return rounded(s * sa * 255u + d * da * (255u - sa), den);
    };
    const std::uint32_t expected =
        (out_a << 24) | (channel(src.r, 0u) << 16) | (channel(src.g, 0u) << 8) | channel(src.b, 255u);
    CHECK(out_a == 192u);
    CHECK(channel(src.r, 0u) == 170u);
    CHECK(channel(src.b, 255u) == 85u);
    CHECK(Argb8888::over(dst, src) == expected);
}
