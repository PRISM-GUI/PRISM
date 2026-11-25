// crdt_pair, desktop. Without arguments: two windows, two Docs, wired in-process (the pair demo of
// doc/integrations/crdt.md). With --hub: one window whose Doc a hub serves on a port, next to the
// browser page it can serve -- the same model, as a peer on the network.
#include "model.hpp"

#include <prism/crdt/hub.hpp>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

using namespace prism;

namespace {

constexpr std::string_view usage_text =
    "usage: crdt_pair [--hub PORT [--static DIR] [--listen ADDR]]\n"
    "  no arguments: two windows, two Docs, wired in-process\n"
    "  --hub PORT:   one window, its Doc served on PORT (127.0.0.1 unless --listen)\n"
    "  --static DIR: also serve DIR as the browser page\n";

struct Args {
    bool hub = false;
    std::uint16_t port = 0;
    std::string static_dir;
    std::string listen = "127.0.0.1";   // listening on the network is an opt-in
};

std::optional<std::uint16_t> port_from(std::string_view text) {
    unsigned value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value > 65535) return std::nullopt;
    return static_cast<std::uint16_t>(value);
}

// `--flag value` pairs; anything else is a usage error.
std::optional<Args> parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        const bool has_value = i + 1 < argc;
        if (flag == "--hub" && has_value) {
            const auto port = port_from(argv[++i]);
            if (!port) return std::nullopt;
            args.hub = true;
            args.port = *port;
        } else if (flag == "--static" && has_value) {
            args.static_dir = argv[++i];
        } else if (flag == "--listen" && has_value) {
            args.listen = argv[++i];
        } else {
            return std::nullopt;
        }
    }
    if (!args.static_dir.empty() && !args.hub) return std::nullopt;
    return args;
}

int run_pair() {
    crdt_pair::Mixer a;
    crdt_pair::Mixer b;

    // The pair, wired in-process: each Doc's updates go straight into the other. Over a network
    // they arrive through a channel and are applied on the logic thread instead.
    const auto a_to_b =
        a.doc.on_update().connect([&b](std::span<const std::byte> update) { b.doc.apply_update(update); });
    const auto b_to_a =
        b.doc.on_update().connect([&a](std::span<const std::byte> update) { a.doc.apply_update(update); });

    return app::run(a, {.title = "Peer A", .width = 340, .height = 220},
                    [&b](app::AppContext& ctx) {
                        ctx.open_window(b, {.title = "Peer B", .width = 340, .height = 220});
                    });
}

int run_hub(const Args& args) {
    crdt_pair::Mixer a;
    crdt::Hub hub(a.doc,
                  {.host = args.listen, .port = args.port, .static_dir = args.static_dir});

    // The page needs the address to join; with --hub 0 the port is only known now.
    std::printf("serving %s on ws://%s:%d/room\n",
                args.static_dir.empty() ? "the model" : args.static_dir.c_str(), args.listen.c_str(),
                static_cast<int>(hub.port()));
    std::fflush(stdout);

    return app::run(a, {.title = "Peer A (hub)", .width = 340, .height = 220},
                    [&hub](app::AppContext& ctx) { crdt_pair::drain_on_logic_thread(ctx, hub); });
}

} // namespace

int main(int argc, char** argv) {
    const auto args = parse_args(argc, argv);
    if (!args) {
        std::fputs(usage_text.data(), stderr);
        return 2;
    }
    return args->hub ? run_hub(*args) : run_pair();
}
