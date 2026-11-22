// How much of the frame changes between two frames, in the scope and the widget demo at the ESP32
// panel's size: the pixels and the 48-row bands a dirty-region renderer would redraw, against the
// whole frame today. A draw list that is the same object in two snapshots did not change (the
// per-widget cache in layout_flatten shares it); one that is not did. With a double-buffered
// panel the back buffer is two frames old, so each frame redraws its own changes and the
// previous frame's. The modelled copy time is bytes / kPsramMBps, the board's measured PSRAM
// copy rate (~58 MB/s); it is a model, not a measurement of the board.
// Run: ./builddir/benchmarks/bench_dirty_area
#include <embedded/audio_scope.hpp>
#include <embedded/widgets_app.hpp>
#include <prism/app/widget_tree.hpp>
#include <prism/raster/canvas.hpp>
#include <prism/raster/draw_snapshot.hpp>
#include <prism/raster/fonts.hpp>
#include <prism/render/damage.hpp>

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace {

using prism::render::SceneSnapshot;
using Clock = std::chrono::steady_clock;

constexpr int W = 800, H = 480, kBandRows = 48, kBands = H / kBandRows;
constexpr double kPsramMBps = 58.0;

using prism::render::PixelRect;

struct Mask {
    std::vector<bool> px = std::vector<bool>(std::size_t(W) * H);
    void add(const PixelRect& r)
    {
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x)
                px[std::size_t(y) * W + x] = true;
    }
    void add(const Mask& o)
    {
        for (std::size_t i = 0; i < px.size(); ++i)
            px[i] = px[i] || o.px[i];
    }
    std::size_t pixels() const { return std::count(px.begin(), px.end(), true); }
    // Bands with at least one dirty pixel: bit k of the result.
    int bands() const
    {
        int n = 0;
        for (int k = 0; k < kBands; ++k) {
            const auto first = px.begin() + std::size_t(k) * kBandRows * W;
            n += std::any_of(first, first + std::size_t(kBandRows) * W, [](bool b) { return b; });
        }
        return n;
    }
    std::vector<int> dirty_bands() const
    {
        std::vector<int> out;
        for (int k = 0; k < kBands; ++k) {
            const auto first = px.begin() + std::size_t(k) * kBandRows * W;
            if (std::any_of(first, first + std::size_t(kBandRows) * W, [](bool b) { return b; }))
                out.push_back(k);
        }
        return out;
    }
    // Rows with a dirty pixel: what a copy of whole rows moves.
    int rows() const
    {
        int n = 0;
        for (int y = 0; y < H; ++y) {
            const auto first = px.begin() + std::size_t(y) * W;
            n += std::any_of(first, first + W, [](bool b) { return b; });
        }
        return n;
    }
};

double draw_bands_ms(const SceneSnapshot& snap, const prism::raster::Fonts& fonts, const Mask& m)
{
    std::vector<std::uint32_t> pixels(std::size_t(W) * kBandRows);
    prism::raster::Canvas band{pixels, W, kBandRows, W, 0};
    constexpr int kRepeats = 10;   // one pass is too short and too cold to time
    const std::vector<int> dirty = m.dirty_bands();
    const auto t0 = Clock::now();
    for (int r = 0; r < kRepeats; ++r)
        for (const int k : dirty) {
            band.retarget(pixels, k * kBandRows);
            band.clear(snap.window.background);
            prism::raster::draw_band(band, snap, fonts);
        }
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / kRepeats;
}

double copy_ms(std::size_t bytes) { return double(bytes) / (kPsramMBps * 1000.0); }

struct Totals {
    double pixels = 0, bands = 0, rows = 0, draw_ms = 0;
    int frames = 0;
};

void report(const char* scene, const Totals& t, double full_draw_ms)
{
    const double n = t.frames;
    const double full_px = double(W) * H;
    fmt::print("{}\n", scene);
    fmt::print("  full redraw:   {:6.0f} px (100%)  {:2d} bands  draw {:5.2f} ms  copy {:5.2f} ms\n", full_px,
               kBands, full_draw_ms, copy_ms(std::size_t(full_px) * 2));
    fmt::print("  dirty (2 frames): {:6.0f} px ({:4.1f}%)  {:4.1f} bands  draw {:5.2f} ms  copy {:5.2f} ms"
               " (whole rows {:5.2f} ms)\n",
               t.pixels / n, 100 * t.pixels / n / full_px, t.bands / n, t.draw_ms / n,
               copy_ms(std::size_t(t.pixels / n) * 2), copy_ms(std::size_t(t.rows / n) * W * 2));
}

// Runs `frames` of `step` and `build`, measuring each frame against the one before it.
template <class Model, class Step>
void run_scene(const char* name, Model& model, prism::raster::Fonts& fonts, int frames, Step step)
{
    prism::app::WidgetTree tree(model);
    tree.set_text_measurer(&fonts);
    std::unique_ptr<SceneSnapshot> prev;
    Mask last_mask;
    prism::render::DamageScratch scratch;
    prism::render::Damage damage;
    Totals t;
    double full_ms = 0;
    for (int f = 0; f < frames; ++f) {
        step(f);
        auto now = tree.build_snapshot(float(W), float(H), 1);
        tree.clear_dirty();
        if (prev) {
            Mask mine;
            prism::render::damage_between(prev.get(), *now, W, H, scratch, damage);
            if (damage.full)
                mine.add({0, 0, W, H});
            for (const PixelRect& r : damage.rects)
                mine.add(r);
            Mask both = mine;
            both.add(last_mask);
            t.pixels += double(both.pixels());
            t.bands += both.bands();
            t.rows += both.rows();
            t.draw_ms += draw_bands_ms(*now, fonts, both);
            Mask all;
            all.add({0, 0, W, H});
            full_ms += draw_bands_ms(*now, fonts, all);
            ++t.frames;
            last_mask = std::move(mine);
        }
        prev = std::move(now);
    }
    report(name, t, full_ms / t.frames);
}

struct ToneSource final : prism_examples::SampleSource {
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
};

} // namespace

int main()
{
    prism::raster::Fonts fonts;
    constexpr int kFrames = 30;
    {
        ToneSource source;
        prism_examples::ScopeState state{source};
        prism_examples::ScopeModel model;
        prism_examples::scope_bind(model, state);
        run_scene("scope: new samples every frame", model, fonts, kFrames, [&](int) {
            source.left = 533;
            prism_examples::scope_tick(model, state);
        });
    }
    {
        prism_examples::WidgetsModel model;
        run_scene("widgets: nothing changes", model, fonts, kFrames, [](int) {});
    }
    {
        prism_examples::WidgetsModel model;
        run_scene("widgets: the timing line changes every frame", model, fonts, kFrames, [&](int f) {
            model.stats.mutate([f](prism::ui::Label<>& l) { l.value = "frame " + std::to_string(f); });
        });
    }
    {
        prism_examples::WidgetsModel model;
        run_scene("widgets: the slider moves every frame", model, fonts, kFrames, [&](int f) {
            model.volume.mutate([f](prism::ui::Slider<>& s) { s.value = (f % 20) / 20.0; });
        });
    }
}
