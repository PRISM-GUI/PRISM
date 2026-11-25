#pragma once

// bind(doc, model): every crdt::Field of a model struct, bound to the key of its member name. A
// nested struct that holds crdt fields contributes a dotted prefix, so its fields keep the shape of
// the model in the document; a core::Field is left alone and stays local to this replica.
//
//   namespace model = prism::crdt;
//   struct Gain { model::Field<double> gain; core::Field<int> previews; };
//   model::Doc doc;
//   model::bind(doc, gain);            // "gain" travels, "previews" does not
//
// Reflection only: without it, bind the fields by hand, which is what the rest of core does too.

#include <prism/crdt/doc.hpp>
#include <prism/crdt/field.hpp>

#include <string_view>
#include <type_traits>

#if __cpp_impl_reflection
#include <meta>
#endif

#if __cpp_impl_reflection

namespace prism::crdt {

namespace bind_detail {

// Does a struct hold a crdt field, directly or in a nested struct? Only such a struct is worth a
// map of its own; a core::Field is not ours to share, and a plain value neither.
template <typename T>
consteval bool has_crdt_fields() {
    if constexpr (!std::is_class_v<T>) return false;
    else {
        static constexpr auto members = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
        bool found = false;
        template for (constexpr auto m : members) {
            using M = std::remove_cvref_t<typename[:std::meta::type_of(m):]>;
            if constexpr (is_crdt_field_v<M>) found = true;
            else if constexpr (std::is_class_v<M> && has_crdt_fields<M>()) found = true;
        }
        return found;
    }
}

} // namespace bind_detail

// The walk itself, over anything that takes bind(key, field) and map(name): a Doc, or one of its
// nested maps.
template <typename Target, typename Model>
void bind_each(Target& target, Model& model) {
    static constexpr auto members = std::define_static_array(
        std::meta::nonstatic_data_members_of(^^Model, std::meta::access_context::unchecked()));
    template for (constexpr auto m : members) {
        auto& member = model.[:m:];
        using M = std::remove_cvref_t<decltype(member)>;
        const std::string_view name = std::meta::identifier_of(m);
        if constexpr (is_crdt_field_v<M>) {
            target.bind(name, member);
        } else if constexpr (std::is_class_v<M> && bind_detail::has_crdt_fields<M>()) {
            auto nested = target.map(name);
            bind_each(nested, member);
        }
    }
}

template <typename Model>
void bind(Doc& doc, Model& model) {
    bind_each(doc, model);
}

} // namespace prism::crdt

#endif // __cpp_impl_reflection
