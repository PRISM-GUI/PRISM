#pragma once

#include <prism/core/field.hpp>

#include <type_traits>

namespace prism::crdt {

// A model field whose value also travels between replicas: a core::Field (same storage, same
// operators, same deferred notifications) marked for the crdt flavor. Doc::bind takes it exactly
// as it takes a core::Field, since it is one; the marker is what lets a model struct say which
// fields a Doc shares without a second, parallel model.
template <typename T>
struct Field : core::Field<T> {
    using core::Field<T>::Field;
    using core::Field<T>::operator=;
};

// What a reflected model walk asks of a member: is this one to share, or does it stay local? A
// core::Field answers no, which is how a model keeps a field to itself.
template <typename T>
struct is_crdt_field : std::false_type {};

template <typename T>
struct is_crdt_field<Field<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_crdt_field_v = is_crdt_field<T>::value;

} // namespace prism::crdt
