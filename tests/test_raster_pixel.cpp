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
