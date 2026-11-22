// frame_app: a model runs on a framebuffer target (here a vector of pixels and a scripted
// touch) through the frame runtime, and returns once the app quits.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "alloc_counter.hpp"

#include <prism/platform/alloc_count.hpp>
#include <prism/platform/frame_app.hpp>
#include <prism/platform/frame_stats.hpp>

#include <prism/app/widget_tree.hpp>
#include <prism/core/connection.hpp>
#include <prism/core/field.hpp>
#include <prism/raster/canvas.hpp>
#include <prism/raster/draw_snapshot.hpp>
#include <prism/raster/fonts.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// The platform hooks the runtime reads, defined by whoever links it: here, operator new's count
// (alloc_counter.hpp) and no CPU or memory counters.
std::uint32_t prism::platform::allocation_count()
{
    return static_cast<std::uint32_t>(prism::test::allocations());
}

std::optional<prism::platform::SystemCounters> prism::platform::system_counters()
{
    return std::nullopt;
}

using namespace prism::core;
using namespace std::chrono_literals;
using prism::platform::FrameContext;

namespace {

constexpr int W = 120, H = 100;

struct Counter {
    Field<prism::ui::Button> plus{{"+1"}};
    Field<int> count{0};
    void view(prism::app::ViewBuilder& vb) { vb.vstack(plus, count); }
};

// A scrolling list of buttons: more rows than the frame holds.
struct ButtonList {
    std::array<Field<prism::ui::Button>, 12> rows;
    ButtonList() {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            auto b = rows[i].get();
            b.text = "row " + std::to_string(i);
            rows[i].set(b);
        }
    }
    void view(prism::app::ViewBuilder& vb)
    {
        vb.scroll([&] {
            for (auto& r : rows)
                vb.widget(r);
        });
    }
};

struct MemorySink final : prism::platform::FrameSink {
    std::vector<std::uint32_t> pixels = std::vector<std::uint32_t>(std::size_t(W) * H);
    std::atomic<int> presents{0};
    std::chrono::milliseconds present_time{0};

    std::span<std::uint32_t> back() override { return pixels; }
    void present() override
    {
        std::this_thread::sleep_for(present_time);
        ++presents;
    }
    Size size() const override { return {Width{float(W)}, Height{float(H)}}; }
};

// A sink that clears its frame itself (as a board's 2D engine does) and records each clear.
struct ClearingSink final : prism::platform::FrameSink {
    std::vector<std::uint32_t> pixels = std::vector<std::uint32_t>(std::size_t(W) * H, 0x12345678u);
    std::atomic<int> presents{0};
    std::atomic<int> clears{0};
    std::atomic<std::uint32_t> cleared_with{0};

    std::span<std::uint32_t> back() override { return pixels; }
    bool clear(std::uint32_t argb) override
    {
        cleared_with = argb;
        ++clears;
        return true;   // done: and it leaves the pixels alone, so a CPU clear would show
    }
    void present() override { ++presents; }
    Size size() const override { return {Width{float(W)}, Height{float(H)}}; }
};

// A sink that keeps what it shows: one buffer (age 1) or two that flip (age 2). It says so through
// buffer_age(), counts the clears the CPU is asked to do (only a whole-frame draw clears), and
// keeps the buffer on screen for the test to read.
struct AgedSink final : prism::platform::FrameSink {
    explicit AgedSink(int buffer_age) : age(buffer_age), buffers(std::size_t(buffer_age)) {
        for (auto& b : buffers)
            b.assign(std::size_t(W) * H, 0x12345678u);   // nothing like a frame: a skipped pixel shows
    }
    int age;
    std::vector<std::vector<std::uint32_t>> buffers;
    std::size_t back_index = 0, shown_index = 0;
    std::atomic<int> presents{0};
    std::atomic<int> clears{0};

    std::span<std::uint32_t> back() override { return buffers[back_index]; }
    bool clear(std::uint32_t) override { ++clears; return false; }
    void present() override
    {
        shown_index = back_index;
        back_index = (back_index + 1) % buffers.size();
        ++presents;
    }
    Size size() const override { return {Width{float(W)}, Height{float(H)}}; }
    int buffer_age() const override { return age; }
    const std::vector<std::uint32_t>& shown() const { return buffers[shown_index]; }
};

// Two numbers one under the other: the test changes them in turn, so two frames in a row damage
// different places.
struct TwoCounters {
    Field<int> first{0};
    Field<int> second{0};
    void view(prism::app::ViewBuilder& vb) { vb.vstack(first, second); }
};

// A model at 120x100, drawn whole.
template <class Model>
std::vector<std::uint32_t> whole_frame(Model& model, float scale = 1.f)
{
    prism::app::WidgetTree tree(model);
    prism::raster::Fonts fonts;
    tree.set_text_measurer(&fonts);
    const auto snap = tree.build_snapshot(float(W) / scale, float(H) / scale, 1);
    std::vector<std::uint32_t> px(std::size_t(W) * H);
    prism::raster::Canvas canvas{px, W, H, W};
    canvas.set_ui_scale(scale);
    canvas.clear(snap->window.background);
    prism::raster::draw(canvas, *snap, fonts);
    return px;
}

std::vector<std::uint32_t> reference_frame(int first, int second)
{
    TwoCounters model;
    model.first.set(first);
    model.second.set(second);
    return whole_frame(model);
}

// Whether `frame` is the counter at some count up to `max`.
bool is_a_whole_counter_frame(const std::vector<std::uint32_t>& frame, int max)
{
    for (int count = 0; count <= max; ++count) {
        Counter model;
        model.count.set(count);
        if (whole_frame(model) == frame)
            return true;
    }
    return false;
}

// Whether `frame` shows the numbers at some state the test passed through (they differ by at most
// one, the first changing first): no pixel of an older frame is left.
bool is_a_whole_frame(const std::vector<std::uint32_t>& frame, int max)
{
    for (int first = 0; first <= max; ++first)
        for (int second = std::max(0, first - 1); second <= first; ++second)
            if (reference_frame(first, second) == frame)
                return true;
    return false;
}

// An AgedSink whose 2D engine clears rectangles (here the CPU does it, and counts): the runtime must
// ask for each damaged rect and then draw on top of what the engine cleared.
struct RectClearSink final : prism::platform::FrameSink {
    std::array<std::vector<std::uint32_t>, 2> buffers{std::vector<std::uint32_t>(std::size_t(W) * H, 0x12345678u),
                                                      std::vector<std::uint32_t>(std::size_t(W) * H, 0x12345678u)};
    std::size_t back_index = 0, shown_index = 0;
    std::atomic<int> presents{0};
    std::atomic<int> rects{0};
    std::atomic<int> whole{0};

    std::span<std::uint32_t> back() override { return buffers[back_index]; }
    bool clear(std::uint32_t) override { ++whole; return false; }
    bool clear_rect(int x0, int y0, int x1, int y1, std::uint32_t argb) override
    {
        ++rects;
        for (int y = y0; y < y1; ++y)
            std::fill(buffers[back_index].begin() + std::ptrdiff_t(y) * W + x0,
                      buffers[back_index].begin() + std::ptrdiff_t(y) * W + x1, argb);
        return true;
    }
    void present() override
    {
        shown_index = back_index;
        back_index ^= 1;
        ++presents;
    }
    Size size() const override { return {Width{float(W)}, Height{float(H)}}; }
    int buffer_age() const override { return 2; }
};

constexpr int kBandRows = 48; // 100 rows: two full bands and a 4-row one

// Two band buffers; flush copies a band into a whole frame, which the test reads after the run.
template <class P>
struct MemoryBandSink final : prism::platform::BandSink<P> {
    using pixel = typename P::storage;
    std::array<std::vector<pixel>, 2> bands{std::vector<pixel>(std::size_t(W) * kBandRows),
                                            std::vector<pixel>(std::size_t(W) * kBandRows)};
    std::vector<pixel> frame = std::vector<pixel>(std::size_t(W) * H);
    std::atomic<int> presents{0};
    std::atomic<int> flushes{0};
    int rows_per_band = kBandRows;
    int age = 0; // what frame_age() says: 1 = the frame memory keeps earlier frames
    std::chrono::milliseconds flush_time{0};

    int band_height() const override { return rows_per_band; }
    int frame_age() const override { return age; }
    std::span<pixel> band(int index) override { return bands[std::size_t(index)]; }
    void flush(int index, int first_row, int rows) override
    {
        std::this_thread::sleep_for(flush_time);
        ++flushes;
        std::copy_n(bands[std::size_t(index)].begin(), std::size_t(W) * rows,
                    frame.begin() + std::ptrdiff_t(first_row) * W);
    }
    void present() override { ++presents; }
    Size size() const override { return {Width{float(W)}, Height{float(H)}}; }
};

// A band sink whose copy takes 5 ms on a helper thread, as a DMA transfer does. band() waits for
// it. A runtime that keeps a band's span across flushes shows two ways: it flushes a buffer it
// did not ask band() for since that buffer's last flush, and, if it draws while the copy runs,
// the band no longer holds at the copy's end what it held at flush. Each counts a violation.
struct SlowCopyBandSink final : prism::platform::BandSink<prism::raster::Argb8888> {
    std::array<std::vector<std::uint32_t>, 2> bands{std::vector<std::uint32_t>(std::size_t(W) * kBandRows),
                                                    std::vector<std::uint32_t>(std::size_t(W) * kBandRows)};
    std::array<std::vector<std::uint32_t>, 2> at_flush;
    std::array<bool, 2> busy{};
    std::array<bool, 2> handed_out{};
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<int> flushes{0};
    std::atomic<int> violations{0};
    std::vector<std::jthread> copies; // last: joined before the buffers and the mutex go

    int band_height() const override { return kBandRows; }
    std::span<std::uint32_t> band(int index) override
    {
        std::unique_lock lock{mu};
        cv.wait(lock, [&] { return !busy[std::size_t(index)]; });
        handed_out[std::size_t(index)] = true;
        return bands[std::size_t(index)];
    }
    void flush(int index, int, int) override
    {
        const auto i = std::size_t(index);
        {
            std::lock_guard lock{mu};
            if (!handed_out[i])
                ++violations;
            handed_out[i] = false;
            busy[i] = true;
            at_flush[i] = bands[i];
        }
        ++flushes;
        copies.emplace_back([this, i] {
            std::this_thread::sleep_for(5ms);
            std::lock_guard lock{mu};
            if (bands[i] != at_flush[i])
                ++violations;
            busy[i] = false;
            cv.notify_all();
        });
    }
    void present() override {}
    Size size() const override { return {Width{float(W)}, Height{float(H)}}; }
};

struct NoTouch final : prism::platform::TouchSource {
    std::optional<Point> read() override { return std::nullopt; }
};

// Runs the counter on `sink` until its first frame is presented.
template <class Sink>
void run_one_frame(Sink& sink)
{
    Counter model;
    NoTouch touch;
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        ctx.on_loop([&] {
            if (sink.presents >= 1)
                ctx.quit();
        });
    });
}

std::uint16_t to_rgb565(std::uint32_t argb)
{
    return prism::raster::Rgb565::pack(Color::rgba(std::uint8_t(argb >> 16), std::uint8_t(argb >> 8),
                                                   std::uint8_t(argb)));
}

// Every RGB565 channel within one unit.
bool near_565(std::uint16_t a, std::uint16_t b)
{
    const auto close = [](int x, int y) { return std::abs(x - y) <= 1; };
    return close(a >> 11, b >> 11) && close((a >> 5) & 0x3F, (b >> 5) & 0x3F) && close(a & 0x1F, b & 0x1F);
}

// No finger, or a tap at `at`: held for a few polls once the first frame is on screen (the
// runtime hit-tests against the shown frame, set just after the first present), then lifted.
struct ScriptedTouch final : prism::platform::TouchSource {
    const MemorySink* sink = nullptr;
    std::optional<Point> at;
    int held_polls = 5;

    std::optional<Point> read() override
    {
        if (!at || sink->presents == 0 || held_polls == 0)
            return std::nullopt;
        --held_polls;
        return at;
    }
};

// The list drawn whole by a fresh tree scrolled by `offset` pixels: no hover, no pressed button.
std::vector<std::uint32_t> list_scrolled_by(ButtonList& model, float offset)
{
    prism::app::WidgetTree tree(model);
    prism::raster::Fonts fonts;
    tree.set_text_measurer(&fonts);
    auto snap = tree.build_snapshot(float(W), float(H), 1);
    tree.scroll_at(tree.leaf_ids().front(), DY{offset});
    snap = tree.build_snapshot(float(W), float(H), 2);
    std::vector<std::uint32_t> px(std::size_t(W) * H);
    prism::raster::Canvas canvas{px, W, H, W};
    canvas.clear(snap->window.background);
    prism::raster::draw(canvas, *snap, fonts);
    return px;
}

bool is_a_clean_scroll(ButtonList& model, const std::vector<std::uint32_t>& frame)
{
    for (float offset = 1.f; offset <= 400.f; offset += 1.f)
        if (list_scrolled_by(model, offset) == frame)
            return true;
    return false;
}

// A finger down at `from`, moved in steps to `to`, held there, then lifted: polls are 16 ms apart.
struct DragTouch final : prism::platform::TouchSource {
    const MemorySink* sink = nullptr;
    Point from{}, to{};
    int steps = 6, holds = 4, poll = 0;
    std::atomic<bool> lifted{false};

    std::optional<Point> read() override
    {
        if (sink->presents == 0)
            return std::nullopt;
        const int n = poll++;
        if (n > steps + holds) {
            lifted = true;
            return std::nullopt;
        }
        const float t = float(std::min(n, steps)) / float(steps);
        return Point{X{from.x.raw() + (to.x.raw() - from.x.raw()) * t},
                     Y{from.y.raw() + (to.y.raw() - from.y.raw()) * t}};
    }
};

// The button's centre, laid out as the runtime lays it out: the same model type, size and fonts.
// In panel pixels, with the UI laid out in logical units of `scale` pixels.
Point button_centre(float scale = 1.f)
{
    Counter twin;
    prism::app::WidgetTree tree(twin);
    prism::raster::Fonts fonts;
    tree.set_text_measurer(&fonts);
    const auto snap = tree.build_snapshot(float(W) / scale, float(H) / scale, 1);
    const auto button = tree.leaf_ids().front();
    for (const auto& [id, r] : snap->geometry)
        if (id == button) {
            const Point c = r.center();
            return {X{c.x.raw() * scale}, Y{c.y.raw() * scale}};
        }
    FAIL("no geometry for the button");
    return {};
}

} // namespace

TEST_CASE("frame_app presents a first frame and returns after quit")
{
    Counter model;
    MemorySink sink;
    ScriptedTouch touch;
    touch.sink = &sink;
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        ctx.on_loop([&] {
            if (sink.presents >= 1)
                ctx.quit();
        });
    });
    CHECK(sink.presents >= 1);
}

TEST_CASE("before_thread_start runs on the caller, for logic then render, before each starts")
{
    Counter model;
    MemorySink sink;
    ScriptedTouch touch;
    std::vector<prism::platform::ThreadRole> roles;
    const auto caller = std::this_thread::get_id();
    bool on_caller = true;
    prism::platform::FrameOptions options{.before_thread_start = [&](prism::platform::ThreadRole r) {
        roles.push_back(r);
        on_caller = on_caller && std::this_thread::get_id() == caller;
    }};
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        ctx.on_loop([&] { if (sink.presents >= 1) ctx.quit(); });
    }, options);
    CHECK(on_caller);
    CHECK(roles == std::vector{prism::platform::ThreadRole::logic, prism::platform::ThreadRole::render});
}

TEST_CASE("a sink that clears its own frame is asked to, and the CPU does not clear it")
{
    Counter model;
    ClearingSink sink;
    ScriptedTouch touch;
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        ctx.on_loop([&] {
            if (sink.presents >= 1)
                ctx.quit();
        });
    });
    REQUIRE(sink.presents >= 1);
    CHECK(sink.clears >= sink.presents);
    CHECK(sink.cleared_with == 0xFF1E1E1Eu);   // the window background, rgba(30, 30, 30)
    // The bottom-right corner is background: no widget draws there, so it keeps what was there.
    CHECK(sink.pixels.back() == 0x12345678u);
}

TEST_CASE("a tap on the button reaches the model on the logic thread")
{
    Counter model;
    MemorySink sink;
    ScriptedTouch touch;
    touch.sink = &sink;
    touch.at = button_centre();
    std::thread::id clicked_on;
    Connection on_click;
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        on_click = model.plus.on_change().connect([&](const prism::ui::Button&) {
            clicked_on = std::this_thread::get_id();
            model.count.set(model.count.get() + 1);
        });
        ctx.on_loop([&] {
            if (model.count.get() == 1)
                ctx.quit();
        });
    });
    CHECK(model.count.get() == 1);
    CHECK(clicked_on != std::thread::id{});
    CHECK(clicked_on != std::this_thread::get_id());
}

TEST_CASE("with a UI scale, the UI is drawn that much larger and a tap lands where it is drawn")
{
    constexpr float scale = 2.f;
    prism::platform::FrameOptions options;
    options.ui_scale = scale;
    Counter model;
    MemorySink sink;
    ScriptedTouch touch;
    touch.sink = &sink;
    touch.at = button_centre(scale);
    Connection on_click;
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        on_click = model.plus.on_change().connect([&](const prism::ui::Button&) {
            model.count.set(model.count.get() + 1);
        });
        ctx.on_loop([&] {
            if (model.count.get() == 1)
                ctx.quit();
        });
    }, options);
    CHECK(model.count.get() == 1);

    Counter fresh;
    MemorySink first;
    NoTouch none;
    prism::platform::frame_app(first, none, fresh, [&](FrameContext& ctx) {
        ctx.on_loop([&] { if (first.presents >= 1) ctx.quit(); });
    }, options);
    CHECK(first.pixels == whole_frame(fresh, scale));
    CHECK(first.pixels != whole_frame(fresh, 1.f));
}

TEST_CASE("after a tap lifts, the button is not left hovered or focused")
{
    Counter model;
    MemorySink sink;
    ScriptedTouch touch;
    touch.sink = &sink;
    touch.at = button_centre();
    Connection on_click;
    std::optional<std::chrono::steady_clock::time_point> lifted;
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        on_click = model.plus.on_change().connect([&](const prism::ui::Button&) {
            model.count.set(model.count.get() + 1);
        });
        ctx.on_loop([&] {
            if (model.count.get() >= 1 && touch.held_polls == 0 && !lifted)
                lifted = std::chrono::steady_clock::now();
            // A frame is drawn only when something changed, so wait by the clock, not by frames.
            if (lifted && std::chrono::steady_clock::now() - *lifted > 300ms)
                ctx.quit();
        });
    });
    REQUIRE(model.count.get() == 1);
    CHECK(sink.pixels == whole_frame(model));   // no hover colour, no focus ring: a fresh tree's frame
}

TEST_CASE("dragging a finger over a scroll view scrolls it and presses nothing")
{
    ButtonList model;
    MemorySink sink;
    DragTouch touch;
    touch.sink = &sink;
    touch.from = Point{X{60.f}, Y{85.f}};
    touch.to = Point{X{60.f}, Y{20.f}};   // up: the list scrolls down
    std::atomic<int> clicks{0};
    std::vector<Connection> connections;
    std::vector<std::uint32_t> first;
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        for (auto& r : model.rows)
            connections.push_back(r.on_change().connect([&](const prism::ui::Button& b) { if (b.click_count) ++clicks; }));
        ctx.on_loop([&] {
            if (first.empty() && sink.presents >= 1)
                first = sink.pixels;
            if (touch.lifted)
                ctx.quit();
        });
    });
    CHECK(clicks == 0);
    REQUIRE_FALSE(first.empty());
    CHECK(sink.pixels != first);
    CHECK(is_a_clean_scroll(model, sink.pixels));   // scrolled, and no button left pressed or hovered
}

TEST_CASE("a tap on a button inside a scroll view still clicks it")
{
    ButtonList model;
    MemorySink sink;
    DragTouch touch;
    touch.sink = &sink;
    touch.from = touch.to = Point{X{60.f}, Y{16.f}};   // on the first row, no movement
    std::atomic<int> clicks{0};
    std::vector<Connection> connections;
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        for (auto& r : model.rows)
            connections.push_back(r.on_change().connect([&](const prism::ui::Button& b) { if (b.click_count) ++clicks; }));
        ctx.on_loop([&] {
            if (touch.lifted && clicks > 0)
                ctx.quit();
            else if (touch.lifted)
                ctx.quit();
        });
    });
    CHECK(clicks == 1);
}

TEST_CASE("quit while the render thread waits ends the run")
{
    Counter model;
    MemorySink sink;
    ScriptedTouch touch;
    touch.sink = &sink;
    const auto start = std::chrono::steady_clock::now();
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        ctx.on_loop([&, polls = 0]() mutable {
            if (sink.presents >= 1 && ++polls >= 3) // the render thread is idle by now
                ctx.quit();
        });
    });
    CHECK(sink.presents >= 1);
    CHECK(std::chrono::steady_clock::now() - start < 1s);
}

TEST_CASE("quit while the render thread is inside present() ends the run")
{
    Counter model;
    MemorySink sink;
    sink.present_time = 50ms;
    ScriptedTouch touch;
    touch.sink = &sink;
    const auto start = std::chrono::steady_clock::now();
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        ctx.on_loop([&] { ctx.quit(); });
    });
    CHECK(std::chrono::steady_clock::now() - start < 1s);
}

TEST_CASE("the timing line reaches on_stats")
{
    Counter model;
    MemorySink sink;
    ScriptedTouch touch;
    touch.sink = &sink;
    std::string line;
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        ctx.on_stats([&](std::string_view l) {
            line = l;
            ctx.quit();
        });
    });
    REQUIRE_FALSE(line.empty());
    CHECK(std::isdigit(static_cast<unsigned char>(line.front())));
    CHECK(line.find("fps") != std::string::npos);
}

// The number after `name ` in the timing line, or -1 when the field is missing or "-".
static double stage_ms(std::string_view line, std::string_view name)
{
    const auto at = line.find(name);
    if (at == std::string_view::npos)
        return -1;
    const std::string rest{line.substr(at + name.size())};
    char* end = nullptr;
    const double v = std::strtod(rest.c_str(), &end);
    return end == rest.c_str() ? -1 : v;
}

TEST_CASE("time a band sink spends flushing counts as present, not draw")
{
    Counter model;
    MemoryBandSink<prism::raster::Argb8888> sink;
    sink.flush_time = 20ms; // as a board's first band waits for the panel flip
    NoTouch touch;
    std::string line;
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        ctx.on_stats([&](std::string_view l) {
            if (stage_ms(l, "draw ") < 0)
                return; // no frame in this period yet
            line = l;
            ctx.quit();
        });
    });
    INFO(line);
    constexpr int bands = (H + kBandRows - 1) / kBandRows;
    CHECK(stage_ms(line, "draw ") >= 0);
    CHECK(stage_ms(line, "draw ") < 10);
    CHECK(stage_ms(line, "present ") >= 20.0 * bands);
}

TEST_CASE("a band sink's frame equals a frame sink's frame")
{
    MemorySink whole;
    run_one_frame(whole);
    REQUIRE(whole.presents >= 1);

    SUBCASE("ARGB8888")
    {
        MemoryBandSink<prism::raster::Argb8888> banded;
        run_one_frame(banded);
        REQUIRE(banded.presents >= 1);
        CHECK(banded.frame == whole.pixels);
    }
    SUBCASE("RGB565: the ARGB frame packed, within a unit (glyph edges blend on a 565 background)")
    {
        MemoryBandSink<prism::raster::Rgb565> banded;
        run_one_frame(banded);
        REQUIRE(banded.presents >= 1);
        int far = 0;
        for (std::size_t i = 0; i < whole.pixels.size(); ++i)
            far += !near_565(banded.frame[i], to_rgb565(whole.pixels[i]));
        CHECK(far == 0);
        CHECK(banded.frame.front() == to_rgb565(whole.pixels.front()));
    }
}

void check_redraws_only_changes(int age)
{
    AgedSink sink{age};
    TwoCounters model;
    NoTouch touch;
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        ctx.on_loop([&] {
            if (sink.presents >= 12)
                ctx.quit();
            else if (model.first.get() == model.second.get())
                model.first.set(model.first.get() + 1); // a new frame each loop, in turn
            else
                model.second.set(model.second.get() + 1);
        });
    });
    REQUIRE(sink.presents >= 12);
    CHECK(sink.clears < sink.presents);
    CHECK(is_a_whole_frame(sink.shown(), model.first.get()));
}

TEST_CASE("a frame sink that keeps its frames is redrawn where the frame changed, not whole")
{
    SUBCASE("one buffer") { check_redraws_only_changes(1); }
    SUBCASE("two buffers that flip") { check_redraws_only_changes(2); }
}

TEST_CASE("a sink that clears rectangles itself is asked for each damaged rect, and the frame is right")
{
    RectClearSink sink;
    TwoCounters model;
    NoTouch touch;
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        ctx.on_loop([&] {
            if (sink.presents >= 12)
                ctx.quit();
            else if (model.first.get() == model.second.get())
                model.first.set(model.first.get() + 1);
            else
                model.second.set(model.second.get() + 1);
        });
    });
    REQUIRE(sink.presents >= 12);
    CHECK(sink.rects > 0);
    CHECK(sink.whole < sink.presents);
    CHECK(is_a_whole_frame(sink.buffers[sink.shown_index], model.first.get()));
}

TEST_CASE("a band sink that keeps its frame gets only the bands that changed")
{
    Counter model;
    MemoryBandSink<prism::raster::Argb8888> sink;
    sink.age = 1;
    NoTouch touch;
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        ctx.on_loop([&] {
            if (sink.presents >= 12)
                ctx.quit();
            else
                model.count.set(model.count.get() + 1);
        });
    });
    constexpr int bands = (H + kBandRows - 1) / kBandRows;
    REQUIRE(sink.presents >= 12);
    CHECK(sink.flushes < sink.presents * bands);
    CHECK(is_a_whole_counter_frame(sink.frame, model.count.get()));
}

TEST_CASE("the runtime never draws into a band whose copy is in flight")
{
    Counter model;
    SlowCopyBandSink sink;
    NoTouch touch;
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        ctx.on_loop([&] {
            if (sink.flushes >= 21)
                ctx.quit();
            else
                model.count.set(model.count.get() + 1); // a new frame each loop: its bands differ
        });
    });
    sink.copies.clear(); // the last copies finish, and check, before the counts are read
    CHECK(sink.flushes >= 21);
    CHECK(sink.violations == 0);
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace {
template <typename F>
bool aborts(F f)
{
    const pid_t pid = fork();
    if (pid == 0) { f(); _exit(0); }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
} // namespace

TEST_CASE("a band sink with no rows per band aborts instead of looping forever")
{
    CHECK(aborts([] {
        alarm(10); // a loop that never ends dies of SIGALRM: the check fails instead of hanging
        MemoryBandSink<prism::raster::Argb8888> sink;
        sink.rows_per_band = 0;
        run_one_frame(sink);
    }));
}

TEST_CASE("registering a hook from inside a running hook aborts")
{
    CHECK(aborts([] {
        Counter model;
        MemorySink sink;
        ScriptedTouch touch;
        touch.sink = &sink;
        prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
            ctx.on_loop([&] { ctx.on_loop([] {}); });
        });
    }));
}
#endif
