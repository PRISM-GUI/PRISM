#include <prism/render/damage.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <variant>

namespace prism::render {

namespace {

using prism::core::Rect;

// The rect a list's first ClipPush keeps everything else in (a widget's list: layout_flatten wraps
// it in its allocated rect). nullopt when there is no leading clip, or it is popped before the list
// ends and the commands after it are free.
std::optional<Rect> leading_clip(const DrawList& dl)
{
    if (dl.commands.empty())
        return std::nullopt;
    const auto* push = std::get_if<ClipPush>(&dl.commands.front());
    if (!push)
        return std::nullopt;
    int depth = 0;
    for (std::size_t i = 0; i + 1 < dl.commands.size(); ++i) {
        if (std::holds_alternative<ClipPush>(dl.commands[i]))
            ++depth;
        else if (std::holds_alternative<ClipPop>(dl.commands[i]) && --depth == 0)
            return std::nullopt;
    }
    return push->rect;
}

// Where entry `i` can draw; nullopt when that is unknown (a hand-built list with no clip and no
// geometry entry).
std::optional<Rect> list_bounds(const SceneSnapshot& s, std::uint32_t i)
{
    if (const auto clip = leading_clip(*s.draw_lists[i]))
        return clip;
    if (i >= s.geometry.size())
        return std::nullopt;
    const Rect& g = s.geometry[i].second;
    return Rect{{X{g.origin.x.raw() - kListMargin}, Y{g.origin.y.raw() - kListMargin}},
                {Width{g.extent.w.raw() + 2 * kListMargin}, Height{g.extent.h.raw() + 2 * kListMargin}}};
}

// Rounded outward and cut to the frame, as the canvas rounds a clip.
PixelRect to_pixels(const Rect& r, int width, int height)
{
    const int x0 = std::clamp(int(std::floor(r.origin.x.raw())), 0, width);
    const int y0 = std::clamp(int(std::floor(r.origin.y.raw())), 0, height);
    const int x1 = std::clamp(int(std::ceil(r.origin.x.raw() + r.extent.w.raw())), 0, width);
    const int y1 = std::clamp(int(std::ceil(r.origin.y.raw() + r.extent.h.raw())), 0, height);
    return {x0, y0, std::max(x0, x1), std::max(y0, y1)};
}

bool empty(const PixelRect& r) { return r.x0 >= r.x1 || r.y0 >= r.y1; }

bool touches(const PixelRect& a, const PixelRect& b)
{
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

PixelRect bounding(const PixelRect& a, const PixelRect& b)
{
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

// Adds `r`, merging it with every rect it touches.
void add_rect(std::vector<PixelRect>& rects, PixelRect r)
{
    if (empty(r))
        return;
    for (std::size_t i = 0; i < rects.size();) {
        if (touches(rects[i], r)) {
            r = bounding(rects[i], r);
            rects[i] = rects.back();
            rects.pop_back();
            i = 0;
        } else {
            ++i;
        }
    }
    rects.push_back(r);
}

void collapse_if_many(std::vector<PixelRect>& rects)
{
    if (rects.size() <= kMaxRects)
        return;
    PixelRect all = rects.front();
    for (const PixelRect& r : rects)
        all = bounding(all, r);
    rects.assign(1, all);
}

void set_full(Damage& out)
{
    out.full = true;
    out.rects.clear();
}

bool chrome_differs(const SceneSnapshot& a, const SceneSnapshot& b)
{
    return !(a.window == b.window) || !(a.viewport == b.viewport) ||
           a.overlay.commands != b.overlay.commands;
}

// Entry indices sorted by widget id. False when two entries share an id (the join below would
// pair the wrong ones).
bool sorted_by_id(const SceneSnapshot& s, std::vector<std::uint32_t>& order)
{
    order.clear();
    for (std::uint32_t i = 0; i < s.draw_lists.size() && i < s.geometry.size(); ++i)
        order.push_back(i);
    std::ranges::sort(order, {}, [&](std::uint32_t i) { return s.geometry[i].first; });
    return std::ranges::adjacent_find(order, {}, [&](std::uint32_t i) { return s.geometry[i].first; }) ==
           order.end();
}

bool has_id(const SceneSnapshot& s, const std::vector<std::uint32_t>& sorted, WidgetId id)
{
    const auto it = std::ranges::lower_bound(sorted, id, {}, [&](std::uint32_t i) { return s.geometry[i].first; });
    return it != sorted.end() && s.geometry[*it].first == id;
}

// The draw order as widget ids, of the widgets both frames have.
void common_order(const SceneSnapshot& s, const SceneSnapshot& other,
                 const std::vector<std::uint32_t>& other_sorted, std::vector<WidgetId>& out)
{
    out.clear();
    for (const std::uint32_t i : s.z_order)
        if (i < s.geometry.size() && has_id(other, other_sorted, s.geometry[i].first))
            out.push_back(s.geometry[i].first);
}

bool same_entry(const SceneSnapshot& a, std::uint32_t i, const SceneSnapshot& b, std::uint32_t j)
{
    const auto same_at = [&](const auto& va, const auto& vb) {
        const bool ha = i < va.size(), hb = j < vb.size();
        return ha == hb && (!ha || va[i] == vb[j]);
    };
    return a.draw_lists[i] == b.draw_lists[j] && a.geometry[i].second == b.geometry[j].second &&
           same_at(a.visible, b.visible) && same_at(a.scale, b.scale);
}

// Adds where entry `i` of `s` draws. False when that is unknown.
bool add_entry(std::vector<PixelRect>& rects, const SceneSnapshot& s, std::uint32_t i, int width, int height)
{
    if (!s.draw_lists[i])
        return true;
    const auto bounds = list_bounds(s, i);
    if (!bounds)
        return false;
    add_rect(rects, to_pixels(*bounds, width, height));
    return true;
}

} // namespace

void damage_between(const SceneSnapshot* before, const SceneSnapshot& after, int width, int height,
                    DamageScratch& scratch, Damage& out)
{
    out.full = false;
    out.rects.clear();
    if (!before || chrome_differs(*before, after) || !sorted_by_id(*before, scratch.before) ||
        !sorted_by_id(after, scratch.after) || before->draw_lists.size() != before->geometry.size() ||
        after.draw_lists.size() != after.geometry.size())
        return set_full(out);

    common_order(*before, after, scratch.after, scratch.order_before);
    common_order(after, *before, scratch.before, scratch.order_after);
    if (scratch.order_before != scratch.order_after)
        return set_full(out);

    bool known = true;
    const auto id_of = [](const SceneSnapshot& s, std::uint32_t i) { return s.geometry[i].first; };
    std::size_t a = 0, b = 0;
    while (known && (a < scratch.before.size() || b < scratch.after.size())) {
        const bool only_after = a == scratch.before.size() ||
                                (b < scratch.after.size() &&
                                 id_of(after, scratch.after[b]) < id_of(*before, scratch.before[a]));
        const bool only_before = b == scratch.after.size() ||
                                 (a < scratch.before.size() &&
                                  id_of(*before, scratch.before[a]) < id_of(after, scratch.after[b]));
        if (only_after) {
            known = add_entry(out.rects, after, scratch.after[b++], width, height);
        } else if (only_before) {
            known = add_entry(out.rects, *before, scratch.before[a++], width, height);
        } else {
            const std::uint32_t i = scratch.before[a++], j = scratch.after[b++];
            if (!same_entry(*before, i, after, j))
                known = add_entry(out.rects, *before, i, width, height) &&
                        add_entry(out.rects, after, j, width, height);
        }
    }
    if (!known)
        return set_full(out);
    collapse_if_many(out.rects);
}

void unite(const Damage& a, const Damage& b, Damage& out)
{
    out.full = a.full || b.full;
    out.rects.clear();
    if (out.full)
        return;
    for (const PixelRect& r : a.rects)
        add_rect(out.rects, r);
    for (const PixelRect& r : b.rects)
        add_rect(out.rects, r);
    collapse_if_many(out.rects);
}

} // namespace prism::render
