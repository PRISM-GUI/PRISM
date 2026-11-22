#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <embedded/audio_scope.hpp>
#include <embedded/widgets_app.hpp>

#include <prism/app/widget_tree.hpp>
#include <prism/raster/canvas.hpp>
#include <prism/raster/draw_snapshot.hpp>
#include <prism/raster/fonts.hpp>
#include <prism/render/damage.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

using prism::app::WidgetTree;
using prism::render::Damage;
using prism::render::DamageScratch;
using prism::render::SceneSnapshot;

namespace {

constexpr int W = 800, H = 480;

using Pixels = std::vector<std::uint32_t>;

Pixels full_frame(const SceneSnapshot& snap, const prism::raster::Fonts& fonts)
{
    Pixels px(std::size_t{W} * H);
    prism::raster::Canvas canvas{px, W, H, W};
    canvas.clear(snap.window.background);
    prism::raster::draw(canvas, snap, fonts);
    return px;
}

// Draws `steps` frames of a model two ways and compares them: every frame whole, and every frame as
// the damage against the one before, over the previous frame's pixels.
template <class Model>
void check_damage_matches_full_redraw(Model& model, int frames, const std::function<void(int)>& step,
                                      bool expect_savings = true)
{
    prism::raster::Fonts fonts;
    WidgetTree tree(model);
    tree.set_text_measurer(&fonts);

    Pixels shown;
    std::unique_ptr<SceneSnapshot> previous;
    DamageScratch scratch;
    Damage damage;
    std::size_t saved_frames = 0;
    for (int f = 0; f < frames; ++f) {
        step(f);
        auto snap = tree.build_snapshot(float(W), float(H), std::uint64_t(f + 1));
        tree.clear_dirty();
        prism::render::damage_between(previous.get(), *snap, W, H, scratch, damage);
        if (previous) {
            prism::raster::Canvas canvas{shown, W, H, W};
            prism::raster::draw_damage(canvas, *snap, fonts, damage);
            saved_frames += !damage.full;
        } else {
            shown = full_frame(*snap, fonts);
        }
        const Pixels reference = full_frame(*snap, fonts);
        REQUIRE_MESSAGE(shown == reference, "frame " << f << ", full damage " << damage.full
                                                      << ", rects " << damage.rects.size());
        previous = std::move(snap);
    }
    if (expect_savings)
        CHECK(saved_frames > 0);
}

} // namespace

TEST_CASE("widgets demo: damage redraw gives the pixels of a full redraw") {
    prism_examples::WidgetsModel model;
    check_damage_matches_full_redraw(model, 12, [&](int f) {
        switch (f % 4) {
        case 0: model.stats.mutate([f](prism::ui::Label<>& l) { l.value = "frame " + std::to_string(f); }); break;
        case 1: model.volume.mutate([f](prism::ui::Slider<>& s) { s.value = (f % 20) / 20.0; }); break;
        case 2: model.counter.set(f); break;
        default: break;
        }
    });
}

TEST_CASE("audio scope: damage redraw gives the pixels of a full redraw") {
    struct Tone final : prism_examples::SampleSource {
        double t = 0;
        std::size_t left = 0;
        std::size_t read(std::span<float> out) override
        {
            const std::size_t n = std::min(out.size(), left);
            left -= n;
            for (std::size_t i = 0; i < n; ++i, t += 1.0 / 16000.0)
                out[i] = float(0.4 * std::sin(2 * 3.14159265 * 440 * t));
            return n;
        }
        float sample_rate() const override { return 16000.f; }
        const char* name() const override { return "tone"; }
    } source;
    prism_examples::ScopeState state{source};
    prism_examples::ScopeModel model;
    prism_examples::scope_bind(model, state);
    check_damage_matches_full_redraw(
        model, 6,
        [&](int) {
            source.left = 533;
            prism_examples::scope_tick(model, state);
        },
        false);
}

TEST_CASE("a scissored canvas clears and draws only inside the scissor") {
    Pixels px(100 * 50, 0xFF000000u);
    prism::raster::Canvas canvas{px, 100, 50, 100};
    using namespace prism::core;
    canvas.set_scissor(Rect{Point{X{10.f}, Y{10.f}}, Size{Width{20.f}, Height{10.f}}});
    canvas.clear(Color::rgba(255, 0, 0));
    canvas.fill_rect(Rect{Point{X{0.f}, Y{0.f}}, Size{Width{100.f}, Height{50.f}}}, Color::rgba(0, 255, 0));
    canvas.reset_clip();   // a draw list ending: the scissor stays
    canvas.fill_rect(Rect{Point{X{0.f}, Y{0.f}}, Size{Width{100.f}, Height{50.f}}}, Color::rgba(0, 0, 255));
    for (int y = 0; y < 50; ++y)
        for (int x = 0; x < 100; ++x) {
            const bool inside = x >= 10 && x < 30 && y >= 10 && y < 20;
            CHECK(px[std::size_t(y) * 100 + x] == (inside ? 0xFF0000FFu : 0xFF000000u));
        }
}
