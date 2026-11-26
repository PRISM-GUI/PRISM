// The crdt_pair model, shared by its three ways of meeting: two windows wired in-process, a desktop
// window whose Doc a hub serves, and a browser page that joins that hub. Only who carries the bytes
// differs (doc/integrations/crdt.md).
#pragma once

#include <prism/prism.hpp>
#include <prism/crdt/bind.hpp>

#include <cstddef>
#include <span>

namespace crdt_pair {

using namespace prism;

namespace model = prism::crdt;   // the whole swap: prism::core would be the local version

enum class Mode { idle, run, hold };

// A mixer laid out for a touch screen. Every widget shares its `.value`; the ranges, captions and
// option names stay local, so each replica can size its own controls.
struct Mixer {
    model::Field<ui::Slider<double>> gain{
        {.value = 0.5, .min = 0.0, .max = 1.0, .step = 0.01, .label = "Gain"}};

    // Four channel faders. A vertical slider draws no caption, hence the fixed labels below them.
    model::Field<ui::Slider<int, ui::Orientation::Vertical>> ch1{
        {.value = 70, .min = 0, .max = 100, .step = 1}};
    model::Field<ui::Slider<int, ui::Orientation::Vertical>> ch2{
        {.value = 55, .min = 0, .max = 100, .step = 1}};
    model::Field<ui::Slider<int, ui::Orientation::Vertical>> ch3{
        {.value = 40, .min = 0, .max = 100, .step = 1}};
    model::Field<ui::Slider<int, ui::Orientation::Vertical>> ch4{
        {.value = 85, .min = 0, .max = 100, .step = 1}};

    model::Field<ui::Dropdown<Mode>> mode{{.value = Mode::idle}};
    model::Field<bool> armed{false};

    // Shared, shown read-only: the Clap button bumps it on this replica.
    model::Field<int> claps{0};

    // What does not: a core::Field is never bound, so it lives in this window alone.
    core::Field<ui::Label<>> ch1_label{{.value = "1"}};
    core::Field<ui::Label<>> ch2_label{{.value = "2"}};
    core::Field<ui::Label<>> ch3_label{{.value = "3"}};
    core::Field<ui::Label<>> ch4_label{{.value = "4"}};
    core::Field<ui::Label<>> armed_label{{.value = "Armed"}};
    core::Field<ui::Button> clap{{.text = "Clap"}};

    model::Doc doc;   // this replica

    Mixer() {
        // simplify: a register, not a counter: two peers clapping at once count once.
        clap.observe([this](const ui::Button&) { ++claps; });
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
        doc.bind("ch1", ch1);
        doc.bind("ch2", ch2);
        doc.bind("ch3", ch3);
        doc.bind("ch4", ch4);
        doc.bind("mode", mode);
        doc.bind("armed", armed);
        doc.bind("claps", claps);
#endif
    }

    void view(app::ViewBuilder& vb) {
        vb.vstack([&] {
            vb.widget(gain);
            vb.hstack([&] {   // spacers spread the faders over the width, for fingers
                vb.spacer();
                vb.vstack([&] { vb.widget(ch1); vb.widget(ch1_label); });
                vb.spacer();
                vb.vstack([&] { vb.widget(ch2); vb.widget(ch2_label); });
                vb.spacer();
                vb.vstack([&] { vb.widget(ch3); vb.widget(ch3_label); });
                vb.spacer();
                vb.vstack([&] { vb.widget(ch4); vb.widget(ch4_label); });
                vb.spacer();
            });
            vb.hstack([&] {
                vb.widget(mode).tooltip("An enum travels by name");
                vb.gap(24.f);
                vb.widget(armed).tooltip("A bool is its own checkbox");
                vb.widget(armed_label);
                vb.spacer();
                vb.widget(clap).tooltip("A local button driving a shared count");
                vb.widget_readonly(claps);
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
