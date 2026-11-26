#include <prism/platform/port.hpp>

#include <utility>

namespace prism::platform {
namespace {

// Written by a board before main() and read while the app runs: no synchronization, as for any
// global configured at startup. An empty Port is no port at all (set_port({}) forgets it), so
// platform::run reports that the same way whether none was ever set or one was cleared.
Port g_port;

} // namespace

void set_port(Port port)
{
    g_port = std::move(port);
}

const Port* port()
{
    return g_port.run_frames ? &g_port : nullptr;
}

} // namespace prism::platform
