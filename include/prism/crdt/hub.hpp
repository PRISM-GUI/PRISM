#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prism::crdt {

class Doc;

// Whether a WebSocket upgrade may proceed, decided from the request alone. A browser always sends
// Origin, so a page this hub did not serve is recognisable; a native client (ours, pycrdt) sends
// none, and no page can forge an absent header. A page `Origin` is trusted only when it equals the
// `Host` and that host is an IP literal or localhost: a DNS-rebinding page reaches this hub under
// its own domain name, so both its Origin and the rewritten Host would name that domain, and the
// literal check refuses it. Any other origin needs to be listed.
bool origin_allowed(std::string_view origin, std::string_view host,
                    std::span<const std::string> allowed);

// One room over a Doc, served on one HTTP port: y-protocols sync on a WebSocket route, a static
// directory for the browser page, and the hub's own replica. Socket threads only carry bytes;
// drain() runs what they received on the logic thread, where every Doc lives.
class Hub {
public:
    struct Options {
        std::string host = "127.0.0.1";   // listening on the network is an explicit opt-in
        std::uint16_t port = 0;
        std::string static_dir;
        std::vector<std::string> allowed_origins{};   // exact Origin values a page may come from
        // simplify: a hub bound to another interface lets everyone who can reach the port join.
        // Ceiling: a LAN user, not a web page. Upgrade: a per-session token in the URL, checked
        // before the upgrade, once a hub is exposed beyond one machine.
    };
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
