#include <prism/raster/draw_snapshot.hpp>

#include <prism/raster/fonts.hpp>

#include <prism/profile/profile.hpp>
#include <prism/render/draw_list.hpp>

#include <cstdint>
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

} // namespace

template <class P>
void draw(BasicCanvas<P>& canvas, const SceneSnapshot& snap, const Fonts& fonts) {
    for (std::uint32_t idx : snap.z_order) {
        if (idx >= snap.draw_lists.size()) continue;   // defensive: a malformed snapshot
        if (snap.draw_lists[idx]) {
            PRISM_PROFILE_SCOPE("draw list");
            draw_list(canvas, *snap.draw_lists[idx], fonts);
        }
    }
    canvas.reset_clip();   // the overlay is rendered last, on top, with no clip
    {
        PRISM_PROFILE_SCOPE("draw list");   // the overlay is a draw list too
        draw_list(canvas, snap.overlay, fonts);
    }
}

template void draw(Canvas&, const SceneSnapshot&, const Fonts&);
template void draw(Canvas565&, const SceneSnapshot&, const Fonts&);

} // namespace prism::raster
