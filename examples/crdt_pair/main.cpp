// crdt_pair: one model, two windows, two Doc replicas wired to each other in-process. Edit a
// window and the other follows; a core::Field stays where it was edited (doc/integrations/crdt.md).
#include <prism/prism.hpp>
#include <prism/crdt/bind.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <utility>

using namespace prism;

namespace model = prism::crdt;   // the whole swap: prism::core would be the local version

enum class Mode { idle, run, hold };

struct Mixer {
    // What travels: plain values, the only kind a crdt::Field holds.
    model::Field<double> gain{0.5};
    model::Field<Mode> mode{Mode::idle};
    model::Field<std::string> name{"lead"};
    model::Field<bool> armed{false};

    // What does not: a core::Field is never bound, so it lives in this window alone.
    core::Field<int> local_clicks{0};

    // The widgets you edit. A plain value draws as a label, so a slider and a text field keep the
    // two editable values and copy each other's changes across (set() skips an equal value, so
    // the copy cannot loop). Field<Mode> and Field<bool> are their own dropdown and checkbox.
    core::Field<ui::Slider<double>> gain_editor{
        {.value = 0.5, .min = 0.0, .max = 1.0, .step = 0.01, .label = "Gain"}};
    core::Field<ui::TextField<>> name_editor{{.value = "lead", .placeholder = "Bus name"}};
    core::Field<ui::Button> nudge{{.text = "Click"}};

    model::Doc doc;   // this replica

    Mixer() {
        gain_editor.observe([this](const ui::Slider<double>& s) { gain = s.value; });
        gain.observe([this](const double& v) {
            ui::Slider<double> s = gain_editor.get();
            s.value = v;
            gain_editor.set(std::move(s));
        });
        name_editor.observe([this](const ui::TextField<>& t) { name = t.value; });
        name.observe([this](const std::string& v) {
            ui::TextField<> t = name_editor.get();
            t.value = v;
            name_editor.set(std::move(t));
        });
        nudge.observe([this](const ui::Button&) { ++local_clicks; });
    }

    // Every window's model gets this once, before its first frame.
    void attach(app::AppContext&) {
#if __cpp_impl_reflection
        model::bind(doc, *this);   // every crdt::Field member, by its name
#else
        doc.bind("gain", gain);    // the manual tier: one line per field, no reflection needed
        doc.bind("mode", mode);
        doc.bind("name", name);
        doc.bind("armed", armed);
#endif
    }

    void view(app::ViewBuilder& vb) {
        vb.vstack([&] {
            vb.widget(gain_editor).tooltip("A double in the model, a slider here");
            vb.hstack([&] {
                vb.widget(mode).tooltip("An enum travels by name");
                vb.widget(armed).tooltip("A bool is its own checkbox");
            });
            vb.widget(name_editor).tooltip("A std::string in the model, a text field here");
            vb.hstack([&] {
                vb.widget(gain).tooltip("The shared value itself, as it travels");
                vb.widget(name).tooltip("The shared value itself, as it travels");
            });
            vb.hstack([&] {
                vb.widget(nudge);
                vb.widget(local_clicks).tooltip("This window only: a core::Field is not bound");
            });
        });
    }
};

int main() {
    Mixer a;
    Mixer b;

    // The pair, wired in-process: each Doc's updates go straight into the other. Over a network
    // they would arrive through a channel and be applied on the logic thread.
    core::Connection a_to_b =
        a.doc.on_update().connect([&b](std::span<const std::byte> update) { b.doc.apply_update(update); });
    core::Connection b_to_a =
        b.doc.on_update().connect([&a](std::span<const std::byte> update) { a.doc.apply_update(update); });

    return app::run(a, {.title = "Peer A", .width = 340, .height = 220},
                    [&b](app::AppContext& ctx) {
                        ctx.open_window(b, {.title = "Peer B", .width = 340, .height = 220});
                    });
}
