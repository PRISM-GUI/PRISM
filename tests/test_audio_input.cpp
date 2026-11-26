// audio::Input over a fake CaptureBackend (include/prism/audio/input.hpp): the blocks a capture
// device pushes come back out of read(), s16 converted to float, on_samples once per arrival, and
// a device nothing lists means ok() false and read() nothing.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/audio/backend.hpp>
#include <prism/audio/input.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace prism;

// The format every test records at, and the fake device's id: a desktop build registers SDL's
// capture backend too, so a test that wants its own blocks asks for the fake by name.
constexpr audio::Format kFormat{.sample_rate = 16000, .channels = 1, .block_frames = 256};

// One open capture device: the test pushes through it as the device's own thread would.
struct FakeStream final : audio::DeviceStream {
    audio::PushFn push;
    audio::Format format;

    [[nodiscard]] bool lost() const override { return false; }
};

// A capture backend a test drives by hand: one default device, whose blocks the test hands over.
class FakeCapture final : public audio::CaptureBackend {
public:
    explicit FakeCapture(audio::SampleType native = audio::SampleType::f32) : native_(native)
    {
        audio::register_capture_backend(*this);
    }
    ~FakeCapture() override { audio::unregister_capture_backend(*this); }

    FakeCapture(const FakeCapture &) = delete;
    FakeCapture &operator=(const FakeCapture &) = delete;

    [[nodiscard]] std::vector<audio::DeviceInfo> devices() override
    {
        return {audio::DeviceInfo{.id = "fake",
                                  .name = "Fake capture",
                                  .rates = {},
                                  .max_channels = 1,
                                  .native = native_,
                                  .is_default = true}};
    }

    [[nodiscard]] std::unique_ptr<audio::DeviceStream>
    open(std::string_view id, const audio::Format &format, audio::PushFn push) override
    {
        ++opens_;
        if (id != "fake")
            return nullptr;
        auto stream = std::make_unique<FakeStream>();
        stream->format = format;
        stream->push = std::move(push);
        open_format_ = format;
        stream_ = stream.get();
        return stream;
    }

    // One mono block: `samples.size()` frames.
    void push(std::span<const float> samples) { deliver(std::as_bytes(samples), int(samples.size())); }
    void push(std::span<const std::int16_t> samples) { deliver(std::as_bytes(samples), int(samples.size())); }

    [[nodiscard]] int opens() const { return opens_; }
    [[nodiscard]] audio::Format open_format() const { return open_format_; }

private:
    void deliver(std::span<const std::byte> bytes, int frames)
    {
        REQUIRE(stream_ != nullptr);
        stream_->push(bytes, frames);
    }

    audio::SampleType native_;
    FakeStream *stream_ = nullptr;
    audio::Format open_format_{};
    int opens_ = 0;
};

bool lists(std::span<const audio::DeviceInfo> devices, std::string_view id)
{
    return std::ranges::any_of(devices, [id](const audio::DeviceInfo &device) { return device.id == id; });
}

} // namespace

TEST_CASE("capture_devices lists a registered capture backend while it lives")
{
    CHECK_FALSE(lists(audio::capture_devices(), "fake"));
    {
        FakeCapture fake;
        CHECK(lists(audio::capture_devices(), "fake"));
    }
    CHECK_FALSE(lists(audio::capture_devices(), "fake"));
}

TEST_CASE("Input hands back the f32 block its device pushed")
{
    FakeCapture fake{audio::SampleType::f32};
    audio::Input in{kFormat, "fake"};
    REQUIRE(in.ok());
    CHECK(in.sample_rate() == doctest::Approx(16000.f));
    CHECK_FALSE(in.name().empty());
    CHECK(fake.opens() == 1);
    CHECK(fake.open_format().sample_rate == 16000);
    CHECK(fake.open_format().channels == 1);

    const std::array<float, 3> block{0.25f, -0.5f, 0.75f};
    fake.push(block);

    std::array<float, 8> out{};
    REQUIRE(in.read(out) == block.size());
    CHECK(out[0] == doctest::Approx(0.25f));
    CHECK(out[1] == doctest::Approx(-0.5f));
    CHECK(out[2] == doctest::Approx(0.75f));
    CHECK(in.read(out) == 0); // nothing left over
}

TEST_CASE("Input converts an s16 block to float")
{
    FakeCapture fake{audio::SampleType::s16};
    audio::Input in{kFormat, "fake"};
    REQUIRE(in.ok());

    const std::array<std::int16_t, 3> block{16384, -32767, 0};
    fake.push(block);

    std::array<float, 3> out{};
    REQUIRE(in.read(out) == block.size());
    CHECK(out[0] == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(out[1] == doctest::Approx(-1.f));
    CHECK(out[2] == 0.f);
}

TEST_CASE("on_samples runs once per block, on the thread that pushed it")
{
    FakeCapture fake;
    std::atomic<int> calls{0};
    std::thread::id callback_thread{};
    audio::Input in{kFormat, "fake", [&] {
                        ++calls;
                        callback_thread = std::this_thread::get_id();
                    }};
    REQUIRE(in.ok());

    const std::array<float, 2> first{0.1f, 0.2f};
    const std::array<float, 1> second{0.3f};
    fake.push(first);
    fake.push(second);
    CHECK(calls.load() == 2);
    CHECK(callback_thread == std::this_thread::get_id());

    std::array<float, 8> out{};
    REQUIRE(in.read(out) == 3); // both blocks are in the ring
    CHECK(out[2] == doctest::Approx(0.3f));
}

TEST_CASE("read takes what fits and leaves the rest buffered")
{
    FakeCapture fake;
    audio::Input in{kFormat, "fake"};
    REQUIRE(in.ok());

    const std::array<float, 4> block{1.f, 2.f, 3.f, 4.f};
    fake.push(block);

    std::array<float, 2> out{};
    CHECK(in.read(out) == 2);
    CHECK(out[0] == 1.f);
    CHECK(out[1] == 2.f);
    CHECK(in.read(out) == 2);
    CHECK(out[0] == 3.f);
    CHECK(out[1] == 4.f);
    CHECK(in.read(out) == 0);
}

TEST_CASE("a device no backend lists leaves Input not ok, reading nothing")
{
    // Not "no backend at all": a desktop build always registers SDL's. The fallback an app takes
    // (the scope's sweep) depends on this one, not on an empty registry.
    audio::Input in{kFormat, "no-such-device"};
    CHECK_FALSE(in.ok());
    CHECK(in.name().empty());
    std::array<float, 16> out{};
    CHECK(in.read(out) == 0);
}
