#include <prism/crdt/doc.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// libyrs.h carries no C++ guard: it has to be included from global scope, never from inside a
// namespace, and only here (include/prism/crdt/ never sees Yrs types).
extern "C" {
#include <libyrs.h>
}

namespace prism::crdt {

namespace {

// One field bound to one key of the root map: how to write a value into it, and the connection
// that carries its edits the other way.
struct Binding {
    std::string key;
    std::function<bool(const Value&)> write;
    core::Connection changed;
};

std::vector<std::byte> copy_bytes(const char* data, std::uint32_t len) {
    if (data == nullptr || len == 0) return {};
    const auto* begin = reinterpret_cast<const std::byte*>(data);
    return std::vector<std::byte>(begin, begin + len);
}

// A stored map value in the variant the fields speak. Anything else (a nested array, a null) is
// not a scalar and reads as monostate, which no field accepts: it is left alone.
Value output_value(const YOutput& output) {
    if (const auto* flag = youtput_read_bool(&output)) return *flag != 0;
    if (const auto* number = youtput_read_float(&output)) return *number;
    if (const auto* integer = youtput_read_long(&output)) return *integer;
    if (const char* text = youtput_read_string(&output)) return std::string(text);
    return std::monostate{};
}

YInput input_for(const Value& value) {
    if (const auto* text = std::get_if<std::string>(&value)) return yinput_string(text->c_str());
    if (const auto* flag = std::get_if<bool>(&value)) {
        return yinput_bool(static_cast<std::uint8_t>(*flag ? Y_TRUE : Y_FALSE));
    }
    if (const auto* number = std::get_if<double>(&value)) return yinput_float(*number);
    if (const auto* integer = std::get_if<std::int64_t>(&value)) return yinput_long(*integer);
    return yinput_undefined();
}

std::optional<Value> current_value(YDoc* doc, const Branch* map, const char* key) {
    YTransaction* txn = ydoc_read_transaction(doc);
    YOutput* output = ymap_get(map, txn, key);
    std::optional<Value> result;
    if (output != nullptr) {
        result = output_value(*output);
        youtput_destroy(output);
    }
    ytransaction_commit(txn);
    return result;
}

void write_value(YDoc* doc, Branch* map, const char* key, const Value& value) {
    YInput input = input_for(value);
    YTransaction* txn = ydoc_write_transaction(doc, 0, nullptr);
    ymap_insert(map, txn, key, &input);
    ytransaction_commit(txn);
}

std::vector<std::byte> encoded_state_vector(YDoc* doc) {
    YTransaction* txn = ydoc_read_transaction(doc);
    std::uint32_t len = 0;
    char* bytes = ytransaction_state_vector_v1(txn, &len);
    std::vector<std::byte> result = copy_bytes(bytes, len);
    if (bytes != nullptr) ybinary_destroy(bytes, len);
    ytransaction_commit(txn);
    return result;
}

std::vector<std::byte> encoded_since(YDoc* doc, std::span<const std::byte> peer_state) {
    YTransaction* txn = ydoc_read_transaction(doc);
    std::uint32_t len = 0;
    // No state vector at all means the whole document, which is what a null pointer asks Yrs for.
    const char* state = peer_state.empty() ? nullptr : reinterpret_cast<const char*>(peer_state.data());
    char* bytes = ytransaction_state_diff_v1(txn, state, static_cast<std::uint32_t>(peer_state.size()),
                                             &len);
    std::vector<std::byte> result = copy_bytes(bytes, len);
    if (bytes != nullptr) ybinary_destroy(bytes, len);
    ytransaction_commit(txn);
    return result;
}

} // namespace

struct Doc::Impl {
    Impl() : doc(ydoc_new()), root_map(ymap(doc, "model")) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        // The fields' callbacks reach into this Doc: let go of them before the document goes.
        bindings.clear();
        ydoc_destroy(doc);
    }

    YDoc* doc;
    Branch* root_map;
    core::SenderHub<std::span<const std::byte>> updates;
    std::vector<Binding> bindings;
};

Doc::Doc() : impl_(std::make_unique<Impl>()) {}
Doc::~Doc() = default;

core::SenderHub<std::span<const std::byte>>& Doc::on_update() { return impl_->updates; }

void Doc::apply_update(std::span<const std::byte> update) {
    if (update.empty()) return;
    YTransaction* txn = ydoc_write_transaction(impl_->doc, 0, nullptr);
    const std::uint8_t result = ytransaction_apply(
        txn, reinterpret_cast<const char*>(update.data()),
        static_cast<std::uint32_t>(update.size()));
    ytransaction_commit(txn);
    if (result != 0) return;
    // Every field takes what the map now holds. A value of the wrong type, or a field that is gone,
    // is simply skipped: write() says so.
    // simplify: every bound key is reread on every update; upgrade: ymap_observe on the root map,
    // and reconcile only the keys it reports.
    for (Binding& binding : impl_->bindings) {
        const std::optional<Value> stored =
            current_value(impl_->doc, impl_->root_map, binding.key.c_str());
        if (stored.has_value()) binding.write(*stored);
    }
}

std::vector<std::byte> Doc::state_vector() const {
    return encoded_state_vector(impl_->doc);
}

std::vector<std::byte> Doc::encode_since(std::span<const std::byte> peer_state) const {
    return encoded_since(impl_->doc, peer_state);
}

std::string Doc::to_json() const {
    YTransaction* txn = ydoc_read_transaction(impl_->doc);
    char* json = ybranch_json(impl_->root_map, txn);
    std::string result = json != nullptr ? std::string(json) : std::string("{}");
    if (json != nullptr) ystring_destroy(json);
    ytransaction_commit(txn);
    return result;
}

void Doc::bind_value(std::string_view key, std::function<bool(const Value&)> write,
                     core::Connection changed) {
    const std::string name(key);
    // Binding never writes. A key the map holds replaces the field's value; an absent one leaves the
    // field as the code set it, so a joining peer cannot push its defaults over a peer's values.
    if (const std::optional<Value> stored =
            current_value(impl_->doc, impl_->root_map, name.c_str())) {
        write(*stored);
    }
    impl_->bindings.push_back(Binding{name, std::move(write), std::move(changed)});
}

void Doc::field_changed(std::string_view key, const Value& value) {
    const std::string name(key);
    if (const std::optional<Value> stored =
            current_value(impl_->doc, impl_->root_map, name.c_str())) {
        // Already there: a remote update that just landed in the field, or its deferred emit after
        // the transaction that carried it. Nothing to write, and nothing to send back.
        if (*stored == value) return;
    }
    // simplify: a local set encodes the whole diff against a state vector taken just before the
    // write, which walks the document; upgrade: ydoc_observe_updates_v1 on a transaction carrying a
    // local origin, and emit what it reports.
    const std::vector<std::byte> before = encoded_state_vector(impl_->doc);
    write_value(impl_->doc, impl_->root_map, name.c_str(), value);
    const std::vector<std::byte> update = encoded_since(impl_->doc, before);
    if (!update.empty()) impl_->updates.emit(std::span<const std::byte>(update));
}

} // namespace prism::crdt
