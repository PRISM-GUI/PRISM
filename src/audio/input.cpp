#include <prism/audio/input.hpp>

#include <prism/dev/spsc_ring.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace prism::audio {
namespace {

// A second of 16 kHz mono before the oldest block goes: the ring's newest-block-wins policy is the
// right one for a display, which only ever shows the latest samples. simplify: a fixed ring, so a
// logic thread that stops reading loses the middle of a burst rather than growing memory.
constexpr std::size_t kRingSamples = 4096;

// The device a recorder gets when it asks for none: the first default capture device.
bool pick_device(const Format& format, std::string_view wanted, DeviceInfo& out)
{
    for (auto& device : capture_devices()) {
        if (!wanted.empty() ? device.id != wanted : !device.is_default) continue;
        if (!device.accepts(format)) return false;
        out = std::move(device);
        return true;
    }
    return false;
}

} // namespace

struct Input::Impl {
    Format format;
    std::function<void()> on_samples;
    std::string name;
    SampleType native = SampleType::f32;
    std::unique_ptr<DeviceStream> stream;
    dev::SpscRing<float, kRingSamples> ring;
    std::vector<float> converted; // one block, s16 -> float or a copy of an f32 block

    Impl(const Format& wanted, std::string_view device_id, std::function<void()> callback)
        : format(wanted), on_samples(std::move(callback))
    {
        const auto frames = std::size_t(std::clamp(format.block_frames, 1, kMaxBlockFrames));
        const auto channels = std::size_t(std::max(format.channels, 1));
        converted.resize(frames * channels);
        start(device_id);
    }

    ~Impl() { stream.reset(); } // the stream joins its thread before it returns

    void start(std::string_view device_id)
    {
        DeviceInfo info;
        if (!pick_device(format, device_id, info)) return;
        name = info.name;
        native = info.native;
        std::string error;
        stream = open_capture_device(
            info.id, format, [this](std::span<const std::byte> block, int frames) { deliver(block, frames); },
            error);
    }

    // On the device's thread: fills the ring, then wakes the logic thread once per block.
    void deliver(std::span<const std::byte> block, int frames)
    {
        const auto channels = std::size_t(std::max(format.channels, 1));
        const auto samples = std::min(std::size_t(frames) * channels, converted.size());
        if (native == SampleType::s16) {
            const auto* in = reinterpret_cast<const std::int16_t*>(block.data());
            convert(std::span<const std::int16_t>(in, samples), std::span<float>(converted).first(samples));
        } else {
            const auto* in = reinterpret_cast<const float*>(block.data());
            std::memcpy(converted.data(), in, samples * sizeof(float));
        }
        ring.push(std::span<const float>(converted).first(samples));
        if (on_samples) on_samples();
    }
};

Input::Input(Format format, std::string_view device_id, std::function<void()> on_samples)
    : impl_(std::make_unique<Impl>(format, device_id, std::move(on_samples)))
{
}

Input::~Input() = default;

bool Input::ok() const { return impl_->stream != nullptr; }

std::string_view Input::name() const { return impl_->name; }

float Input::sample_rate() const { return float(impl_->format.sample_rate); }

std::size_t Input::read(std::span<float> out) { return impl_->ring.pop(out); }

} // namespace prism::audio
