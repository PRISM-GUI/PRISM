#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/app/event_routing.hpp>
#include <prism/app/table_paint.hpp>
#include <prism/app/widget_tree.hpp>
#include <prism/delegates/menu_delegates.hpp>
#include <prism/input/hit_test.hpp>
#include <prism/ui/frame.hpp>
#include <prism/ui/tooltip.hpp>

namespace prism::core {} namespace prism::render {} namespace prism::input {}
namespace prism::ui {} namespace prism::app {}
namespace prism {
using namespace core; using namespace render; using namespace input;
using namespace ui; using namespace app;
}
using namespace prism;
using prism::app::widget_detail::route_event;

namespace {

// A measurer a tooltip needs: without text of a width, it draws nothing.
struct FixedAdvance final : TextMeasurer {
    float width(std::string_view t, float, prism::render::FontStyle) const override {
        return 7.f * float(t.size());
    }
    float line_height(float, prism::render::FontStyle) const override { return 20.f; }
};

template <typename Cmd>
std::vector<const Cmd*> commands_of(const DrawList& dl) {
    std::vector<const Cmd*> out;
    for (const auto& cmd : dl.commands)
        if (const auto* c = std::get_if<Cmd>(&cmd)) out.push_back(c);
    return out;
}

const RoundedRect* rounded_box(const DrawList& dl) {
    const auto boxes = commands_of<RoundedRect>(dl);
    return boxes.empty() ? nullptr : boxes.front();
}

bool same_extent(Rect a, Rect b) {
    return a.extent.w.raw() == doctest::Approx(b.extent.w.raw()) &&
           a.extent.h.raw() == doctest::Approx(b.extent.h.raw());
}

// One widget, under a theme, built twice: a widget records at the size the first pass measured.
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
        static_cast<void>(tree.build_snapshot(400, 300, 1));
        tree.clear_dirty();
        snap = tree.build_snapshot(400, 300, 2);
    }

    Rect box() const { return *find_widget_rect(*snap, tree.leaf_ids().front()); }

    DrawList all_draws() const {
        DrawList merged;
        for (const auto& dl : snap->draw_lists)
            for (const auto& cmd : dl->commands) merged.commands.push_back(cmd);
        return merged;
    }
};

std::vector<const GradientRect*> gradients_in(const DrawList& dl) { return commands_of<GradientRect>(dl); }

bool uses(const std::vector<const GradientRect*>& list, const Gradient& fill) {
    return std::any_of(list.begin(), list.end(), [&](const GradientRect* g) {
        return g->from == fill.from && g->to == fill.to;
    });
}

} // namespace

TEST_CASE("a group frame is rounded by the theme's corner radius") {
    const Framing group{.look = FrameLook::Group, .title = "Devices"};
    const Theme modern = modern_dark_theme();
    const DrawList box = frame_draws(group, Size{Width{200}, Height{120}}, modern);
    const RoundedRect* rounded = rounded_box(box);
    REQUIRE(rounded != nullptr);
    CHECK(rounded->radius == doctest::Approx(modern.corner_radius));
    CHECK(rounded->rect.extent.w.raw() == doctest::Approx(200).epsilon(0.05));

    const Theme classic = classic_dark_theme();
    const DrawList old = frame_draws(group, Size{Width{200}, Height{120}}, classic);
    CHECK(rounded_box(old) == nullptr);   // a square outline, as before

    CHECK(same_extent(box.bounding_box(), old.bounding_box()));
}

TEST_CASE("a card frame is rounded by the theme's corner radius") {
    const Framing card{.look = FrameLook::Card};
    const Theme modern = modern_dark_theme();
    const DrawList box = frame_draws(card, Size{Width{180}, Height{100}}, modern);
    const RoundedRect* rounded = rounded_box(box);
    REQUIRE(rounded != nullptr);
    CHECK(rounded->radius == doctest::Approx(modern.corner_radius));
    CHECK(rounded->thickness == 0.f);   // the surface, not only its border
    CHECK(rounded->color == modern.popup_bg);

    const Theme classic = classic_dark_theme();
    CHECK(rounded_box(frame_draws(card, Size{Width{180}, Height{100}}, classic)) == nullptr);
}

TEST_CASE("a tooltip is rounded by the theme's corner radius") {
    FixedAdvance measurer;
    Point pointer{X{40}, Y{40}};
    const Size window{Width{400}, Height{300}};

    Theme modern = modern_dark_theme();
    modern.measurer = &measurer;
    DrawList tip;
    draw_tooltip(tip, "Refresh the list", pointer, window, modern);
    const RoundedRect* rounded = rounded_box(tip);
    REQUIRE(rounded != nullptr);
    CHECK(rounded->radius == doctest::Approx(modern.corner_radius));

    Theme classic = classic_dark_theme();
    classic.measurer = &measurer;
    DrawList old;
    draw_tooltip(old, "Refresh the list", pointer, window, classic);
    CHECK(rounded_box(old) == nullptr);
    REQUIRE_FALSE(old.commands.empty());
    CHECK(std::holds_alternative<FilledRect>(old.commands.front()));

    CHECK(same_extent(tip.bounding_box(), old.bounding_box()));
}

// The model types live here, at file scope: reflection needs a name that is not local to a
// function.
struct Tabs {
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

TEST_CASE("the selected tab is a gradient pill") {
    const Theme modern = modern_dark_theme();
    const Theme classic = classic_dark_theme();

    Tabs model;
    WidgetTree modern_tree{model};
    modern_tree.set_theme(modern);
    static_cast<void>(modern_tree.build_snapshot(400, 200, 1));
    modern_tree.clear_dirty();
    const auto snap = modern_tree.build_snapshot(400, 200, 2);

    DrawList tabs;
    for (const auto& dl : snap->draw_lists)
        for (const auto& cmd : dl->commands) tabs.commands.push_back(cmd);
    const auto pills = gradients_in(tabs);
    REQUIRE_FALSE(pills.empty());
    CHECK(uses(pills, modern.primary_fill));
    CHECK(std::any_of(pills.begin(), pills.end(), [](const GradientRect* g) {
        return g->radius > 0.f && g->rect.extent.w.raw() > g->rect.extent.h.raw();
    }));

    Tabs other;
    WidgetTree classic_tree{other};
    classic_tree.set_theme(classic);
    static_cast<void>(classic_tree.build_snapshot(400, 200, 1));
    classic_tree.clear_dirty();
    const auto old = classic_tree.build_snapshot(400, 200, 2);
    DrawList classic_tabs;
    for (const auto& dl : old->draw_lists)
        for (const auto& cmd : dl->commands) classic_tabs.commands.push_back(cmd);
    CHECK(gradients_in(classic_tabs).empty());
}

struct Rows {
    List<std::string> rows;
    Field<std::optional<std::size_t>> picked{std::size_t{1}};

    Rows() {
        rows.push_back("one");
        rows.push_back("two");
        rows.push_back("three");
    }

    void view(WidgetTree::ViewBuilder& vb) { vb.list(rows).selection(picked); }
};

TEST_CASE("a selected list row is a rounded band in the accent") {
    const Theme modern = modern_dark_theme();
    Rows model;
    WidgetTree tree{model};
    tree.set_theme(modern);
    static_cast<void>(tree.build_snapshot(300, 200, 1));
    tree.clear_dirty();
    const auto snap = tree.build_snapshot(300, 200, 2);

    DrawList all;
    for (const auto& dl : snap->draw_lists)
        for (const auto& cmd : dl->commands) all.commands.push_back(cmd);
    const auto boxes = commands_of<RoundedRect>(all);
    const auto in_accent = std::find_if(boxes.begin(), boxes.end(), [&](const RoundedRect* r) {
        return r->thickness == 0.f && (r->color == modern.accent || r->color == modern.text_selection);
    });
    REQUIRE(in_accent != boxes.end());
    CHECK((*in_accent)->radius > 0.f);

    const Theme classic = classic_dark_theme();
    Rows other;
    WidgetTree old_tree{other};
    old_tree.set_theme(classic);
    static_cast<void>(old_tree.build_snapshot(300, 200, 1));
    old_tree.clear_dirty();
    const auto old = old_tree.build_snapshot(300, 200, 2);
    DrawList classic_all;
    for (const auto& dl : old->draw_lists)
        for (const auto& cmd : dl->commands) classic_all.commands.push_back(cmd);
    CHECK(rounded_box(classic_all) == nullptr);
}

TEST_CASE("a selected table row is a rounded band in the accent") {
    const Theme modern = modern_dark_theme();
    const Theme classic = classic_dark_theme();

    auto selected_row = [](const Theme& theme) {
        TableState ts;
        ts.viewport_w = Width{200};
        ts.row_height = Height{24.f};
        ts.source.column_count = [] { return std::size_t{1}; };
        ts.source.row_count = [] { return std::size_t{1}; };
        ts.source.cell_text = [](std::size_t, std::size_t) { return std::string{"cell"}; };
        ts.source.header = [](std::size_t) { return std::string_view{"head"}; };
        ts.selected_row.set(std::size_t{0});
        const std::vector<float> edges{0.f, 200.f};
        return paint_table_row(ts, 0, edges, theme);
    };

    const WidgetNode modern_row = selected_row(modern);
    const WidgetNode classic_row = selected_row(classic);
    const auto band = rounded_box(modern_row.draws);
    REQUIRE(band != nullptr);
    CHECK(band->radius > 0.f);
    CHECK(band->thickness == 0.f);
    CHECK((band->color == modern.accent || band->color == modern.table_selected));
    CHECK(rounded_box(classic_row.draws) == nullptr);
    CHECK(same_extent(modern_row.draws.bounding_box(), classic_row.draws.bounding_box()));
}

struct Host {
    Field<Popup> sheet{{.open = false, .modal = true}};
    Field<Label<>> behind{{"content"}};
    Field<Label<>> inside{{"a row"}};

    void view(WidgetTree::ViewBuilder& vb) {
        vb.vstack([&] {
            vb.widget(behind);
            vb.popup(sheet, [&](WidgetTree::ViewBuilder& card) { card.vstack(inside); });
        });
    }
};

TEST_CASE("a popup card is rounded by the theme's corner radius") {
    const Theme modern = modern_dark_theme();

    auto card_box = [](const Theme& theme) {
        Host model;
        WidgetTree tree{model};
        tree.set_theme(theme);
        static_cast<void>(tree.build_snapshot(500, 400, 1));
        model.sheet.set({.open = true, .modal = true});
        return *tree.build_snapshot(500, 400, 3);
    };

    const SceneSnapshot modern_snap = card_box(modern);
    const RoundedRect* card = rounded_box(modern_snap.overlay);
    REQUIRE(card != nullptr);
    CHECK(card->radius == doctest::Approx(modern.corner_radius));
}

struct MenuHost {
    Field<Menu> file{{.title = "File", .items = {{"Open"}, {"Save"}}}};
    Field<Label<>> below{{"content"}};

    void view(WidgetTree::ViewBuilder& vb) { vb.vstack(file, below); }
};

TEST_CASE("a menu panel is rounded by the theme's corner radius") {
    const Theme modern = modern_dark_theme();

    MenuHost model;
    WidgetTree tree{model};
    tree.set_theme(modern);
    const auto first = tree.build_snapshot(400, 300, 1);
    const Rect title = *find_widget_rect(*first, tree.leaf_ids().front());
    const Point middle{X{title.origin.x.raw() + 10.f}, Y{title.origin.y.raw() + 10.f}};

    route_event(tree, first.get(), MouseButton{middle, 1, true});
    route_event(tree, first.get(), MouseButton{middle, 1, false});
    const auto snap = tree.build_snapshot(400, 300, 2);
    REQUIRE(tree.popup_owner() != 0);

    const auto panels = commands_of<RoundedRect>(snap->overlay);
    REQUIRE_FALSE(panels.empty());
    CHECK(panels.front()->radius == doctest::Approx(modern.corner_radius));
}
