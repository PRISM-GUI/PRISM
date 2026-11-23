#pragma once

#include <prism/audio/output.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The seam between an Output and a platform's sound hardware. Each platform registers one
// DeviceBackend (SDL on the desktop, the board's codec and Bluetooth stack in prism-esp32), and an
// Output opens a DeviceStream through it. Devices outlive the Outputs that play on them, which is
// what lets a board open its codec once at boot.
namespace prism::audio {

// One device a backend offers. A stream opened on it receives blocks of `native` samples.
struct DeviceInfo {
    std::string id;
    std::string name;
    std::vector<int> rates;   // empty: any rate
    int max_channels = 2;
    SampleType native = SampleType::f32;
    bool is_default = false;

    // Whether a stream opened on this device can play `format`: a listed rate or any rate, and no
    // more channels than the device takes.
    [[nodiscard]] bool accepts(const Format& format) const;
};

// One open device. Its destructor must not return while a pull runs. Once lost() is true it calls
// pull no more. Pull may start before open() returns.
class DeviceStream {
public:
    virtual ~DeviceStream() = default;

    [[nodiscard]] virtual bool lost() const = 0;

    // true when the device takes the volume itself (hardware volume, the platform's mixer), false
    // when the Output has to scale the samples.
    virtual bool set_volume(float volume)
    {
        (void)volume;
        return false;
    }
};

// What a stream calls to fill a block: `frames` frames of the device's native samples, i.e.
// frames * channels samples, written into the byte block the device handed over. It runs on the
// device's thread, so it must not block or allocate.
using PullFn = std::function<void(std::span<std::byte> native_block, int frames)>;

class DeviceBackend {
public:
    virtual ~DeviceBackend() = default;

    [[nodiscard]] virtual std::vector<DeviceInfo> devices() = 0;

    // Opens `id` at `format`; on failure returns null and fills `error`.
    [[nodiscard]] virtual std::unique_ptr<DeviceStream> open(std::string_view id, const Format& format,
                                                             PullFn pull, std::string& error) = 0;
};

// A backend registers itself while it can serve devices; every Output built afterwards sees it.
void register_backend(DeviceBackend& backend);
void unregister_backend(DeviceBackend& backend);

// Every device of every registered backend, the default device first when one is listed.
std::vector<DeviceInfo> devices();

// Opens `id` through the backend that lists it; nothing and an `error` when none does, or when the
// backend refuses. An Output resolves the device with devices() first, so that it knows the type
// of the block before any pull can arrive.
std::unique_ptr<DeviceStream> open_device(std::string_view id, const Format& format, PullFn pull,
                                          std::string& error);

} // namespace prism::audio
