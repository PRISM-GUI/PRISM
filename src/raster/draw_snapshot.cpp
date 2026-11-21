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

template <class P>
void draw_list(BasicCanvas<P>& canvas, const DrawList& dl, const Fonts& fonts) {
    for (const DrawCmd& cmd : dl.commands) {
        std::visit([&](const auto& c) {
            using K = std::remove_cvref_t<decltype(c)>;
            if constexpr (std::is_same_v<K, FilledRect>) {
                canvas.fill_rect(c.rect, c.color);
            } else if constexpr (std::is_same_v<K, RectOutline>) {
                canvas.rect_outline(c.rect, c.color, c.thickness);
            } else if constexpr (std::is_same_v<K, TextCmd>) {
                if (!c.text.empty()) {
                    PRISM_PROFILE_SCOPE("text");
                    canvas.text(fonts, c.text, c.origin, c.size, c.color, c.angle, c.anchor, c.font);
                }
            } else if constexpr (std::is_same_v<K, ClipPush>) {
                canvas.push_clip(c.rect);
            } else if constexpr (std::is_same_v<K, ClipPop>) {
                canvas.pop_clip();
            } else if constexpr (std::is_same_v<K, RoundedRect>) {
                canvas.rounded_rect(c.rect, c.color, c.radius, c.thickness);
            } else if constexpr (std::is_same_v<K, Line>) {
                PRISM_PROFILE_SCOPE("line");
                canvas.line(c.from, c.to, c.color, c.thickness);
            } else if constexpr (std::is_same_v<K, Polyline>) {
                PRISM_PROFILE_SCOPE("polyline");
                canvas.polyline(c.points, c.color, c.thickness);
            } else if constexpr (std::is_same_v<K, Circle>) {
                canvas.circle(c.center, c.radius, c.color, c.thickness);
            } else if constexpr (std::is_same_v<K, FilledPolygon>) {
                canvas.filled_polygon(c.points, c.color);
            } else if constexpr (std::is_same_v<K, ImageCmd>) {
                if (c.image) canvas.image(*c.image, c.rect, c.alpha, c.tint);
            }
        }, cmd);
    }
}

// What a list that misses the band leaves behind: its effect on the clip stack.
template <class P>
void apply_clips(BasicCanvas<P>& canvas, const DrawList& dl) {
    for (const DrawCmd& cmd : dl.commands) {
        if (const auto* push = std::get_if<ClipPush>(&cmd)) canvas.push_clip(push->rect);
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

// Rows [top, bottom) against the band's rows.
template <class P>
bool overlaps_rows(const BasicCanvas<P>& band, float top, float bottom) {
    return bottom > float(band.first_row()) && top < float(band.first_row() + band.height());
}

template <class P>
bool reaches(const BasicCanvas<P>& band, const SceneSnapshot& snap, std::uint32_t idx) {
    // Rounded outward, as the canvas rounds a clip.
    if (const auto clip = leading_clip(*snap.draw_lists[idx]))
        return overlaps_rows(band, std::floor(clip->origin.y.raw()),
                             std::ceil(clip->origin.y.raw() + clip->extent.h.raw()));
    if (idx >= snap.geometry.size()) return true;
    // simplify: with no leading clip, the list is trusted to draw at most kBandMargin outside its
    // geometry rect (a board's own drawing, a hand-built list); one that draws further is cut in
    // a band. Upgrade: a bounds rect per draw list, measured when the list is built.
    const Rect& r = snap.geometry[idx].second;
    return overlaps_rows(band, r.origin.y.raw() - kBandMargin,
                         r.origin.y.raw() + r.extent.h.raw() + kBandMargin);
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
void draw(BasicCanvas<P>& canvas, const SceneSnapshot& snap, const Fonts& fonts) {
    draw_band(canvas, snap, fonts);
}

template void draw_band(Canvas&, const SceneSnapshot&, const Fonts&);
template void draw_band(Canvas565&, const SceneSnapshot&, const Fonts&);
template void draw(Canvas&, const SceneSnapshot&, const Fonts&);
template void draw(Canvas565&, const SceneSnapshot&, const Fonts&);

} // namespace prism::raster
