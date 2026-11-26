#pragma once

// platform::run(model, cfg, setup): an app's one entry point on a board, the same call it makes
// on the desktop (app::run forwards here). The app is a model and its view; the board supplied a
// Port (platform/port.hpp) before main(), and this runs the model on the port's frame runtime with
// a real app::AppContext -- attach(AppContext&) once, before the first frame, then setup on the
// logic thread. Returns when the run ends.

#include <prism/app/model_app.hpp>
#include <prism/app/widget_tree.hpp>
#include <prism/platform/port.hpp>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>

namespace prism::app {
struct AppConfig;
}

namespace prism::platform {

namespace run_detail {

// src/platform/run.cpp: builds the app's AppContext for `tree` (the panel is its window), runs
// attach(AppContext&) and then the app's setup, and registers the per-loop drain and the port's
// timing line on `fc`.
void start(FrameContext& fc, const Port& port, app::WidgetTree& tree,
           std::function<void(app::AppContext&)> setup, const app::AppConfig& cfg);

} // namespace run_detail

// The model must outlive the call; while the run lasts, only the logic thread may change it.
template <class Model>
int run(Model& model, const app::AppConfig& cfg, std::function<void(app::AppContext&)> setup = {})
{
    const Port* const registered = port();
    if (!registered) {
        std::fputs("prism::platform::run: no port registered: the board's startup code must call "
                   "prism::platform::set_port() before main()\n",
                   stderr);
        std::abort();
    }

    // The runtime makes the tree before it calls setup, and hands it over afterwards: the factory
    // leaves it where setup finds it (both run on the logic thread, one after the other).
    auto tree = std::make_shared<app::WidgetTree*>(nullptr);
    registered->run_frames(
        [&model, tree] {
            auto made = std::make_unique<app::WidgetTree>(model);
            *tree = made.get();
            return made;
        },
        [registered, tree, setup = std::move(setup), &cfg](FrameContext& fc) {
            run_detail::start(fc, *registered, **tree, setup, cfg);
        });
    return 0;
}

} // namespace prism::platform
