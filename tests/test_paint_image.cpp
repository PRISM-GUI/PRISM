// paint_image (raster/paint_image.hpp): a canvas painted by a callback, frozen as an Image in the
// canvas' own format.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/raster/paint_image.hpp>
#include <prism/raster/pixel.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace prism;
using namespace prism::core;
using namespace prism::raster;

namespace {

Rect rect(float x, float y, float w, float h) {
    return Rect{Point{X{x}, Y{y}}, Size{Width{w}, Height{h}}};
}

std::vector<std::uint16_t> words(const render::Image& image) {
    const auto px = image.pixels();
    std::vector<std::uint16_t> out(px.size() / 2);
    std::memcpy(out.data(), px.data(), out.size() * 2);
    return out;
}

} // namespace

TEST_CASE("painting an Rgb565 canvas gives an rgb565 image") {
    const auto image = paint_image<Rgb565>(4, 2, [](Canvas565& canvas) {
        canvas.fill_rect(rect(0, 0, 4, 2), Color::rgba(255, 0, 0));
    });
    REQUIRE(image);
    CHECK(image->format() == render::PixelFormat::rgb565);
    CHECK(image->width() == 4);
    CHECK(image->height() == 2);
    const auto packed = words(*image);
    REQUIRE(packed.size() == 8U);
    for (const auto word : packed) CHECK(word == Rgb565::pack(Color::rgba(255, 0, 0)));
}

TEST_CASE("an Argb8888 canvas' pixels come out as RGBA bytes") {
    const auto image = paint_image<Argb8888>(2, 1, [](Canvas& canvas) {
        canvas.fill_rect(rect(0, 0, 2, 1), Color::rgba(0, 0, 255));
    });
    REQUIRE(image);
    CHECK(image->format() == render::PixelFormat::rgba8888);
    const auto px = image->pixels();
    REQUIRE(px.size() == 8U);
    CHECK(px[0] == 0);
    CHECK(px[1] == 0);
    CHECK(px[2] == 255);
    CHECK(px[3] == 255);
    CHECK(px[4] == 0);
    CHECK(px[6] == 255);
    CHECK(px[7] == 255);
}

TEST_CASE("an Argb8888 canvas starts transparent") {
    const auto image = paint_image<Argb8888>(2, 2, [](Canvas& canvas) {
        canvas.fill_rect(rect(0, 0, 1, 1), Color::rgba(255, 0, 0));
    });
    REQUIRE(image);
    const auto px = image->pixels();
    REQUIRE(px.size() == 16U);
    // The painted corner is opaque red; the rest was never touched.
    CHECK(px[0] == 255);
    CHECK(px[3] == 255);
    CHECK(px[4] == 0);   // the next pixel's green
    CHECK(px[7] == 0);   // and its alpha
}

TEST_CASE("half red over nothing keeps its colour and half its alpha") {
    const auto image = paint_image<Argb8888>(1, 1, [](Canvas& canvas) {
        canvas.fill_rect(rect(0, 0, 1, 1), Color::rgba(255, 0, 0, 128));
    });
    REQUIRE(image);
    const auto px = image->pixels();
    REQUIRE(px.size() == 4U);
    CHECK(px[0] == 255);
    CHECK(px[1] == 0);
    CHECK(px[2] == 0);
    CHECK(px[3] == 128);
}

TEST_CASE("a size that is not positive paints nothing") {
    CHECK_FALSE(paint_image<Argb8888>(0, 4, [](Canvas&) {}));
    CHECK_FALSE(paint_image<Argb8888>(4, 0, [](Canvas&) {}));
    CHECK_FALSE(paint_image<Rgb565>(-1, 4, [](Canvas565&) {}));
}
