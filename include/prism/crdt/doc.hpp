#pragma once

#include <prism/core/connection.hpp>
#include <prism/core/enum_names.hpp>
#include <prism/core/field.hpp>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace prism::crdt {

// What one shared key holds: the scalars a Yjs map carries, in the shape a peer in another
// language reads back. bind() converts to and from T, so ycpp stays in src/crdt/.
using Value = std::variant<std::monostate, std::int64_t, double, bool, std::string>;

namespace value_detail {

// A double holds integers exactly up to this magnitude. A wider one travels as decimal text,
// because a peer sees Yjs numbers as IEEE 754 doubles (a JS Number).
inline constexpr std::int64_t largest_exact_integer = std::int64_t{1} << 53;

template <typename T>
concept shared_scalar = std::integral<T> || std::floating_point<T> ||
                        std::same_as<T, std::string> || std::is_scoped_enum_v<T>;

// A widget shares one member: a slider's number, a dropdown's choice. Everything else it carries
// (a range, a caption, the option names) is drawn locally and never enters the document.
template <typename T>
concept shared_through_value = requires(const T& t) {
    requires shared_scalar<std::remove_cvref_t<decltype((t.value))>>;
};

// A whole number a double holds exactly travels as a Yjs number, which a JS peer reads as a plain
// Number. A wider one travels as decimal text: every peer sees a Yjs number as an IEEE 754 double
// and would round it.
inline Value integral_value(std::int64_t value) {
    if (value > largest_exact_integer || value < -largest_exact_integer)
        return std::to_string(value);
    return static_cast<double>(value);
}

inline Value integral_value(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(largest_exact_integer)) return std::to_string(value);
    return static_cast<double>(value);
}

template <typename T>
bool parse_integer(std::string_view text, T& out) {
    const char* const first = text.data();
    const char* const last = first + text.size();
    if constexpr (std::is_signed_v<T>) {
        std::int64_t parsed{};
        const auto [end, error] = std::from_chars(first, last, parsed);
        if (error != std::errc{} || end != last || !std::in_range<T>(parsed)) return false;
        out = static_cast<T>(parsed);
    } else {
        std::uint64_t parsed{};
        const auto [end, error] = std::from_chars(first, last, parsed);
        if (error != std::errc{} || end != last || !std::in_range<T>(parsed)) return false;
        out = static_cast<T>(parsed);
    }
    return true;
}

// An enum travels as its enumerator name, so a peer that does not share the C++ numbering still
// reads the same value. The name is matched against the type's own enumerators, which is what
// core's enum helpers give whether or not the build has reflection.
template <typename T>
bool enum_from_name(std::string_view name, T& out) {
    for (std::size_t i = 0; i < core::enum_count<T>(); ++i) {
        if (core::enum_label<T>(i) != name) continue;
        out = core::enum_from_index<T>(i);
        return true;
    }
    return false;
}

// A field value on its way to a Doc. The order matters: bool is an integer, and every case a map
// key can hold must be matched before the numeric ones.
template <typename T>
Value to_value(const T& value) {
    if constexpr (std::is_scoped_enum_v<T>) {
        return core::enum_label<T>(core::enum_index(value));
    } else if constexpr (std::same_as<T, std::string>) {
        return value;
    } else if constexpr (std::same_as<T, bool>) {
        return value;
    } else if constexpr (std::floating_point<T>) {
        return static_cast<double>(value);
    } else if constexpr (std::integral<T>) {
        if constexpr (std::is_signed_v<T>)
            return integral_value(static_cast<std::int64_t>(value));
        else
            return integral_value(static_cast<std::uint64_t>(value));
    } else {
        static_assert(shared_scalar<T>,
                      "a crdt field holds an integer, a floating point number, a bool, a "
                      "std::string or a scoped enum, and nothing else");
        return std::monostate{};
    }
}

// Reads value into out, or returns false and leaves out alone when the types disagree: a peer can
// write anything into a key, and an unknown enum name is as wrong as a string in a number.
template <typename T>
bool from_value(const Value& value, T& out) {
    if constexpr (std::is_scoped_enum_v<T>) {
        const auto* name = std::get_if<std::string>(&value);
        return name && enum_from_name(*name, out);
    } else if constexpr (std::same_as<T, std::string>) {
        const auto* text = std::get_if<std::string>(&value);
        if (!text) return false;
        out = *text;
        return true;
    } else if constexpr (std::same_as<T, bool>) {
        const auto* flag = std::get_if<bool>(&value);
        if (!flag) return false;
        out = *flag;
        return true;
    } else if constexpr (std::floating_point<T>) {
        if (const auto* number = std::get_if<double>(&value)) {
            out = static_cast<T>(*number);
            return true;
        }
        // A peer that stores whole numbers sends an integer where a double field is expected.
        if (const auto* integer = std::get_if<std::int64_t>(&value)) {
            out = static_cast<T>(*integer);
            return true;
        }
        return false;
    } else if constexpr (std::integral<T>) {
        // A Yjs number is a double: an exact whole one converts back, 2.5 and 1e300 do not.
        if (const auto* number = std::get_if<double>(&value)) {
            const double whole = std::trunc(*number);
            if (whole != *number || whole < -static_cast<double>(largest_exact_integer) ||
                whole > static_cast<double>(largest_exact_integer))
                return false;
            const auto rounded = static_cast<std::int64_t>(whole);
            if (!std::in_range<T>(rounded)) return false;
            out = static_cast<T>(rounded);
            return true;
        }
        // A wide integer comes as decimal text, and a peer that is not a JS one may send a long.
        if (const auto* integer = std::get_if<std::int64_t>(&value)) {
            if (!std::in_range<T>(*integer)) return false;
            out = static_cast<T>(*integer);
            return true;
        }
        if (const auto* text = std::get_if<std::string>(&value)) return parse_integer(*text, out);
        return false;
    } else {
        static_assert(shared_scalar<T>,
                      "a crdt field holds an integer, a floating point number, a bool, a "
                      "std::string or a scoped enum, and nothing else");
        return false;
    }
}

// What a T contributes to the map: the value itself, or the member a widget shares.
template <typename T>
Value to_shared_value(const T& value) {
    if constexpr (shared_through_value<T>) return to_value(value.value);
    else return to_value(value);
}

template <typename T>
bool from_shared_value(const Value& value, T& out) {
    if constexpr (shared_through_value<T>) return from_value(value, out.value);
    else return from_value(value, out);
}

// Two values for one key. A whole number reads back as a long or as a Yjs number depending on how
// the peer encoded it, so numbers compare numerically and everything else exactly.
inline bool same_value(const Value& lhs, const Value& rhs) {
    const auto* left_integer = std::get_if<std::int64_t>(&lhs);
    const auto* left_number = std::get_if<double>(&lhs);
    const auto* right_integer = std::get_if<std::int64_t>(&rhs);
    const auto* right_number = std::get_if<double>(&rhs);
    if (left_integer && right_integer) return *left_integer == *right_integer;
    if (left_number && right_number) return *left_number == *right_number;
    if (left_integer && right_number) return static_cast<double>(*left_integer) == *right_number;
    if (left_number && right_integer) return *left_number == static_cast<double>(*right_integer);
    return lhs == rhs;
}

// The key a field is bound to, with the token that says the field is still there. The field's own
// hub holds this, so the token dies with the field; the Doc keeps the weak side.
struct Tracked_key {
    std::string map_path;
    std::string key;
    std::shared_ptr<const bool> alive;
};

} // namespace value_detail

// One ycpp document, and the fields bound to keys of its root map: a local edit goes out as an
// update (Yjs v1, which a JS or Python peer reads), an applied update lands in the field through
// set(), so views and Derived follow it like any other edit. Logic thread only, like every model
// value. Field<T> values are converted here; ycpp never reaches this header.
class Doc {
public:
    class Map;

    Doc();
    ~Doc();
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;

    // Binds field to key of the root map. Binding never writes: a key the map already holds wins
    // over the field's value, and an absent one leaves the field alone. The defaults come from the
    // code, so every peer starts from the same ones and nobody pushes a default over a value. What
    // travels is a real local edit. Takes a crdt::Field too: it is-a core::Field, and binding does
    // not care which. A widget-typed field (a slider, a dropdown) shares its `.value` member; its
    // own settings stay local to this replica.
    template <typename T>
    void bind(std::string_view key, core::Field<T>& field) {
        bind_under({}, key, field);
    }

    // A prefix for the keys of a nested struct: binding "on" under map("in") binds "in.on" in the
    // root map. A dotted key, not a nested map: two replicas creating the same nested map each
    // would keep only one of them, and the other's edits with it.
    Map map(std::string_view key);

    // A local edit, encoded: what to hand a peer.
    core::SenderHub<std::span<const std::byte>>& on_update();
    void apply_update(std::span<const std::byte> update);

    std::vector<std::byte> state_vector() const;
    std::vector<std::byte> encode_since(std::span<const std::byte> peer_state) const;

    // The root map as JSON: what a peer in another language sees of this document.
    std::string to_json() const;

private:
    // bind() for the root map (an empty prefix) and for a dotted prefix under it. The header
    // converts T to a Value and back, and hands over the connection that reports the field's own
    // edits, because a SenderHub<const T&> cannot be named without T.
    template <typename T>
    void bind_under(std::string_view map_path, std::string_view key, core::Field<T>& field) {
        static_assert(value_detail::shared_scalar<T> || value_detail::shared_through_value<T>,
                      "a crdt field holds an integer, a floating point number, a bool, a "
                      "std::string or a scoped enum, or a widget whose value is one, and nothing "
                      "else");
        // The token below is owned by the lambda watching the field, and held weakly by the lambda
        // writing to it: a Doc outliving its field stops hearing from it, and never calls into the
        // field again.
        auto alive = std::make_shared<const bool>(true);
        auto write = [&field, token = std::weak_ptr<const bool>(alive)](const Value& value) {
            if (token.expired()) return false;
            // Starts from what the field holds, so a widget keeps its own range or option names
            // and only its shared member changes.
            T next = field.get();
            if (!value_detail::from_shared_value(value, next)) return false;
            field.set(std::move(next));
            return true;
        };
        core::Connection changed = field.on_change().connect(
            [this, bound = value_detail::Tracked_key{std::string(map_path), std::string(key),
                                                     std::move(alive)}](const T& next) {
                field_changed(bound.map_path, bound.key, value_detail::to_shared_value(next));
            });
        bind_value(map_path, key, std::move(write), std::move(changed));
    }

    // The non-template half: the header has converted T already.
    void bind_value(std::string_view map_path, std::string_view key,
                    std::function<bool(const Value&)> write, core::Connection changed);
    void field_changed(std::string_view map_path, std::string_view key, const Value& value);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A group of keys in the root map, reached by the dotted prefix that names it.
class Doc::Map {
public:
    template <typename T>
    void bind(std::string_view key, core::Field<T>& field) {
        owner_->bind_under(path_, key, field);
    }

    // A group nested in this one: the prefixes join with a dot.
    Map map(std::string_view key) const;

private:
    friend class Doc;
    Map(Doc& owner, std::string path) : owner_(&owner), path_(std::move(path)) {}

    Doc* owner_;
    std::string path_;
};

} // namespace prism::crdt
