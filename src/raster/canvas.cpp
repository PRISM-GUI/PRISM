#include <prism/raster/canvas.hpp>

#include <prism/raster/fonts.hpp>

#include <prism/core/utf8.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <cstdint>

namespace prism::raster {
using namespace prism::core;
using prism::render::Image;
using prism::render::TextAnchor;

namespace {

// First pixel index whose centre lies in [lo, hi): x + 0.5 >= lo and x + 0.5 < hi.
inline int span_begin(float lo) { return static_cast<int>(std::ceil(lo - 0.5f)); }
inline int span_end(float hi) { return static_cast<int>(std::ceil(hi - 0.5f)); }

// Always on (not an assert): a short span would be written past its end, and the check is cheap.
void require_span(std::size_t size, int stride, int height) {
    if (size >= std::size_t(stride) * height) return;
    std::fputs("prism::raster::Canvas: pixel span smaller than stride * height\n", stderr);
    std::abort();
}

// The hairline samples i in [0, n] (at t = i / n) whose row y0 + dy * t can round into rows
// [r0, r1), widened by a sample and a row: a band then walks only its part of a long line, and
// blend_pixel still clips every sample, so the pixels are those of the whole walk.
std::pair<int, int> samples_in_rows(float y0, float dy, int n, int r0, int r1) {
    if (dy == 0.f) {
        const long row = std::lround(y0);
        return row >= r0 && row < r1 ? std::pair{0, n} : std::pair{0, -1};
    }
    const float ta = std::clamp((float(r0) - 1.f - y0) / dy, 0.f, 1.f);
    const float tb = std::clamp((float(r1) + 1.f - y0) / dy, 0.f, 1.f);
    const float fn = float(n);
    return {std::max(0, int(std::floor(std::min(ta, tb) * fn)) - 1),
            std::min(n, int(std::ceil(std::max(ta, tb) * fn)) + 1)};
}

} // namespace

template <class P>
BasicCanvas<P>::BasicCanvas(std::span<pixel> pixels, int width, int height, int stride, int first_row)
    : pixels_(pixels), width_(width), height_(height), stride_(stride), first_row_(first_row) {
    require_span(pixels.size(), stride, height);
    clear_scissor();
}

template <class P>
void BasicCanvas<P>::retarget(std::span<pixel> pixels, int first_row) {
    require_span(pixels.size(), stride_, height_);
    pixels_ = pixels;
    first_row_ = first_row;
    clear_scissor();
}

template <class P>
typename BasicCanvas<P>::pixel* BasicCanvas<P>::row(int y) const {
    return pixels_.data() + static_cast<std::size_t>(y - first_row_) * stride_;
}

template <class P>
std::pair<int, int> BasicCanvas<P>::clip_rows(int y0, int y1) const {
    return {std::max(y0, clip_.y0), std::min(y1, clip_.y1)};
}

template <class P>
typename BasicCanvas<P>::Clip BasicCanvas<P>::clip_of(Rect r) const {
    int x0 = static_cast<int>(std::floor(r.origin.x.raw()));
    int y0 = static_cast<int>(std::floor(r.origin.y.raw()));
    int x1 = static_cast<int>(std::ceil(r.origin.x.raw() + r.extent.w.raw()));
    int y1 = static_cast<int>(std::ceil(r.origin.y.raw() + r.extent.h.raw()));
    x0 = std::max(x0, 0);  y0 = std::max(y0, first_row_);
    x1 = std::min(x1, width_);  y1 = std::min(y1, first_row_ + height_);
    if (x0 > x1) x0 = x1;
    if (y0 > y1) y0 = y1;
    return {x0, y0, x1, y1};
}

template <class P>
void BasicCanvas<P>::reset_clip() {
    clip_ = scissor_;
    clip_stack_.clear();
}

template <class P>
void BasicCanvas<P>::set_scissor(Rect r) {
    scissor_ = clip_of(r);
    reset_clip();
}

template <class P>
void BasicCanvas<P>::clear_scissor() {
    scissor_ = {0, first_row_, width_, first_row_ + height_};
    reset_clip();
}

template <class P>
std::span<const Point> BasicCanvas<P>::scaled(std::span<const Point> pts) {
    if (ui_scale_ == 1.f) return pts;
    scaled_points_.clear();
    for (const Point& p : pts)
        scaled_points_.push_back(Point{X{p.x.raw() * ui_scale_}, Y{p.y.raw() * ui_scale_}});
    return scaled_points_;
}

template <class P>
Rect BasicCanvas<P>::scissor_area() const {
    return Rect{Point{X{float(scissor_.x0)}, Y{float(scissor_.y0)}},
                Size{Width{float(scissor_.x1 - scissor_.x0)}, Height{float(scissor_.y1 - scissor_.y0)}}};
}

template <class P>
void BasicCanvas<P>::push_clip(Rect r) {
    // The snapshot's ClipPush rects are already resolved against the outer clips (resolve_clips),
    // so intersecting with the current clip too changes nothing for a published snapshot -- and
    // keeps an unresolved one from widening the clip. Intersected with the canvas bounds as well.
    Clip c = clip_of(r);
    c.x0 = std::max(c.x0, clip_.x0);  c.y0 = std::max(c.y0, clip_.y0);
    c.x1 = std::min(c.x1, clip_.x1);  c.y1 = std::min(c.y1, clip_.y1);
    clip_stack_.push_back(clip_);
    clip_ = c;
}

template <class P>
void BasicCanvas<P>::pop_clip() {
    if (!clip_stack_.empty()) {
        clip_ = clip_stack_.back();
        clip_stack_.pop_back();
    } else {
        reset_clip();
    }
}

template <class P>
Rect BasicCanvas<P>::clip() const {
    return Rect{Point{X{static_cast<float>(clip_.x0)}, Y{static_cast<float>(clip_.y0)}},
                Size{Width{static_cast<float>(clip_.x1 - clip_.x0)},
                     Height{static_cast<float>(clip_.y1 - clip_.y0)}}};
}

template <class P>
void BasicCanvas<P>::clear(Color c) {
    // Writes the pixel as-is (no blending): the framebuffer is fully repainted each frame, and a
    // translucent clear over the previous frame is not a case the board needs.
    const pixel packed = P::pack(c);
    for (int y = scissor_.y0; y < scissor_.y1; ++y)
        std::fill_n(row(y) + scissor_.x0, scissor_.x1 - scissor_.x0, packed);
}

template <class P>
void BasicCanvas<P>::fill_span(int y, int xa, int xb, Color c) {
    if (y < clip_.y0 || y >= clip_.y1) return;
    xa = std::max(xa, clip_.x0);
    xb = std::min(xb, clip_.x1);
    if (xa >= xb) return;
    pixel* px = row(y);
    if (c.a == 255) {
        const pixel p = P::pack(c);
        for (int x = xa; x < xb; ++x) px[x] = p;
    } else {
        for (int x = xa; x < xb; ++x) px[x] = P::over(px[x], c);
    }
}

template <class P>
void BasicCanvas<P>::fill_xspan(int y, float xa, float xb, Color c) {
    fill_span(y, span_begin(xa), span_end(xb), c);
}

template <class P>
void BasicCanvas<P>::fill_rect(Rect r, Color c) {
    const auto [y0, y1] = clip_rows(span_begin(r.origin.y.raw()), span_end(r.origin.y.raw() + r.extent.h.raw()));
    for (int y = y0; y < y1; ++y)
        fill_xspan(y, r.origin.x.raw(), r.origin.x.raw() + r.extent.w.raw(), c);
}

template <class P>
void BasicCanvas<P>::rect_outline(Rect r, Color c, float thickness) {
    if (thickness <= 0.f) return;
    const float x0 = r.origin.x.raw(), y0 = r.origin.y.raw();
    const float w = r.extent.w.raw(), h = r.extent.h.raw();
    if (thickness * 2.f >= std::min(w, h)) {   // the bands would cover the whole rect
        fill_rect(r, c);
        return;
    }
    fill_rect(Rect{Point{X{x0}, Y{y0}}, Size{Width{w}, Height{thickness}}}, c);
    fill_rect(Rect{Point{X{x0}, Y{y0 + h - thickness}}, Size{Width{w}, Height{thickness}}}, c);
    fill_rect(Rect{Point{X{x0}, Y{y0 + thickness}}, Size{Width{thickness}, Height{h - 2.f * thickness}}}, c);
    fill_rect(Rect{Point{X{x0 + w - thickness}, Y{y0 + thickness}}, Size{Width{thickness}, Height{h - 2.f * thickness}}}, c);
}

template <class P>
std::optional<std::pair<float, float>> BasicCanvas<P>::rounded_span(int y, Rect r, float radius) const {
    const float x0 = r.origin.x.raw(), y0 = r.origin.y.raw();
    const float x1 = x0 + r.extent.w.raw(), y1 = y0 + r.extent.h.raw();
    const float cy = static_cast<float>(y) + 0.5f;
    if (cy < y0 || cy >= y1) return std::nullopt;
    float inset = 0.f;
    auto corner = [&](float dy) {   // dy > 0: the row is inside the corner band
        if (dy > 0.f) inset = std::max(inset, radius - std::sqrt(std::max(0.f, radius * radius - dy * dy)));
    };
    corner((y0 + radius) - cy);   // top corners
    corner(cy - (y1 - radius));   // bottom corners
    const float xa = x0 + inset, xb = x1 - inset;
    if (!(xb > xa)) return std::nullopt;
    return std::pair{xa, xb};
}

template <class P>
void BasicCanvas<P>::rounded_rect(Rect r, Color c, float radius, float thickness) {
    const float w = r.extent.w.raw(), h = r.extent.h.raw();
    radius = std::clamp(radius, 0.f, std::min(w, h) / 2.f);
    if (radius <= 0.5f) {   // a square corner needs no arcs (as the SDL backend special-cases)
        if (thickness > 0.f) rect_outline(r, c, thickness);
        else fill_rect(r, c);
        return;
    }
    if (thickness <= 0.f) {
        const auto [y0, y1] = clip_rows(span_begin(r.origin.y.raw()), span_end(r.origin.y.raw() + h));
        for (int y = y0; y < y1; ++y)
            if (auto s = rounded_span(y, r, radius)) fill_xspan(y, s->first, s->second, c);
        return;
    }
    // Stroke: pixels within thickness/2 of the outline = the rounded rect grown by thickness/2
    // minus the one shrunk by thickness/2 (offsetting a rounded rect keeps its corner radius).
    const float t2 = thickness / 2.f;
    const float x0 = r.origin.x.raw(), y0 = r.origin.y.raw();
    const Rect outer{Point{X{x0 - t2}, Y{y0 - t2}}, Size{Width{w + 2.f * t2}, Height{h + 2.f * t2}}};
    const Rect inner{Point{X{x0 + t2}, Y{y0 + t2}}, Size{Width{std::max(0.f, w - 2.f * t2)}, Height{std::max(0.f, h - 2.f * t2)}}};
    const auto [y0i, y1i] = clip_rows(span_begin(outer.origin.y.raw()),
                                      span_end(outer.origin.y.raw() + outer.extent.h.raw()));
    for (int y = y0i; y < y1i; ++y) {
        auto so = rounded_span(y, outer, radius + t2);
        if (!so) continue;
        auto si = rounded_span(y, inner, std::max(0.f, radius - t2));
        if (!si) {
            fill_xspan(y, so->first, so->second, c);
        } else {
            fill_xspan(y, so->first, si->first, c);
            fill_xspan(y, si->second, so->second, c);
        }
    }
}

template <class P>
void BasicCanvas<P>::line(Point from, Point to, Color c, float thickness) {
    const float dx = to.x.raw() - from.x.raw(), dy = to.y.raw() - from.y.raw();
    const float len = std::sqrt(dx * dx + dy * dy);
    if (thickness <= 1.f) {
        // Hairline: set the pixel under each half-pixel sample along the segment (the SDL backend
        // uses SDL_RenderLine for these; this is its integer twin).
        const int n = std::max(1, static_cast<int>(std::ceil(len * 2.f)));
        const auto [i0, i1] = samples_in_rows(from.y.raw(), dy, n, clip_.y0, clip_.y1);
        for (int i = i0; i <= i1; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            blend_pixel(static_cast<int>(std::lround(from.x.raw() + dx * t)),
                        static_cast<int>(std::lround(from.y.raw() + dy * t)), c);
        }
        return;
    }
    // Thick line: the segment as a square-ended quad. simplify: square ends, no caps or joins --
    // polyline() below draws each segment separately, so joins are covered by overlap.
    const float ux = len > 0.f ? dx / len : 1.f, uy = len > 0.f ? dy / len : 0.f;
    const float nx = -uy * thickness / 2.f, ny = ux * thickness / 2.f;
    const Point quad[4] = {
        Point{X{from.x.raw() + nx}, Y{from.y.raw() + ny}},
        Point{X{to.x.raw() + nx}, Y{to.y.raw() + ny}},
        Point{X{to.x.raw() - nx}, Y{to.y.raw() - ny}},
        Point{X{from.x.raw() - nx}, Y{from.y.raw() - ny}},
    };
    filled_polygon(quad, c);
}

namespace {

// A plot trace: points that only go right, and a line thin enough that the square ends and the
// joins of the general path cannot be told from the fast one's.
constexpr float kTraceMaxThickness = 4.f;

bool is_trace(std::span<const Point> pts, float thickness) {
    if (pts.size() < 2 || thickness <= 1.f || thickness > kTraceMaxThickness) return false;
    for (std::size_t i = 1; i < pts.size(); ++i)
        if (!(pts[i].x.raw() >= pts[i - 1].x.raw())) return false;   // also false for NaN
    return true;
}

} // namespace

template <class P>
void BasicCanvas<P>::fill_column(int x, float lo, float hi, Color c) {
    if (x < clip_.x0 || x >= clip_.x1) return;
    const auto [y0, y1] = clip_rows(span_begin(lo), span_end(hi));
    for (int y = y0; y < y1; ++y)
        row(y)[x] = P::over(row(y)[x], c);
}

// A thick line that only goes right is, in each pixel column, one vertical run: the rows whose
// centre lies within the strip's vertical half-extent of the line at the column's centre. That
// is the centre rule the quad path applies, evaluated once per column instead of once per
// segment and row (the digital oscilloscope's one run per column, for a thick line).
// simplify: the ends are cut vertically instead of square, and no joins are drawn. A vertical
// segment (a step in the data) is its own column's run from one end to the other. Upgrade:
// coverage-weighted runs (anti-aliasing) if traces ever need to be smooth.
template <class P>
void BasicCanvas<P>::thick_trace(std::span<const Point> pts, Color c, float thickness) {
    const float half = thickness / 2.f;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const float x0 = pts[i - 1].x.raw(), y0 = pts[i - 1].y.raw();
        const float x1 = pts[i].x.raw(), y1 = pts[i].y.raw();
        if (x1 <= x0) {
            fill_column(static_cast<int>(std::floor(x0)), std::min(y0, y1) - half, std::max(y0, y1) + half, c);
            continue;
        }
        const float slope = (y1 - y0) / (x1 - x0);
        const float reach = half * std::sqrt(1.f + slope * slope);
        const int k0 = std::max(span_begin(x0), clip_.x0), k1 = std::min(span_end(x1), clip_.x1);
        for (int k = k0; k < k1; ++k) {
            const float y = y0 + slope * (static_cast<float>(k) + 0.5f - x0);
            fill_column(k, y - reach, y + reach, c);
        }
    }
}

template <class P>
void BasicCanvas<P>::polyline(std::span<const Point> pts, Color c, float thickness) {
    if (is_trace(pts, thickness)) return thick_trace(pts, c, thickness);
    for (std::size_t i = 1; i < pts.size(); ++i)
        line(pts[i - 1], pts[i], c, thickness);
}

template <class P>
void BasicCanvas<P>::circle(Point center, float radius, Color c, float thickness) {
    if (radius <= 0.f) return;
    const float cx = center.x.raw(), cy = center.y.raw();
    if (thickness <= 0.f) {
        const float r2 = radius * radius;
        const auto [y0, y1] = clip_rows(span_begin(cy - radius), span_end(cy + radius));
        for (int y = y0; y < y1; ++y) {
            const float d = static_cast<float>(y) + 0.5f - cy;
            if (std::abs(d) > radius) continue;
            const float half = std::sqrt(r2 - d * d);
            fill_xspan(y, cx - half, cx + half, c);
        }
        return;
    }
    // Stroke: an annulus around the nominal circle, |dist - radius| <= thickness/2 (the SDL
    // backend strokes the same outline through a mesh).
    const float ro = radius + thickness / 2.f, ri = std::max(0.f, radius - thickness / 2.f);
    const auto [y0, y1] = clip_rows(span_begin(cy - ro), span_end(cy + ro));
    for (int y = y0; y < y1; ++y) {
        const float d = static_cast<float>(y) + 0.5f - cy;
        if (std::abs(d) > ro) continue;
        const float ho = std::sqrt(ro * ro - d * d);
        const float hi = std::abs(d) <= ri ? std::sqrt(ri * ri - d * d) : 0.f;
        if (hi > 0.f) {
            fill_xspan(y, cx - ho, cx - hi, c);
            fill_xspan(y, cx + hi, cx + ho, c);
        } else {
            fill_xspan(y, cx - ho, cx + ho, c);
        }
    }
}

template <class P>
void BasicCanvas<P>::filled_polygon(std::span<const Point> pts, Color c) {
    const std::size_t n = pts.size();
    if (n < 3) return;
    float min_y = pts[0].y.raw(), max_y = pts[0].y.raw();
    for (const Point p : pts) {
        min_y = std::min(min_y, p.y.raw());
        max_y = std::max(max_y, p.y.raw());
    }
    std::vector<float>& xs = crossings_;
    xs.reserve(n); // a row crosses at most n edges: no growth inside the row loop
    const auto [y0, y1] = clip_rows(span_begin(min_y), span_end(max_y));
    for (int y = y0; y < y1; ++y) {
        const float yc = static_cast<float>(y) + 0.5f;
        xs.clear();
        for (std::size_t i = 0; i < n; ++i) {
            const Point a = pts[i], b = pts[(i + 1) % n];
            const float ya = a.y.raw(), yb = b.y.raw();
            if ((ya <= yc) != (yb <= yc)) {   // half-open: a vertex on the line counts once
                const float t = (yc - ya) / (yb - ya);
                xs.push_back(a.x.raw() + t * (b.x.raw() - a.x.raw()));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t k = 0; k + 1 < xs.size(); k += 2)
            fill_xspan(y, xs[k], xs[k + 1], c);
    }
}

template <class P>
void BasicCanvas<P>::image(const Image& img, Rect dest, std::uint8_t alpha, Color tint) {
    const float dx0 = dest.origin.x.raw(), dy0 = dest.origin.y.raw();
    const float dw = dest.extent.w.raw(), dh = dest.extent.h.raw();
    if (dw <= 0.f || dh <= 0.f) return;
    const int iw = img.width(), ih = img.height();
    const std::span<const std::uint8_t> px = img.pixels();
    const int x0 = std::max(span_begin(dx0), clip_.x0), x1 = std::min(span_end(dx0 + dw), clip_.x1);
    const auto [y0, y1] = clip_rows(span_begin(dy0), span_end(dy0 + dh));
    for (int y = y0; y < y1; ++y) {
        const float v = (static_cast<float>(y) + 0.5f - dy0) / dh;
        const int sy = std::clamp(static_cast<int>(v * ih), 0, ih - 1);
        for (int x = x0; x < x1; ++x) {
            const float u = (static_cast<float>(x) + 0.5f - dx0) / dw;
            const int sx = std::clamp(static_cast<int>(u * iw), 0, iw - 1);
            const std::uint8_t* t = px.data() + (static_cast<std::size_t>(sy) * iw + sx) * 4;
            const int sa = (static_cast<int>(t[3]) * alpha + 127) / 255;
            if (sa == 0) continue;
            blend_pixel(x, y,
                        Color::rgba(static_cast<std::uint8_t>((t[0] * tint.r + 127) / 255),
                                    static_cast<std::uint8_t>((t[1] * tint.g + 127) / 255),
                                    static_cast<std::uint8_t>((t[2] * tint.b + 127) / 255),
                                    static_cast<std::uint8_t>(sa)));
        }
    }
}

template <class P>
void BasicCanvas<P>::text(const Fonts& fonts, std::string_view utf8, Point origin, float size, Color col,
                  float angle, TextAnchor anchor, FontStyle style) {
    if (utf8.empty() || size <= 0.f) return;
    const float w = fonts.width(utf8, size, style);
    const float h = fonts.line_height(size, style);
    float bx = origin.x.raw(), by = origin.y.raw();
    if (anchor == TextAnchor::Center) {
        bx -= w / 2.f;
        by -= h / 2.f;
    }
    // simplify: angle != 0 is drawn unrotated (the SDL backend rotates the rasterized text box
    // around its pivot). Upgrade: rotate the pen per glyph -- exact for 90-degree multiples.
    (void)angle;
    const float baseline = by + fonts.ascent(size, style);
    const int face = Fonts::face_of(style);
    float pen = bx;
    std::uint32_t prev = 0;
    for (std::size_t i = 0; i < utf8.size();) {
        const std::size_t next = prism::core::utf8::next(utf8, i);
        const std::uint32_t cp = decode_codepoint(utf8, i);
        if (prev != 0) pen += fonts.kern(face, prev, cp, size);
        const auto g = fonts.glyph(face, cp, size);
        if (g && g->w > 0 && g->h > 0)
            blend_bitmap(static_cast<int>(std::lround(pen + g->xoff)),
                         static_cast<int>(std::lround(baseline + g->yoff)),
                         g->w, g->h, g->bitmap.data(), col);
        pen += g ? g->advance : fonts.advance(face, cp, size);
        prev = cp;
        i = next;
    }
}

template <class P>
void BasicCanvas<P>::blend_pixel(int x, int y, Color c) {
    if (x < clip_.x0 || x >= clip_.x1 || y < clip_.y0 || y >= clip_.y1) return;
    pixel& px = row(y)[x];
    px = P::over(px, c);
}

template <class P>
void BasicCanvas<P>::blend_bitmap(int x, int y, int w, int h, const std::uint8_t* coverage, Color c) {
    const auto [j0, j1] = clip_rows(y, y + h);
    for (int j = j0 - y; j < j1 - y; ++j) {
        for (int i = 0; i < w; ++i) {
            const int a = (static_cast<int>(c.a) * coverage[static_cast<std::size_t>(j) * w + i] + 127) / 255;
            if (a == 0) continue;
            blend_pixel(x + i, y + j,
                        Color::rgba(c.r, c.g, c.b, static_cast<std::uint8_t>(a)));
        }
    }
}

template class BasicCanvas<Argb8888>;
template class BasicCanvas<Rgb565>;

} // namespace prism::raster
