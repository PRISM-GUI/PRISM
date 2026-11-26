// crdt_pair, desktop: two windows, two Docs, wired in-process -- the pair demo of
// doc/integrations/crdt.md. One window whose Doc a hub serves is examples/crdt_hub.
#include "model.hpp"

#include <cstddef>
#include <span>

using namespace prism;

int main() {
    crdt_pair::Mixer a;
    crdt_pair::Mixer b;

    // The pair, wired in-process: each Doc's updates go straight into the other. Over a network
    // they arrive through a channel and are applied on the logic thread instead.
    const auto a_to_b = a.doc.on_update().connect(
        [&b](std::span<const std::byte> update) { b.doc.apply_update(update); });
    const auto b_to_a = b.doc.on_update().connect(
        [&a](std::span<const std::byte> update) { a.doc.apply_update(update); });

    return app::run(a, {.title = "Peer A", .width = 800, .height = 480},
                    [&b](app::AppContext& ctx) {
                        ctx.open_window(b, {.title = "Peer B", .width = 800, .height = 480});
                    });
}
