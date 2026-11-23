#include <prism/render/image_ops.hpp>

#include <prism/raster/pixel.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace prism::render {

namespace {

// One source pixel as straight R, G, B, A bytes. rgb888 has no alpha, so it reads as opaque.
std::array<std::uint8_t, 4> read_pixel(const std::byte* row, int x, SourceFormat format) {
    switch (format) {
    case SourceFormat::rgb888: {
        const auto* p = reinterpret_cast<const std::uint8_t*>(row) + x * 3;
        return {p[0], p[1], p[2], 255};
    }
    case SourceFormat::rgba8888: {
        const auto* p = reinterpret_cast<const std::uint8_t*>(row) + x * 4;
        return {p[0], p[1], p[2], p[3]};
    }
    case SourceFormat::rgb565: {
        std::uint16_t word = 0;
        std::memcpy(&word, row + x * 2, 2);
        const std::uint32_t r5 = word >> 11, g6 = (word >> 5) & 0x3Fu, b5 = word & 0x1Fu;
        return {static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2)),
                static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4)),
                static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2)), 255};
    }
    }
    return {0, 0, 0, 0};
}

// 64-bit sums: a 4000x4000 picture into a 15x15 box puts 71k pixels through one channel sum, each
// worth up to 255*255, so a 32-bit accumulator wraps.
int div_round(std::uint64_t sum, std::uint64_t count) {
    return static_cast<int>((sum + count / 2) / count);
}

// One output pixel's source rectangle added up with the channels weighted by their alpha: the
// colour of a transparent pixel then cannot tint its neighbours, and the averages divide the
// alpha back out.
struct Sums {
    std::uint64_t r = 0, g = 0, b = 0, a = 0;
};

Sums sum_block(const PixelView& view, int x0, int x1, int y0, int y1) {
    Sums sums;
    for (int y = y0; y < y1; ++y) {
        const auto* row = view.bytes.data() + static_cast<std::ptrdiff_t>(y) * view.stride;
        for (int x = x0; x < x1; ++x) {
            const auto pixel = read_pixel(row, x, view.format);
            sums.r += static_cast<std::uint64_t>(pixel[0]) * pixel[3];
            sums.g += static_cast<std::uint64_t>(pixel[1]) * pixel[3];
            sums.b += static_cast<std::uint64_t>(pixel[2]) * pixel[3];
            sums.a += pixel[3];
        }
    }
    return sums;
}

Color average(const Sums& sums, std::uint64_t count) {
    const auto alpha = static_cast<std::uint8_t>(div_round(sums.a, count));
    if (sums.a == 0) return Color::rgba(0, 0, 0, 0);
    return Color::rgba(static_cast<std::uint8_t>(div_round(sums.r, sums.a)),
                       static_cast<std::uint8_t>(div_round(sums.g, sums.a)),
                       static_cast<std::uint8_t>(div_round(sums.b, sums.a)), alpha);
}

// The size `width x height` takes inside the box: the aspect ratio kept, never enlarged. Both
// sides come from integer cross-multiplication, not from a float ratio: 22/13 in float32 lands
// just under 1, and truncating it would lose a pixel of the box.
struct Fit {
    int width = 0, height = 0;
};

Fit fit_size(int width, int height, int box_width, int box_height) {
    if (width <= 0 || height <= 0 || box_width <= 0 || box_height <= 0) return {};
    const auto wide = static_cast<std::int64_t>(box_width) * height;
    const auto tall = static_cast<std::int64_t>(box_height) * width;
    if (wide <= tall) {
        // The box width is the tighter side: it is the size, and the height follows from it.
        if (box_width >= width) return {width, height};
        return {box_width,
                std::max(1, static_cast<int>(static_cast<std::int64_t>(height) * box_width / width))};
    }
    if (box_height >= height) return {width, height};
    return {std::max(1, static_cast<int>(static_cast<std::int64_t>(width) * box_height / height)), box_height};
}

struct ByteWriter {
    std::vector<std::uint8_t> bytes;
    void push(Color color) { bytes.insert(bytes.end(), {color.r, color.g, color.b, color.a}); }
};

struct WordWriter {
    std::vector<std::uint16_t> words;
    void push(Color color) { words.push_back(prism::raster::Rgb565::pack(color)); }
};

// Source rows and columns of output pixel i: [i*src/dst, (i+1)*src/dst), so the blocks tile the
// source exactly and no row is dropped.
int edge(int index, int source, int destination) {
    return static_cast<int>(static_cast<long>(index) * source / destination);
}

template <class Writer>
void average_into(const PixelView& view, Fit fit, Writer& writer) {
    for (int y = 0; y < fit.height; ++y) {
        const int y0 = edge(y, view.height, fit.height);
        const int y1 = edge(y + 1, view.height, fit.height);
        for (int x = 0; x < fit.width; ++x) {
            const int x0 = edge(x, view.width, fit.width);
            const int x1 = edge(x + 1, view.width, fit.width);
            const auto count = static_cast<std::uint64_t>(x1 - x0) * static_cast<std::uint64_t>(y1 - y0);
            writer.push(average(sum_block(view, x0, x1, y0, y1), count));
        }
    }
}

bool describes_itself(const PixelView& view) {
    if (view.width <= 0 || view.height <= 0) return false;
    const auto bpp = bytes_per_pixel(view.format);
    if (bpp == 0 || view.stride < view.width * bpp) return false;
    const auto needed = static_cast<std::size_t>(view.height - 1) * static_cast<std::size_t>(view.stride) +
                        static_cast<std::size_t>(view.width) * bpp;
    return view.bytes.size() >= needed;
}

} // namespace

std::shared_ptr<const Image> fit_pixels(PixelView view, core::Size box, PixelFormat out) {
    if (!describes_itself(view)) return nullptr;
    const auto fit = fit_size(view.width, view.height, static_cast<int>(box.w.raw()),
                              static_cast<int>(box.h.raw()));
    if (fit.width <= 0 || fit.height <= 0) return nullptr;

    if (out == PixelFormat::rgb565) {
        WordWriter writer;
        writer.words.reserve(static_cast<std::size_t>(fit.width) * fit.height);
        average_into(view, fit, writer);
        return Image::from_rgb565(fit.width, fit.height, std::move(writer.words)).value_or(nullptr);
    }
    ByteWriter writer;
    writer.bytes.reserve(static_cast<std::size_t>(fit.width) * fit.height * 4);
    average_into(view, fit, writer);
    return Image::from_rgba(fit.width, fit.height, std::move(writer.bytes)).value_or(nullptr);
}

PixelView view_of(const Image& image) {
    const auto format = image.format() == PixelFormat::rgb565 ? SourceFormat::rgb565 : SourceFormat::rgba8888;
    return {std::as_bytes(image.pixels()), image.width(), image.height(),
            image.width() * bytes_per_pixel(format), format};
}

std::shared_ptr<const Image> scaled(const std::shared_ptr<const Image>& image, core::Size box) {
    if (!image) return nullptr;
    const auto fit = fit_size(image->width(), image->height(), static_cast<int>(box.w.raw()),
                              static_cast<int>(box.h.raw()));
    // It already fits: copy-on-nothing would cost the caller an image and a re-upload.
    if (fit.width == image->width() && fit.height == image->height()) return image;
    return fit_pixels(view_of(*image), box, image->format());
}

std::shared_ptr<const Image> converted(const std::shared_ptr<const Image>& image, PixelFormat format) {
    if (!image) return nullptr;
    if (image->format() == format) return image;
    const core::Size box{core::Width{static_cast<float>(image->width())},
                         core::Height{static_cast<float>(image->height())}};
    return fit_pixels(view_of(*image), box, format);
}

// Whitening is a per-pixel loop over four bytes, so an rgb565 image becomes rgba8888 first: doing
// it in place would touch its neighbours' bytes.
std::shared_ptr<const Image> Image::as_symbolic() const {
    std::vector<std::uint8_t> px;
    if (format_ == PixelFormat::rgba8888) {
        px = pixels_;
    } else {
        const core::Size box{core::Width{static_cast<float>(width_)}, core::Height{static_cast<float>(height_)}};
        const auto rgba = fit_pixels(view_of(*this), box, PixelFormat::rgba8888);
        if (!rgba) return nullptr;
        px.assign(rgba->pixels().begin(), rgba->pixels().end());
    }
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) px[i] = px[i + 1] = px[i + 2] = 255;
    auto image = std::shared_ptr<Image>(new Image(width_, height_, PixelFormat::rgba8888, std::move(px)));
    image->symbolic_ = true;
    return image;
}

} // namespace prism::render
