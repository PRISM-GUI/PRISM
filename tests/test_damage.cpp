#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/render/damage.hpp>
#include <prism/render/scene_snapshot.hpp>

#include <memory>

using namespace prism::core;
using namespace prism::render;

namespace {

constexpr int W = 200, H = 100;

Rect rect(float x, float y, float w, float h) { return {Point{X{x}, Y{y}}, Size{Width{w}, Height{h}}}; }

// A widget's list as layout_flatten makes it: wrapped in a clip to its rect.
std::shared_ptr<const DrawList> widget_list(Rect r, Color c)
{
    auto dl = std::make_shared<DrawList>();
    dl->commands.emplace_back(ClipPush{r});
    dl->commands.emplace_back(FilledRect{r, c});
    dl->commands.emplace_back(ClipPop{});
    return dl;
}

struct Entry {
    WidgetId id;
    Rect rect;
    std::shared_ptr<const DrawList> list;
};

SceneSnapshot snapshot(const std::vector<Entry>& entries)
{
    SceneSnapshot s;
    s.viewport = Size{Width{float(W)}, Height{float(H)}};
    for (std::uint32_t i = 0; i < entries.size(); ++i) {
        s.geometry.emplace_back(entries[i].id, entries[i].rect);
        s.draw_lists.push_back(entries[i].list);
        s.z_order.push_back(i);
    }
    return s;
}

Damage between(const SceneSnapshot* before, const SceneSnapshot& after)
{
    Damage d;
    DamageScratch scratch;
    damage_between(before, after, W, H, scratch, d);
    return d;
}

const Rect kA = rect(10, 10, 40, 20), kB = rect(100, 50, 30, 30);

} // namespace

TEST_CASE("no previous frame is full damage") {
    const auto s = snapshot({{1, kA, widget_list(kA, Color::rgba(1, 2, 3))}});
    CHECK(between(nullptr, s).full);
}

TEST_CASE("the same lists in both frames are no damage") {
    const auto la = widget_list(kA, Color::rgba(1, 2, 3)), lb = widget_list(kB, Color::rgba(4, 5, 6));
    const auto before = snapshot({{1, kA, la}, {2, kB, lb}});
    const auto after = snapshot({{1, kA, la}, {2, kB, lb}});
    const Damage d = between(&before, after);
    CHECK_FALSE(d.full);
    CHECK(d.rects.empty());
}

TEST_CASE("a replaced list damages its own clip rect, and only that") {
    const auto la = widget_list(kA, Color::rgba(1, 2, 3)), lb = widget_list(kB, Color::rgba(4, 5, 6));
    const auto before = snapshot({{1, kA, la}, {2, kB, lb}});
    const auto after = snapshot({{1, kA, la}, {2, kB, widget_list(kB, Color::rgba(9, 9, 9))}});
    const Damage d = between(&before, after);
    REQUIRE(d.rects.size() == 1);
    CHECK(d.rects[0].x0 == 100);
    CHECK(d.rects[0].y0 == 50);
    CHECK(d.rects[0].x1 == 130);
    CHECK(d.rects[0].y1 == 80);
}

TEST_CASE("a widget that moved damages where it was and where it is") {
    const auto l1 = widget_list(kA, Color::rgba(1, 2, 3));
    const Rect moved = rect(60, 10, 40, 20);
    const auto before = snapshot({{1, kA, l1}});
    const auto after = snapshot({{1, moved, widget_list(moved, Color::rgba(1, 2, 3))}});
    const Damage d = between(&before, after);
    CHECK_FALSE(d.full);
    const auto covers = [&](int x, int y) {
        for (const PixelRect& r : d.rects)
            if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1)
                return true;
        return false;
    };
    CHECK(covers(15, 15));
    CHECK(covers(65, 15));
}

TEST_CASE("a widget that appeared or went away is damage") {
    const auto la = widget_list(kA, Color::rgba(1, 2, 3)), lb = widget_list(kB, Color::rgba(4, 5, 6));
    const auto one = snapshot({{1, kA, la}});
    const auto two = snapshot({{1, kA, la}, {2, kB, lb}});
    CHECK(between(&one, two).rects.size() == 1);
    CHECK(between(&two, one).rects.size() == 1);
}

TEST_CASE("a changed viewport, background, or draw order is full damage") {
    const auto la = widget_list(kA, Color::rgba(1, 2, 3)), lb = widget_list(kB, Color::rgba(4, 5, 6));
    const auto base = snapshot({{1, kA, la}, {2, kB, lb}});

    auto resized = base;
    resized.viewport = Size{Width{300.f}, Height{H}};
    CHECK(between(&base, resized).full);

    auto themed = base;
    themed.window.background = Color::rgba(250, 250, 250);
    CHECK(between(&base, themed).full);

    auto reordered = base;
    reordered.z_order = {1, 0};   // a widget raised over another
    CHECK(between(&base, reordered).full);
}

TEST_CASE("the overlay: unchanged is no damage, changed is full damage") {
    const auto la = widget_list(kA, Color::rgba(1, 2, 3));
    auto before = snapshot({{1, kA, la}});
    before.overlay.commands.emplace_back(FilledRect{kB, Color::rgba(7, 7, 7)});
    auto same = snapshot({{1, kA, la}});
    same.overlay.commands.emplace_back(FilledRect{kB, Color::rgba(7, 7, 7)});
    CHECK(between(&before, same).rects.empty());
    CHECK_FALSE(between(&before, same).full);

    auto other = snapshot({{1, kA, la}});
    other.overlay.commands.emplace_back(FilledRect{kB, Color::rgba(8, 8, 8)});
    CHECK(between(&before, other).full);
}

TEST_CASE("a list with no clip and no geometry is unbounded: full damage") {
    auto hand = std::make_shared<DrawList>();
    hand->commands.emplace_back(FilledRect{kA, Color::rgba(1, 2, 3)});
    SceneSnapshot before, after;
    before.viewport = after.viewport = Size{Width{float(W)}, Height{float(H)}};
    before.draw_lists = {hand};
    before.z_order.assign(1, 0);   // not = {0}: GCC 14 under ASan misreads it as an out-of-bounds copy
    after.draw_lists = {std::make_shared<DrawList>(*hand)};
    after.z_order.assign(1, 0);
    CHECK(between(&before, after).full);
}

TEST_CASE("damage is cut to the frame") {
    const Rect edge = rect(180, 80, 60, 60);
    const auto before = snapshot({{1, edge, widget_list(edge, Color::rgba(1, 2, 3))}});
    const auto after = snapshot({{1, edge, widget_list(edge, Color::rgba(9, 9, 9))}});
    const Damage d = between(&before, after);
    REQUIRE(d.rects.size() == 1);
    CHECK(d.rects[0].x1 == W);
    CHECK(d.rects[0].y1 == H);
}

TEST_CASE("overlapping rects are merged into their bounding box") {
    Damage a, b, out;
    a.rects = {{0, 0, 50, 50}};
    b.rects = {{40, 40, 90, 90}};
    unite(a, b, out);
    REQUIRE(out.rects.size() == 1);
    CHECK(out.rects[0].x0 == 0);
    CHECK(out.rects[0].x1 == 90);
    CHECK(out.rects[0].y1 == 90);
}

TEST_CASE("apart rects stay apart; full wins") {
    Damage a, b, out;
    a.rects = {{0, 0, 10, 10}};
    b.rects = {{100, 50, 110, 60}};
    unite(a, b, out);
    CHECK(out.rects.size() == 2);
    b.full = true;
    unite(a, b, out);
    CHECK(out.full);
    CHECK(out.rects.empty());
}

TEST_CASE("many rects collapse to one bounding box") {
    Damage a, b, out;
    for (int i = 0; i < 20; ++i)
        a.rects.push_back({i * 10, i * 4, i * 10 + 5, i * 4 + 2});
    unite(a, b, out);
    CHECK(out.rects.size() == 1);
}
