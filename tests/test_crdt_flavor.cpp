#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/crdt/bind.hpp>
#include <prism/crdt/flavor.hpp>

#include <cstddef>
#include <span>

// One model body, compiled twice: as a user writes it against core, and against crdt. The swap is
// the alias, and nothing else.
namespace local {
namespace model = prism::core;
#include "crdt_flavor_model.inc"
} // namespace local

namespace shared {
namespace model = prism::crdt;
#include "crdt_flavor_model.inc"
} // namespace shared

TEST_CASE("flavor swap") {
    local::M mine;
    local::check(mine);

    shared::M ours;
    shared::check(ours);
}

#if __cpp_impl_reflection
TEST_CASE("bind walks the struct") {
    struct S {
        prism::crdt::Field<int> a;
        prism::core::Field<int> local;
        struct In {
            prism::crdt::Field<bool> on;
        } in;
    };

    prism::crdt::Doc a;
    prism::crdt::Doc b;
    S model_a;
    S model_b;
    prism::crdt::bind(a, model_a);
    prism::crdt::bind(b, model_b);

    prism::core::Connection to_b = a.on_update().connect(
        [&b](std::span<const std::byte> update) { b.apply_update(update); });
    prism::core::Connection to_a = b.on_update().connect(
        [&a](std::span<const std::byte> update) { a.apply_update(update); });

    model_a.a = 7;
    model_a.local = 1;
    model_a.in.on = true;

    CHECK(model_b.a.get() == 7);
    CHECK(model_b.in.on.get() == true);
    CHECK(model_b.local.get() == 0);   // a core::Field stays where it was set
}
#endif
