#include <prism/crdt/hub.hpp>

#include <prism/core/channel.hpp>
#include <prism/crdt/doc.hpp>
#include <prism/crdt/sync.hpp>

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
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

// How long a socket read may wait before the owning thread looks at its flags again. Also the
// slice in which a dropped or stopping peer is noticed.
constexpr auto read_slice = std::chrono::milliseconds{100};

constexpr auto connect_timeout = std::chrono::seconds{3};

// How long one frame write may block. A peer that stopped reading would otherwise keep the writer
// inside a socket write forever; with this, the write fails and the peer is dropped.
constexpr auto write_timeout = std::chrono::seconds{1};

// A peer that reads nothing would otherwise grow its queue without bound. Past this, it is
// dropped: a connection that cannot keep up must cost neither memory nor the logic thread time.
constexpr std::size_t max_queued_bytes = 4 << 20;

bool ascii_iequals(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

// "127.0.0.1:8000" -> "127.0.0.1", "[::1]:8000" -> "[::1]".
std::string_view hostname_of(std::string_view host) {
    if (host.starts_with('[')) {
        const auto close = host.find(']');
        return close == std::string_view::npos ? host : host.substr(0, close + 1);
    }
    return host.substr(0, host.find(':'));
}

bool is_dotted_quad(std::string_view hostname) {
    std::size_t parts = 0;
    while (true) {
        const auto dot = hostname.find('.');
        const auto part = hostname.substr(0, dot);
        if (part.empty() || part.size() > 3) return false;
        unsigned value = 0;
        for (const char digit : part) {
            if (digit < '0' || digit > '9') return false;
            value = value * 10 + static_cast<unsigned>(digit - '0');
        }
        if (value > 255) return false;
        if (dot == std::string_view::npos) return ++parts == 4;
        hostname.remove_prefix(dot + 1);
        ++parts;
    }
}

// An IP literal owns no domain name, so a page loaded from it cannot be a rebinding page.
bool is_ip_literal(std::string_view hostname) {
    return hostname.starts_with('[') || is_dotted_quad(hostname);
}

bool same_origin(std::string_view origin, std::string_view host) {
    const auto hostname = hostname_of(host);
    if (!is_ip_literal(hostname) && !ascii_iequals(hostname, "localhost")) return false;
    for (const std::string_view scheme : {"http://", "https://"})
        if (origin.starts_with(scheme) && ascii_iequals(origin.substr(scheme.size()), host))
            return true;
    return false;
}

std::vector<std::byte> to_bytes(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}

bool carries_message(httplib::ws::ReadResult result) {
    return result == httplib::ws::ReadResult::Text || result == httplib::ws::ReadResult::Binary;
}

// The outgoing side of one connection: the logic thread only appends here, and one writer thread
// owns every socket write. cpp-httplib's ws::WebSocket keeps separate read and write mutexes (and
// writes from its own heartbeat thread), so a writer thread is supported next to the reader. The
// socket pointer is safe because the reader thread owns the socket's lifetime and joins this
// writer before the socket dies; nothing on the logic thread ever touches a socket.
template <typename Socket>
class Outbox {
public:
    explicit Outbox(Socket& socket) : socket_(&socket) {}

    // Logic thread. False once this peer is gone, and when this message overflowed the cap: the
    // peer is then dropped, and its session goes away on the next drain.
    bool push(std::span<const std::byte> message) {
        std::lock_guard lock{mutex_};
        if (closed_) return false;
        // The cap bounds the unread backlog, not one message: an idle peer always takes the next
        // one, or a document grown past the cap could never reach a newcomer (its catch-up is one
        // message). One incoming message is bounded by CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH.
        if (!queue_.empty() && queued_bytes_ + message.size() > max_queued_bytes) {
            close_locked();
            return false;
        }
        queue_.emplace_back(message.begin(), message.end());
        queued_bytes_ += message.size();
        ready_.notify_one();
        return true;
    }

    bool closed() const {
        std::lock_guard lock{mutex_};
        return closed_;
    }

    // Any thread: no more sends, wake the writer, discard what is still queued.
    void stop() {
        std::lock_guard lock{mutex_};
        close_locked();
    }

    // The writer thread, until stop() or a write the socket refused.
    void write_until_closed() {
        std::vector<std::byte> message;
        while (take(message)) {
            if (!socket_->send(reinterpret_cast<const char*>(message.data()), message.size())) {
                stop();
                return;
            }
        }
    }

private:
    bool take(std::vector<std::byte>& message) {
        std::unique_lock lock{mutex_};
        ready_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        message = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= message.size();
        return true;
    }

    void close_locked() {
        closed_ = true;
        queue_.clear();
        queued_bytes_ = 0;
        ready_.notify_all();
    }

    Socket* socket_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::vector<std::byte>> queue_;
    std::size_t queued_bytes_ = 0;
    bool closed_ = false;
};

using ServerOutbox = Outbox<httplib::ws::WebSocket>;
using ClientOutbox = Outbox<httplib::ws::WebSocketClient>;

// What a socket thread hands to the logic thread. One channel carries all three so their order
// stays the order the threads produced them in.
struct ServerEvent {
    enum class Kind { connected, message, disconnected };

    Kind kind;
    std::uint64_t client = 0;
    std::shared_ptr<ServerOutbox> outbox{};
    std::vector<std::byte> message{};
};

ServerEvent connected_event(std::uint64_t client, std::shared_ptr<ServerOutbox> outbox) {
    return ServerEvent{
        .kind = ServerEvent::Kind::connected, .client = client, .outbox = std::move(outbox)};
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
    std::shared_ptr<ClientOutbox> outbox{};
    std::vector<std::byte> message{};
};

} // namespace

bool origin_allowed(std::string_view origin, std::string_view host,
                    std::span<const std::string> allowed) {
    if (origin.empty()) return true;
    const bool listed = std::ranges::find_if(
                            allowed, [&](const std::string& candidate) { return candidate == origin; }) !=
                        allowed.end();
    return listed || same_origin(origin, host);
}

struct Hub::Impl {
    Impl(Doc& document, Hub::Options hub_options)
        : doc(&document), options(std::move(hub_options)) {
        events.observe([this](const ServerEvent& event) { handle(event); });
        serve_files();
        // The browser page needs both to use shared memory for the wasm build.
        server.set_default_headers({{"Cross-Origin-Opener-Policy", "same-origin"},
                                    {"Cross-Origin-Embedder-Policy", "require-corp"}});
        server.set_write_timeout(write_timeout);   // a peer that stopped reading fails, it never blocks
        server.set_pre_routing_handler(
            [this](const httplib::Request& request, httplib::Response& response) {
                if (!httplib::detail::is_websocket_upgrade(request) ||
                    origin_allowed(request.get_header_value("Origin"), request.get_header_value("Host"),
                                   options.allowed_origins))
                    return httplib::Server::HandlerResponse::Unhandled;
                response.status = 403;
                return httplib::Server::HandlerResponse::Handled;
            });
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
        const auto outbox = std::make_shared<ServerOutbox>(socket);
        const auto client = next_client.fetch_add(1);
        events.send(connected_event(client, outbox));

        std::jthread writer{[&outbox] { outbox->write_until_closed(); }};
        socket.set_read_timeout(read_slice);   // so this loop notices stopping, and a dropped peer

        std::string text;
        while (!stopping && !outbox->closed()) {
            const auto result = socket.read(text);
            if (carries_message(result)) events.send(message_event(client, text));
            else if (result != httplib::ws::ReadResult::Timeout) break;
        }

        outbox->stop();
        writer.join();
        socket.close();
        events.send(disconnected_event(client));
    }

    void handle(const ServerEvent& event) {
        switch (event.kind) {
        case ServerEvent::Kind::connected: adopt(event.client, event.outbox); break;
        case ServerEvent::Kind::message: receive(event.client, event.message); break;
        case ServerEvent::Kind::disconnected: sessions.erase(event.client); break;
        }
    }

    // The hub speaks first, so a client answers with what it already has: that is how an edit made
    // before its session existed still reaches this Doc.
    void adopt(std::uint64_t client, const std::shared_ptr<ServerOutbox>& outbox) {
        const auto send = [outbox](std::span<const std::byte> message) { outbox->push(message); };
        auto session = std::make_unique<SyncSession>(*doc, send);
        session->start();
        sessions.emplace(client, Session{outbox, std::move(session)});
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
            if (client != sender) session.outbox->push(message);
    }

    // A session whose peer was dropped has had its queue cleared already; forgetting it here stops
    // the relay and drops its Doc subscription.
    void sweep_dropped() {
        std::erase_if(sessions, [](const auto& entry) { return entry.second.outbox->closed(); });
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
        std::shared_ptr<ServerOutbox> outbox;
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

// Stopping wakes every writer and ends every read loop; stopping the server ends the accept loop,
// whose pool then joins every socket thread before this returns.
Hub::~Hub() {
    impl_->stopping = true;
    for (const auto& entry : impl_->sessions) entry.second.outbox->stop();
    impl_->server.stop();
    impl_->serve = std::jthread{};
}

std::uint16_t Hub::port() const { return impl_->port; }

void Hub::drain() {
    impl_->events.drain_notifications();
    impl_->sweep_dropped();
}

void Hub::set_waker(std::function<void()> fn) { impl_->events.set_waker(std::move(fn)); }

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
        socket.set_write_timeout(write_timeout);
        socket.set_read_timeout(read_slice);   // so this loop notices stop, and a dropped peer
        if (!socket.is_valid() || !socket.connect()) {
            events.send(ClientEvent{.kind = ClientEvent::Kind::closed});
            return;
        }

        const auto outbox = std::make_shared<ClientOutbox>(socket);
        events.send(ClientEvent{.kind = ClientEvent::Kind::connected, .outbox = outbox});

        std::jthread writer{[&outbox] { outbox->write_until_closed(); }};
        std::string text;
        while (!stop.stop_requested() && !outbox->closed()) {
            const auto result = socket.read(text);
            if (carries_message(result))
                events.send(ClientEvent{.kind = ClientEvent::Kind::message, .message = to_bytes(text)});
            else if (result != httplib::ws::ReadResult::Timeout) break;
        }

        outbox->stop();
        writer.join();
        socket.close();
        events.send(ClientEvent{.kind = ClientEvent::Kind::closed});
    }

    void handle(const ClientEvent& event) {
        switch (event.kind) {
        case ClientEvent::Kind::connected: adopt(event.outbox); break;
        case ClientEvent::Kind::message:
            if (session) session->receive(event.message);
            break;
        case ClientEvent::Kind::closed:
            session.reset();   // drops the on_update connection with it
            connected = false;
            break;
        }
    }

    void adopt(const std::shared_ptr<ClientOutbox>& outbox) {
        const auto send = [outbox](std::span<const std::byte> message) { outbox->push(message); };
        session = std::make_unique<SyncSession>(*doc, send);
        session->start();
        connected = true;
    }

    Doc* doc;
    std::string url;
    core::Channel<ClientEvent> events;
    std::unique_ptr<SyncSession> session;
    bool connected = false;
    std::jthread worker;
};

Client::Client(Doc& doc, std::string url) : impl_(std::make_unique<Impl>(doc, std::move(url))) {}
Client::~Client() = default;

void Client::drain() { impl_->events.drain_notifications(); }

bool Client::connected() const { return impl_->connected; }

void Client::set_waker(std::function<void()> fn) { impl_->events.set_waker(std::move(fn)); }

} // namespace prism::crdt
