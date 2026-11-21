#pragma once

// Draws a published SceneSnapshot into a canvas of either pixel format: walks z_order
// back-to-front, each entry's draw list, then the overlay on top with no clip. ClipPush pushes the (already resolved) rect as the
// new clip -- intersected with the canvas bounds -- and ClipPop restores the previous one.
// Coordinates are logical points = pixels here (scale 1).
//
// Clearing is the caller's job: fill the canvas with snap.window.background first, as the SDL
// backend clears with it (SDL_RenderClear in SdlWindow::render_snapshot).

#include <prism/raster/canvas.hpp>

#include <prism/render/scene_snapshot.hpp>

namespace prism::raster {

template <class P>
void draw(BasicCanvas<P>& canvas, const prism::render::SceneSnapshot& snap, const Fonts& fonts);

extern template void draw(Canvas&, const prism::render::SceneSnapshot&, const Fonts&);
extern template void draw(Canvas565&, const prism::render::SceneSnapshot&, const Fonts&);

} // namespace prism::raster
