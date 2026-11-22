#pragma once

// What changed between two frames, in pixels: the part of the screen a renderer has to redraw when
// the frame it draws into already shows the previous one (or the one before it: see unite).
//
// A widget's draw list is shared between snapshots while the widget is unchanged (the per-widget
// cache in layout_flatten), so a list that is the same object in both frames did not change and
// one that is not did. Everything this cannot tell apart is full damage: the answer is always
// "redraw at least this", never less.
//
// simplify: a changed overlay (popups, drag ghosts, focus rings) and a changed relative draw
// order redraw the whole frame, and a list is bounded by its leading clip or its geometry rect, not measured.
// Upgrade: per-command bounds for the overlay, and a z-order diff.
// simplify: more than kMaxRects rects collapse to their bounding box. Upgrade: a smarter merge
// (rect union by area) if a scene turns out to scatter many small changes.

#include <prism/render/scene_snapshot.hpp>

#include <cstdint>
#include <vector>

namespace prism::render {

// A list that is not wrapped in a clip may draw this far outside its geometry rect.
inline constexpr float kListMargin = 16.f;
inline constexpr std::size_t kMaxRects = 8;

struct PixelRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // [x0, x1) x [y0, y1)
    bool operator==(const PixelRect&) const = default;
};

struct Damage {
    bool full = false;               // the whole frame; `rects` is then empty
    std::vector<PixelRect> rects;    // disjoint-or-merged, cut to the frame
    [[nodiscard]] bool empty() const { return !full && rects.empty(); }
};

// Working memory of damage_between, kept by the caller so a frame allocates nothing once it has grown.
struct DamageScratch {
    std::vector<std::uint32_t> before, after;   // entry indices sorted by widget id
    std::vector<WidgetId> order_before, order_after;   // draw order of the widgets both frames have
};

// `before` is the frame the buffer shows now, nullptr when there is none (first frame).
// `width` x `height` is the frame in pixels; the snapshots are in logical units, `scale` pixels each
// (FrameOptions::ui_scale).
void damage_between(const SceneSnapshot* before, const SceneSnapshot& after, int width, int height,
                    DamageScratch& scratch, Damage& out, float scale = 1.f);

// The damage of two successive frames together: what a buffer that is two frames old must redraw.
// `out` may not be `a` or `b`.
void unite(const Damage& a, const Damage& b, Damage& out);

} // namespace prism::render
