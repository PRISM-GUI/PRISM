#pragma once

// One area-average routine behind every way PRISM resizes or converts pixels: image decoders,
// covers that need a smaller picture, and callers that need a different format. Images are shared
// and immutable, so `scaled` and `converted` hand back either a new one or the same pointer when
// nothing has to change.

#include <prism/core/types.hpp>
#include <prism/render/draw_list.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace prism::render {

// Where a view's bytes come from: an decoded bitmap (PNG via SDL, a hardware JPEG decoder), a
// canvas' pixels, or an Image. rgb888 has no alpha, so its pixels are opaque.
enum class SourceFormat : std::uint8_t { rgb888, rgba8888, rgb565 };

// Pixels of one source format, row-major, with `stride` bytes between the start of one row and the
// next. A stride longer than a row (an engine padding its lines out, say) is stepped over, not
// read: only the first width*bytes_per_pixel of each row are pixels.
struct PixelView {
    std::span<const std::byte> bytes;
    int width = 0;
    int height = 0;
    int stride = 0;
    SourceFormat format = SourceFormat::rgba8888;
};

constexpr int bytes_per_pixel(SourceFormat format) {
    switch (format) {
    case SourceFormat::rgb888: return 3;
    case SourceFormat::rgba8888: return 4;
    case SourceFormat::rgb565: return 2;
    }
    return 0;
}

constexpr int bytes_per_pixel(PixelFormat format) {
    switch (format) {
    case PixelFormat::rgba8888: return 4;
    case PixelFormat::rgb565: return 2;
    }
    return 0;
}

// `view` scaled into `box` and written as `out`. Each output pixel is the area average of the
// source rectangle it covers (integer edges at i*src/dst), so downsizing a picture averages it
// rather than dropping rows; the size keeps the aspect ratio and never grows. Null when the view
// is empty, `box` is not positive, or the bytes do not describe the width, height and stride.
std::shared_ptr<const Image> fit_pixels(PixelView view, core::Size box, PixelFormat out);

// The image's own pixels as a view: a stride of width*bytes_per_pixel, and its format.
PixelView view_of(const Image& image);

// The image at most `box` big, in its own format. The same pointer when it already fits: enlarging
// is never what a caller wants here.
std::shared_ptr<const Image> scaled(const std::shared_ptr<const Image>& image, core::Size box);

// The image in `format`, at its own size. The same pointer when it already is in `format`.
std::shared_ptr<const Image> converted(const std::shared_ptr<const Image>& image, PixelFormat format);

} // namespace prism::render
