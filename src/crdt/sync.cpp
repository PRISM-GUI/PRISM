#include <prism/crdt/sync.hpp>

#include <prism/crdt/doc.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace prism::crdt {

namespace {

// y-protocols' outer message types: 0 is sync, 1 is awareness, which a session ignores.
constexpr std::uint64_t message_type_sync = 0;
constexpr std::uint64_t last_sync_kind = static_cast<std::uint64_t>(SyncKind::update);

// lib0's var-uint, the way y-protocols and lib0 write it: seven bits per byte, low group first,
// the high bit set on all but the last. Five bytes hold 35 bits, which covers every length a
// frame can carry; past that the value is not one this codec could have written.
std::optional<std::uint32_t> read_varuint(std::span<const std::byte> message, std::size_t& pos) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
        if (pos == message.size()) return std::nullopt;
        const unsigned byte = std::to_integer<unsigned>(message[pos++]);
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if (byte < 0x80) {
            if (value > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
            return static_cast<std::uint32_t>(value);
        }
    }
    return std::nullopt;
}

void write_varuint(std::vector<std::byte>& message, std::uint64_t value) {
    for (; value >= 0x80; value >>= 7) {
        message.push_back(static_cast<std::byte>((value & 0x7F) | 0x80));
    }
    message.push_back(static_cast<std::byte>(value));
}

} // namespace

std::vector<std::byte> encode_sync(SyncKind kind, std::span<const std::byte> payload) {
    std::vector<std::byte> message;
    message.reserve(payload.size() + 8);
    write_varuint(message, message_type_sync);
    write_varuint(message, static_cast<std::uint64_t>(kind));
    write_varuint(message, payload.size());
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::optional<SyncMessage> decode_sync(std::span<const std::byte> message) {
    std::size_t pos = 0;
    const std::optional<std::uint32_t> type = read_varuint(message, pos);
    if (!type.has_value() || *type != message_type_sync) return std::nullopt;
    const std::optional<std::uint32_t> kind = read_varuint(message, pos);
    if (!kind.has_value() || *kind > last_sync_kind) return std::nullopt;
    const std::optional<std::uint32_t> length = read_varuint(message, pos);
    if (!length.has_value() || message.size() - pos < *length) return std::nullopt;

    const std::byte* first = message.data() + pos;
    return SyncMessage{static_cast<SyncKind>(*kind), std::vector<std::byte>(first, first + *length)};
}

SyncSession::SyncSession(Doc& doc, std::function<void(std::span<const std::byte>)> send)
    : doc_(&doc), send_(std::move(send)) {
    updates_ = doc.on_update().connect([this](std::span<const std::byte> update) {
        send_message(SyncKind::update, update);
    });
}

void SyncSession::start() {
    send_message(SyncKind::step1, doc_->state_vector());
}

void SyncSession::receive(std::span<const std::byte> message) {
    const std::optional<SyncMessage> decoded = decode_sync(message);
    // Awareness, or bytes that are not a sync message at all: there is nothing to answer.
    if (!decoded.has_value()) return;
    switch (decoded->kind) {
    case SyncKind::step1:
        send_message(SyncKind::step2, doc_->encode_since(decoded->payload));
        return;
    case SyncKind::step2:
    case SyncKind::update:
        doc_->apply_update(decoded->payload);
        return;
    }
}

void SyncSession::send_message(SyncKind kind, std::span<const std::byte> payload) {
    const std::vector<std::byte> message = encode_sync(kind, payload);
    send_(message);
}

} // namespace prism::crdt
