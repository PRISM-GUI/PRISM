// The audio scope (examples/embedded) at the ESP32 panel's size, on the host: what one frame
// costs to record (the snapshot) and to draw, draw list by draw list, with the command kinds of
// the slowest, and the same frame drawn in 48-row bands. Run: ./builddir/benchmarks/bench_scope_frame
#include <embedded/audio_scope.hpp>
#include <prism/app/widget_tree.hpp>
#include <prism/raster/canvas.hpp>
#include <prism/raster/draw_snapshot.hpp>
#include <prism/raster/fonts.hpp>

#include <fmt/core.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ranges>
#include <variant>
#include <vector>

namespace {

constexpr int W = 800, H = 480;
using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// One frame's worth of samples at 30 fps per tick (533 at 16 kHz), then dry: the scope reads
// its source until it is empty.
struct ToneSource final : prism_examples::SampleSource {
    double t = 0;
    std::size_t left = 0;   // refilled by the benchmark before each tick
    std::size_t read(std::span<float> out) override
    {
        const std::size_t n = std::min(out.size(), left);
        left -= n;
        for (std::size_t i = 0; i < n; ++i, t += 1.0 / 16000.0)
            out[i] = float(0.4 * std::sin(2 * 3.14159265 * 440 * t) + 0.1 * std::sin(2 * 3.14159265 * 3100 * t));
        return n;
    }
    float sample_rate() const override { return 16000.f; }
    const char* name() const override { return "tone"; }
};

struct Timed {
    double ms;
    std::size_t index;
};

constexpr int kBandRows = 48;

// The frame as the ESP32 draws it: one 48-row band buffer, cleared and drawn ten times.
double draw_banded(const prism::render::SceneSnapshot& snap, const prism::raster::Fonts& fonts)
{
    constexpr int kRepeats = 50;
    std::vector<std::uint32_t> band_pixels(std::size_t(W) * kBandRows);
    prism::raster::Canvas band{band_pixels, W, kBandRows, W, 0};
    const auto t0 = Clock::now();
    for (int r = 0; r < kRepeats; ++r)
        for (int y = 0; y < H; y += kBandRows) {
            band.retarget(band_pixels, y);
            band.clear(snap.window.background);
            prism::raster::draw_band(band, snap, fonts);
        }
    return ms_since(t0) / kRepeats;
}

} // namespace

int main()
{
    ToneSource source;
    prism_examples::ScopeState state{source};
    prism_examples::ScopeModel model;
    prism_examples::scope_bind(model, state);
    prism::raster::Fonts fonts;
    prism::app::WidgetTree tree(model);
    tree.set_text_measurer(&fonts);
    std::vector<std::uint32_t> pixels(std::size_t(W) * H);
    prism::raster::Canvas canvas{pixels, W, H, W};

    constexpr int kFrames = 50;
    double tick = 0, record = 0, draw = 0;
    std::vector<double> per_list;
    std::shared_ptr<const prism::render::SceneSnapshot> snap;
    for (int f = 0; f < kFrames; ++f) {
        auto t0 = Clock::now();
        source.left = 533;
        prism_examples::scope_tick(model, state);   // new samples: both plots change
        tick += ms_since(t0);
        t0 = Clock::now();
        snap = tree.build_snapshot(float(W), float(H), 1);
        tree.clear_dirty();
        record += ms_since(t0);
        t0 = Clock::now();
        canvas.clear(snap->window.background);
        prism::raster::draw(canvas, *snap, fonts);
        draw += ms_since(t0);
    }
    fmt::print("per frame: tick (FFT) {:.2f} ms, record {:.2f} ms, draw {:.2f} ms ({} draw lists)\n",
               tick / kFrames, record / kFrames,
               draw / kFrames, snap->draw_lists.size());

    fmt::print("banded ({} bands of {}): draw {:.2f} ms\n", H / kBandRows, kBandRows,
               draw_banded(*snap, fonts));

    std::vector<Timed> lists;
    for (std::size_t i = 0; i < snap->draw_lists.size(); ++i) {
        if (!snap->draw_lists[i])
            continue;
        prism::render::SceneSnapshot one;
        one.window = snap->window;
        one.draw_lists = {snap->draw_lists[i]};
        one.z_order = {0};
        const auto t0 = Clock::now();
        for (int r = 0; r < 20; ++r)
            prism::raster::draw(canvas, one, fonts);
        lists.push_back({ms_since(t0) / 20, i});
    }
    std::ranges::sort(lists, std::greater{}, &Timed::ms);
    for (const Timed& t : lists | std::views::take(5)) {
        std::array<int, std::variant_size_v<prism::render::DrawCmd>> kinds{};
        for (const auto& cmd : snap->draw_lists[t.index]->commands)
            ++kinds[cmd.index()];
        fmt::print("list {:3}: {:.3f} ms, commands by kind {}\n", t.index, t.ms,
                   fmt::join(kinds, " "));
    }
}
