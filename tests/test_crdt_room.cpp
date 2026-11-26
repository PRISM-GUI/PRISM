// crdt::Room: the hub's relay without sockets (doc/design/sync.md, "Step 3"; the plan's Task 1).
// A peer is a Doc, a SyncSession and the two directions of an in-memory link; the test carries
// every message across by hand, so no socket and no thread is involved.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/crdt/doc.hpp>
#include <prism/crdt/field.hpp>
#include <prism/crdt/room.hpp>
#include <prism/crdt/sync.hpp>

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using namespace prism;
using namespace prism::crdt;

namespace {

using Bytes = std::vector<std::byte>;
using Inbox = std::vector<Bytes>;

// One peer on the far side of an in-memory link: its own replica, the session that speaks for it,
// what it sent to the room (to_room) and what the room sent it (from_room).
struct Peer {
    Doc doc;
    Field<double> gain;
    Inbox to_room;
    Inbox from_room;
    SyncSession session;

    Peer() : session(doc, [this](std::span<const std::byte> message) {
                to_room.emplace_back(message.begin(), message.end());
            }) {
        doc.bind("gain", gain);
    }

    // What Room::join() gets: the room's messages land in from_room, delivered by hand later.
    Room::Send room_send() {
        return [this](std::span<const std::byte> message) {
            from_room.emplace_back(message.begin(), message.end());
        };
    }
};

// A room and the replica it reads and writes.
struct Board {
    Doc doc;
    Field<double> gain;
    Room room;

    Board() : room(doc) { doc.bind("gain", gain); }
};

struct Link {
    Room::PeerId id;
    Peer* peer;
};

// What the peer sent, handed to the room in the order it was produced.
std::size_t deliver_to_room(Peer& peer, Room& room, Room::PeerId id) {
    const Inbox pending = std::exchange(peer.to_room, {});
    for (const Bytes& message : pending) room.receive(id, message);
    return pending.size();
}

// What the room sent this peer, handed to its session in the order it was produced.
std::size_t deliver_to_peer(Peer& peer) {
    const Inbox pending = std::exchange(peer.from_room, {});
    for (const Bytes& message : pending) peer.session.receive(message);
    return pending.size();
}

// Walks every link until no message is left: a step1 is answered with a step2, a step2 or update
// applied and relayed, and neither provokes a reply of its own, so this reaches a fixed point.
// A message the room relayed waits in another peer's inbox until that peer's link is walked.
void settle(Room& room, std::initializer_list<Link> links) {
    for (int round = 0; round < 16; ++round) {
        bool moved = false;
        for (const Link& link : links) {
            moved = deliver_to_room(*link.peer, room, link.id) > 0 || moved;
            moved = deliver_to_peer(*link.peer) > 0 || moved;
        }
        if (!moved) return;
    }
    FAIL("the peers and the room did not settle");
}

// The room's join() speaks first; each peer then asks for what it lacks, as Client::Impl::adopt
// does on a real connection. Both directions have to be spoken: the room's step1 brings the peer's
// state in, the peer's step1 brings the room's state out.
void connect(Room& room, std::initializer_list<Link> links) {
    for (const Link& link : links) {
        room.join(link.id, link.peer->room_send());
        link.peer->session.start();
    }
    settle(room, links);
}

} // namespace

TEST_CASE("join sends exactly one message, a step1") {
    Board board;
    Peer a;

    board.room.join(1, a.room_send());

    CHECK(board.room.size() == 1);
    REQUIRE(a.from_room.size() == 1);
    const std::optional<SyncMessage> message = decode_sync(a.from_room.front());
    REQUIRE(message.has_value());
    CHECK(message->kind == SyncKind::step1);
    CHECK(message->payload == board.doc.state_vector());
    CHECK(a.to_room.empty());
}

TEST_CASE("an update from one peer reaches the other, and never goes back to its sender") {
    Board board;
    Peer a;
    Peer b;
    connect(board.room, {{1, &a}, {2, &b}});
    REQUIRE(a.from_room.empty());
    REQUIRE(b.from_room.empty());

    a.gain = 2.5;
    CHECK(deliver_to_room(a, board.room, 1) == 1);

    // The sender's own session applied its edit; the room's Doc took the relayed update too.
    CHECK(board.gain.get() == 2.5);
    CHECK(a.from_room.empty());

    REQUIRE(b.from_room.size() == 1);
    const std::optional<SyncMessage> relayed = decode_sync(b.from_room.front());
    REQUIRE(relayed.has_value());
    CHECK(relayed->kind == SyncKind::update);

    deliver_to_peer(b);
    CHECK(b.gain.get() == 2.5);
}

TEST_CASE("a step1 from a peer is answered to that peer only") {
    Board board;
    Peer a;
    Peer b;
    connect(board.room, {{1, &a}, {2, &b}});

    a.session.start();
    REQUIRE(a.to_room.size() == 1);
    CHECK(deliver_to_room(a, board.room, 1) == 1);

    CHECK(b.from_room.empty());
    REQUIRE(a.from_room.size() == 1);
    const std::optional<SyncMessage> answer = decode_sync(a.from_room.front());
    REQUIRE(answer.has_value());
    CHECK(answer->kind == SyncKind::step2);
}

TEST_CASE("a peer that left gets nothing, and unknown ids are no-ops") {
    Board board;
    Peer a;
    Peer b;
    connect(board.room, {{1, &a}, {2, &b}});

    board.room.leave(1);
    CHECK(board.room.size() == 1);

    b.gain = 3.5;
    CHECK(deliver_to_room(b, board.room, 2) == 1);
    CHECK(board.gain.get() == 3.5);
    CHECK(a.from_room.empty());
    CHECK(board.room.size() == 1);

    CHECK_NOTHROW(board.room.receive(9, encode_sync(SyncKind::step1, {})));
    CHECK_NOTHROW(board.room.leave(9));
    CHECK(board.room.size() == 1);
    CHECK(a.from_room.empty());
    CHECK(b.from_room.empty());
}

TEST_CASE("a late joiner catches up with the room's document") {
    Board board;
    Peer a;
    Peer b;
    connect(board.room, {{1, &a}, {2, &b}});

    a.gain = 2.5;
    settle(board.room, {{1, &a}, {2, &b}});
    b.gain = 3.5;
    settle(board.room, {{1, &a}, {2, &b}});
    REQUIRE(board.gain.get() == 3.5);
    REQUIRE(a.gain.get() == 3.5);

    Peer c;
    board.room.join(3, c.room_send());
    c.session.start();
    settle(board.room, {{1, &a}, {2, &b}, {3, &c}});

    CHECK(board.room.size() == 3);
    CHECK(c.doc.to_json() == board.doc.to_json());
    CHECK(c.gain.get() == 3.5);
}
