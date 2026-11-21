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

constexpr int kBandRows = 48; // 100 rows: two full bands and a 4-row one

// Two band buffers; flush copies a band into a whole frame, which the test reads after the run.
template <class P>
struct MemoryBandSink final : prism::platform::BandSink<P> {
    using pixel = typename P::storage;
    std::array<std::vector<pixel>, 2> bands{std::vector<pixel>(std::size_t(W) * kBandRows),
                                            std::vector<pixel>(std::size_t(W) * kBandRows)};
    std::vector<pixel> frame = std::vector<pixel>(std::size_t(W) * H);
    std::atomic<int> presents{0};

    int band_height() const override { return kBandRows; }
    std::span<pixel> band(int index) override { return bands[std::size_t(index)]; }
    void flush(int index, int first_row, int rows) override
    {
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

// The button's centre, laid out as the runtime lays it out: the same model type, size and fonts.
Point button_centre()
{
    Counter twin;
    prism::app::WidgetTree tree(twin);
    prism::raster::Fonts fonts;
    tree.set_text_measurer(&fonts);
    const auto snap = tree.build_snapshot(float(W), float(H), 1);
    const auto button = tree.leaf_ids().front();
    for (const auto& [id, r] : snap->geometry)
        if (id == button)
            return r.center();
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
