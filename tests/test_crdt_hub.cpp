// crdt::Hub and crdt::Client over a real socket (doc/design/sync.md, "Step 2"; the plan's Task 3).
// Every test pumps the logic threads itself: drain() the hub and each client in a loop until the
// expected state shows, with a deadline so a lost message fails instead of hanging.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/crdt/doc.hpp>
#include <prism/crdt/field.hpp>
#include <prism/crdt/hub.hpp>

#include <httplib.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

using namespace prism;
using namespace prism::crdt;

namespace {

using Pump = std::function<void()>;

// One round of drain() on every logic thread involved, every 5 ms, until ready says so.
bool until(const Pump& pump, const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        pump();
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::string room_url(std::uint16_t port, std::string_view room) {
    return "ws://127.0.0.1:" + std::to_string(port) + "/" + std::string(room);
}

Hub::Options local_options(std::string static_dir = {}) {
    return Hub::Options{.host = "127.0.0.1", .port = 0, .static_dir = std::move(static_dir)};
}

} // namespace

TEST_CASE("an edit on one client reaches the other") {
    Doc hub_doc;
    Hub hub(hub_doc, local_options());

    Doc a_doc;
    Doc b_doc;
    Field<double> a_gain;
    Field<double> b_gain;
    a_doc.bind("gain", a_gain);
    b_doc.bind("gain", b_gain);
    Client a(a_doc, room_url(hub.port(), "room"));
    Client b(b_doc, room_url(hub.port(), "room"));

    const Pump pump = [&] { hub.drain(); a.drain(); b.drain(); };
    REQUIRE(until(pump, [&] { return a.connected() && b.connected(); }));

    a_gain = 2.5;

    CHECK(until(pump, [&] { return b_gain.get() == 2.5; }));
}

TEST_CASE("a late joiner gets the hub's full state after the first client is gone") {
    Doc hub_doc;
    Field<double> hub_gain;
    hub_doc.bind("gain", hub_gain);
    Hub hub(hub_doc, local_options());

    {
        Doc a_doc;
        Field<double> a_gain;
        a_doc.bind("gain", a_gain);
        Client a(a_doc, room_url(hub.port(), "room"));

        const Pump pump = [&] { hub.drain(); a.drain(); };
        REQUIRE(until(pump, [&] { return a.connected(); }));

        a_gain = 3.0;
        REQUIRE(until(pump, [&] { return hub_gain.get() == 3.0; }));
    }

    // b joins with no other client left, so what it receives can only come from the hub's Doc.
    Doc b_doc;
    Field<double> b_gain;
    b_doc.bind("gain", b_gain);
    Client b(b_doc, room_url(hub.port(), "room"));

    const Pump pump = [&] { hub.drain(); b.drain(); };
    CHECK(until(pump, [&] { return b_gain.get() == 3.0; }));
}

TEST_CASE("a client dropping mid-session leaves the other syncing") {
    Doc hub_doc;
    Field<double> hub_gain;
    hub_doc.bind("gain", hub_gain);
    Hub hub(hub_doc, local_options());

    Doc a_doc;
    Doc b_doc;
    Field<double> a_gain;
    Field<double> b_gain;
    a_doc.bind("gain", a_gain);
    b_doc.bind("gain", b_gain);
    auto a = std::make_unique<Client>(a_doc, room_url(hub.port(), "room"));
    Client b(b_doc, room_url(hub.port(), "room"));

    const Pump pump = [&] {
        hub.drain();
        if (a) a->drain();
        b.drain();
    };
    REQUIRE(until(pump, [&] { return a->connected() && b.connected(); }));

    a.reset();
    b_gain = 4.5;

    CHECK(until(pump, [&] { return hub_gain.get() == 4.5; }));
}

TEST_CASE("the hub's own edits reach every client") {
    Doc hub_doc;
    Field<double> hub_gain;
    hub_doc.bind("gain", hub_gain);
    Hub hub(hub_doc, local_options());

    Doc a_doc;
    Doc b_doc;
    Field<double> a_gain;
    Field<double> b_gain;
    a_doc.bind("gain", a_gain);
    b_doc.bind("gain", b_gain);
    Client a(a_doc, room_url(hub.port(), "room"));
    Client b(b_doc, room_url(hub.port(), "room"));

    const Pump pump = [&] { hub.drain(); a.drain(); b.drain(); };
    REQUIRE(until(pump, [&] { return a.connected() && b.connected(); }));

    hub_gain = 5.5;

    CHECK(until(pump, [&] { return a_gain.get() == 5.5 && b_gain.get() == 5.5; }));
}

TEST_CASE("static files are served with the COOP and COEP headers") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "prism_crdt_hub_static";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "index.html") << "<!doctype html><title>prism</title>";

    Doc doc;
    Hub hub(doc, local_options(dir.string()));

    httplib::Client client("127.0.0.1", hub.port());
    const httplib::Result page = client.Get("/index.html");
    REQUIRE(page);
    CHECK(page->status == 200);
    CHECK(page->body.find("<!doctype html>") != std::string::npos);
    CHECK(page->get_header_value("Cross-Origin-Opener-Policy") == "same-origin");
    CHECK(page->get_header_value("Cross-Origin-Embedder-Policy") == "require-corp");

    std::filesystem::remove_all(dir);
}

TEST_CASE("hub shutdown with clients connected joins its threads") {
    // A hang in ~Hub fails the suite through its timeout rather than a doctest assertion.
    Doc hub_doc;
    auto hub = std::make_unique<Hub>(hub_doc, local_options());

    Doc a_doc;
    Doc b_doc;
    Client a(a_doc, room_url(hub->port(), "room"));
    Client b(b_doc, room_url(hub->port(), "room"));

    const Pump pump = [&] { hub->drain(); a.drain(); b.drain(); };
    REQUIRE(until(pump, [&] { return a.connected() && b.connected(); }));

    hub.reset();

    // The Doc outlives the transport and stays a normal Doc.
    Field<double> gain;
    hub_doc.bind("gain", gain);
    gain = 1.0;
    CHECK(gain.get() == 1.0);
}
