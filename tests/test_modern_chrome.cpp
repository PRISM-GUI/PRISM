#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/app/event_routing.hpp>
#include <prism/app/widget_tree.hpp>
#include <prism/delegates/menu_delegates.hpp>
#include <prism/input/hit_test.hpp>

namespace prism::core {} namespace prism::render {} namespace prism::input {}
namespace prism::ui {} namespace prism::app {}
namespace prism {
using namespace core; using namespace render; using namespace input;
using namespace ui; using namespace app;
}
using namespace prism;
using prism::app::widget_detail::route_event;

namespace {

template <typename Cmd>
std::vector<const Cmd*> commands_of(const DrawList& dl) {
    std::vector<const Cmd*> out;
    for (const auto& cmd : dl.commands)
        if (const auto* c = std::get_if<Cmd>(&cmd)) out.push_back(c);
    return out;
}

DrawList merged(const SceneSnapshot& snap) {
    DrawList all;
    for (const auto& dl : snap.draw_lists)
        for (const auto& cmd : dl->commands) all.commands.push_back(cmd);
    return all;
}

// A tree under a theme, laid out twice: a widget records at the size the first pass measured.
template <typename Model>
struct Shown {
    Model model;
    WidgetTree tree{model};
    std::unique_ptr<SceneSnapshot> snap;
    uint64_t version = 1;

    explicit Shown(const Theme& theme) {
        tree.set_theme(theme);
        static_cast<void>(tree.build_snapshot(400, 300, version++));
        tree.clear_dirty();
        snap = tree.build_snapshot(400, 300, version++);
    }
    void redraw() { snap = tree.build_snapshot(400, 300, version++); }
    DrawList draws() const { return merged(*snap); }
    Rect rect_of(WidgetId id) const { return *find_widget_rect(*snap, id); }
    void click(WidgetId id) {
        const Rect r = rect_of(id);
        const Point p{X{r.origin.x.raw() + 6.f}, Y{r.origin.y.raw() + 6.f}};
        route_event(tree, snap.get(), MouseButton{p, 1, true});
        route_event(tree, snap.get(), MouseButton{p, 1, false});
        redraw();
    }
    void tab() {
        route_event(tree, snap.get(), KeyPress{keys::tab, 0});
        redraw();
    }
};

bool has_focus_ring(const DrawList& dl, const Theme& t) {
    for (const auto* r : commands_of<RoundedRect>(dl))
        if (r->color == t.focus_ring && r->thickness > 0.f) return true;
    for (const auto* r : commands_of<RectOutline>(dl))
        if (r->color == t.focus_ring) return true;
    return false;
}

} // namespace

struct TwoButtons {
    Field<Button> first{{.text = "One"}};
    Field<Button> second{{.text = "Two"}};
    void view(WidgetTree::ViewBuilder& vb) { vb.vstack(first, second); }
};

TEST_CASE("a click focuses a widget without its focus ring; Tab shows the ring") {
    for (const Theme& theme : {modern_dark_theme(), classic_dark_theme()}) {
        Shown<TwoButtons> app{theme};
        const WidgetId first = app.tree.leaf_ids().front();

        app.click(first);
        CHECK(app.tree.focused_id() == first);
        CHECK_FALSE(has_focus_ring(app.draws(), theme));

        app.tab();
        CHECK(app.tree.focused_id() != first);
        CHECK(has_focus_ring(app.draws(), theme));

        app.click(first);
        CHECK_FALSE(has_focus_ring(app.draws(), theme));
    }
}

struct Pages {
    Field<TabBar<>> bar;
    Field<Label<>> first{{"one"}};
    Field<Label<>> second{{"two"}};
    void view(WidgetTree::ViewBuilder& vb) {
        vb.tabs(bar, [&] {
            vb.tab("Alpha", [&](WidgetTree::ViewBuilder& page) { page.widget(first); });
            vb.tab("Beta", [&](WidgetTree::ViewBuilder& page) { page.widget(second); });
        });
    }
};

TEST_CASE("a modern tab bar marks the selected tab with an underline, not a filled pill") {
    const Theme modern = modern_dark_theme();
    Shown<Pages> app{modern};
    const WidgetId bar = app.tree.leaf_ids().front();
    const Rect bar_rect = app.rect_of(bar);
    const DrawList all = app.draws();

    CHECK(commands_of<GradientRect>(all).empty());
    const auto marks = commands_of<RoundedRect>(all);
    const auto underline = std::find_if(marks.begin(), marks.end(), [&](const RoundedRect* r) {
        return r->color == modern.tab_accent && r->thickness == 0.f && r->rect.extent.h.raw() <= 3.f;
    });
    REQUIRE(underline != marks.end());
    CHECK((*underline)->rect.origin.y.raw() + (*underline)->rect.extent.h.raw()
          == doctest::Approx(bar_rect.origin.y.raw() + bar_rect.extent.h.raw()).epsilon(0.01));

    const auto lines = commands_of<Line>(all);
    CHECK(std::any_of(lines.begin(), lines.end(), [&](const Line* l) {
        return l->to.x.raw() - l->from.x.raw() >= bar_rect.extent.w.raw() - 1.f;
    }));
}

TEST_CASE("a modern tab page keeps a margin from the window's edge; a classic one does not") {
    Shown<Pages> modern{modern_dark_theme()};
    const WidgetId label = modern.tree.leaf_ids().back();
    CHECK(modern.rect_of(label).origin.x.raw() == doctest::Approx(modern_dark_theme().page_margin));
    CHECK(modern_dark_theme().page_margin > 0.f);

    Shown<Pages> classic{classic_dark_theme()};
    CHECK(classic.rect_of(classic.tree.leaf_ids().back()).origin.x.raw() == doctest::Approx(0.f));
}

struct Text {
    Field<Label<>> note{{"A note"}};
    Field<Menu> file{{.title = "File", .items = {{"Open"}}}};
    Field<std::string> value{"a read-only value"};
    Field<int> count{3};
    void view(WidgetTree::ViewBuilder& vb) { vb.vstack(file, note, value, count); }
};

TEST_CASE("modern labels, read-only values and idle menu titles sit on the page, without a box") {
    const Theme modern = modern_dark_theme();
    Shown<Text> app{modern};
    const DrawList drawn = app.draws();
    for (const auto* r : commands_of<FilledRect>(drawn)) CHECK(r->color.a == 0);
    for (const auto* r : commands_of<RoundedRect>(drawn)) CHECK(r->color.a == 0);

    const Theme classic = classic_dark_theme();
    Shown<Text> old{classic};
    const DrawList old_drawn = old.draws();
    const auto boxes = commands_of<FilledRect>(old_drawn);
    CHECK(std::any_of(boxes.begin(), boxes.end(), [&](const FilledRect* r) { return r->color == classic.surface; }));
}

TEST_CASE("a modern label keeps its classic size") {
    Shown<Text> modern{modern_dark_theme()};
    Shown<Text> classic{classic_dark_theme()};
    for (std::size_t i = 0; i < 4; ++i) {
        const Rect a = modern.rect_of(modern.tree.leaf_ids()[i]);
        const Rect b = classic.rect_of(classic.tree.leaf_ids()[i]);
        CHECK(a.extent.h.raw() == doctest::Approx(b.extent.h.raw()));
    }
}

TEST_CASE("the modern themes give the window chrome their own palette") {
    for (const Theme& t : {modern_dark_theme(), modern_light_theme()}) {
        CHECK(t.chrome_bg != classic_dark_theme().chrome_bg);
        CHECK(t.chrome_bg != classic_light_theme().chrome_bg);
        CHECK(t.divider != classic_dark_theme().divider);
        CHECK(t.scrollbar_thumb != classic_dark_theme().scrollbar_thumb);
    }
}

struct FieldAndButton {
    Field<TextField<>> find{{.placeholder = "Find"}};
    Field<Button> go{{.text = "Replace all", .primary = false}};
    void view(WidgetTree::ViewBuilder& vb) { vb.vstack(find, go); }
};

// The colour a widget's box is filled with: its first opaque fill.
const DrawList& draws_of(const SceneSnapshot& snap, WidgetId id) {
    for (std::size_t i = 0; i < snap.geometry.size(); ++i)
        if (snap.geometry[i].first == id) return *snap.draw_lists[i];
    FAIL("no such widget");
    throw;
}

Color box_fill(const SceneSnapshot& snap, WidgetId id) {
    for (const auto& cmd : draws_of(snap, id).commands) {
        if (const auto* r = std::get_if<RoundedRect>(&cmd); r && r->thickness == 0.f && r->color.a) return r->color;
        if (const auto* r = std::get_if<FilledRect>(&cmd); r && r->color.a) return r->color;
    }
    return Color::rgba(0, 0, 0, 0);
}

TEST_CASE("a modern button is filled apart from a text field, and has no border") {
    for (const Theme& theme : {modern_dark_theme(), modern_light_theme()}) {
        Shown<FieldAndButton> app{theme};
        auto ids = app.tree.leaf_ids();   // the field above the button
        std::ranges::sort(ids, {}, [&](WidgetId id) { return app.rect_of(id).origin.y.raw(); });
        const WidgetId field = ids[0], button = ids[1];
        CHECK(box_fill(*app.snap, field) != box_fill(*app.snap, button));
        CHECK(box_fill(*app.snap, button) == theme.button);
        for (const auto& cmd : draws_of(*app.snap, button).commands)
            if (const auto* r = std::get_if<RoundedRect>(&cmd)) CHECK(r->thickness == 0.f);
    }
    const Theme classic = classic_dark_theme();
    CHECK(classic.button == classic.surface);
}

TEST_CASE("a classic button is filled like the classic fields") {
    for (const Theme& t : {classic_dark_theme(), classic_light_theme()}) {
        CHECK(t.button == t.surface);
        CHECK(t.button_hover == t.surface_hover);
        CHECK(t.button_active == t.surface_active);
    }
}
