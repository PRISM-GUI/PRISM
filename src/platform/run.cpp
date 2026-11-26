#include <prism/platform/run.hpp>

#include <prism/core/mpsc_queue.hpp>
#include <prism/ui/animation.hpp>

#include "frame_backend.hpp"

#include <atomic>
#include <functional>
#include <memory>

namespace prism::platform::run_detail {
namespace {

// One run's state, alive as long as the frame runtime loops: the hook registered on the
// FrameContext holds it, so it is destroyed on the logic thread with that context -- after the last
// turn and before the logic thread ends.
struct FrameRun {
    FrameRun(FrameContext& fc, app::WidgetTree& tree)
        : window_{fc.size()},
          backend_{std::make_unique<platform_detail::FrameBackend>(window_, [&fc] { fc.quit(); })},
          tree_{&tree}
    {
        ctx_ = std::make_unique<app::AppContext>(
            loop_.get_scheduler(), clock_, window_, backend_, registry_, key_handler_,
            post_dispatch_hook_, queue_, scheduled_, closed_, drain_);

        // This is the logic thread from here on: post() runs inline, and attach() below sees it.
        app::detail_is_logic_thread() = true;
        ctx_->set_loop_drains_posts(true);
        tree.bind_waker(ctx_->waker());
        *drain_ = [this] {
            tree_->attach_blocks(*ctx_);
            tree_->drain_shared();
        };
        tree_->attach_blocks(*ctx_);   // the model's attach(): once, before the first frame
    }

    // A poster may outlive the run (worker threads often do): closing the gate first makes it a
    // no-op instead of a post into a dead AppContext.
    ~FrameRun() { ctx_->close_waker(); }

    app::AppContext& context() { return *ctx_; }

    // One logic loop: animations, then whatever other threads posted since the last turn.
    void turn()
    {
        if (clock_.active())
            clock_.tick(ui::AnimationClock::clock::now());
        tree_->attach_blocks(*ctx_);   // blocks the last turn built
        ctx_->drain_posted();
    }

private:
    // Declaration order is construction and reverse destruction order: the context last, so
    // everything it points at is still there when it goes.
    stdexec::run_loop loop_;   // a scheduler for AppContext; this loop drains the posts itself
    ui::AnimationClock clock_;
    platform_detail::PanelWindow window_;
    app::Backend backend_;   // the frame target's: one panel, no second window, quit ends the run
    app::WindowRegistry registry_;   // empty on purpose: the runtime owns the tree, not the registry
    // simplify: with no registry entry, AppContext::set_theme reaches no tree (the frame tree keeps
    // its default theme). Add a non-owning entry to WindowRegistry when a board app needs to switch.
    std::function<void(const input::KeyPress&)> key_handler_;
    std::function<void()> post_dispatch_hook_;
    std::shared_ptr<core::mpsc_queue<std::function<void()>>> queue_ =
        std::make_shared<core::mpsc_queue<std::function<void()>>>();
    std::shared_ptr<std::atomic<bool>> scheduled_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> closed_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::function<void()>> drain_ = std::make_shared<std::function<void()>>();
    app::WidgetTree* tree_;
    std::unique_ptr<app::AppContext> ctx_;
};

} // namespace

void start(FrameContext& fc, const Port& port, app::WidgetTree& tree,
           std::function<void(app::AppContext&)> setup, const app::AppConfig&)
{
    // A board has no desktop to place a window on, no size to remember and no settings file:
    // title, width/height, settings and a timed quit are all the desktop's, so they are ignored
    // here. The run ends when the app closes its window -- close_main_window reaches the backend.
    auto run = std::make_shared<FrameRun>(fc, tree);

    // The hooks keep the run alive; it goes when the runtime's own hooks do.
    fc.on_loop([run] { run->turn(); });
    if (port.on_stats) {
        auto on_stats = port.on_stats;
        fc.on_stats([on_stats](std::string_view line) { on_stats(line); });
    }

    if (setup)
        setup(run->context());
}

} // namespace prism::platform::run_detail
