#pragma once

// The crdt flavor of core: the same model vocabulary, plus a Field whose value travels and a Doc
// that carries it. A model written against it names prism::crdt instead of prism::core:
//
//   namespace model = prism::crdt;
//   struct Gain { model::Field<double> gain; model::Derived<double> doubled{...}; };
//
// What is re-exported here is the model types a struct holds, so the swap is one alias and the rest
// of the file reads the same. List moves to crdt in step 3, when the shared list exists; until then
// it is core's, and swapping the alias changes nothing about it.

#include <prism/core/channel.hpp>
#include <prism/core/derived.hpp>
#include <prism/core/latest.hpp>
#include <prism/core/list.hpp>
#include <prism/core/shared.hpp>
#include <prism/core/state.hpp>

#include <prism/crdt/doc.hpp>
#include <prism/crdt/field.hpp>

namespace prism::crdt {

// Field is crdt's own (field.hpp); these are core's, unchanged.
using core::Channel;
using core::Derived;
using core::Latest;
using core::List;
using core::Shared;
using core::State;

} // namespace prism::crdt
