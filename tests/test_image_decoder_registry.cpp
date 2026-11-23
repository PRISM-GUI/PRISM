// The image decoder registry (render/image_decoder.hpp): who is asked, in what order, and what a
// caller is told when nobody can read the bytes.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/raster/pixel.hpp>
#include <prism/render/draw_list.hpp>
#include <prism/render/image_decoder.hpp>
#include <prism/render/png_encode.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace prism;
using namespace prism::core;
using namespace prism::render;

namespace {

std::span<const std::byte> bytes_of(std::string_view s) {
    return std::as_bytes(std::span{s.data(), s.size()});
}

// A 1x1 image the fake decoders hand back: its red channel says which one answered.
std::shared_ptr<const Image> dot(std::uint8_t red) {
    return *Image::from_rgba(1, 1, {red, 0, 0, 255});
}

// A decoder that answers whatever it is asked, and records what it was asked.
class FakeDecoder final : public ImageDecoder {
public:
    explicit FakeDecoder(std::uint8_t red) : red_(red) {}

    std::shared_ptr<const Image> decode(std::span<const std::byte> bytes, const DecodeOptions& options,
                                        std::string& error) override {
        (void)bytes;
        ++calls;
        last = options;
        if (throws) throw std::runtime_error{"boom"};
        if (fails) {
            error = failure;
            return nullptr;
        }
        return dot(red_);
    }

    int calls = 0;
    bool fails = false;
    bool throws = false;
    std::string failure = "not mine";
    DecodeOptions last;

private:
    std::uint8_t red_;
};

// An empty registry for one test's duration. This binary also links SDL's decoder, so a test that
// counts who was asked puts it aside first; the destructor hands it back in its old order.
class EmptyRegistry {
public:
    EmptyRegistry() {
        saved_ = decoders();
        for (auto* decoder : saved_) unregister_decoder(*decoder);
    }
    ~EmptyRegistry() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) register_decoder(**it);
    }
    EmptyRegistry(const EmptyRegistry&) = delete;
    EmptyRegistry& operator=(const EmptyRegistry&) = delete;

private:
    std::vector<ImageDecoder*> saved_;
};

// A decoder that will not answer until two calls are inside it at once: if decodes were serialised,
// the first would give up instead of meeting the second. It times out rather than hang, so the test
// fails with a message instead of never finishing.
class RendezvousDecoder final : public ImageDecoder {
public:
    std::shared_ptr<const Image> decode(std::span<const std::byte> bytes, const DecodeOptions& options,
                                        std::string& error) override {
        (void)bytes;
        (void)options;
        std::unique_lock lock{mutex_};
        if (++inside_ == 2) {
            met_ = true;
            wake_.notify_all();
        }
        if (!wake_.wait_for(lock, std::chrono::seconds{2}, [this] { return met_; })) {
            error = "decodes were serialised";
            return nullptr;
        }
        return dot(30);
    }

private:
    std::mutex mutex_;
    std::condition_variable wake_;
    int inside_ = 0;
    bool met_ = false;
};

} // namespace

TEST_CASE("the most recently registered decoder is asked first") {
    EmptyRegistry empty;
    FakeDecoder older{10};
    FakeDecoder newer{20};
    register_decoder(older);
    register_decoder(newer);

    const auto image = decode_image(bytes_of("bytes"));

    REQUIRE(image);
    CHECK((*image)->pixels()[0] == 20);
    CHECK(newer.calls == 1);
    CHECK(older.calls == 0);
}

TEST_CASE("a decoder that cannot read the bytes falls through to the next") {
    EmptyRegistry empty;
    FakeDecoder older{10};
    FakeDecoder newer{20};
    newer.fails = true;
    newer.failure = "not mine";
    register_decoder(older);
    register_decoder(newer);

    const auto image = decode_image(bytes_of("bytes"));

    REQUIRE(image);
    CHECK((*image)->pixels()[0] == 10);
    CHECK(newer.calls == 1);
    CHECK(older.calls == 1);
}

TEST_CASE("when every decoder fails, the last one tried reports why") {
    EmptyRegistry empty;
    FakeDecoder older{10};
    FakeDecoder newer{20};
    older.fails = true;
    older.failure = "older says no";
    newer.fails = true;
    newer.failure = "newer says no";
    register_decoder(older);
    register_decoder(newer);

    const auto image = decode_image(bytes_of("bytes"));

    REQUIRE_FALSE(image);
    // Newer is tried first, older last: the error a caller sees is the final attempt's.
    CHECK(image.error() == "older says no");
}

TEST_CASE("with no decoder registered the error says so") {
    EmptyRegistry empty;

    const auto image = decode_image(bytes_of("bytes"));

    REQUIRE_FALSE(image);
    CHECK(image.error() == "no image decoder");
}

TEST_CASE("an unregistered decoder is not asked again") {
    EmptyRegistry empty;
    FakeDecoder older{10};
    FakeDecoder newer{20};
    register_decoder(older);
    register_decoder(newer);

    unregister_decoder(newer);
    const auto image = decode_image(bytes_of("bytes"));

    REQUIRE(image);
    CHECK((*image)->pixels()[0] == 10);
    CHECK(newer.calls == 0);
    const auto live = decoders();
    CHECK(std::ranges::find(live, &newer) == live.end());
    CHECK(std::ranges::find(live, &older) != live.end());
}

TEST_CASE("a decoder that returns nothing without saying why is a failure") {
    EmptyRegistry empty;
    FakeDecoder silent{10};
    silent.fails = true;
    silent.failure.clear();
    register_decoder(silent);

    const auto image = decode_image(bytes_of("bytes"));

    REQUIRE_FALSE(image);
    CHECK(image.error() == "the image decoder failed");
}

TEST_CASE("a decoder that throws fails, and the next one still gets its turn") {
    EmptyRegistry empty;
    FakeDecoder good{10};
    FakeDecoder throwing{20};
    throwing.throws = true;
    register_decoder(good);
    register_decoder(throwing);

    REQUIRE(decode_image(bytes_of("bytes")));
    const auto image = decode_image(bytes_of("bytes"));
    REQUIRE(image);
    CHECK((*image)->pixels()[0] == 10);

    // On its own, the throw is what the caller is told.
    unregister_decoder(good);
    const auto alone = decode_image(bytes_of("bytes"));
    REQUIRE_FALSE(alone);
    CHECK(alone.error() == "boom");
}

TEST_CASE("the options asked for reach the decoder") {
    EmptyRegistry empty;
    FakeDecoder decoder{10};
    register_decoder(decoder);
    const DecodeOptions options{.fit = Size{Width{40}, Height{30}}, .format = PixelFormat::rgb565};

    REQUIRE(decode_image(bytes_of("bytes"), options));

    REQUIRE(decoder.last.fit.has_value());
    CHECK(decoder.last.fit->w.raw() == 40);
    CHECK(decoder.last.fit->h.raw() == 30);
    CHECK(decoder.last.format == PixelFormat::rgb565);
}

TEST_CASE("an SVG is still drawn into the box the older overload names") {
    constexpr std::string_view svg =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">)"
        R"(<rect width="10" height="10" fill="#00ff00"/></svg>)";

    const auto image = decode_image(bytes_of(svg), Size{Width{32}, Height{24}});

    REQUIRE(image);
    CHECK((*image)->width() == 24);    // a square image in a 32 x 24 box
    CHECK((*image)->height() == 24);
    CHECK((*image)->format() == PixelFormat::rgba8888);
}

TEST_CASE("the platform's decoder turns a PNG into rgb565 when asked") {
    const std::vector<std::uint8_t> rgba{255, 0, 0, 255, 0, 255, 0, 255};
    const auto png = encode_png(2, 1, rgba);

    const auto image = decode_image(std::as_bytes(std::span{png}),
                                    DecodeOptions{.fit = std::nullopt, .format = PixelFormat::rgb565});

    REQUIRE(image);
    CHECK((*image)->format() == PixelFormat::rgb565);
    CHECK((*image)->width() == 2);
    CHECK((*image)->height() == 1);
    const auto px = (*image)->pixels();
    REQUIRE(px.size() == 4U);
    std::uint16_t first = 0;
    std::memcpy(&first, px.data(), 2);
    CHECK(first == prism::raster::Rgb565::pack(Color::rgba(255, 0, 0)));
    std::uint16_t second = 0;
    std::memcpy(&second, px.data() + 2, 2);
    CHECK(second == prism::raster::Rgb565::pack(Color::rgba(0, 255, 0)));
}

TEST_CASE("the options' fit scales a bitmap down") {
    // 4x4 red, asked for in a 2x2 box: the platform's decoder hands back 2x2.
    const std::vector<std::uint8_t> rgba(4 * 4 * 4, 255);
    const auto png = encode_png(4, 4, rgba);

    const auto image = decode_image(std::as_bytes(std::span{png}),
                                    DecodeOptions{.fit = Size{Width{2}, Height{2}},
                                                  .format = PixelFormat::rgba8888});

    REQUIRE(image);
    CHECK((*image)->width() == 2);
    CHECK((*image)->height() == 2);
}

TEST_CASE("two threads decode at the same time") {
    EmptyRegistry empty;
    RendezvousDecoder decoder;
    register_decoder(decoder);

    std::expected<std::shared_ptr<const Image>, std::string> first;
    std::expected<std::shared_ptr<const Image>, std::string> second;
    std::thread one{[&first] { first = decode_image(bytes_of("bytes")); }};
    std::thread two{[&second] { second = decode_image(bytes_of("bytes")); }};
    one.join();
    two.join();

    REQUIRE(first);
    REQUIRE(second);
    CHECK((*first)->pixels()[0] == 30);
    CHECK((*second)->pixels()[0] == 30);
}
