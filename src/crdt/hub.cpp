#include <prism/crdt/hub.hpp>

#include <prism/core/channel.hpp>
#include <prism/crdt/doc.hpp>
#include <prism/crdt/sync.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace prism::crdt {

namespace {

// How long a socket read may wait before the owning thread looks at its stop flag again. Also the
// slice every send/lifecycle change becomes visible in, on a peer that has gone quiet.
constexpr auto read_slice = std::chrono::milliseconds{100};

constexpr auto connect_timeout = std::chrono::seconds{3};

// A socket owned by one thread, reachable from the logic thread for sends. The owner attaches it
// before reading from it and detaches it under the same mutex before the socket dies, so a send
// from the logic thread either reaches a live socket or nothing -- never a dangling one.
template <typename Socket>
class Link {
public:
    void attach(Socket& socket) {
        std::lock_guard lock{mutex_};
        socket_ = &socket;
    }

    void detach() {
        std::lock_guard lock{mutex_};
        socket_ = nullptr;
    }

    bool send(std::span<const std::byte> message) {
        std::lock_guard lock{mutex_};
        if (!socket_) return false;
        return socket_->send(reinterpret_cast<const char*>(message.data()), message.size());
    }

    void close() {
        std::lock_guard lock{mutex_};
        if (socket_) socket_->close();
    }

private:
    std::mutex mutex_;
    Socket* socket_ = nullptr;
};

using ServerLink = Link<httplib::ws::WebSocket>;
using ClientLink = Link<httplib::ws::WebSocketClient>;

std::vector<std::byte> to_bytes(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}

bool carries_message(httplib::ws::ReadResult result) {
    return result == httplib::ws::ReadResult::Text || result == httplib::ws::ReadResult::Binary;
}

// What a socket thread hands to the logic thread. One channel carries all three so their order
// stays the order the threads produced them in.
struct ServerEvent {
    enum class Kind { connected, message, disconnected };

    Kind kind;
    std::uint64_t client = 0;
    std::shared_ptr<ServerLink> link{};
    std::vector<std::byte> message{};
};

ServerEvent connected_event(std::uint64_t client, std::shared_ptr<ServerLink> link) {
    return ServerEvent{.kind = ServerEvent::Kind::connected, .client = client, .link = std::move(link)};
}

ServerEvent message_event(std::uint64_t client, std::string& text) {
    return ServerEvent{.kind = ServerEvent::Kind::message, .client = client, .message = to_bytes(text)};
}

ServerEvent disconnected_event(std::uint64_t client) {
    return ServerEvent{.kind = ServerEvent::Kind::disconnected, .client = client};
}

struct ClientEvent {
    enum class Kind { connected, message, closed };

    Kind kind;
    std::vector<std::byte> message{};
};

} // namespace

struct Hub::Impl {
    Impl(Doc& document, Hub::Options hub_options)
        : doc(&document), options(std::move(hub_options)) {
        events.observe([this](const ServerEvent& event) { handle(event); });
        serve_files();
        // The browser page needs both to use shared memory for the wasm build.
        server.set_default_headers({{"Cross-Origin-Opener-Policy", "same-origin"},
                                    {"Cross-Origin-Embedder-Policy", "require-corp"}});
        // simplify: one room for every path -- every connection shares this Doc. Map a room name
        // to its own Doc when more than one document per hub is wanted.
        server.WebSocket("/.*", [this](const httplib::Request&, httplib::ws::WebSocket& socket) {
            serve_client(socket);
        });
        port = bind();
        serve = std::jthread([this] { server.listen_after_bind(); });
    }

    // Socket threads only queue what arrived; every Doc and SyncSession stays on the logic thread.
    void serve_client(httplib::ws::WebSocket& socket) {
        const auto link = std::make_shared<ServerLink>();
        link->attach(socket);
        const auto client = next_client.fetch_add(1);
        events.send(connected_event(client, link));
        socket.set_read_timeout(read_slice);   // so this loop notices stopping on a silent peer

        std::string text;
        while (!stopping) {
            const auto result = socket.read(text);
            if (carries_message(result)) events.send(message_event(client, text));
            else if (result != httplib::ws::ReadResult::Timeout) break;
        }

        link->detach();
        events.send(disconnected_event(client));
    }

    void handle(const ServerEvent& event) {
        switch (event.kind) {
        case ServerEvent::Kind::connected: adopt(event.client, event.link); break;
        case ServerEvent::Kind::message: receive(event.client, event.message); break;
        case ServerEvent::Kind::disconnected: sessions.erase(event.client); break;
        }
    }

    // The hub speaks first, so a client answers with what it already has: that is how an edit made
    // before its session existed still reaches this Doc.
    void adopt(std::uint64_t client, const std::shared_ptr<ServerLink>& link) {
        const auto send = [link](std::span<const std::byte> message) { link->send(message); };
        auto session = std::make_unique<SyncSession>(*doc, send);
        session->start();
        sessions.emplace(client, Session{link, std::move(session)});
    }

    // The sender's own session applies its message to the hub's Doc (answering a step1, applying a
    // step2 or update); a step2/update then goes on to the peers unchanged, because applying it
    // here emits nothing on Doc::on_update(). A step1 is answered only, never passed on.
    void receive(std::uint64_t client, std::span<const std::byte> message) {
        const auto session = sessions.find(client);
        if (session == sessions.end()) return;   // it disconnected before this was drained
        session->second.sync->receive(message);
        relay(client, message);
    }

    void relay(std::uint64_t sender, std::span<const std::byte> message) {
        const auto decoded = decode_sync(message);
        if (!decoded || decoded->kind == SyncKind::step1) return;
        for (const auto& [client, session] : sessions)
            if (client != sender) session.link->send(message);
    }

    std::uint16_t bind() {
        const int bound = options.port == 0
                              ? server.bind_to_any_port(options.host)
                              : (server.bind_to_port(options.host, static_cast<int>(options.port))
                                     ? static_cast<int>(options.port)
                                     : -1);
        if (bound < 0) throw std::runtime_error{"crdt hub could not bind " + options.host};
        return static_cast<std::uint16_t>(bound);
    }

    void serve_files() {
        if (!options.static_dir.empty()) server.set_mount_point("/", options.static_dir);
    }

    struct Session {
        std::shared_ptr<ServerLink> link;
        std::unique_ptr<SyncSession> sync;
    };

    Doc* doc;
    Hub::Options options;
    httplib::Server server;
    std::uint16_t port = 0;
    std::atomic<std::uint64_t> next_client{0};
    std::atomic<bool> stopping{false};
    core::Channel<ServerEvent> events;
    std::map<std::uint64_t, Session> sessions;
    std::jthread serve;
};

Hub::Hub(Doc& doc, Options options) : impl_(std::make_unique<Impl>(doc, std::move(options))) {}

// Closing each connection tells its peer we are done, and ends its read loop; stopping the server
// ends the accept loop, whose pool then joins every socket thread before this returns.
Hub::~Hub() {
    impl_->stopping = true;
    for (const auto& entry : impl_->sessions) entry.second.link->close();
    impl_->server.stop();
    impl_->serve = std::jthread{};
}

std::uint16_t Hub::port() const { return impl_->port; }

void Hub::drain() { impl_->events.drain_notifications(); }

struct Client::Impl {
    Impl(Doc& document, std::string address)
        : doc(&document), url(std::move(address)) {
        events.observe([this](const ClientEvent& event) { handle(event); });
        worker = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    // simplify: one connect attempt, no reconnect -- a dropped socket stays down until the Client
    // is replaced. Add a backoff loop here when a session has to survive a network blip.
    void run(const std::stop_token stop) {
        httplib::ws::WebSocketClient socket{url};
        socket.set_connection_timeout(connect_timeout);
        socket.set_read_timeout(read_slice);   // so this loop notices stop, and flushes sends
        if (!socket.is_valid() || !socket.connect()) {
            events.send(ClientEvent{.kind = ClientEvent::Kind::closed});
            return;
        }

        link->attach(socket);
        events.send(ClientEvent{.kind = ClientEvent::Kind::connected});

        std::string text;
        while (!stop.stop_requested()) {
            const auto result = socket.read(text);
            if (carries_message(result)) events.send(ClientEvent{.kind = ClientEvent::Kind::message, .message = to_bytes(text)});
            else if (result != httplib::ws::ReadResult::Timeout) break;
        }

        link->detach();
        socket.close();
        events.send(ClientEvent{.kind = ClientEvent::Kind::closed});
    }

    void handle(const ClientEvent& event) {
        switch (event.kind) {
        case ClientEvent::Kind::connected: adopt(); break;
        case ClientEvent::Kind::message:
            if (session) session->receive(event.message);
            break;
        case ClientEvent::Kind::closed:
            session.reset();   // drops the on_update connection with it
            connected = false;
            break;
        }
    }

    void adopt() {
        const auto send = [socket = link](std::span<const std::byte> message) { socket->send(message); };
        session = std::make_unique<SyncSession>(*doc, send);
        session->start();
        connected = true;
    }

    Doc* doc;
    std::string url;
    std::shared_ptr<ClientLink> link = std::make_shared<ClientLink>();
    core::Channel<ClientEvent> events;
    std::unique_ptr<SyncSession> session;
    bool connected = false;
    std::jthread worker;
};

Client::Client(Doc& doc, std::string url) : impl_(std::make_unique<Impl>(doc, std::move(url))) {}
Client::~Client() = default;

void Client::drain() { impl_->events.drain_notifications(); }

bool Client::connected() const { return impl_->connected; }

} // namespace prism::crdt
