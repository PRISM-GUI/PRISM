#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/raster/pixel.hpp>
#include <prism/render/draw_list.hpp>
#include <prism/render/image_ops.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

using namespace prism::core;
using namespace prism::render;
using prism::raster::Rgb565;

namespace {

Size box(int w, int h) {
    return Size{Width{static_cast<float>(w)}, Height{static_cast<float>(h)}};
}

PixelView bytes_view(const std::vector<std::uint8_t>& px, int w, int h, int stride, SourceFormat format) {
    return {std::as_bytes(std::span{px}), w, h, stride, format};
}

std::vector<std::uint16_t> words(const Image& image) {
    const auto px = image.pixels();
    std::vector<std::uint16_t> out(px.size() / 2);
    std::memcpy(out.data(), px.data(), out.size() * 2);
    return out;
}

// A pixel of an rgba8888 image.
Color pixel(const Image& image, int x, int y) {
    const auto px = image.pixels();
    const auto at = static_cast<std::size_t>(y * image.width() + x) * 4;
    return Color::rgba(px[at], px[at + 1], px[at + 2], px[at + 3]);
}

std::shared_ptr<const Image> rgba_image(int w, int h, std::vector<std::uint8_t> px) {
    return *Image::from_rgba(w, h, std::move(px));
}

} // namespace

TEST_CASE("fit_pixels averages each source block into one pixel") {
    // 4x4 -> 2x2: every output pixel is the rounded mean of its 2x2 block, so not every mean is a
    // whole number (2.5 rounds to 3).
    std::vector<std::uint8_t> px;
    for (int i = 0; i < 16; ++i) {
        px.push_back(static_cast<std::uint8_t>(i * 10));
        px.push_back(static_cast<std::uint8_t>(255 - i * 10));
        px.push_back(static_cast<std::uint8_t>(i));
        px.push_back(255);
    }
    const auto source = rgba_image(4, 4, std::move(px));
    const auto out = fit_pixels(view_of(*source), box(2, 2), PixelFormat::rgba8888);
    REQUIRE(out);
    CHECK(out->width() == 2);
    CHECK(out->height() == 2);
    CHECK(pixel(*out, 0, 0) == Color::rgba(25, 230, 3));
    CHECK(pixel(*out, 1, 0) == Color::rgba(45, 210, 5));
    CHECK(pixel(*out, 0, 1) == Color::rgba(105, 150, 11));
    CHECK(pixel(*out, 1, 1) == Color::rgba(125, 130, 13));
}

TEST_CASE("fit_pixels averages a picture whose block overflows 32-bit sums") {
    // 600x600 into 2x2: 90k pixels per output pixel, each 255*255 of channel sum, so a 32-bit
    // accumulator would wrap and the white would come out grey.
    const std::vector<std::uint8_t> white(600 * 600 * 4, 255);
    const auto out = fit_pixels(bytes_view(white, 600, 600, 600 * 4, SourceFormat::rgba8888), box(2, 2),
                                PixelFormat::rgba8888);
    REQUIRE(out);
    REQUIRE(out->width() == 2);
    REQUIRE(out->height() == 2);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) CHECK(pixel(*out, x, y) == Color::rgba(255, 255, 255, 255));
    }
}

TEST_CASE("fit_pixels keeps the aspect ratio inside the box") {
    const std::vector<std::uint8_t> px(400 * 200 * 3, 200);
    const auto out = fit_pixels(bytes_view(px, 400, 200, 400 * 3, SourceFormat::rgb888), box(100, 100),
                                PixelFormat::rgba8888);
    REQUIRE(out);
    CHECK(out->width() == 100);
    CHECK(out->height() == 50);
}

TEST_CASE("fit_pixels fills a box the picture does not divide evenly") {
    // 22/13 in float32 is just under 1, so a ratio truncated from it would lose a pixel of the box.
    const std::vector<std::uint8_t> big(23 * 23 * 3, 40);
    const auto small =
        fit_pixels(bytes_view(big, 23, 23, 23 * 3, SourceFormat::rgb888), box(7, 7), PixelFormat::rgba8888);
    REQUIRE(small);
    CHECK(small->width() == 7);
    CHECK(small->height() == 7);

    const std::vector<std::uint8_t> almost(22 * 22 * 3, 40);
    const auto sized = fit_pixels(bytes_view(almost, 22, 22, 22 * 3, SourceFormat::rgb888), box(13, 13),
                                  PixelFormat::rgba8888);
    REQUIRE(sized);
    CHECK(sized->width() == 13);
    CHECK(sized->height() == 13);
}

TEST_CASE("fit_pixels does not enlarge a view smaller than the box") {
    const std::vector<std::uint8_t> px(10 * 10 * 4, 90);
    const auto out = fit_pixels(bytes_view(px, 10, 10, 10 * 4, SourceFormat::rgba8888), box(100, 100),
                                PixelFormat::rgba8888);
    REQUIRE(out);
    CHECK(out->width() == 10);
    CHECK(out->height() == 10);
    CHECK(pixel(*out, 3, 3) == Color::rgba(90, 90, 90, 90));
}

TEST_CASE("fit_pixels ignores the padding a longer stride carries") {
    // Ten real pixels a row, then six bytes of 0xFF padding: none of it may reach the picture.
    // 10x2 -> 5x1, so each output pixel averages a 2x2 block of real pixels.
    constexpr int kWidth = 10;
    constexpr int kStride = 16 * 3;
    std::vector<std::uint8_t> px(kStride * 2, 0xFF);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            auto* p = &px[static_cast<std::size_t>(y) * kStride + x * 3];
            p[0] = 10;
            p[1] = 20;
            p[2] = 30;
        }
    }
    const auto out = fit_pixels(bytes_view(px, kWidth, 2, kStride, SourceFormat::rgb888), box(5, 1),
                                PixelFormat::rgba8888);
    REQUIRE(out);
    CHECK(out->width() == 5);
    CHECK(out->height() == 1);
    for (int x = 0; x < 5; ++x) CHECK(pixel(*out, x, 0) == Color::rgba(10, 20, 30, 255));
}

TEST_CASE("fit_pixels does not let a transparent pixel's colour bleed") {
    // One opaque red and three fully transparent greens: the colour is the red one, and the alpha
    // is the quarter of it that is there.
    const std::vector<std::uint8_t> px{255, 0, 0, 255, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0};
    const auto out = fit_pixels(bytes_view(px, 2, 2, 8, SourceFormat::rgba8888), box(1, 1), PixelFormat::rgba8888);
    REQUIRE(out);
    CHECK(out->width() == 1);
    CHECK(pixel(*out, 0, 0) == Color::rgba(255, 0, 0, 64));
}

TEST_CASE("fit_pixels packs rgb565 output the way Rgb565::pack does") {
    const std::vector<std::uint8_t> px{255, 0, 0, 0, 255, 0};
    const auto out = fit_pixels(bytes_view(px, 2, 1, 6, SourceFormat::rgb888), box(2, 2), PixelFormat::rgb565);
    REQUIRE(out);
    REQUIRE(out->format() == PixelFormat::rgb565);
    const auto packed = words(*out);
    REQUIRE(packed.size() == 2U);
    CHECK(packed[0] == Rgb565::pack(Color::rgba(255, 0, 0)));
    CHECK(packed[1] == Rgb565::pack(Color::rgba(0, 255, 0)));
}

TEST_CASE("fit_pixels refuses a view whose bytes do not describe it") {
    const std::vector<std::uint8_t> px(10 * 2 * 4, 255);
    // A stride shorter than a row would read the next row as part of this one.
    CHECK_FALSE(fit_pixels(bytes_view(px, 10, 2, 10 * 4 - 1, SourceFormat::rgba8888), box(5, 1),
                           PixelFormat::rgba8888));
    // One byte short of the last row.
    const std::vector<std::uint8_t> short_px(10 * 2 * 4 - 1, 255);
    CHECK_FALSE(fit_pixels(bytes_view(short_px, 10, 2, 10 * 4, SourceFormat::rgba8888), box(5, 1),
                           PixelFormat::rgba8888));
    // No pixels, and no box to put them in.
    CHECK_FALSE(fit_pixels(bytes_view(px, 0, 2, 0, SourceFormat::rgba8888), box(5, 1), PixelFormat::rgba8888));
    CHECK_FALSE(fit_pixels(bytes_view(px, 10, 2, 10 * 4, SourceFormat::rgba8888), box(0, 1), PixelFormat::rgba8888));
}

TEST_CASE("scaled and converted hand back the same image when nothing changes") {
    const auto image = rgba_image(10, 10, std::vector<std::uint8_t>(10 * 10 * 4, 7));
    CHECK(scaled(image, box(100, 100)) == image); // it already fits: never enlarged
    CHECK(scaled(image, box(10, 10)) == image);
    CHECK(converted(image, PixelFormat::rgba8888) == image);

    const auto smaller = scaled(image, box(5, 5));
    REQUIRE(smaller);
    CHECK(smaller != image);
    CHECK(smaller->width() == 5);
    CHECK(smaller->height() == 5);
    CHECK(smaller->format() == PixelFormat::rgba8888);

    const auto other = converted(image, PixelFormat::rgb565);
    REQUIRE(other);
    CHECK(other != image);
    CHECK(other->format() == PixelFormat::rgb565);
    CHECK(other->width() == 10);
}

TEST_CASE("a trip through rgb565 stays within its quantisation") {
    // rgb565 carries no alpha, so the pixels are opaque: the trip is about colour only.
    const std::vector<std::uint8_t> px{255, 0,   0,   255, 0,   255, 0,   255, 0,   0,
                                       255, 255, 128, 128, 128, 255, 1,   254, 127, 255};
    const auto source = rgba_image(5, 1, px);
    const auto packed = converted(source, PixelFormat::rgb565);
    REQUIRE(packed);
    const auto back = converted(packed, PixelFormat::rgba8888);
    REQUIRE(back);
    REQUIRE(back->width() == 5);
    for (int x = 0; x < 5; ++x) {
        const auto before = pixel(*source, x, 0);
        const auto after = pixel(*back, x, 0);
        CHECK(std::abs(static_cast<int>(before.r) - static_cast<int>(after.r)) <= 4);
        CHECK(std::abs(static_cast<int>(before.g) - static_cast<int>(after.g)) <= 2);
        CHECK(std::abs(static_cast<int>(before.b) - static_cast<int>(after.b)) <= 4);
        CHECK(after.a == before.a);
    }
}

TEST_CASE("from_rgb565 needs exactly one word per pixel") {
    CHECK_FALSE(Image::from_rgb565(2, 2, std::vector<std::uint16_t>(3, 0)));
    CHECK_FALSE(Image::from_rgb565(0, 2, {}));

    const auto image = Image::from_rgb565(2, 1, {0xF800, 0x001F});
    REQUIRE(image.has_value());
    CHECK((*image)->width() == 2);
    CHECK((*image)->height() == 1);
    CHECK((*image)->format() == PixelFormat::rgb565);

    const auto view = view_of(**image);
    CHECK(view.format == SourceFormat::rgb565);
    CHECK(view.width == 2);
    CHECK(view.height == 1);
    CHECK(view.stride == 4);
    CHECK(view.bytes.size() == 4U);
    const auto packed = words(**image);
    REQUIRE(packed.size() == 2U);
    CHECK(packed[0] == 0xF800);
    CHECK(packed[1] == 0x001F);
}
