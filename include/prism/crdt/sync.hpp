#pragma once

#include <prism/core/connection.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace prism::crdt {

class Doc;

// The three y-protocols sync sub-types (sync.js): step1 asks for the peer's state vector, step2
// answers with what the peer lacks, update carries a live edit.
enum class SyncKind : std::uint8_t { step1 = 0, step2 = 1, update = 2 };

// One whole binary message, as a WebSocket frame carries it: the sync message type, the sub-type
// and the payload length as var-uints (lib0's encoding), then the payload.
std::vector<std::byte> encode_sync(SyncKind, std::span<const std::byte> payload);

struct SyncMessage { SyncKind kind; std::vector<std::byte> payload; };

// nullopt: not sync, or malformed. An awareness message (type 1) is ignored like any other
// non-sync type; a bad var-uint or a length past the end is not a message to act on either.
std::optional<SyncMessage> decode_sync(std::span<const std::byte> message);

// One peer's half of a y-protocols session over a Doc. start() sends SyncStep1; receive() handles
// one message; a local edit leaves through send() as an Update. No socket here: a transport is a
// thin shell around this, so the same session serves the desktop, the browser and an ESP-IDF board.
class SyncSession {
public:
    SyncSession(Doc& doc, std::function<void(std::span<const std::byte>)> send);
    // The connection back to the Doc holds this session's address: a copy would be wired to the
    // original.
    SyncSession(const SyncSession&) = delete;
    void start();                                  // sends SyncStep1
    void receive(std::span<const std::byte> message);

private:
    void send_message(SyncKind kind, std::span<const std::byte> payload);

    Doc* doc_;
    std::function<void(std::span<const std::byte>)> send_;
    // A local edit leaves through send() as it happens. A received one is applied, and the Doc
    // emits nothing for an applied update, so it never goes back out.
    core::Connection updates_;
};

} // namespace prism::crdt
