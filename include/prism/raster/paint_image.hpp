#pragma once

// Painting into an Image: a canvas over pixels nobody else can see, painted by a callback and
// frozen as an Image. What a widget that draws its own picture (a sparkline, a badge, a logo
// rendered from a shape) needs, and the way to get that picture in a format the screen reads
// without converting it first.

#include <prism/raster/canvas.hpp>
#include <prism/render/draw_list.hpp>

#include <functional>
#include <memory>

namespace prism::raster {

// A width x height image of P's format, painted by `paint`. The canvas starts empty: fully
// transparent for Argb8888, black for Rgb565. Null when either size is not positive.
template <class P>
std::shared_ptr<const render::Image> paint_image(int width, int height,
                                                const std::function<void(BasicCanvas<P>&)>& paint);

} // namespace prism::raster
