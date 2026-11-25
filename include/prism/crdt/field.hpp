#pragma once

#include <prism/core/field.hpp>

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

} // namespace prism::crdt
