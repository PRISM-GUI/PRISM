// The board apps as apps: examples/embedded's Scope through platform::run with a test port (the
// same route an app's main() takes), and the two apps' freedom from FrameContext hooks -- the
// timing line is the port's, the scope feeds itself from its own source.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "alloc_counter.hpp"

#include <embedded/audio_scope.hpp>
#include <embedded/widgets_app.hpp>

#include <prism/app/run.hpp>
#include <prism/platform/alloc_count.hpp>
#include <prism/platform/frame_app.hpp>
#include <prism/platform/frame_stats.hpp>
#include <prism/platform/port.hpp>
#include <prism/platform/run.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The two hooks PRISM's platform library asks of a program linking it: the allocation count is
// operator new's count (alloc_counter.hpp), and this host reports no system counters.
std::uint32_t prism::platform::allocation_count()
{
    return static_cast<std::uint32_t>(prism::test::allocations());
}

std::optional<prism::platform::SystemCounters> prism::platform::system_counters()
{
    return std::nullopt;
}

namespace {
constexpr int W = 480, H = 272;

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

// The port tests/test_platform_run.cpp uses: a memory sink, no touch, and a quit once the case's
// predicate says so (or after max_polls loops, so a broken app fails rather than hangs). The
// timing line is collected both where the port hands it on (Port::on_stats, what platform::run
// registers) and from a hook of this port's own.
struct TestPort {
    MemorySink sink;
    NoTouch touch;
    std::function<bool()> done = [] { return true; };
    bool quit_on_stats = false;
    int max_polls = 600; // 600 x 16 ms: ten seconds, enough for a stats line or a level readout
    std::vector<std::string> port_lines;
    std::vector<std::string> hook_lines;

    TestPort()
    {
        prism::platform::Port port;
        port.run_frames = [this](prism::platform::TreeFactory make_tree,
                                 std::function<void(prism::platform::FrameContext &)> setup) {
            prism::platform::run_frames(
                sink, touch, std::move(make_tree),
                [this, setup = std::move(setup)](prism::platform::FrameContext &fc) {
                    setup(fc); // platform::run's glue: the AppContext, then the app's setup
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
} // namespace

TEST_CASE("the scope app feeds its plots and its title from its own source")
{
    prism_examples::Scope scope;
    TestPort port;
    // Whichever source the machine offers (a capture device, or the sweep when none opened), the
    // title's "S/s" only appears once samples have gone through and been counted.
    port.done = [&] {
        return scope.model.title.get().value.find("S/s") != std::string_view::npos;
    };

    const int rc = prism::platform::run(scope.model, {.title = "Scope"},
                                        [&scope](prism::app::AppContext &ctx) { scope.attach(ctx); });

    CHECK(rc == 0);
    const std::string title(scope.model.title.get().value);
    CHECK(title.find("S/s") != std::string::npos);
    CHECK(scope.model.waveform.series_len(0) == prism_examples::ScopeState::kWaveformSamples);
    CHECK(port.sink.presents.load() > 0);
}

TEST_CASE("the timing line is the port's, with no hook of the app's")
{
    prism_examples::Scope scope;
    TestPort port;
    port.done = [] { return false; }; // the run lasts until the first timing line
    port.quit_on_stats = true;

    const int rc = prism::platform::run(scope.model, {.title = "Scope"},
                                        [&scope](prism::app::AppContext &ctx) { scope.attach(ctx); });

    CHECK(rc == 0);
    REQUIRE_FALSE(port.port_lines.empty());
    CHECK(port.port_lines.front().find("fps") != std::string::npos);
}
