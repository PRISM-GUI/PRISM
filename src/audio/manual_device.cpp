#include <prism/audio/manual_device.hpp>

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>

namespace prism::audio {
namespace {

constexpr const char* kDeviceId = "manual";

} // namespace

struct ManualDevice::Impl final : DeviceBackend {
    Impl(SampleType type, std::vector<int> device_rates) : native(type), rates(std::move(device_rates)) {}

    // The Output's side of the device: it sets the volume and reports whether the device went away.
    struct Stream final : DeviceStream {
        explicit Stream(Impl& device) : device(device)
        {
            device.opened = true;
            device.lost_flag = false;
        }

        ~Stream() override
        {
            device.pull = nullptr;   // the Output is gone: pulls from now on are silence
            device.opened = false;
        }

        [[nodiscard]] bool lost() const override { return device.lost_flag; }

        bool set_volume(float volume) override
        {
            device.volume = volume;
            return true;   // this device takes the volume itself, so Output never scales
        }

        Impl& device;
    };

    std::vector<DeviceInfo> devices() override
    {
        return {DeviceInfo{.id = kDeviceId,
                           .name = "Manual device",
                           .rates = rates,
                           .max_channels = 2,
                           .native = native,
                           .is_default = false}};
    }

    std::unique_ptr<DeviceStream> open(std::string_view id, const Format& wanted, PullFn pull,
                                       std::string& error) override
    {
        if (id != kDeviceId) {
            error = "no such device";
            return nullptr;
        }
        if (!devices().front().accepts(wanted)) {
            error = "the device cannot play this format";
            return nullptr;
        }
        format = wanted;
        this->pull = std::move(pull);
        s16.clear();
        f32.clear();
        return std::make_unique<Stream>(*this);
    }

    std::size_t sample_count(int frames) const
    {
        return static_cast<std::size_t>(std::max(frames, 0)) * static_cast<std::size_t>(channels());
    }

    int channels() const { return format.channels > 0 ? format.channels : 2; }

    // Render stops here too: a pull with no Output, after a loss, or of the other sample type is
    // silence that never reaches the Output's render function.
    bool can_render() const { return opened && !lost_flag && pull != nullptr; }

    std::span<const std::int16_t> pull_s16(int frames)
    {
        const auto samples = sample_count(frames);
        s16.assign(samples, 0);
        if (native == SampleType::s16 && can_render()) pull(std::as_writable_bytes(std::span{s16}), frames);
        return s16;
    }

    std::span<const float> pull_f32(int frames)
    {
        const auto samples = sample_count(frames);
        f32.assign(samples, 0);
        if (native == SampleType::f32 && can_render()) pull(std::as_writable_bytes(std::span{f32}), frames);
        return f32;
    }

    SampleType native;
    std::vector<int> rates;
    Format format;
    PullFn pull;
    std::atomic<bool> opened{false};
    std::atomic<bool> lost_flag{false};
    std::atomic<float> volume{1.f};
    std::vector<std::int16_t> s16;
    std::vector<float> f32;
};

ManualDevice::ManualDevice(SampleType native, std::vector<int> rates)
    : impl_(std::make_unique<Impl>(native, std::move(rates)))
{
    register_backend(*impl_);
}

// The Outputs playing on this device must be gone before it dies: their stream holds a reference
// to it, and closing it is the device's own business.
ManualDevice::~ManualDevice() { unregister_backend(*impl_); }

std::span<const std::int16_t> ManualDevice::pull_s16(int frames) { return impl_->pull_s16(frames); }

std::span<const float> ManualDevice::pull_f32(int frames) { return impl_->pull_f32(frames); }

void ManualDevice::lose() { impl_->lost_flag = true; }

bool ManualDevice::open() const { return impl_->opened; }

float ManualDevice::device_volume() const { return impl_->volume; }

} // namespace prism::audio
