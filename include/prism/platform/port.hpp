#pragma once

// What a board hands PRISM once, before main(): how to run the frame runtime on its display and
// its touch panel. Everything board-specific lives here -- the panel and its driver, the touch
// controller, the thread attributes, where the timing line goes (a console, a Wi-Fi log) -- and
// none of it reaches an app, which calls platform::run (platform/run.hpp) exactly as it calls
// app::run on the desktop.

#include <prism/platform/frame_app.hpp>

#include <functional>
#include <string_view>

namespace prism::platform {

struct Port {
    // The board's run: prism's frame runtime over the port's sink (a frame buffer, or a band
    // sink where the panel takes its pixels a band at a time), its touch source and its
    // FrameOptions. platform::run calls it with a tree factory and the setup that builds the
    // app's AppContext.
    std::function<void(TreeFactory, std::function<void(FrameContext&)>)> run_frames;
    // Where the timing line goes. Optional.
    std::function<void(std::string_view)> on_stats;
};

// Replaces the port. A board calls it once, before main() -- from its startup code (an
// app_main wrapper, a program-start hook) or a static initializer. An empty Port forgets it.
void set_port(Port port);

// The port, or nullptr until a board set one.
const Port* port();

} // namespace prism::platform
