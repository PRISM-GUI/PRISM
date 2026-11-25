#include <prism/crdt/hub.hpp>

#include <prism/core/channel.hpp>
#include <prism/crdt/doc.hpp>
#include <prism/crdt/sync.hpp>

#include <emscripten/websocket.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace prism::crdt {

namespace {

// What the browser's callbacks hand to the logic thread. Closed covers both "the upgrade failed"
// and "the socket is gone", which the Client treats the same way: no session until a new Client.
struct ClientEvent {
    enum class Kind { connected, message, closed };

    Kind kind;
    std::vector<std::byte> message{};
};

std::vector<std::byte> to_bytes(const std::uint8_t* data, std::uint32_t size) {
    const auto* begin = reinterpret_cast<const std::byte*>(data);
    return {begin, begin + size};
}

} // namespace

// The browser's WebSocket is a JavaScript object. Emscripten dispatches its events on the main
// browser thread's event loop -- whichever thread created the socket; createOnMainThread is
// documented but not implemented, every socket lives on the main thread -- and every call from a
// pthread into that library is a synchronous proxy the main thread runs. So this client keeps the
// native one's shape (bytes into a channel, drain() on the logic thread) with callbacks that do
// nothing but copy bytes in. A send is such a proxy; it copies the bytes into the browser's buffer
// and returns, so a peer that stopped reading still cannot stall the logic thread -- only a main
// thread that is itself blocked could, and it never waits on the logic thread.
struct Client::Impl {
    // The socket is created here, on the thread that builds the Client: main() runs on the browser
    // main thread, so this call is direct and no proxy is involved. Registering the handlers right
    // after cannot miss an event, because this task cannot be interrupted.
    Impl(Doc& document, std::string address) : doc(&document), url(std::move(address)) {
        events.observe([this](const ClientEvent& event) { handle(event); });
        connect();
    }

    ~Impl() { disconnect(); }

    void drain() { events.drain_notifications(); }

    void connect() {
        EmscriptenWebSocketCreateAttributes attributes;
        emscripten_websocket_init_create_attributes(&attributes);
        attributes.url = url.c_str();
        attributes.createOnMainThread = true;
        socket = emscripten_websocket_new(&attributes);
        if (socket <= 0) {
            events.send(ClientEvent{.kind = ClientEvent::Kind::closed});
            return;
        }
        emscripten_websocket_set_onopen_callback(socket, this, on_open);
        emscripten_websocket_set_onmessage_callback(socket, this, on_message);
        emscripten_websocket_set_onclose_callback(socket, this, on_close);
    }

    // delete() unhooks every handler, so a close event after this never reaches a destroyed Impl.
    // The session goes first: its send lambda must not outlive the socket it writes to.
    void disconnect() {
        session.reset();
        if (socket <= 0) return;
        emscripten_websocket_close(socket, 1000, "bye");
        emscripten_websocket_delete(socket);
        socket = 0;
    }

    static bool on_open(int, const EmscriptenWebSocketOpenEvent*, void* user_data) {
        static_cast<Impl*>(user_data)->events.send(ClientEvent{.kind = ClientEvent::Kind::connected});
        return true;
    }

    static bool on_message(int, const EmscriptenWebSocketMessageEvent* event, void* user_data) {
        static_cast<Impl*>(user_data)->events.send(
            ClientEvent{.kind = ClientEvent::Kind::message,
                        .message = to_bytes(event->data, event->numBytes)});
        return true;
    }

    static bool on_close(int, const EmscriptenWebSocketCloseEvent*, void* user_data) {
        static_cast<Impl*>(user_data)->events.send(ClientEvent{.kind = ClientEvent::Kind::closed});
        return true;
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
        session = std::make_unique<SyncSession>(*doc, [this](std::span<const std::byte> message) {
            emscripten_websocket_send_binary(socket, const_cast<std::byte*>(message.data()),
                                             static_cast<std::uint32_t>(message.size()));
        });
        session->start();
        connected = true;
    }

    Doc* doc;
    std::string url;
    core::Channel<ClientEvent> events;
    std::unique_ptr<SyncSession> session;
    bool connected = false;
    EMSCRIPTEN_WEBSOCKET_T socket = 0;
};

Client::Client(Doc& doc, std::string url) : impl_(std::make_unique<Impl>(doc, std::move(url))) {}
Client::~Client() = default;

void Client::drain() { impl_->drain(); }

bool Client::connected() const { return impl_->connected; }

void Client::set_waker(std::function<void()> fn) { impl_->events.set_waker(std::move(fn)); }

} // namespace prism::crdt
