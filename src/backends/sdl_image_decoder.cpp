// The SDL3_image decoder, in the registry from the start (render/image_decoders.cpp): a program that
// links the SDL backend reads PNG, JPEG, SVG and the rest with no call of its own.
#include <prism/render/image_decoder.hpp>
#include <prism/render/image_ops.hpp>

#include "sdl_registered.hpp"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace prism::render {
namespace {

// SDL's surface as an Image: four bytes a pixel, straight alpha, one row at a time past the pitch.
std::expected<std::shared_ptr<const Image>, std::string> to_image(SDL_Surface* decoded) {
    SDL_Surface* rgba = SDL_ConvertSurface(decoded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(decoded);
    if (!rgba) return std::unexpected(std::string{SDL_GetError()});
    const int width = rgba->w;
    const int height = rgba->h;
    const auto row = static_cast<std::size_t>(width) * 4;
    std::vector<std::uint8_t> pixels(row * static_cast<std::size_t>(height));
    const auto* source = static_cast<const std::uint8_t*>(rgba->pixels);
    for (int y = 0; y < height; ++y) {
        std::memcpy(pixels.data() + static_cast<std::size_t>(y) * row,
                    source + static_cast<std::size_t>(y) * static_cast<std::size_t>(rgba->pitch), row);
    }
    SDL_DestroySurface(rgba);
    return Image::from_rgba(width, height, std::move(pixels));
}

class SdlImageDecoder final : public ImageDecoder {
public:
    std::shared_ptr<const Image> decode(std::span<const std::byte> bytes, const DecodeOptions& options,
                                        std::string& error) override {
        SDL_IOStream* io = SDL_IOFromConstMem(bytes.data(), bytes.size());
        if (!io) {
            error = SDL_GetError();
            return nullptr;
        }
        SDL_Surface* decoded = nullptr;
        if (options.fit && IMG_isSVG(io)) {
            // A vector image drawn at the size asked for keeps lines sharp; scaling it afterwards
            // would blur what the SVG could have drawn afresh.
            decoded = IMG_LoadSizedSVG_IO(io, static_cast<int>(options.fit->w.raw()),
                                          static_cast<int>(options.fit->h.raw()));
            SDL_CloseIO(io); // LoadSizedSVG_IO leaves the stream to its caller
        } else {
            SDL_SeekIO(io, 0, SDL_IO_SEEK_SET); // the probe above may have read ahead
            decoded = IMG_Load_IO(io, true);    // this one closes the stream
        }
        if (!decoded) {
            error = SDL_GetError();
            return nullptr;
        }
        const auto image = to_image(decoded);
        if (!image) {
            error = image.error();
            return nullptr;
        }
        const auto fitted = options.fit ? scaled(*image, *options.fit) : *image;
        if (!fitted) {
            error = "cannot scale the image";
            return nullptr;
        }
        return converted(fitted, options.format);
    }
};

} // namespace

// One decoder for the process, made on first use: render/image_decoders.cpp starts with it.
ImageDecoder& sdl_image_decoder() {
    static SdlImageDecoder decoder;
    return decoder;
}

} // namespace prism::render
