#include <prism/render/image_decoder.hpp>

#include <algorithm>
#include <exception>
#include <fstream>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace prism::render {
namespace {

// The live decoders, in registration order. Function-local statics so a decoder registering from
// another static's constructor cannot run before them. Readers (decodes) take it shared, so the
// process can decode on several threads; changing the list takes it alone.
std::shared_mutex& registry_mutex() {
    static std::shared_mutex mutex;
    return mutex;
}

std::vector<ImageDecoder*>& registry() {
    static std::vector<ImageDecoder*> registered;
    return registered;
}

} // namespace

void register_decoder(ImageDecoder& decoder) {
    const std::unique_lock lock{registry_mutex()};
    if (std::ranges::find(registry(), &decoder) == registry().end()) registry().push_back(&decoder);
}

void unregister_decoder(ImageDecoder& decoder) {
    const std::unique_lock lock{registry_mutex()};
    std::erase(registry(), &decoder);
}

std::vector<ImageDecoder*> decoders() {
    const std::unique_lock lock{registry_mutex()};
    return {registry().rbegin(), registry().rend()};
}

// Decodes take the registry shared and unregistering takes it alone: decodes run side by side, and
// unregistering waits for the ones in flight, so a decoder never dies under a caller. A decoder
// must not call back into the registry (decoders(), register_decoder()): that waits on this same
// lock. One that is not safe to run twice at once serialises itself, as it should.
std::expected<std::shared_ptr<const Image>, std::string>
decode_image(std::span<const std::byte> bytes, const DecodeOptions& options) {
    const std::shared_lock lock{registry_mutex()};
    if (registry().empty()) return std::unexpected(std::string{"no image decoder"});
    std::string failure;
    // Newest first: the platform's own decoder is asked before anything a caller added.
    for (auto it = registry().rbegin(); it != registry().rend(); ++it) {
        std::string error;
        try {
            if (auto image = (*it)->decode(bytes, options, error); image) return image;
        } catch (const std::exception& e) {
            error = e.what();
        } catch (...) {
            error = "the image decoder threw";
        }
        failure = error.empty() ? std::string{"the image decoder failed"} : std::move(error);
    }
    return std::unexpected(std::move(failure));
}

std::expected<std::shared_ptr<const Image>, std::string>
decode_image(std::span<const std::byte> bytes, std::optional<core::Size> svg_size) {
    return decode_image(bytes, DecodeOptions{.fit = svg_size, .format = PixelFormat::rgba8888});
}

std::expected<std::shared_ptr<const Image>, std::string>
load_image(const std::filesystem::path& path, const DecodeOptions& options) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected("cannot read " + path.string());
    std::vector<char> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return decode_image(std::as_bytes(std::span{data}), options);
}

std::expected<std::shared_ptr<const Image>, std::string>
load_image(const std::filesystem::path& path, std::optional<core::Size> svg_size) {
    return load_image(path, DecodeOptions{.fit = svg_size, .format = PixelFormat::rgba8888});
}

} // namespace prism::render
