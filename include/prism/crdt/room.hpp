#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace prism::crdt {

class Doc;

// The relay at the centre of a hub: every peer is an id and a way to send it bytes. Call it from
// one thread (the logic thread): it reads and writes the Doc.
class Room {
public:
    using PeerId = std::uint64_t;
    using Send = std::function<void(std::span<const std::byte>)>;

    explicit Room(Doc& doc);
    ~Room();
    Room(const Room&) = delete;
    Room& operator=(const Room&) = delete;

    void join(PeerId peer, Send send);   // the room speaks first: SyncSession::start()
    void receive(PeerId peer, std::span<const std::byte> message);   // unknown peer: ignored
    void leave(PeerId peer);             // unknown peer: ignored
    std::size_t size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace prism::crdt
