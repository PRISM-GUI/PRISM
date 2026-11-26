// crdt_pair, browser: one window whose Doc joins the hub that served the page, at
// ws://<page host>/room. The desktop tool of the same name is the other peer.
#include "model.hpp"

#include <prism/crdt/hub.hpp>

#include <emscripten.h>

#include <string>

using namespace prism;

namespace {

// The page came from the hub, so its own origin is the hub to join.
std::string room_url() {
    const std::string origin = emscripten_run_script_string(
        "(location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host");
    return origin + "/room";
}

} // namespace

int main() {
    crdt_pair::Mixer model;
    crdt::Client client(model.doc, room_url());
    return app::run(model, {.title = "Browser peer", .width = 800, .height = 480},
                    [&client](app::AppContext& ctx) {
                        crdt_pair::drain_on_logic_thread(ctx, client);
                    });
}
