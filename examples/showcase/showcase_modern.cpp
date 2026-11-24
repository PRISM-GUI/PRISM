#include "showcase_common.hpp"

#include <cstring>

using namespace prism;

// The modern look (spike): the same widgets in ui::modern_dark_theme() or ui::modern_light_theme(),
// chosen by the second argument ("dark" or "light").
struct ModernSample {
    core::Field<ui::TextField<>> search{{.value = "", .placeholder = "Search..."}};
    core::Field<ui::ComboBox> output{{.value = "Speaker", .placeholder = "Output",
                                     .options = {"Speaker", "Headset", "HDMI"}}};
    core::Field<ui::Slider<>> volume{{.value = 62, .min = 0, .max = 100, .step = 1, .label = "Volume", .unit = "%"}};
    core::Field<ui::ProgressBar> loading{{.value = 0.72, .label = "Loading... 72%"}};
    core::Field<ui::Checkbox> notify{{.checked = true, .label = "Mail notification"}};
    core::Field<ui::Checkbox> beta{{.checked = false, .label = "Beta features"}};
    core::Field<ui::Button> cancel{{.text = "Cancel", .primary = false}};
    core::Field<ui::Button> save{{.text = "Save", .primary = true}};
    core::Field<ui::Button> mute{{.text = "Mute", .primary = false, .checked = true}};

    void view(app::ViewBuilder& vb) {
        // simplify: explicit gaps; a theme spacing the stacks apply is part of the design.
        vb.vstack([&] {
            vb.widget(search);
            vb.gap(10.f);
            vb.widget(output);
            vb.gap(14.f);
            vb.widget(volume);
            vb.gap(10.f);
            vb.widget(loading);
            vb.gap(12.f);
            vb.hstack([&] {
                vb.widget(notify);
                vb.gap(16.f);
                vb.widget(beta);
            });
            vb.gap(14.f);
            vb.hstack([&] {
                vb.widget(cancel);
                vb.gap(10.f);
                vb.widget(mute);
                vb.gap(10.f);
                vb.widget(save);
            });
        });
    }
};

int main(int argc, char* argv[]) {
    ModernSample model;
    const bool light = argc > 2 && std::strcmp(argv[2], "light") == 0;
    return showcase(argc, argv, model, 420, 340, [light](app::AppContext& ctx) {
        ctx.set_theme(light ? ui::modern_light_theme() : ui::modern_dark_theme());
    });
}
