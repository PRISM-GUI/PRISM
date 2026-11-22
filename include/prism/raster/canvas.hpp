#pragma once

// A software rasterizer target: a framebuffer that PRISM's draw commands paint into, in the pixel
// format P (pixel.hpp: Argb8888 or Rgb565). `stride` is in pixels, so row y starts at
// pixels[y * stride].
//
// A canvas can also cover a band of a taller frame: the buffer then holds rows
// [first_row, first_row + height) of a frame `width` wide, every operation takes frame
// coordinates and draws only the band's rows. Drawing a frame band by band gives the same pixels
// as drawing it whole (doc/design/band-rendering.md).
//
// Coverage rule: a pixel is covered when its centre (x + 0.5, y + 0.5) lies inside the shape
// (half-open [x0, x1) x [y0, y1) for rects), so adjacent integer rects tile exactly with no seam.
// The clip rect is different: like the SDL backend it is rounded outward (floor top-left, ceil
// bottom-right) so a float-precision edge never crops content that legitimately falls inside it.
//
// simplify: shapes are aliased (binary coverage); only text glyph coverage is blended, so text is
// anti-aliased. Upgrade: coverage-based AA for rounded rects, thick lines and circles -- a
// per-pixel signed-distance/area estimate, as render/shape_mesh.hpp's 1px alpha fringe does for
// the GPU paths.

#include <prism/core/types.hpp>
#include <prism/raster/pixel.hpp>
#include <prism/render/draw_list.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace prism::raster {

class Fonts;

using prism::core::Color;
using prism::core::Point;
using prism::core::Rect;
using prism::render::FontStyle;
using prism::render::TextAnchor;

template <class P>
class BasicCanvas {
public:
    using pixel = typename P::storage;

    BasicCanvas(std::span<pixel> pixels, int width, int height, int stride)
        : BasicCanvas(pixels, width, height, stride, 0) {}
    // A band: `pixels` holds the frame rows [first_row, first_row + height).
    BasicCanvas(std::span<pixel> pixels, int width, int height, int stride, int first_row);

    // Points the canvas at another buffer of the same size (the next back buffer, or the next
    // band at `first_row`) and resets the clip. A canvas reused this way keeps its scratch
    // storage: after the first frame, drawing does not allocate.
    void retarget(std::span<pixel> pixels) { retarget(pixels, first_row_); }
    void retarget(std::span<pixel> pixels, int first_row);

    int width() const { return width_; }
    int height() const { return height_; }
    int stride() const { return stride_; }
    int first_row() const { return first_row_; }
    std::span<pixel> pixels() const { return pixels_; }

    // --- scissor: the part of the canvas that drawing may touch at all, `clear` included. It
    // survives reset_clip and keeps clips inside it; retarget removes it. Redrawing only the
    // damaged part of a frame (render/damage.hpp) sets it to each damaged rect in turn.
    void set_scissor(Rect r);
    void clear_scissor();
    [[nodiscard]] Rect scissor_area() const;   // the scissor cut to the canvas, or the canvas

    // --- clip (a stack; push intersects the current clip with the given rect) ---
    void reset_clip();          // the scissor, or the whole canvas (a band's rows)
    void push_clip(Rect r);
    void pop_clip();
    [[nodiscard]] Rect clip() const;

    // --- drawing; every operation is clipped and src-over blended ---
    void clear(Color c);                                    // whole buffer (the scissor), ignoring the clip
    void fill_rect(Rect r, Color c);
    void rect_outline(Rect r, Color c, float thickness);    // bands inside the rect
    void rounded_rect(Rect r, Color c, float radius, float thickness);  // thickness 0 = filled
    void line(Point from, Point to, Color c, float thickness);
    void polyline(std::span<const Point> pts, Color c, float thickness);  // each segment a thick line
    void circle(Point center, float radius, Color c, float thickness);   // thickness 0 = filled
    // Scanline fill, even-odd rule. `pts` is a closed loop in the order given; PRISM emits
    // triangle-strip order, which for the convex outlines it uses is the boundary order.
    void filled_polygon(std::span<const Point> pts, Color c);
    // The image scaled into `dest` nearest-neighbour, multiplied by `alpha` and `tint`
    // (a symbolic icon is white in the file: `tint` is the text colour beside it).
    void image(const prism::render::Image& img, Rect dest, std::uint8_t alpha, Color tint);
    // Via the font module: origin is the top-left of the text box (TextAnchor::TopLeft) or its
    // centre (Center); the box is width(text) x line_height(size), the baseline ascent(size)
    // below the top -- the same metrics ui::TextMeasurer reports.
    void text(const Fonts& fonts, std::string_view utf8, Point origin, float size, Color c,
              float angle, TextAnchor anchor, FontStyle style);

    // --- pixel level (the text path uses these) ---
    void blend_pixel(int x, int y, Color c);   // src-over
    // Blends an 8-bit coverage bitmap (w x h, row-major) with its top-left at pixel (x, y):
    // effective alpha per pixel is c.a * coverage / 255.
    void blend_bitmap(int x, int y, int w, int h, const std::uint8_t* coverage, Color c);

private:
    struct Clip { int x0, y0, x1, y1; };   // pixel range [x0, x1) x [y0, y1)

    [[nodiscard]] Clip clip_of(Rect r) const;             // r rounded outward, cut to the canvas
    [[nodiscard]] std::pair<int, int> clip_rows(int y0, int y1) const;  // [y0, y1) cut to the clip
    [[nodiscard]] pixel* row(int y) const;                // frame row y in the buffer
    void fill_span(int y, int xa, int xb, Color c);       // row y, pixels [xa, xb), clip applied
    void fill_xspan(int y, float xa, float xb, Color c);  // centre rule on a float x span
    // x span (floats, centre rule) of the rounded rect at row y; nullopt when the row misses it.
    [[nodiscard]] std::optional<std::pair<float, float>> rounded_span(int y, Rect r, float radius) const;

    std::span<pixel> pixels_;
    int width_, height_, stride_, first_row_;
    std::vector<Clip> clip_stack_;   // empty means the clip is the whole canvas
    std::vector<float> crossings_;   // filled_polygon's scratch: kept so drawing does not allocate
    Clip clip_;
    Clip scissor_;   // the whole canvas unless set_scissor
};

// Defined in canvas.cpp for these two formats only.
extern template class BasicCanvas<Argb8888>;
extern template class BasicCanvas<Rgb565>;

using Canvas = BasicCanvas<Argb8888>;
using Canvas565 = BasicCanvas<Rgb565>;

} // namespace prism::raster
