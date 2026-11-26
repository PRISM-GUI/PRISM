// platform::run: one entry point on a board. A board registers a Port (platform/port.hpp) once,
// before main(); platform::run then runs the model on the frame runtime with a real
// app::AppContext, the same one model_app gives a desktop app. Here the Port is a test one: a
// memory sink, no touch, and a quit once the case's predicate says so.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "alloc_counter.hpp"

#include <prism/app/run.hpp>
#include <prism/app/widget_tree.hpp>
#include <prism/core/field.hpp>
#include <prism/platform/alloc_count.hpp>
#include <prism/platform/frame_app.hpp>
#include <prism/platform/frame_stats.hpp>
#include <prism/platform/port.hpp>
#include <prism/platform/run.hpp>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The platform hooks the frame runtime reads, defined by whoever links it: here, operator new's
// count (alloc_counter.hpp) and no CPU or memory counters.
std::uint32_t prism::platform::allocation_count()
{
    return static_cast<std::uint32_t>(prism::test::allocations());
}

std::optional<prism::platform::SystemCounters> prism::platform::system_counters()
{
    return std::nullopt;
}

namespace {

constexpr int W = 120, H = 100;

struct MemorySink final : prism::platform::FrameSink {
    std::vector<std::uint32_t> pixels = std::vector<std::uint32_t>(std::size_t(W) * H);
    std::atomic<int> presents{0};

    std::span<std::uint32_t> back() override { return pixels; }
    void present() override { ++presents; }
    prism::core::Size size() const override
    {
        return {prism::core::Width{float(W)}, prism::core::Height{float(H)}};
    }
};

struct NoTouch final : prism::platform::TouchSource {
    std::optional<prism::core::Point> read() override { return std::nullopt; }
};

// A Port that runs the frame runtime on the memory sink above. `done()` is polled once per logic
// loop and quits the run; a case that never gets there quits after `max_polls` loops instead, so a
// broken runtime fails a CHECK rather than hanging the suite. The timing line is collected both
// where the port hands it on (Port::on_stats -- what platform::run must register) and from a hook
// of this port's own, registered after platform::run's setup returned.
struct TestPort {
    MemorySink sink;
    NoTouch touch;
    std::function<bool()> done = [] { return true; };
    bool quit_on_stats = false;
    int max_polls = 40;   // 40 x 16 ms: enough for a post from another thread to land
    std::vector<std::string> port_lines;   // what Port::on_stats received
    std::vector<std::string> hook_lines;

    TestPort()
    {
        prism::platform::Port port;
        port.run_frames = [this](prism::platform::TreeFactory make_tree,
                                 std::function<void(prism::platform::FrameContext&)> setup) {
            prism::platform::run_frames(
                sink, touch, std::move(make_tree),
                [this, setup = std::move(setup)](prism::platform::FrameContext& fc) {
                    setup(fc);   // platform::run's glue: the AppContext, then the app's setup
                    fc.on_stats([this, fc_p = &fc](std::string_view line) {
                        hook_lines.emplace_back(line);
                        if (quit_on_stats)
                            fc_p->quit();
                    });
                    fc.on_loop([this, fc_p = &fc, polls = 0]() mutable {
                        if (done() || ++polls >= max_polls)
                            fc_p->quit();
                    });
                });
        };
        port.on_stats = [this](std::string_view line) { port_lines.emplace_back(line); };
        prism::platform::set_port(std::move(port));
    }
};

// A model as a board app writes one: a view, and an attach() the runtime calls once with the
// AppContext. It records which thread that was, so a post's thread can be compared with it.
struct CounterModel {
    prism::core::Field<int> count{0};
    int attaches = 0;
    std::thread::id attach_thread{};
    bool attached_before_post = false;

    void attach(prism::app::AppContext&)
    {
        ++attaches;
        attach_thread = std::this_thread::get_id();
    }
    void view(prism::app::ViewBuilder& vb) { vb.vstack(count); }
};

// A second model, for open_window.
struct OtherModel {
    prism::core::Field<int> value{1};
    void view(prism::app::ViewBuilder& vb) { vb.vstack(value); }
};

} // namespace

TEST_CASE("platform::run calls the model's attach(AppContext&) once, before setup's first post")
{
    CounterModel model;
    TestPort port;
    bool setup_ran = false;
    bool post_ran = false;
    std::thread::id setup_thread{};
    port.done = [&] { return post_ran; };

    const int rc = prism::platform::run(model, {}, [&](prism::app::AppContext& ctx) {
        setup_ran = true;
        setup_thread = std::this_thread::get_id();
        ctx.post([&] {
            post_ran = true;
            model.attached_before_post = model.attaches == 1;
        });
    });

    CHECK(rc == 0);
    CHECK(setup_ran);
    CHECK(model.attaches == 1);
    CHECK(post_ran);
    CHECK(model.attached_before_post);
    // setup and attach run on the logic thread, not the caller's.
    CHECK(setup_thread == model.attach_thread);
    CHECK(model.attach_thread != std::this_thread::get_id());
}

TEST_CASE("a poster called from another thread runs its function on the logic thread")
{
    CounterModel model;
    TestPort port;
    std::thread::id posted_on{};
    std::atomic<bool> ran{false};
    port.done = [&] { return ran.load(); };

    prism::platform::run(model, {}, [&](prism::app::AppContext& ctx) {
        auto poster = ctx.poster();
        std::thread worker{[&, poster = std::move(poster)] {
            poster([&] {
                posted_on = std::this_thread::get_id();
                ran.store(true);
            });
        }};
        worker.join();   // it only enqueues, so this does not need the logic thread
    });

    CHECK(ran.load());
    CHECK(posted_on != std::thread::id{});
    CHECK(posted_on == model.attach_thread);   // the thread attach() ran on
    CHECK(posted_on != std::this_thread::get_id());
}

TEST_CASE("post_deferred runs its function on the logic thread")
{
    CounterModel model;
    TestPort port;
    std::thread::id deferred_on{};
    bool ran = false;
    port.done = [&] { return ran; };

    prism::platform::run(model, {}, [&](prism::app::AppContext& ctx) {
        ctx.post_deferred([&] {
            deferred_on = std::this_thread::get_id();
            ran = true;
        });
    });

    CHECK(ran);
    CHECK(deferred_on == model.attach_thread);
    CHECK(deferred_on != std::this_thread::get_id());
}

TEST_CASE("open_window returns 0 on a frame target")
{
    CounterModel model;
    OtherModel other;
    TestPort port;
    bool tried = false;
    prism::app::WindowId opened = 1;   // not 0: the check below must see what open_window returned
    port.done = [&] { return tried; };

    prism::platform::run(model, {}, [&](prism::app::AppContext& ctx) {
        opened = ctx.open_window(other, {.title = "other", .width = 320, .height = 240});
        tried = true;
    });

    CHECK(tried);
    CHECK(opened == 0);
    CHECK(model.attaches == 1);   // a failed window request does not attach the model twice
}

TEST_CASE("the timing line reaches the port's on_stats")
{
    CounterModel model;
    TestPort port;
    port.done = [] { return false; };   // the run lasts until the first line
    port.quit_on_stats = true;
    port.max_polls = 300;               // a line comes once a second

    prism::platform::run(model, {});

    REQUIRE_FALSE(port.port_lines.empty());
    const std::string& line = port.port_lines.front();
    CHECK(std::isdigit(static_cast<unsigned char>(line.front())));
    CHECK(line.find("fps") != std::string::npos);
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

TEST_CASE("platform::run with no port set aborts")
{
    CHECK(aborts([] {
        alarm(10);   // a run that does not abort dies of SIGALRM: the check fails instead of hanging
        // An empty Port is a port that was never set: the same "no board" case the runtime must
        // report to stderr and abort on.
        prism::platform::set_port({});
        CounterModel model;
        prism::platform::run(model, {});
    }));
}
#endif
