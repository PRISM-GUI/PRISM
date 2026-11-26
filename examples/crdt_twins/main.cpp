// crdt_twins: one window, two Mixers, each its own Doc -- and the real y-protocols sync between
// them (`SyncSession`), not a raw `apply_update` hand-over, so the page runs step1/step2/update
// exactly as two browsers would (doc/integrations/crdt.md).
//
// Every shared field logs its changes on stdout, one line per change, which the wasm shell sends to
// console.log: tests/web/crdt_twins.mjs drives the sliders in a browser and reads those lines.
#include <crdt_pair/model.hpp>
#include <prism/core/enum_names.hpp>
#include <prism/crdt/sync.hpp>

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace {

using namespace prism;
using crdt_pair::Mixer;
using crdt_pair::Mode;

void log_change(char peer, std::string_view field, const std::string& value) {
    fmt::print(stdout, "twin {} {}={}\n", peer, field, value);
    std::fflush(stdout);
}

std::string text_of(double value) { return fmt::format("{:.2f}", value); }
std::string text_of(int value) { return fmt::format("{}", value); }
std::string text_of(bool value) { return value ? "true" : "false"; }
std::string text_of(Mode value) { return core::enum_label<Mode>(core::enum_index(value)); }

// One observe per shared field: a change made here and the same change arriving from the other
// replica both come out of the field, so the test recognizes a change by its field and value, not by
// which line came first.
void log_changes(char peer, Mixer& mixer) {
    mixer.gain.observe([peer](const ui::Slider<double>& slider) {
        log_change(peer, "gain", text_of(slider.value));
    });
    mixer.ch1.observe([peer](const ui::Slider<int, ui::Orientation::Vertical>& slider) {
        log_change(peer, "ch1", text_of(slider.value));
    });
    mixer.ch2.observe([peer](const ui::Slider<int, ui::Orientation::Vertical>& slider) {
        log_change(peer, "ch2", text_of(slider.value));
    });
    mixer.ch3.observe([peer](const ui::Slider<int, ui::Orientation::Vertical>& slider) {
        log_change(peer, "ch3", text_of(slider.value));
    });
    mixer.ch4.observe([peer](const ui::Slider<int, ui::Orientation::Vertical>& slider) {
        log_change(peer, "ch4", text_of(slider.value));
    });
    mixer.mode.observe([peer](const ui::Dropdown<Mode>& dropdown) {
        log_change(peer, "mode", text_of(dropdown.value));
    });
    mixer.armed.observe([peer](bool armed) { log_change(peer, "armed", text_of(armed)); });
    mixer.claps.observe([peer](int claps) { log_change(peer, "claps", text_of(claps)); });
}

// Two halves of one sync, crossed: what A's Doc emits leaves through B's session as a message, and
// the other way round. Each session speaks only for its own Doc; start() is the step1 handshake.
struct Wire {
    Wire(crdt::Doc& a, crdt::Doc& b)
        : a_to_b_(a, [this](std::span<const std::byte> message) { b_to_a_.receive(message); }),
          b_to_a_(b, [this](std::span<const std::byte> message) { a_to_b_.receive(message); }) {
        a_to_b_.start();
        b_to_a_.start();
    }

    crdt::SyncSession a_to_b_;
    crdt::SyncSession b_to_a_;
};

struct Twins {
    Mixer a;
    Mixer b;

    core::Field<ui::Label<>> a_title{{.value = "Peer A"}};
    core::Field<ui::Label<>> b_title{{.value = "Peer B"}};

    Wire wire{a.doc, b.doc};

    void attach(app::AppContext& ctx) {
        a.attach(ctx);
        b.attach(ctx);
        log_changes('A', a);
        log_changes('B', b);
        std::printf("twin ready\n");
        std::fflush(stdout);
    }

    void view(app::ViewBuilder& vb) {
        vb.hstack([&] {
            vb.vstack([&] {
                vb.widget(a_title);
                a.view(vb);
            });
            vb.gap(24.f);
            vb.vstack([&] {
                vb.widget(b_title);
                b.view(vb);
            });
        });
    }
};

} // namespace

int main() {
    Twins twins;
    return app::run(twins, {.title = "Mixer twins", .width = 1100, .height = 560});
}
