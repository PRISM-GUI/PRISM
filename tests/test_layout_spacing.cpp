// A stack whose children set no gap of their own spaces them by the theme's `spacing`. A stack
// with one explicit `vb.gap()` keeps only its own gap, and each stack decides for itself.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/app/widget_tree.hpp>

#include <string>
#include <string_view>

using namespace prism;
using namespace prism::core;
using namespace prism::render;
using namespace prism::ui;
using prism::app::WidgetTree;

namespace {

// Ten points apart, everything else as the classic theme draws it.
Theme spaced_theme() {
    Theme t = classic_dark_theme();
    t.spacing = 10.f;
    return t;
}

const Rect* rect_showing(const SceneSnapshot& s, std::string_view text) {
    for (std::size_t i = 0; i < s.draw_lists.size(); ++i)
        for (auto& c : s.draw_lists[i]->commands)
            if (auto* t = std::get_if<TextCmd>(&c); t && t->text == text) return &s.geometry[i].second;
    return nullptr;
}

float origin_y(const SceneSnapshot& s, std::string_view text) {
    const Rect* r = rect_showing(s, text);
    REQUIRE(r != nullptr);
    return r->origin.y.raw();
}

float bottom_of(const SceneSnapshot& s, std::string_view text) {
    const Rect* r = rect_showing(s, text);
    REQUIRE(r != nullptr);
    return r->origin.y.raw() + r->extent.h.raw();
}

struct Stacked {
    Field<Label<>> one{Label<>{"one"}};
    Field<Label<>> two{Label<>{"two"}};
    Field<Label<>> three{Label<>{"three"}};
    void view(WidgetTree::ViewBuilder& vb) { vb.vstack(one, two, three); }
};

struct WithGap {
    Field<Label<>> one{Label<>{"one"}};
    Field<Label<>> two{Label<>{"two"}};
    Field<Label<>> three{Label<>{"three"}};
    void view(WidgetTree::ViewBuilder& vb) {
        vb.vstack([&] {
            vb.widget(one);
            vb.widget(two);
            vb.gap(4);
            vb.widget(three);
        });
    }
};

// The outer stack sets no gap, the inner one does.
struct InnerGap {
    Field<Label<>> one{Label<>{"one"}};
    Field<Label<>> two{Label<>{"two"}};
    Field<Label<>> three{Label<>{"three"}};
    void view(WidgetTree::ViewBuilder& vb) {
        vb.vstack([&] {
            vb.widget(one);
            vb.vstack([&] {
                vb.widget(two);
                vb.gap(4);
                vb.widget(three);
            });
        });
    }
};

// The outer stack sets a gap, the inner one does not.
struct OuterGap {
    Field<Label<>> one{Label<>{"one"}};
    Field<Label<>> two{Label<>{"two"}};
    Field<Label<>> three{Label<>{"three"}};
    void view(WidgetTree::ViewBuilder& vb) {
        vb.vstack([&] {
            vb.widget(one);
            vb.gap(4);
            vb.vstack([&] {
                vb.widget(two);
                vb.widget(three);
            });
        });
    }
};

template <typename M>
SceneSnapshot snapshot_of(M& model, const Theme& theme) {
    WidgetTree tree{model};
    tree.set_theme(theme);
    // Widgets record at their size: the first pass measures, the second lays out with it.
    static_cast<void>(tree.build_snapshot(400, 300, 1));
    tree.clear_dirty();
    return *tree.build_snapshot(400, 300, 2);
}

} // namespace

TEST_CASE("a stack keeps its children the theme's spacing apart") {
    Stacked model;
    const auto snap = snapshot_of(model, spaced_theme());

    CHECK(origin_y(snap, "two") - bottom_of(snap, "one") == doctest::Approx(10.f));
    CHECK(origin_y(snap, "three") - bottom_of(snap, "two") == doctest::Approx(10.f));
}

TEST_CASE("a stack with a gap of its own gets no theme spacing") {
    WithGap model;
    const auto snap = snapshot_of(model, spaced_theme());

    CHECK(origin_y(snap, "two") - bottom_of(snap, "one") == doctest::Approx(0.f));
    CHECK(origin_y(snap, "three") - bottom_of(snap, "two") == doctest::Approx(4.f));
}

TEST_CASE("a nested stack without a gap of its own takes the spacing") {
    OuterGap model;
    const auto snap = snapshot_of(model, spaced_theme());

    CHECK(origin_y(snap, "two") - bottom_of(snap, "one") == doctest::Approx(4.f));
    CHECK(origin_y(snap, "three") - bottom_of(snap, "two") == doctest::Approx(10.f));
}

TEST_CASE("a nested stack with a gap of its own keeps it, the outer stack keeps the spacing") {
    InnerGap model;
    const auto snap = snapshot_of(model, spaced_theme());

    CHECK(origin_y(snap, "two") - bottom_of(snap, "one") == doctest::Approx(10.f));
    CHECK(origin_y(snap, "three") - bottom_of(snap, "two") == doctest::Approx(4.f));
}

TEST_CASE("spacing 0 changes nothing") {
    Stacked model;
    const auto snap = snapshot_of(model, classic_dark_theme());

    CHECK(origin_y(snap, "two") - bottom_of(snap, "one") == doctest::Approx(0.f));
    CHECK(origin_y(snap, "three") - bottom_of(snap, "two") == doctest::Approx(0.f));
}
