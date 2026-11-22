#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/raster/canvas.hpp>
#include <prism/raster/fonts.hpp>

#include <prism/render/draw_list.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace prism::core;
using prism::render::Image;

namespace {

constexpr std::uint32_t kCleared = 0xFF000000u;   // opaque black: "untouched"

template <int W, int H>
struct Fb {
    std::array<std::uint32_t, W * H> px{};
    prism::raster::Canvas canvas{px, W, H, W};
    Fb() { canvas.clear(Color::rgba(0, 0, 0)); }
    std::uint32_t at(int x, int y) const { return px[static_cast<std::size_t>(y) * W + x]; }
    bool touched(int x, int y) const { return at(x, y) != kCleared; }
    int count_touched() const {
        int n = 0;
        for (std::uint32_t p : px) if (p != kCleared) ++n;
        return n;
    }
};

Rect rect(float x, float y, float w, float h) {
    return Rect{Point{X{x}, Y{y}}, Size{Width{w}, Height{h}}};
}
Point pt(float x, float y) { return Point{X{x}, Y{y}}; }

} // namespace

TEST_CASE("fill_rect writes exactly its pixels (corners in, one past out)") {
    Fb<16, 16> fb;
    fb.canvas.fill_rect(rect(2, 3, 4, 5), Color::rgba(255, 0, 0));
    CHECK(fb.at(2, 3) == 0xFFFF0000u);   // top-left corner in
    CHECK(fb.at(5, 7) == 0xFFFF0000u);   // bottom-right corner in
    CHECK_FALSE(fb.touched(6, 3));       // one past the right edge out
    CHECK_FALSE(fb.touched(2, 8));       // one past the bottom edge out
    CHECK_FALSE(fb.touched(1, 3));
    CHECK(fb.count_touched() == 4 * 5);
}

TEST_CASE("the clip rect limits fill_rect, pop restores it") {
    Fb<16, 16> fb;
    fb.canvas.push_clip(rect(2, 2, 6, 6));
    fb.canvas.fill_rect(rect(0, 0, 16, 16), Color::rgba(255, 255, 255));
    CHECK(fb.touched(2, 2));
    CHECK(fb.touched(7, 7));
    CHECK_FALSE(fb.touched(1, 5));   // left of the clip
    CHECK_FALSE(fb.touched(8, 5));   // right of the clip ([2, 8) x [2, 8))
    CHECK(fb.count_touched() == 36);
    fb.canvas.pop_clip();
    fb.canvas.fill_rect(rect(0, 0, 1, 1), Color::rgba(0, 255, 0));
    CHECK(fb.touched(0, 0));
}

TEST_CASE("50% alpha over white blends to the expected value") {
    Fb<8, 8> fb;
    fb.canvas.clear(Color::rgba(255, 255, 255));
    fb.canvas.fill_rect(rect(0, 0, 4, 4), Color::rgba(0, 0, 0, 128));
    const std::uint32_t px = fb.at(1, 1);
    const int r = static_cast<int>((px >> 16) & 0xFF);
    const int g = static_cast<int>((px >> 8) & 0xFF);
    const int b = static_cast<int>(px & 0xFF);
    // 255 * (1 - 128/255) = 127.0
    CHECK(std::abs(r - 127) <= 1);
    CHECK(std::abs(g - 127) <= 1);
    CHECK(std::abs(b - 127) <= 1);
    CHECK((px >> 24) == 255);   // alpha stays opaque over an opaque destination
}

TEST_CASE("rounded_rect leaves the corner pixel outside the radius untouched") {
    Fb<16, 16> fb;
    fb.canvas.rounded_rect(rect(2, 2, 12, 12), Color::rgba(255, 255, 255), 4.f, 0.f);
    CHECK_FALSE(fb.touched(2, 2));   // centre (2.5, 2.5) is ~4.9 from the corner centre (6, 6) > 4
    CHECK(fb.touched(6, 2));         // top edge, past the corner arc
    CHECK(fb.touched(2, 6));         // left edge, past the corner arc
    CHECK(fb.touched(7, 7));         // middle
    CHECK(fb.count_touched() > 100); // most of the 12x12 box
}

TEST_CASE("a thickness-1 line from (0,0) to (9,0) covers 10 pixels") {
    Fb<16, 16> fb;
    fb.canvas.line(pt(0, 0), pt(9, 0), Color::rgba(255, 255, 255), 1.f);
    for (int x = 0; x <= 9; ++x) CHECK(fb.touched(x, 0));
    CHECK(fb.count_touched() == 10);
    CHECK_FALSE(fb.touched(0, 1));
}

TEST_CASE("filled circle: centre pixel set, outside not") {
    Fb<16, 16> fb;
    fb.canvas.circle(pt(8, 8), 5.f, Color::rgba(255, 255, 255), 0.f);
    CHECK(fb.touched(8, 8));
    CHECK(fb.touched(12, 8));    // centre (12.5, 8.5): dist ~4.5 <= 5
    CHECK_FALSE(fb.touched(14, 8));   // dist 6.5 > 5
    CHECK_FALSE(fb.touched(0, 0));
    CHECK(fb.count_touched() > 60);   // ~pi * 25 = 78
}

TEST_CASE("filled_polygon fills a triangle within 10% of its analytic area") {
    Fb<24, 24> fb;
    const Point tri[3] = {pt(0, 0), pt(20, 0), pt(0, 20)};
    fb.canvas.filled_polygon(tri, Color::rgba(255, 255, 255));
    const int n = fb.count_touched();
    CHECK(std::abs(n - 200) <= 20);   // area = 20 * 20 / 2 = 200
}

TEST_CASE("image scaled 2x maps texels (nearest neighbour)") {
    const std::vector<std::uint8_t> rgba = {
        255, 0, 0, 255,   0, 255, 0, 255,     // red, green
        0, 0, 255, 255,   255, 255, 0, 255,   // blue, yellow
    };
    auto img = Image::from_rgba(2, 2, rgba);
    REQUIRE(img.has_value());
    Fb<8, 8> fb;
    fb.canvas.image(**img, rect(0, 0, 4, 4), 255, Color::rgba(255, 255, 255));
    CHECK(fb.at(0, 0) == 0xFFFF0000u);
    CHECK(fb.at(1, 1) == 0xFFFF0000u);   // the 2x2 block of the top-left texel
    CHECK(fb.at(2, 0) == 0xFF00FF00u);
    CHECK(fb.at(3, 1) == 0xFF00FF00u);
    CHECK(fb.at(0, 2) == 0xFF0000FFu);
    CHECK(fb.at(3, 3) == 0xFFFFFF00u);
    CHECK_FALSE(fb.touched(4, 0));   // outside the dest rect
}

namespace {

constexpr int kFrameW = 120, kFrameH = 100, kBandH = 48;

const prism::raster::Fonts& test_fonts() {
    static const prism::raster::Fonts fonts;
    return fonts;
}

// A band walks only the samples of a hairline that can reach its rows: lines that are steep,
// shallow, drawn upwards, on a band edge (row 47.5 rounds to 48), or running off the frame.
template <class P>
void draw_hairlines(prism::raster::BasicCanvas<P>& c) {
    const Color ink = Color::rgba(250, 250, 250);
    c.line(pt(3, 2), pt(9, 97), ink, 1.f);
    c.line(pt(1, 44.3f), pt(118, 52.7f), ink, 1.f);
    c.line(pt(117, 95.5f), pt(20, 3.5f), ink, 1.f);
    c.line(pt(0, 47.5f), pt(119, 47.5f), ink, 1.f);
    c.line(pt(50, -30), pt(65, 140), ink, 1.f);
    c.line(pt(100, 47.4f), pt(101, 48.6f), ink, 1.f);
}

template <class P>
void draw_scene(prism::raster::BasicCanvas<P>& c) {
    const std::vector<std::uint8_t> rgba = {
        255, 0, 0, 255,   0, 255, 0, 200,
        0, 0, 255, 255,   255, 255, 0, 128,
    };
    static const auto img = Image::from_rgba(2, 2, rgba);
    c.clear(Color::rgba(20, 30, 40));
    c.text(test_fonts(), "Band edge Wg", pt(4, 40), 16.f, Color::rgba(255, 255, 255), 0.f,
           prism::render::TextAnchor::TopLeft, prism::render::FontStyle{});
    c.line(pt(5, 10), pt(110, 90), Color::rgba(255, 0, 0, 180), 4.f);
    draw_hairlines(c);
    c.rounded_rect(rect(10, 40, 100, 20), Color::rgba(0, 200, 0, 140), 8.f, 0.f);
    c.rounded_rect(rect(12, 30, 90, 40), Color::rgba(0, 0, 250), 6.f, 3.f);
    const Point poly[5] = {pt(70, 20), pt(115, 45), pt(100, 80), pt(60, 75), pt(55, 50)};
    c.filled_polygon(poly, Color::rgba(200, 100, 0, 120));
    c.circle(pt(60, 49), 12.f, Color::rgba(250, 250, 0, 200), 0.f);
    c.circle(pt(30, 70), 9.f, Color::rgba(0, 250, 250), 2.f);
    c.image(**img, rect(80.5f, 44.3f, 13.f, 11.f), 230, Color::rgba(255, 255, 255));
    c.push_clip(rect(40, 45.5f, 50, 10));
    c.fill_rect(rect(0, 0, 120, 100), Color::rgba(255, 0, 255, 90));
    c.pop_clip();
}

template <class P>
void check_bands_equal_whole() {
    using pixel = typename P::storage;
    std::vector<pixel> whole(kFrameW * kFrameH), banded(kFrameW * kFrameH);
    prism::raster::BasicCanvas<P> full{whole, kFrameW, kFrameH, kFrameW};
    draw_scene(full);
    for (int first = 0; first < kFrameH; first += kBandH) {
        const int h = std::min(kBandH, kFrameH - first);
        std::vector<pixel> band(static_cast<std::size_t>(kFrameW) * h);
        prism::raster::BasicCanvas<P> c{band, kFrameW, h, kFrameW, first};
        draw_scene(c);
        std::ranges::copy(band, banded.begin() + static_cast<std::ptrdiff_t>(first) * kFrameW);
    }
    int differing = 0;
    for (std::size_t i = 0; i < whole.size(); ++i)
        if (whole[i] != banded[i]) ++differing;
    CHECK(differing == 0);
}

// Hairlines with fractional ends at every angle, from a centre off the pixel grid.
void draw_fan(prism::raster::Canvas& c) {
    c.clear(Color::rgba(0, 0, 0));
    for (int k = 0; k < 64; ++k) {
        const float a = float(k) * 0.0981748f;   // 2 pi / 64
        c.line(pt(40.3f, 30.7f), pt(40.3f + 37.1f * std::cos(a), 30.7f + 33.9f * std::sin(a)),
               Color::rgba(255, 255, 255), 1.f);
    }
}

} // namespace

TEST_CASE("hairlines drawn in bands of 1 to 7 rows equal the frame drawn whole") {
    constexpr int w = 80, h = 64;
    std::vector<std::uint32_t> whole(w * h);
    prism::raster::Canvas full{whole, w, h, w};
    draw_fan(full);
    for (int rows = 1; rows <= 7; ++rows) {
        std::vector<std::uint32_t> banded(w * h);
        for (int first = 0; first < h; first += rows) {
            const int n = std::min(rows, h - first);
            prism::raster::Canvas band{std::span(banded).subspan(std::size_t(first) * w, std::size_t(n) * w),
                                       w, n, w, first};
            draw_fan(band);
        }
        INFO("bands of ", rows, " rows");
        CHECK(whole == banded);
    }
}

TEST_CASE("a frame drawn in bands equals the frame drawn whole") {
    SUBCASE("Argb8888") { check_bands_equal_whole<prism::raster::Argb8888>(); }
    SUBCASE("Rgb565") { check_bands_equal_whole<prism::raster::Rgb565>(); }
}

TEST_CASE("a band canvas clips to its rows and retargets to the next band") {
    std::array<std::uint32_t, 8 * 4> band{};
    prism::raster::Canvas c{band, 8, 4, 8, 10};
    CHECK(c.clip().origin.y.raw() == 10.f);
    CHECK(c.clip().extent.h.raw() == 4.f);
    c.clear(Color::rgba(0, 0, 0));
    c.fill_rect(rect(0, 0, 8, 11), Color::rgba(255, 255, 255));   // frame rows 0-10: only row 10 here
    CHECK(band[0] == 0xFFFFFFFFu);
    CHECK(band[8] == kCleared);
    c.retarget(band, 14);
    CHECK(c.clip().origin.y.raw() == 14.f);
}

namespace {

using Pts = std::vector<Point>;

Pts sine_trace(float amplitude, float step) {
    Pts pts;
    for (float x = 4.f; x < 196.f; x += step)
        pts.push_back(pt(x, 30.f + amplitude * std::sin(x * 0.15f)));
    return pts;
}

// The same points drawn one segment at a time: what a polyline is defined as.
template <class F>
void draw_segments(F& fb, const Pts& pts, Color c, float thickness) {
    for (std::size_t i = 1; i < pts.size(); ++i) fb.canvas.line(pts[i - 1], pts[i], c, thickness);
}

} // namespace

TEST_CASE("a thick polyline is its segments drawn one by one") {
    const Color white = Color::rgba(255, 255, 255);
    for (const float thickness : {1.5f, 2.f, 3.f}) {
        for (const float step : {0.5f, 1.f, 3.f}) {
            const Pts pts = sine_trace(20.f, step);
            Fb<200, 60> whole, apart;
            whole.canvas.polyline(pts, white, thickness);
            draw_segments(apart, pts, white, thickness);
            int differ = 0;
            for (int y = 0; y < 60; ++y)
                for (int x = 0; x < 200; ++x) differ += whole.touched(x, y) != apart.touched(x, y);
            CAPTURE(thickness);
            CAPTURE(step);
            CHECK(differ <= apart.count_touched() / 8);
        }
    }
}

TEST_CASE("a steep rise in a thick polyline leaves no gap") {
    const Color white = Color::rgba(255, 255, 255);
    Fb<40, 60> fb;
    fb.canvas.polyline(Pts{pt(5.f, 5.f), pt(20.f, 5.f), pt(21.f, 50.f), pt(35.f, 50.f)}, white, 2.f);
    for (int y = 6; y < 50; ++y) {
        bool any = false;
        for (int x = 18; x < 24; ++x) any = any || fb.touched(x, y);
        CAPTURE(y);
        CHECK(any);
    }
}

TEST_CASE("a steep spike in a thick polyline does not overshoot its end points") {
    const Color white = Color::rgba(255, 255, 255);
    for (const float thickness : {1.5f, 2.f, 4.f}) {
        Fb<30, 80> fb;
        fb.canvas.polyline(Pts{pt(5.f, 20.f), pt(10.f, 20.f), pt(11.f, 60.f), pt(12.f, 20.f), pt(20.f, 20.f)},
                           white, thickness);
        int lowest = -1, highest = 80;
        for (int y = 0; y < 80; ++y)
            for (int x = 0; x < 30; ++x)
                if (fb.touched(x, y)) { lowest = std::max(lowest, y); highest = std::min(highest, y); }
        CAPTURE(thickness);
        CHECK(lowest <= 60 + thickness);
        CHECK(highest >= 20 - thickness);
    }
}

// The square-ended strip as a polygon: what a thick line was drawn as before it became one run
// per pixel column.
template <class F>
void draw_strip(F& fb, Point from, Point to, Color c, float thickness) {
    const float dx = to.x.raw() - from.x.raw(), dy = to.y.raw() - from.y.raw();
    const float len = std::sqrt(dx * dx + dy * dy);
    const float nx = -dy / len * thickness / 2.f, ny = dx / len * thickness / 2.f;
    const Point quad[4] = {pt(from.x.raw() + nx, from.y.raw() + ny), pt(to.x.raw() + nx, to.y.raw() + ny),
                           pt(to.x.raw() - nx, to.y.raw() - ny), pt(from.x.raw() - nx, from.y.raw() - ny)};
    fb.canvas.filled_polygon(quad, c);
}

TEST_CASE("a thick line covers the pixels of its strip, in every direction and thickness") {
    const Color white = Color::rgba(255, 255, 255);
    int segments = 0, worst = 0;
    for (const float thickness : {1.5f, 2.f, 3.f, 6.f}) {
        for (int angle = 0; angle < 360; angle += 15) {
            for (const float length : {0.5f, 3.f, 40.f}) {
                const float a = float(angle) * 3.14159265f / 180.f;
                const Point from = pt(50.3f, 50.6f);
                const Point to = pt(50.3f + length * std::cos(a), 50.6f + length * std::sin(a));
                Fb<100, 100> line, strip;
                line.canvas.line(from, to, white, thickness);
                draw_strip(strip, from, to, white, thickness);
                int differ = 0;
                for (int i = 0; i < 100 * 100; ++i)
                    differ += (line.px[std::size_t(i)] != kCleared) != (strip.px[std::size_t(i)] != kCleared);
                ++segments;
                worst = std::max(worst, differ);
            }
        }
    }
    CHECK(segments > 0);
    CHECK(worst <= 2);   // a pixel centre on an edge may fall either way
}

TEST_CASE("a polyline that goes back in x is its segments, pixel for pixel") {
    const Color white = Color::rgba(255, 255, 255);
    const Pts pts{pt(10.f, 10.f), pt(60.f, 30.f), pt(30.f, 45.f), pt(90.f, 50.f), pt(70.f, 5.f)};
    Fb<100, 60> whole, apart;
    whole.canvas.polyline(pts, white, 2.f);
    draw_segments(apart, pts, white, 2.f);
    CHECK(whole.px == apart.px);
}

TEST_CASE("a thin polyline (one pixel or less) is its segments, pixel for pixel") {
    const Color white = Color::rgba(255, 255, 255);
    const Pts pts = sine_trace(20.f, 1.f);
    Fb<200, 60> whole, apart;
    whole.canvas.polyline(pts, white, 1.f);
    draw_segments(apart, pts, white, 1.f);
    CHECK(whole.px == apart.px);
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
// The bounds check is always on, so the child is a fork whose death is inspected with waitpid.
namespace {
template <typename F>
bool aborts(F f) {
    const pid_t pid = fork();
    if (pid == 0) { f(); _exit(0); }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
} // namespace

TEST_CASE("Canvas rejects a pixel span smaller than stride * height") {
    std::array<std::uint32_t, 4 * 4 - 1> too_short{};
    CHECK(aborts([&] { prism::raster::Canvas c{too_short, 4, 4, 4}; }));
    std::array<std::uint32_t, 4 * 4> ok{};
    CHECK_FALSE(aborts([&] { prism::raster::Canvas c{ok, 4, 4, 4}; }));
}

TEST_CASE("Canvas::retarget rejects a pixel span smaller than stride * height") {
    std::array<std::uint32_t, 4 * 4> ok{};
    std::array<std::uint32_t, 4 * 4 - 1> too_short{};
    prism::raster::Canvas c{ok, 4, 4, 4};
    CHECK(aborts([&] { c.retarget(too_short); }));
}
#endif
