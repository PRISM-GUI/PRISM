#include <prism/crdt/room.hpp>

#include <prism/crdt/doc.hpp>
#include <prism/crdt/sync.hpp>

#include <map>
#include <memory>
#include <span>
#include <utility>

namespace prism::crdt {

struct Room::Impl {
    explicit Impl(Doc& document) : doc(&document) {}

    // The room speaks first, so a client answers with what it already has: that is how an edit made
    // before its session existed still reaches this Doc.
    void join(PeerId peer, Send send) {
        auto session = std::make_unique<SyncSession>(*doc, send);
        session->start();
        sessions.emplace(peer, Session{std::move(send), std::move(session)});
    }

    // The sender's own session applies its message to the room's Doc (answering a step1, applying a
    // step2 or update); a step2/update then goes on to the peers unchanged, because applying it
    // here emits nothing on Doc::on_update(). A step1 is answered only, never passed on.
    void receive(PeerId peer, std::span<const std::byte> message) {
        const auto session = sessions.find(peer);
        if (session == sessions.end()) return;   // it left before this arrived
        session->second.sync->receive(message);
        relay(peer, message);
    }

    void relay(PeerId sender, std::span<const std::byte> message) {
        const auto decoded = decode_sync(message);
        if (!decoded || decoded->kind == SyncKind::step1) return;
        for (const auto& [peer, session] : sessions)
            if (peer != sender) session.send(message);
    }

    // Forgetting a peer stops the relay to it and drops its session's Doc subscription.
    void leave(PeerId peer) { sessions.erase(peer); }

    struct Session {
        Send send;
        std::unique_ptr<SyncSession> sync;
    };

    Doc* doc;
    std::map<PeerId, Session> sessions;
};

Room::Room(Doc& doc) : impl_(std::make_unique<Impl>(doc)) {}
Room::~Room() = default;

void Room::join(PeerId peer, Send send) { impl_->join(peer, std::move(send)); }

void Room::receive(PeerId peer, std::span<const std::byte> message) {
    impl_->receive(peer, message);
}

void Room::leave(PeerId peer) { impl_->leave(peer); }

std::size_t Room::size() const { return impl_->sessions.size(); }

} // namespace prism::crdt
