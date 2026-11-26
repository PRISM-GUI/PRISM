// crdt_hub, desktop: one window whose Doc a hub serves on port 8000, next to the browser page the
// same hub hands out (the web build of crdt_pair). Two local windows instead: examples/crdt_pair.
#include <crdt_pair/model.hpp>
#include <prism/crdt/hub.hpp>

using namespace prism;

int main() {
    crdt_pair::Mixer model;
    // static_dir: this directory holds the browser build -- copy or symlink it to ./web. A board's
    // hub ignores it and serves its embedded page. `.host = "0.0.0.0"` is the one-word opt-in to
    // listening beyond loopback.
    crdt::Hub hub(model.doc, {.port = 8000, .static_dir = "web"});
    return app::run(model, {.title = "Mixer"}, [&hub](app::AppContext& ctx) {
        crdt_pair::drain_on_logic_thread(ctx, hub);
    });
}
