// A hub with one bound field, for tests/test_crdt_agent.py: prints the port it
// listens on, then waits for a peer to set gain. A separate program so the Python
// side talks to a real hub over a real socket, not to a Doc in process.
#include <prism/crdt/doc.hpp>
#include <prism/crdt/field.hpp>
#include <prism/crdt/hub.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

namespace {
constexpr double startup_value = 0.5;
constexpr auto peer_budget = std::chrono::seconds{15};
constexpr auto round = std::chrono::milliseconds{5};

bool wait_for_peer(prism::crdt::Hub& hub, const prism::crdt::Field<double>& gain) {
    const auto deadline = std::chrono::steady_clock::now() + peer_budget;
    while (gain.get() == startup_value) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        hub.drain();
        std::this_thread::sleep_for(round);
    }
    return true;
}
}  // namespace

int main() {
    using namespace prism::crdt;
    Doc doc;
    Field<double> gain{startup_value};
    doc.bind("gain", gain);
    Hub hub(doc, {.host = "127.0.0.1", .port = 0, .static_dir = {}});
    std::printf("PORT %u\n", static_cast<unsigned>(hub.port()));
    std::fflush(stdout);
    if (!wait_for_peer(hub, gain)) {
        std::fprintf(stderr, "no peer set gain within 15 s\n");
        return 1;
    }
    std::printf("GAIN %g\n", gain.get());
    return 0;
}
