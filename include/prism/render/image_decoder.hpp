#pragma once

#include <prism/core/types.hpp>
#include <prism/render/draw_list.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Image files (PNG, JPEG, SVG, ...) decoded into an Image. The decoders are registered backends,
// like the audio devices: SDL3_image's is there from the start, a board registers its hardware
// JPEG engine, and a build with none reports "no image decoder". Safe to call from any thread.
namespace prism::render {

// What a caller wants out of the bytes. `fit` is the box to scale into -- the largest size that
// fits it, keeping the aspect ratio, never growing -- and is omitted for the file's own size; for
// an SVG it is the box the vector is drawn in. `format` is how the pixels should be stored.
struct DecodeOptions {
    std::optional<core::Size> fit;
    PixelFormat format = PixelFormat::rgba8888;
};

// One decoder for one kind of bytes. `decode` returns null and sets `error` when the bytes are not
// its format; a decoder that throws, or returns null without an error, is treated as a failure and
// the next one is asked.
class ImageDecoder {
public:
    virtual ~ImageDecoder() = default;

    virtual std::shared_ptr<const Image> decode(std::span<const std::byte> bytes,
                                                const DecodeOptions& options, std::string& error) = 0;
};

// The registry asks the most recently registered decoder first, so the platform's own (SDL's, a
// board's hardware decoder) is the one that gets a first look.
void register_decoder(ImageDecoder& decoder);
void unregister_decoder(ImageDecoder& decoder);

// The decoders registered right now, most recently registered first. A program that wants to be
// the only decoder -- or tests that need an empty registry -- unregisters these and puts them back.
std::vector<ImageDecoder*> decoders();

std::expected<std::shared_ptr<const Image>, std::string>
decode_image(std::span<const std::byte> bytes, const DecodeOptions& options = {});

// The spelling older callers use, where only an SVG had a size to ask for: the box is the `fit`
// above, so a bitmap is scaled into it as well.
std::expected<std::shared_ptr<const Image>, std::string>
decode_image(std::span<const std::byte> bytes, std::optional<core::Size> svg_size);

std::expected<std::shared_ptr<const Image>, std::string>
load_image(const std::filesystem::path& path, const DecodeOptions& options = {});

std::expected<std::shared_ptr<const Image>, std::string>
load_image(const std::filesystem::path& path, std::optional<core::Size> svg_size);

} // namespace prism::render
