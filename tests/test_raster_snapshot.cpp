#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/raster/canvas.hpp>
#include <prism/raster/draw_snapshot.hpp>
#include <prism/raster/fonts.hpp>

#include <embedded/audio_scope.hpp>
#include <embedded/widgets_app.hpp>

#include <prism/app/widget_tree.hpp>
#include <prism/core/field.hpp>
#include <prism/render/scene_snapshot.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

using namespace prism::core;
using prism::app::ViewBuilder;
using prism::app::WidgetTree;
using prism::render::SceneSnapshot;
using prism::render::TextCmd;

namespace {

constexpr int W = 480, H = 272;

// A small model with a hand-written view() (no reflection needed): a label, a button, a
// checkbox and a slider, laid out as the board's firmware builds them.
struct DemoModel {
    Field<std::string> title{"PRISM raster demo"};
    Field<int> counter{42};
    Field<prism::ui::Label<>> heading{{.value = "counter demo", .heading = true}};
    Field<prism::ui::Button> run{{"Run"}};
    Field<bool> enabled{true};
    Field<prism::ui::Slider<>> volume{{.value = 0.5}};

    void view(ViewBuilder& vb) {
        vb.vstack(title, counter, heading, run, enabled, volume);
    }
};

} // namespace

TEST_CASE("a laid-out snapshot draws into a 480x272 canvas") {
    DemoModel model;
    WidgetTree tree(model);
    prism::raster::Fonts fonts;   // the measurer: layout and drawing use the same metrics
    tree.set_text_measurer(&fonts);
    auto snap = tree.build_snapshot(W, H, 1);
    REQUIRE(snap != nullptr);
    tree.clear_dirty();

    // On the heap: 510 KB would overflow the 64 KB stack Emscripten gives main's thread.
    std::vector<std::uint32_t> fb(std::size_t{W} * H);
    prism::raster::Canvas canvas{fb, W, H, W};
    canvas.clear(snap->window.background);
    prism::raster::draw(canvas, *snap, fonts);

    // not all one colour
    const std::set<std::uint32_t> colors(fb.begin(), fb.end());
    CHECK(colors.size() > 1);
    // the primary button's fill is on screen: theme primary = rgba(40, 105, 180)
    CHECK(colors.contains(0xFF2869B4u));

    // The heading label's text pixels are inside the label's geometry rect. A heading draws no
    // box behind its text, so inside its text box every bright pixel is text ink.
    std::size_t list = snap->draw_lists.size();
    const TextCmd* heading = nullptr;
    for (std::size_t i = 0; i < snap->draw_lists.size(); ++i) {
        if (!snap->draw_lists[i]) continue;
        for (const auto& cmd : snap->draw_lists[i]->commands)
            if (const auto* t = std::get_if<TextCmd>(&cmd); t && t->text == "counter demo") {
                list = i;
                heading = t;
            }
    }
    REQUIRE(heading != nullptr);
    REQUIRE(list < snap->geometry.size());
    const Rect g = snap->geometry[list].second;

    const float tw = fonts.width(heading->text, heading->size, heading->font);
    const float th = fonts.line_height(heading->size, heading->font);
    const float bx0 = heading->origin.x.raw(), by0 = heading->origin.y.raw();
    const float bx1 = bx0 + tw, by1 = by0 + th;
    // the text box is laid out inside the widget's rect
    CHECK(g.origin.x.raw() <= bx0 + 0.5f);
    CHECK(g.origin.y.raw() <= by0 + 0.5f);
    CHECK(bx1 <= g.origin.x.raw() + g.extent.w.raw() + 1.f);
    CHECK(by1 <= g.origin.y.raw() + g.extent.h.raw() + 1.f);

    int ink = 0;
    // scan the text box itself (pixel centres inside [bx0, bx1) x [by0, by1))
    const int x0 = std::max(0, static_cast<int>(std::ceil(bx0 - 0.5f)));
    const int x1 = std::min(W - 1, static_cast<int>(std::ceil(bx1 - 0.5f)) - 1);
    const int y0 = std::max(0, static_cast<int>(std::ceil(by0 - 0.5f)));
    const int y1 = std::min(H - 1, static_cast<int>(std::ceil(by1 - 0.5f)) - 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const std::uint32_t p = fb[static_cast<std::size_t>(y) * W + x];
            const int r = (p >> 16) & 0xFF, gg = (p >> 8) & 0xFF, b = p & 0xFF;
            if (r > 150 && gg > 150 && b > 150) {   // heading text is theme.text (220, 220, 220)
                ++ink;
                INFO("ink pixel at ", x, ",", y);
                CHECK(g.contains(Point{X{static_cast<float>(x) + 0.5f},
                                        Y{static_cast<float>(y) + 0.5f}}));
            }
        }
    CHECK(ink > 0);   // the text actually drew
}

namespace {

using prism::raster::BasicCanvas;
using prism::raster::Fonts;

Rect rect(float x, float y, float w, float h) {
    return Rect{Point{X{x}, Y{y}}, Size{Width{w}, Height{h}}};
}

template <class P>
std::vector<typename P::storage> draw_whole(const SceneSnapshot& snap, const Fonts& fonts, int w, int h) {
    std::vector<typename P::storage> frame(std::size_t(w) * h);
    BasicCanvas<P> canvas{frame, w, h, w};
    canvas.clear(snap.window.background);
    prism::raster::draw(canvas, snap, fonts);
    return frame;
}

// As a board draws: one band buffer, reused for every band, copied into the frame after.
template <class P>
std::vector<typename P::storage> draw_in_bands(const SceneSnapshot& snap, const Fonts& fonts, int w, int h,
                                               int band_rows) {
    std::vector<typename P::storage> frame(std::size_t(w) * h);
    std::vector<typename P::storage> band_pixels(std::size_t(w) * band_rows);
    BasicCanvas<P> band{band_pixels, w, band_rows, w, 0};
    for (int y = 0; y < h; y += band_rows) {
        band.retarget(band_pixels, y);
        band.clear(snap.window.background);
        prism::raster::draw_band(band, snap, fonts);
        std::ranges::copy(band_pixels, frame.begin() + std::ptrdiff_t(y) * w);
    }
    return frame;
}

template <class P>
void check_bands_equal_whole(const SceneSnapshot& snap, const Fonts& fonts, int w, int h, int band_rows) {
    const auto whole = draw_whole<P>(snap, fonts, w, h);
    const auto banded = draw_in_bands<P>(snap, fonts, w, h, band_rows);
    const auto [first, _] = std::ranges::mismatch(whole, banded);
    const auto at = std::size_t(first - whole.begin());
    INFO("first difference at ", at % std::size_t(w), ",", at / std::size_t(w));
    CHECK(whole == banded);
    CHECK(std::set(whole.begin(), whole.end()).size() > 1);   // something was drawn
}

void check_all_formats(const SceneSnapshot& snap, const Fonts& fonts, int w, int h, int band_rows) {
    SUBCASE("ARGB8888") { check_bands_equal_whole<prism::raster::Argb8888>(snap, fonts, w, h, band_rows); }
    SUBCASE("RGB565") { check_bands_equal_whole<prism::raster::Rgb565>(snap, fonts, w, h, band_rows); }
}

// 533 samples (one 30 fps frame at 16 kHz) once, then dry.
struct OneFrameTone final : prism_examples::SampleSource {
    std::size_t left = 533;
    double t = 0;
    std::size_t read(std::span<float> out) override {
        const std::size_t n = std::min(out.size(), left);
        left -= n;
        for (std::size_t i = 0; i < n; ++i, t += 1.0 / 16000.0)
            out[i] = float(0.4 * std::sin(2 * 3.14159265 * 440 * t) + 0.1 * std::sin(2 * 3.14159265 * 3100 * t));
        return n;
    }
    float sample_rate() const override { return 16000.f; }
    const char* name() const override { return "tone"; }
};

std::shared_ptr<prism::render::DrawList> filled(Rect r, Color c) {
    auto dl = std::make_shared<prism::render::DrawList>();
    dl->filled_rect(r, c);
    return dl;
}

} // namespace

TEST_CASE("the widget demo drawn in 48-row bands equals the whole frame") {
    prism_examples::WidgetsModel model;
    WidgetTree tree(model);
    Fonts fonts;
    tree.set_text_measurer(&fonts);
    auto snap = tree.build_snapshot(800, 480, 1);
    REQUIRE(snap != nullptr);
    check_all_formats(*snap, fonts, 800, 480, 48);
}

TEST_CASE("the audio scope drawn in 48-row bands equals the whole frame") {
    OneFrameTone source;
    prism_examples::ScopeState state{source};
    prism_examples::ScopeModel model;
    prism_examples::scope_bind(model, state);
    prism_examples::scope_tick(model, state);
    WidgetTree tree(model);
    Fonts fonts;
    tree.set_text_measurer(&fonts);
    auto snap = tree.build_snapshot(800, 480, 1);
    REQUIRE(snap != nullptr);
    check_all_formats(*snap, fonts, 800, 480, 48);
}

// 8-row bands: the band [32, 40) holds part of the fill but misses the geometry rect (rows 10 to
// 30) by more than a band; only the margin brings the list into it.
TEST_CASE("a draw list up to kBandMargin outside its geometry rect is drawn in its bands") {
    SceneSnapshot snap;
    snap.geometry = {{1, rect(10, 10, 20, 20)}};
    snap.draw_lists = {filled(rect(10, 25, 20, 20), Color::rgba(200, 40, 40))};
    snap.z_order = {0};
    Fonts fonts;
    check_all_formats(snap, fonts, 64, 64, 8);
}

// A widget's list is wrapped in its allocated rect, which can be far larger than the drawn
// bounding box its geometry holds: the leading clip, not the geometry, bounds the list.
TEST_CASE("a list with a leading clip is drawn in every band its clip reaches") {
    SceneSnapshot snap;
    auto dl = std::make_shared<prism::render::DrawList>();
    dl->clip_push(Point{X{0}, Y{0}}, Size{Width{100}, Height{60}});
    dl->filled_rect(rect(0, 40, 64, 15), Color::rgba(200, 40, 40));
    dl->clip_pop();
    snap.geometry = {{1, rect(10, 10, 20, 20)}};
    snap.draw_lists = {dl};
    snap.z_order = {0};
    Fonts fonts;
    check_all_formats(snap, fonts, 64, 64, 8);
}

// A board's own drawing may clip a part, pop, and draw on: its first clip bounds nothing.
TEST_CASE("a list whose first clip is popped before its end falls back to its geometry") {
    SceneSnapshot snap;
    auto dl = std::make_shared<prism::render::DrawList>();
    dl->clip_push(Point{X{0}, Y{0}}, Size{Width{64}, Height{8}});
    dl->filled_rect(rect(0, 0, 64, 8), Color::rgba(40, 40, 200));
    dl->clip_pop();
    dl->filled_rect(rect(0, 40, 64, 8), Color::rgba(200, 40, 40));
    snap.geometry = {{1, rect(0, 0, 64, 48)}};
    snap.draw_lists = {dl};
    snap.z_order = {0};
    Fonts fonts;
    check_all_formats(snap, fonts, 64, 64, 8);
}

// A scroll view's clip entries (a ClipPush list over the viewport, a ClipPop list with an empty
// rect) miss most bands, yet the widgets between them are clipped by them in every band.
TEST_CASE("clip entries that miss a band still clip the lists drawn in it") {
    SceneSnapshot snap;
    auto push = std::make_shared<prism::render::DrawList>();
    push->clip_push(Point{X{0}, Y{0}}, Size{Width{64}, Height{8}});
    auto pop = std::make_shared<prism::render::DrawList>();
    pop->clip_pop();
    snap.geometry = {{1, rect(0, 0, 64, 8)}, {2, rect(0, 40, 64, 8)}, {0, rect(0, 0, 0, 0)},
                     {3, rect(0, 50, 64, 6)}};
    snap.draw_lists = {push, filled(rect(0, 40, 64, 8), Color::rgba(200, 40, 40)), pop,
                       filled(rect(0, 50, 64, 6), Color::rgba(40, 200, 40))};
    snap.z_order = {0, 1, 2, 3};
    Fonts fonts;
    check_all_formats(snap, fonts, 64, 64, 8);
}

// A snapshot built by hand may leave geometry empty: its lists are then drawn in every band.
TEST_CASE("a draw list with no geometry entry is drawn in every band") {
    SceneSnapshot snap;
    snap.draw_lists = {filled(rect(0, 40, 64, 8), Color::rgba(200, 40, 40))};
    snap.z_order = {0};
    Fonts fonts;
    check_all_formats(snap, fonts, 64, 64, 8);
}
