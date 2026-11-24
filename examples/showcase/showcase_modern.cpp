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

    core::Field<ui::TabBar<>> page;

    void view(app::ViewBuilder& vb) {
        vb.tabs(page, [&] {
            vb.tab("Audio", [&](app::ViewBuilder& p) { audio(p); });
            vb.tab("Display", [&](app::ViewBuilder&) {});
            vb.tab("Network", [&](app::ViewBuilder&) {});
        });
    }

    void audio(app::ViewBuilder& vb) {
        vb.vstack([&] {
            vb.widget(search);
            vb.group("Sound", [&] {
                vb.widget(output);
                vb.widget(volume);
            });
            vb.widget(loading);
            vb.hstack([&] {
                vb.widget(notify);
                vb.widget(beta);
            });
            vb.hstack([&] {
                vb.widget(cancel);
                vb.widget(mute);
                vb.widget(save);
            });
        });
    }
};

int main(int argc, char* argv[]) {
    ModernSample model;
    const bool light = argc > 2 && std::strcmp(argv[2], "light") == 0;
    return showcase(argc, argv, model, 420, 440, [light](app::AppContext& ctx) {
        ctx.set_theme(light ? ui::modern_light_theme() : ui::modern_dark_theme());
    });
}
