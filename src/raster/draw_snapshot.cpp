#include <prism/raster/draw_snapshot.hpp>

#include <prism/raster/fonts.hpp>

#include <prism/profile/profile.hpp>
#include <prism/render/draw_list.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <variant>

namespace prism::raster {
using namespace prism::core;
using namespace prism::render;

namespace {

Rect scaled(const Rect& r, float s) {
    return Rect{Point{X{r.origin.x.raw() * s}, Y{r.origin.y.raw() * s}},
                Size{Width{r.extent.w.raw() * s}, Height{r.extent.h.raw() * s}}};
}

Point scaled(Point p, float s) { return Point{X{p.x.raw() * s}, Y{p.y.raw() * s}}; }

// The canvas draws in pixels; the commands are in logical units, `canvas.ui_scale()` pixels each.
template <class P>
void draw_list(BasicCanvas<P>& canvas, const DrawList& dl, const Fonts& fonts) {
    const float s = canvas.ui_scale();
    for (const DrawCmd& cmd : dl.commands) {
        std::visit([&](const auto& c) {
            using K = std::remove_cvref_t<decltype(c)>;
            if constexpr (std::is_same_v<K, FilledRect>) {
                canvas.fill_rect(scaled(c.rect, s), c.color);
            } else if constexpr (std::is_same_v<K, RectOutline>) {
                canvas.rect_outline(scaled(c.rect, s), c.color, c.thickness * s);
            } else if constexpr (std::is_same_v<K, TextCmd>) {
                if (!c.text.empty()) {
                    PRISM_PROFILE_SCOPE("text");
                    canvas.text(fonts, c.text, scaled(c.origin, s), c.size * s, c.color, c.angle, c.anchor, c.font);
                }
            } else if constexpr (std::is_same_v<K, ClipPush>) {
                canvas.push_clip(scaled(c.rect, s));
            } else if constexpr (std::is_same_v<K, ClipPop>) {
                canvas.pop_clip();
            } else if constexpr (std::is_same_v<K, RoundedRect>) {
                canvas.rounded_rect(scaled(c.rect, s), c.color, c.radius * s, c.thickness * s);
            } else if constexpr (std::is_same_v<K, Line>) {
                PRISM_PROFILE_SCOPE("line");
                canvas.line(scaled(c.from, s), scaled(c.to, s), c.color, c.thickness * s);
            } else if constexpr (std::is_same_v<K, Polyline>) {
                PRISM_PROFILE_SCOPE("polyline");
                canvas.polyline(canvas.scaled(c.points), c.color, c.thickness * s);
            } else if constexpr (std::is_same_v<K, Circle>) {
                canvas.circle(scaled(c.center, s), c.radius * s, c.color, c.thickness * s);
            } else if constexpr (std::is_same_v<K, FilledPolygon>) {
                canvas.filled_polygon(canvas.scaled(c.points), c.color);
            } else if constexpr (std::is_same_v<K, ImageCmd>) {
                if (c.image) canvas.image(*c.image, scaled(c.rect, s), c.alpha, c.tint);
            }
        }, cmd);
    }
}

// What a list that misses the band leaves behind: its effect on the clip stack.
template <class P>
void apply_clips(BasicCanvas<P>& canvas, const DrawList& dl) {
    const float s = canvas.ui_scale();
    for (const DrawCmd& cmd : dl.commands) {
        if (const auto* push = std::get_if<ClipPush>(&cmd)) canvas.push_clip(scaled(push->rect, s));
        else if (std::holds_alternative<ClipPop>(cmd)) canvas.pop_clip();
    }
}

// The rect a list's first ClipPush keeps everything else in: a widget's list (layout_flatten
// wraps it in its allocated rect). nullopt when the list has no leading clip, or when that clip
// is popped before the list ends and the commands after it are free.
std::optional<Rect> leading_clip(const DrawList& dl) {
    if (dl.commands.empty()) return std::nullopt;
    const auto* push = std::get_if<ClipPush>(&dl.commands.front());
    if (!push) return std::nullopt;
    int depth = 0;
    for (std::size_t i = 0; i + 1 < dl.commands.size(); ++i) {
        if (std::holds_alternative<ClipPush>(dl.commands[i])) ++depth;
        else if (std::holds_alternative<ClipPop>(dl.commands[i]) && --depth == 0) return std::nullopt;
    }
    return push->rect;
}

// A rect (x, y, w, h as floats) against the area the canvas can draw in.
template <class P>
bool overlaps_area(const BasicCanvas<P>& band, float x, float y, float w, float h) {
    const Rect a = band.scissor_area();
    return x + w > a.origin.x.raw() && x < a.origin.x.raw() + a.extent.w.raw() &&
           y + h > a.origin.y.raw() && y < a.origin.y.raw() + a.extent.h.raw();
}

template <class P>
bool reaches(const BasicCanvas<P>& band, const SceneSnapshot& snap, std::uint32_t idx) {
    const float s = band.ui_scale();
    // Rounded outward, as the canvas rounds a clip.
    if (const auto clip = leading_clip(*snap.draw_lists[idx])) {
        const Rect px = scaled(*clip, s);
        const float x = std::floor(px.origin.x.raw()), y = std::floor(px.origin.y.raw());
        return overlaps_area(band, x, y, std::ceil(px.origin.x.raw() + px.extent.w.raw()) - x,
                             std::ceil(px.origin.y.raw() + px.extent.h.raw()) - y);
    }
    if (idx >= snap.geometry.size()) return true;
    // simplify: with no leading clip, the list is trusted to draw at most kBandMargin outside its
    // geometry rect (a board's own drawing, a hand-built list); one that draws further is cut in
    // a band. Upgrade: a bounds rect per draw list, measured when the list is built.
    const Rect r = scaled(snap.geometry[idx].second, s);
    const float margin = kBandMargin * s;
    return overlaps_area(band, r.origin.x.raw() - margin, r.origin.y.raw() - margin,
                         r.extent.w.raw() + 2 * margin, r.extent.h.raw() + 2 * margin);
}

} // namespace

template <class P>
void draw_band(BasicCanvas<P>& band, const SceneSnapshot& snap, const Fonts& fonts) {
    for (std::uint32_t idx : snap.z_order) {
        if (idx >= snap.draw_lists.size() || !snap.draw_lists[idx]) continue;   // an index past the lists, or an empty slot
        const DrawList& dl = *snap.draw_lists[idx];
        if (!reaches(band, snap, idx)) {
            apply_clips(band, dl);
            continue;
        }
        PRISM_PROFILE_SCOPE("draw list");
        draw_list(band, dl, fonts);
    }
    band.reset_clip();   // the overlay is rendered last, on top, with no clip
    {
        PRISM_PROFILE_SCOPE("draw list");   // the overlay is a draw list too
        draw_list(band, snap.overlay, fonts);
    }
}

template <class P>
void draw_damage(BasicCanvas<P>& canvas, const SceneSnapshot& snap, const Fonts& fonts,
                 const prism::render::Damage& damage) {
    if (damage.full) {
        canvas.clear_scissor();
        canvas.clear(snap.window.background);
        draw_band(canvas, snap, fonts);
        return;
    }
    for (const prism::render::PixelRect& r : damage.rects) {
        canvas.set_scissor(Rect{Point{X{float(r.x0)}, Y{float(r.y0)}},
                                Size{Width{float(r.x1 - r.x0)}, Height{float(r.y1 - r.y0)}}});
        canvas.clear(snap.window.background);
        draw_band(canvas, snap, fonts);
    }
    canvas.clear_scissor();
}

template <class P>
void draw(BasicCanvas<P>& canvas, const SceneSnapshot& snap, const Fonts& fonts) {
    draw_band(canvas, snap, fonts);
}

template void draw_band(Canvas&, const SceneSnapshot&, const Fonts&);
template void draw_band(Canvas565&, const SceneSnapshot&, const Fonts&);
template void draw_damage(Canvas&, const SceneSnapshot&, const Fonts&, const prism::render::Damage&);
template void draw_damage(Canvas565&, const SceneSnapshot&, const Fonts&, const prism::render::Damage&);
template void draw(Canvas&, const SceneSnapshot&, const Fonts&);
template void draw(Canvas565&, const SceneSnapshot&, const Fonts&);

} // namespace prism::raster
