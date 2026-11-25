// crdt::SyncSession and the y-protocols sync codec (doc/design/sync.md, "Step 2"; the plan's
// Task 2). The byte strings are what y-protocols/sync.js writes: a var-uint message type, a
// var-uint sub-type, then a var-uint length and the payload.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/crdt/doc.hpp>
#include <prism/crdt/field.hpp>
#include <prism/crdt/sync.hpp>

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <span>
#include <vector>

using namespace prism;
using namespace prism::crdt;

namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const unsigned value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

// A payload long enough that its length needs more than one var-uint byte, with every byte value
// in it so a mis-sized copy shows.
std::vector<std::byte> payload_of(std::size_t size) {
    std::vector<std::byte> result(size);
    for (std::size_t i = 0; i < size; ++i) result[i] = static_cast<std::byte>(i % 256);
    return result;
}

// What a session's send is handed, kept for the test to read back.
struct Outbox {
    std::vector<std::vector<std::byte>> messages;

    std::function<void(std::span<const std::byte>)> send() {
        return [this](std::span<const std::byte> message) {
            messages.emplace_back(message.begin(), message.end());
        };
    }
};

} // namespace

TEST_CASE("an empty step1 is the three-byte sync header") {
    CHECK(encode_sync(SyncKind::step1, {}) == bytes({0, 0, 0}));
}

TEST_CASE("each sync kind round-trips") {
    const std::vector<std::byte> payload = bytes({0x00, 0x7F, 0x80, 0xFF, 0x2A});

    for (const SyncKind kind : {SyncKind::step1, SyncKind::step2, SyncKind::update}) {
        const std::vector<std::byte> encoded = encode_sync(kind, payload);
        const std::optional<SyncMessage> decoded = decode_sync(encoded);

        REQUIRE(decoded.has_value());
        CHECK(decoded->kind == kind);
        CHECK(decoded->payload == payload);
    }
}

TEST_CASE("a 300-byte payload travels with a two-byte var-uint length") {
    const std::vector<std::byte> payload = payload_of(300);
    const std::vector<std::byte> message = encode_sync(SyncKind::update, payload);

    // 300 = 0b100101100: low seven bits 0x2C, the rest 0b10, so lib0 writes 0xAC then 0x02.
    REQUIRE(message.size() == payload.size() + 4);
    CHECK(message[0] == std::byte{0});
    CHECK(message[1] == std::byte{2});
    CHECK(message[2] == std::byte{0xAC});
    CHECK(message[3] == std::byte{0x02});

    const std::optional<SyncMessage> decoded = decode_sync(message);
    REQUIRE(decoded.has_value());
    CHECK(decoded->kind == SyncKind::update);
    CHECK(decoded->payload == payload);
}

TEST_CASE("messages that are not sync decode to nothing") {
    CHECK_FALSE(decode_sync(bytes({1, 5, 3, 1, 2, 3})).has_value());   // awareness
    CHECK_FALSE(decode_sync(bytes({2, 0, 1, 0})).has_value());         // a type PRISM does not know
}

TEST_CASE("truncated messages decode to nothing") {
    CHECK_FALSE(decode_sync({}).has_value());
    CHECK_FALSE(decode_sync(bytes({0})).has_value());              // a sync type, no sub-type
    CHECK_FALSE(decode_sync(bytes({0, 2})).has_value());           // an update, no length
    CHECK_FALSE(decode_sync(bytes({0, 2, 0x05, 0x01})).has_value());   // length past the end
    CHECK_FALSE(decode_sync(bytes({0, 2, 0x80})).has_value());         // ends inside a var-uint
    CHECK_FALSE(decode_sync(bytes({0, 2, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F})).has_value());
}

TEST_CASE("an overlong var-uint decodes to nothing") {
    // Eleven continuation bytes overflow any length this codec can hold, whatever it ends with.
    std::vector<std::byte> message = bytes({0, 2});
    for (int i = 0; i < 11; ++i) message.push_back(std::byte{0xFF});
    message.push_back(std::byte{0x00});

    CHECK_FALSE(decode_sync(message).has_value());
}

TEST_CASE("start asks for the peer's state as step1") {
    Doc doc;
    Outbox outbox;
    SyncSession session(doc, outbox.send());

    session.start();

    REQUIRE(outbox.messages.size() == 1);
    const std::optional<SyncMessage> message = decode_sync(outbox.messages.front());
    REQUIRE(message.has_value());
    CHECK(message->kind == SyncKind::step1);
    CHECK(message->payload == doc.state_vector());
}

TEST_CASE("a step1 is answered with step2 carrying what the peer lacks") {
    Doc source;
    Field<double> gain;
    source.bind("gain", gain);
    gain = 3.0;

    Outbox outbox;
    SyncSession session(source, outbox.send());

    Doc peer;
    Field<double> peer_gain;
    peer.bind("gain", peer_gain);
    session.receive(encode_sync(SyncKind::step1, peer.state_vector()));

    REQUIRE(outbox.messages.size() == 1);
    const std::optional<SyncMessage> answer = decode_sync(outbox.messages.front());
    REQUIRE(answer.has_value());
    CHECK(answer->kind == SyncKind::step2);

    peer.apply_update(answer->payload);
    CHECK(peer_gain.get() == 3.0);
}

TEST_CASE("a local edit leaves as an update") {
    Doc doc;
    Field<double> gain;
    doc.bind("gain", gain);
    Outbox outbox;
    SyncSession session(doc, outbox.send());

    gain = 2.5;

    REQUIRE(outbox.messages.size() == 1);
    const std::optional<SyncMessage> message = decode_sync(outbox.messages.front());
    REQUIRE(message.has_value());
    CHECK(message->kind == SyncKind::update);

    Doc peer;
    Field<double> peer_gain;
    peer.bind("gain", peer_gain);
    peer.apply_update(message->payload);
    CHECK(peer_gain.get() == 2.5);
}

TEST_CASE("two sessions start and then carry edits both ways") {
    Doc a;
    Doc b;
    Field<double> gain_a;
    Field<double> gain_b;
    a.bind("gain", gain_a);
    b.bind("gain", gain_b);

    // The send functions only know their peer once both sessions exist.
    SyncSession* b_side = nullptr;
    SyncSession* a_side = nullptr;
    SyncSession session_a(a, [&b_side](std::span<const std::byte> message) {
        b_side->receive(message);
    });
    SyncSession session_b(b, [&a_side](std::span<const std::byte> message) {
        a_side->receive(message);
    });
    b_side = &session_b;
    a_side = &session_a;

    session_a.start();
    session_b.start();

    gain_a = 2.5;
    CHECK(gain_b.get() == 2.5);

    gain_b = 3.5;
    CHECK(gain_a.get() == 3.5);
}

TEST_CASE("a received update is not echoed back") {
    Doc origin;
    Doc mirror;
    Field<double> origin_gain;
    Field<double> mirror_gain;
    origin.bind("gain", origin_gain);
    mirror.bind("gain", mirror_gain);
    origin_gain = 3.5;

    Outbox outbox;
    SyncSession session(mirror, outbox.send());
    session.receive(encode_sync(SyncKind::update, origin.encode_since(mirror.state_vector())));

    CHECK(mirror_gain.get() == 3.5);
    CHECK(outbox.messages.empty());
}

TEST_CASE("an awareness message is ignored and the session keeps going") {
    Doc doc;
    Field<double> gain;
    doc.bind("gain", gain);
    Outbox outbox;
    SyncSession session(doc, outbox.send());

    // Type 1, then a var-uint length and the awareness update's bytes, as y-protocols writes it.
    CHECK_NOTHROW(session.receive(bytes({1})));
    CHECK_NOTHROW(session.receive(bytes({1, 4, 0x01, 0x02, 0x03, 0x04})));
    CHECK(outbox.messages.empty());

    Doc origin;
    Field<double> origin_gain;
    origin.bind("gain", origin_gain);
    origin_gain = 4.5;
    session.receive(encode_sync(SyncKind::update, origin.encode_since(doc.state_vector())));

    CHECK(gain.get() == 4.5);
    CHECK(outbox.messages.empty());
}

TEST_CASE("malformed messages are ignored and the session keeps going") {
    Doc doc;
    Field<double> gain;
    doc.bind("gain", gain);
    Outbox outbox;
    SyncSession session(doc, outbox.send());

    const std::vector<std::vector<std::byte>> broken = {
        {},
        bytes({0}),
        bytes({0, 2}),
        bytes({0, 2, 0x05, 0x01}),
        bytes({0, 2, 0x80}),
        bytes({0, 2, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F}),
        bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF}),
        bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00}),
    };
    for (const std::vector<std::byte>& message : broken) {
        CHECK_NOTHROW(session.receive(message));
    }
    CHECK(outbox.messages.empty());

    Doc origin;
    Field<double> origin_gain;
    origin.bind("gain", origin_gain);
    origin_gain = 5.5;
    session.receive(encode_sync(SyncKind::update, origin.encode_since(doc.state_vector())));

    CHECK(gain.get() == 5.5);
    CHECK(outbox.messages.empty());
}
