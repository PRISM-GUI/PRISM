// The crdt_pair model, shared by its three ways of meeting: two windows wired in-process, a desktop
// window whose Doc a hub serves, and a browser page that joins that hub. Only who carries the bytes
// differs (doc/integrations/crdt.md).
#pragma once

#include <prism/prism.hpp>
#include <prism/crdt/bind.hpp>

#include <cstddef>
#include <span>
#include <string>

namespace crdt_pair {

using namespace prism;

namespace model = prism::crdt;   // the whole swap: prism::core would be the local version

enum class Mode { idle, run, hold };

struct Mixer {
    // What travels: plain values, the only kind a crdt::Field holds. A plain double is a spin box
    // and a plain std::string a text field, so each window edits the shared values themselves.
    model::Field<double> gain{0.5};
    model::Field<Mode> mode{Mode::idle};
    model::Field<std::string> name{"lead"};
    model::Field<bool> armed{false};

    // What does not: a core::Field is never bound, so it lives in this window alone.
    core::Field<int> local_clicks{0};

    core::Field<ui::Button> nudge{{.text = "Click"}};

    model::Doc doc;   // this replica

    Mixer() {
        nudge.observe([this](const ui::Button&) { ++local_clicks; });
    }

    // Every window's model gets this once, before its first frame.
    void attach(app::AppContext&) { bind(); }

    // Ties each shared field to its key in the Doc. A runtime with no attach (the board's
    // frame_app) calls it itself, once, before its first frame.
    void bind() {
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
            vb.widget(gain).tooltip("A double in the model, a spin box here");
            vb.hstack([&] {
                vb.widget(mode).tooltip("An enum travels by name");
                vb.widget(armed).tooltip("A bool is its own checkbox");
            });
            vb.widget(name).tooltip("A std::string in the model, a text field here");
            vb.hstack([&] {
                vb.widget(nudge);
                vb.widget_readonly(local_clicks).tooltip("This window only: a core::Field is not bound");
            });
        });
    }
};

// A Hub or a Client carries its peer's bytes in from another thread; they have to be drained where
// the Doc lives. Its waker posts that drain on the logic thread, and the deferred post covers
// whatever arrived before the waker existed.
template <typename Peer>
void drain_on_logic_thread(app::AppContext& ctx, Peer& peer) {
    const auto drain = [&peer] { peer.drain(); };
    peer.set_waker([poster = ctx.poster(), drain] { poster(drain); });
    ctx.post_deferred(drain);
}

} // namespace crdt_pair
