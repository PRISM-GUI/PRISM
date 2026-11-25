#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace prism::crdt {

class Doc;

// One room over a Doc, served on one HTTP port: y-protocols sync on a WebSocket route, a static
// directory for the browser page, and the hub's own replica. Socket threads only carry bytes;
// drain() runs what they received on the logic thread, where every Doc lives.
class Hub {
public:
    struct Options { std::string host = "0.0.0.0"; std::uint16_t port = 0; std::string static_dir; };
    explicit Hub(Doc& doc, Options);   // starts serving; port 0 picks one; throws when it cannot bind
    ~Hub();                            // stops and joins
    std::uint16_t port() const;
    void drain();                      // logic thread: apply what clients sent, relay, send local edits

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A native y-protocols client: connects on its own thread, reads what the hub sends into a
// channel, and drain() applies it on the logic thread. Reconnects are out of scope.
class Client {
public:
    Client(Doc& doc, std::string url);  // ws://host:port/room ; connects on its own thread
    ~Client();
    void drain();                       // logic thread
    bool connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace prism::crdt
