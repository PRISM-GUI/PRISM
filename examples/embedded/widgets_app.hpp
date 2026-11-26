#pragma once

// The widget demo: a title, a counter that "+1" increments, a checkbox and a slider. A
// hand-written view() -- the board's toolchain has no reflection. Widgets are the delegates'
// default size (30 px tall, slider 30 px with a caption) plus explicit gaps, which on each board's
// panel is as finger-sized as the layout gets. The demo has no timing line of its own: the port's
// console shows that (platform's on_stats), not the app.

#include <prism/app/widget_tree.hpp>
#include <prism/core/connection.hpp>
#include <prism/core/field.hpp>

namespace prism_examples {

struct WidgetsModel {
    prism::core::Field<prism::ui::Label<>> title{{.value = "PRISM", .heading = true}};
    prism::core::Field<int> counter{0};
    prism::core::Field<prism::ui::Button> increment{{.text = "+1"}};
    prism::core::Field<prism::ui::Checkbox> enabled{{.checked = true, .label = "enabled"}};
    prism::core::Field<prism::ui::Slider<>> volume{
        {.value = 0.5, .min = 0.0, .max = 1.0, .step = 0.05, .label = "volume"}};
    prism::core::Connection on_increment = increment.on_change().connect(
        [this](const prism::ui::Button &) { counter.set(counter.get() + 1); });

    WidgetsModel() = default;
    // The connection's callback holds `this`: a copy or move would count on the old model.
    WidgetsModel(const WidgetsModel &) = delete;
    WidgetsModel &operator=(const WidgetsModel &) = delete;
    WidgetsModel(WidgetsModel &&) = delete;
    WidgetsModel &operator=(WidgetsModel &&) = delete;

    void view(prism::app::WidgetTree::ViewBuilder &vb)
    {
        vb.vstack([&] {
            vb.widget(title);
            vb.gap(12.f);
            vb.widget_readonly(counter);   // the button writes it, nobody types into it
            vb.gap(12.f);
            vb.hstack([&] {
                vb.widget(increment);
                vb.gap(16.f);
                vb.widget(enabled);
            });
            vb.gap(12.f);
            vb.widget(volume);
        });
    }
};

} // namespace prism_examples
