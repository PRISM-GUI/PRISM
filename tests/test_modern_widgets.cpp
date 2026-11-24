#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/app/widget_tree.hpp>
#include <prism/delegates/form_delegates.hpp>
#include <prism/input/hit_test.hpp>
#include <prism/ui/delegate.hpp>

namespace prism::core {} namespace prism::render {} namespace prism::input {}
namespace prism::ui {} namespace prism::app {}
namespace prism {
using namespace core; using namespace render; using namespace input;
using namespace ui; using namespace app;
}
using namespace prism;

namespace {

enum class Mode { Off, Eco, Boost };

// The smallest rect holding both.
Rect joined(Rect a, Rect b) {
    const float x0 = std::min(a.origin.x.raw(), b.origin.x.raw());
    const float y0 = std::min(a.origin.y.raw(), b.origin.y.raw());
    const float x1 = std::max(a.origin.x.raw() + a.extent.w.raw(), b.origin.x.raw() + b.extent.w.raw());
    const float y1 = std::max(a.origin.y.raw() + a.extent.h.raw(), b.origin.y.raw() + b.extent.h.raw());
    return {Point{X{x0}, Y{y0}}, Size{Width{x1 - x0}, Height{y1 - y0}}};
}

// One widget, under a theme. Built twice: a widget records at the size the first pass measured,
// so the second pass draws it at its real size.
template <typename T>
struct One {
    Field<T> f;
    explicit One(T v) : f(std::move(v)) {}
    void view(WidgetTree::ViewBuilder& vb) { vb.widget(f); }
};

template <typename T>
struct Rendered {
    One<T> model;
    WidgetTree tree{model};
    std::unique_ptr<SceneSnapshot> snap;

    Rendered(T v, const Theme& theme) : model(std::move(v)) {
        tree.set_theme(theme);
        static_cast<void>(tree.build_snapshot(300, 200, 1));
        tree.clear_dirty();
        snap = tree.build_snapshot(300, 200, 2);
    }

    // The box the layout gave the widget, in window coordinates.
    Rect box() const { return *find_widget_rect(*snap, tree.leaf_ids().front()); }

    std::vector<const GradientRect*> gradients() const {
        std::vector<const GradientRect*> out;
        for (const auto& dl : snap->draw_lists)
            for (const auto& cmd : dl->commands)
                if (const auto* g = std::get_if<GradientRect>(&cmd)) out.push_back(g);
        return out;
    }

    // The whole drawing of the widget, in its own coordinates.
    Rect drawn() const {
        Rect all{Point{X{0}, Y{0}}, Size{Width{0}, Height{0}}};
        bool first = true;
        for (const auto& dl : snap->draw_lists) {
            if (dl->commands.empty()) continue;
            const Rect b = dl->bounding_box();
            all = first ? b : joined(all, b);
            first = false;
        }
        return all;
    }
};

bool uses(const std::vector<const GradientRect*>& gradient_list, const Gradient& fill) {
    return std::any_of(gradient_list.begin(), gradient_list.end(),
                       [&](const GradientRect* g) { return g->from == fill.from && g->to == fill.to; });
}

} // namespace

TEST_CASE("a primary button fills with the theme's primary gradient") {
    const Theme t = modern_dark_theme();
    Rendered<Button> button{Button{.text = "Apply", .primary = true}, t};
    REQUIRE(uses(button.gradients(), t.primary_fill));
    CHECK(button.gradients().front()->radius > 0.f);   // rounded, the modern box

    const Theme classic = classic_dark_theme();
    Rendered<Button> old{Button{.text = "Apply", .primary = true}, classic};
    CHECK(old.gradients().empty());
}

TEST_CASE("a checked checkbox fills with the accent gradient") {
    const Theme t = modern_dark_theme();
    Rendered<Checkbox> box{Checkbox{.checked = true, .label = "Sync"}, t};
    CHECK(uses(box.gradients(), t.accent_fill));

    Rendered<Checkbox> old{Checkbox{.checked = true, .label = "Sync"}, classic_dark_theme()};
    CHECK(old.gradients().empty());
}

TEST_CASE("a selected radio fills with the accent gradient") {
    const Theme t = modern_dark_theme();
    Rendered<Radio<Mode>> radio{Radio<Mode>{.value = Mode::Eco}, t};
    CHECK(uses(radio.gradients(), t.accent_fill));

    Rendered<Radio<Mode>> old{Radio<Mode>{.value = Mode::Eco}, classic_dark_theme()};
    CHECK(old.gradients().empty());
}

TEST_CASE("a toggle that is on fills with the accent gradient") {
    const Theme t = modern_dark_theme();
    Rendered<Button> toggle{Button{.text = "Wi-Fi", .checked = true}, t};
    CHECK(uses(toggle.gradients(), t.accent_fill));

    Rendered<Button> old{Button{.text = "Wi-Fi", .checked = true}, classic_dark_theme()};
    CHECK(old.gradients().empty());
}

TEST_CASE("a slider's filled track is a gradient pill") {
    const Theme t = modern_dark_theme();
    Rendered<Slider<>> slider{Slider<>{.value = 0.5, .min = 0.0, .max = 1.0}, t};

    const auto gradient_list = slider.gradients();
    REQUIRE_FALSE(gradient_list.empty());
    const auto pill = std::find_if(gradient_list.begin(), gradient_list.end(), [&](const GradientRect* g) {
        return g->from == t.primary_fill.from && g->to == t.primary_fill.to;
    });
    REQUIRE(pill != gradient_list.end());
    CHECK((*pill)->radius == (*pill)->rect.extent.h.raw() / 2.f);   // half the track's height
    CHECK((*pill)->rect.extent.h.raw() < (*pill)->rect.extent.w.raw());   // a track, not a block

    Rendered<Slider<>> old{Slider<>{.value = 0.5, .min = 0.0, .max = 1.0}, classic_dark_theme()};
    CHECK(old.gradients().empty());
}

TEST_CASE("a slider's thumb stays inside the box the layout gave it") {
    const Theme t = modern_dark_theme();
    for (const double value : {0.0, 1.0, 0.35}) {
        Rendered<Slider<>> slider{Slider<>{.value = value, .min = 0.0, .max = 1.0}, t};
        const Rect drawing = slider.drawn();
        const Rect box = slider.box();
        CHECK(drawing.origin.x.raw() >= 0.f);
        CHECK(drawing.origin.y.raw() >= 0.f);
        CHECK(drawing.origin.x.raw() + drawing.extent.w.raw() <= doctest::Approx(box.extent.w.raw()));
        CHECK(drawing.origin.y.raw() + drawing.extent.h.raw() <= doctest::Approx(box.extent.h.raw()));
    }
}

TEST_CASE("a progress bar's filled part is the primary gradient") {
    const Theme t = modern_dark_theme();
    Rendered<ProgressBar> bar{ProgressBar{.value = 0.4, .min = 0.0, .max = 1.0}, t};
    CHECK(uses(bar.gradients(), t.primary_fill));

    Rendered<ProgressBar> old{ProgressBar{.value = 0.4, .min = 0.0, .max = 1.0}, classic_dark_theme()};
    CHECK(old.gradients().empty());
}

TEST_CASE("the modern themes do not change any of these widgets' size") {
    const Theme modern = modern_dark_theme();
    const Theme classic = classic_dark_theme();

    auto same_size = [](Rect a, Rect b) {
        return a.extent.w.raw() == doctest::Approx(b.extent.w.raw()) &&
               a.extent.h.raw() == doctest::Approx(b.extent.h.raw());
    };

    CHECK(same_size(Rendered<Button>{Button{.text = "Apply"}, modern}.box(),
                    Rendered<Button>{Button{.text = "Apply"}, classic}.box()));
    CHECK(same_size(Rendered<Checkbox>{Checkbox{.checked = true, .label = "Sync"}, modern}.box(),
                    Rendered<Checkbox>{Checkbox{.checked = true, .label = "Sync"}, classic}.box()));
    CHECK(same_size(Rendered<Radio<Mode>>{Radio<Mode>{.value = Mode::Eco}, modern}.box(),
                    Rendered<Radio<Mode>>{Radio<Mode>{.value = Mode::Eco}, classic}.box()));
    CHECK(same_size(Rendered<Button>{Button{.text = "Wi-Fi", .checked = true}, modern}.box(),
                    Rendered<Button>{Button{.text = "Wi-Fi", .checked = true}, classic}.box()));
    CHECK(same_size(Rendered<Slider<>>{Slider<>{.value = 0.5, .min = 0.0, .max = 1.0}, modern}.box(),
                    Rendered<Slider<>>{Slider<>{.value = 0.5, .min = 0.0, .max = 1.0}, classic}.box()));
    CHECK(same_size(Rendered<ProgressBar>{ProgressBar{.value = 0.4, .min = 0.0, .max = 1.0}, modern}.box(),
                    Rendered<ProgressBar>{ProgressBar{.value = 0.4, .min = 0.0, .max = 1.0}, classic}.box()));
}
