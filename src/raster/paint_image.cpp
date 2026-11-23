#include <prism/raster/paint_image.hpp>

#include <prism/render/draw_list.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace prism::raster {
namespace {

// Argb8888 keeps 0xAARRGGBB in memory as B, G, R, A; an Image wants R, G, B, A.
std::vector<std::uint8_t> rgba_bytes(std::span<const std::uint32_t> words) {
    std::vector<std::uint8_t> rgba(words.size() * 4);
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto word = words[i];
        rgba[i * 4 + 0] = static_cast<std::uint8_t>(word >> 16);        // R
        rgba[i * 4 + 1] = static_cast<std::uint8_t>(word >> 8);         // G
        rgba[i * 4 + 2] = static_cast<std::uint8_t>(word);              // B
        rgba[i * 4 + 3] = static_cast<std::uint8_t>(word >> 24);        // A
    }
    return rgba;
}

} // namespace

template <class P>
std::shared_ptr<const render::Image>
paint_image(int width, int height, const std::function<void(BasicCanvas<P>&)>& paint) {
    if (width <= 0 || height <= 0) return nullptr;
    // Zero-filled: transparent for Argb8888, black for Rgb565.
    std::vector<typename P::storage> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    BasicCanvas<P> canvas{pixels, width, height, width};
    paint(canvas);
    if constexpr (std::is_same_v<P, Argb8888>) {
        return render::Image::from_rgba(width, height, rgba_bytes(pixels)).value_or(nullptr);
    } else {
        return render::Image::from_rgb565(width, height, std::move(pixels)).value_or(nullptr);
    }
}

template std::shared_ptr<const render::Image>
paint_image<Argb8888>(int width, int height, const std::function<void(BasicCanvas<Argb8888>&)>& paint);
template std::shared_ptr<const render::Image>
paint_image<Rgb565>(int width, int height, const std::function<void(BasicCanvas<Rgb565>&)>& paint);

} // namespace prism::raster
