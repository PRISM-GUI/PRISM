#pragma once

// Draws a published SceneSnapshot into a canvas of either pixel format: walks z_order
// back-to-front, each entry's draw list, then the overlay on top with no clip. ClipPush pushes the (already resolved) rect as the
// new clip -- intersected with the canvas bounds -- and ClipPop restores the previous one.
// Coordinates are logical points = pixels here (scale 1).
//
// Clearing is the caller's job: fill the canvas with snap.window.background first, as the SDL
// backend clears with it (SDL_RenderClear in SdlWindow::render_snapshot).
//
// draw_band draws the part of the frame a band canvas holds (canvas.hpp): the draw lists whose
// geometry rect, grown by kBandMargin on each side, reaches the band's rows, and the overlay. A
// list that misses the band still applies its ClipPush/ClipPop, so the lists after it are clipped
// as in the whole frame. A list with no geometry entry (a snapshot built by hand) is always
// drawn. Banded output equals the whole frame as long as a widget draws at most kBandMargin
// pixels outside its geometry rect (doc/design/band-rendering.md).

#include <prism/raster/canvas.hpp>

#include <prism/render/scene_snapshot.hpp>

namespace prism::raster {

inline constexpr float kBandMargin = 16.f;

template <class P>
void draw_band(BasicCanvas<P>& band, const prism::render::SceneSnapshot& snap, const Fonts& fonts);

// The whole frame: draw_band over a canvas that holds every row.
template <class P>
void draw(BasicCanvas<P>& canvas, const prism::render::SceneSnapshot& snap, const Fonts& fonts);

extern template void draw_band(Canvas&, const prism::render::SceneSnapshot&, const Fonts&);
extern template void draw_band(Canvas565&, const prism::render::SceneSnapshot&, const Fonts&);
extern template void draw(Canvas&, const prism::render::SceneSnapshot&, const Fonts&);
extern template void draw(Canvas565&, const prism::render::SceneSnapshot&, const Fonts&);

} // namespace prism::raster
