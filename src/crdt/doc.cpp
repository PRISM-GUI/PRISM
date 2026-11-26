#include <prism/crdt/doc.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ycpp/doc.hpp>
#include <ycpp/map.hpp>

namespace prism::crdt {

namespace {

// One field bound to one key of the root map (a nested struct's key is dotted: "in.on"): how to
// write a value into it, and the connection that carries its edits the other way.
struct Binding {
    std::string key;
    std::function<bool(const Value&)> write;
    core::Connection changed;
};

// Yjs gives every replica a random 32-bit client id, which is what keeps two peers' edits apart
// and lets a merge keep both. A hosted build asks the OS, plus a counter that keeps replicas in one
// process apart; a board has no entropy source, so it takes the clock instead.
// simplify: a board booted at the same tick as another picks the same seed; upgrade: the chip's
// unique id, or the platform's entropy source where it has one.
std::uint64_t next_client_id() {
    static std::atomic<std::uint64_t> counter{0};
#if defined(PRISM_FRAME_PORT)
    const std::uint64_t seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#else
    const std::uint64_t seed = std::random_device{}();
#endif
    // 32 bits, as Yjs's own random client id is: a JS peer reads it as a plain Number and must not
    // see it rounded.
    return (seed + counter.fetch_add(1)) & 0xffffffffULL;
}

std::vector<std::byte> copy_bytes(std::span<const ycpp::byte> bytes) {
    if (bytes.empty()) return {};
    const auto* begin = reinterpret_cast<const std::byte*>(bytes.data());
    return std::vector<std::byte>(begin, begin + bytes.size());
}

std::span<const ycpp::byte> as_ycpp(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const ycpp::byte*>(bytes.data()), bytes.size()};
}

// A stored map value in the variant the fields speak. Anything else (a nested array, a map, a null)
// is not a scalar and reads as monostate, which no field accepts: it is left alone.
Value output_value(const ycpp::Any& value) {
    switch (value.kind()) {
        case ycpp::AnyKind::boolean: return value.as_bool();
        case ycpp::AnyKind::integer: return value.as_integer();
        case ycpp::AnyKind::real: return value.as_real();
        case ycpp::AnyKind::big_int: return value.as_big_int();
        case ycpp::AnyKind::string: return std::string(value.as_string());
        default: return std::monostate{};
    }
}

ycpp::Any input_for(const Value& value) {
    if (const auto* text = std::get_if<std::string>(&value)) return ycpp::Any(std::string_view(*text));
    if (const auto* flag = std::get_if<bool>(&value)) return ycpp::Any(*flag);
    if (const auto* number = std::get_if<double>(&value)) return ycpp::Any(*number);
    if (const auto* integer = std::get_if<std::int64_t>(&value)) return ycpp::Any(*integer);
    return ycpp::Any(nullptr);
}

std::string join_key(std::string_view map_path, std::string_view key) {
    return map_path.empty() ? std::string(key) : std::string(map_path) + '.' + std::string(key);
}

std::optional<Value> current_value(const ycpp::MapRef& map, std::string_view key) {
    std::optional<ycpp::Any> output = map.get(key);
    if (!output.has_value()) return std::nullopt;
    return output_value(*output);
}

void write_value(ycpp::MapRef& map, std::string_view key, const Value& value) {
    map.set(key, input_for(value));
}

std::vector<std::byte> encoded_state_vector(const ycpp::Doc& doc) {
    ycpp::Encoder encoder;
    doc.state_vector().encode(encoder);
    return copy_bytes(encoder.bytes());
}

// A peer's state vector, or the empty one when it sent none or sent something unreadable: an empty
// state vector asks for the whole document, which is the safe answer to a catch-up request.
ycpp::StateVector decoded_state_vector(std::span<const std::byte> peer_state) {
    if (peer_state.empty()) return ycpp::StateVector{};
    ycpp::Decoder decoder(as_ycpp(peer_state));
    std::expected<ycpp::StateVector, ycpp::Error> decoded = ycpp::StateVector::decode(decoder);
    if (!decoded.has_value()) return ycpp::StateVector{};
    return std::move(*decoded);
}

std::vector<std::byte> encoded_since(const ycpp::Doc& doc, std::span<const std::byte> peer_state) {
    return copy_bytes(doc.encode_state_as_update(decoded_state_vector(peer_state)));
}

} // namespace

struct Doc::Impl {
    Impl() : doc(next_client_id()), root_map(doc.map("model")) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ycpp::Doc doc;
    ycpp::MapRef root_map;
    core::SenderHub<std::span<const std::byte>> updates;
    std::vector<Binding> bindings;
    // The fields' callbacks reach into this Doc: let go of them before the document goes. The
    // document is a member, so it is destroyed after this body, where bindings are already empty.
    ~Impl() { bindings.clear(); }
};

Doc::Doc() : impl_(std::make_unique<Impl>()) {}
Doc::~Doc() = default;

core::SenderHub<std::span<const std::byte>>& Doc::on_update() { return impl_->updates; }

void Doc::apply_update(std::span<const std::byte> update) {
    if (update.empty()) return;
    if (!impl_->doc.apply_update(as_ycpp(update)).has_value()) return;
    // Every field takes what the map now holds. A value of the wrong type, or a field that is gone,
    // is simply skipped: write() says so.
    // simplify: every bound key is reread on every update; upgrade: observe the root map, and
    // reconcile only the keys it reports.
    for (Binding& binding : impl_->bindings) {
        const std::optional<Value> stored = current_value(impl_->root_map, binding.key);
        if (stored.has_value()) binding.write(*stored);
    }
}

std::vector<std::byte> Doc::state_vector() const {
    return encoded_state_vector(impl_->doc);
}

std::vector<std::byte> Doc::encode_since(std::span<const std::byte> peer_state) const {
    return encoded_since(impl_->doc, peer_state);
}

std::string Doc::to_json() const { return impl_->root_map.to_json(); }

Doc::Map Doc::map(std::string_view key) { return Map(*this, std::string(key)); }

Doc::Map Doc::Map::map(std::string_view key) const { return Map(*owner_, join_key(path_, key)); }

void Doc::bind_value(std::string_view map_path, std::string_view key,
                     std::function<bool(const Value&)> write, core::Connection changed) {
    // A nested struct's fields are keys of the root map with a dotted name: creating a nested map
    // lazily loses an edit when two replicas each create one for the same key.
    const std::string name = join_key(map_path, key);
    // Binding never writes: a key the map holds replaces the field's value, an absent one leaves the
    // field as the code set it, so a joining peer cannot push its defaults over a peer's values.
    if (const std::optional<Value> stored = current_value(impl_->root_map, name)) {
        write(*stored);
    }
    impl_->bindings.push_back(Binding{name, std::move(write), std::move(changed)});
}

void Doc::field_changed(std::string_view map_path, std::string_view key, const Value& value) {
    const std::string name = join_key(map_path, key);
    if (const std::optional<Value> stored = current_value(impl_->root_map, name)) {
        // Already there: a remote update that just landed in the field, or its deferred emit after
        // the transaction that carried it. Nothing to write, and nothing to send back.
        if (value_detail::same_value(*stored, value)) return;
    }
    // simplify: a local set encodes the whole diff against a state vector taken just before the
    // write, which walks the document; upgrade: a transaction origin and an update observer, and
    // emit what it reports for a local origin only.
    const std::vector<std::byte> before = encoded_state_vector(impl_->doc);
    write_value(impl_->root_map, name, value);
    const std::vector<std::byte> update = encoded_since(impl_->doc, before);
    if (!update.empty()) impl_->updates.emit(std::span<const std::byte>(update));
}

} // namespace prism::crdt
